// Manual host-behaviour probe for the panel host-parameter concept
// (doc/mdmm_panel_host_parameters.md). It answers questions only a real DAW can:
// how a MIDI-mapped bool behaves, how large one relative encoder tick is, and
// whether a host accepts a plugin-initiated re-centre as the new relative base.
// Probe v2 adds a non-automatable encoder and configurable re-centre timing.
// Every observed value change is appended to ~/Library/Logs/GearmulatorHostProbe.log
// (or the platform equivalent of the user's application data folder).

#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <atomic>
#include <memory>
#include <vector>

namespace
{
	enum class RecentreMode
	{
		Off,
		NotifyWithoutGesture,
		NotifyWithGesture
	};

	struct LogEntry
	{
		double milliseconds = 0.0;
		int parameter = 0;
		float normalized = 0.0f;
		bool messageThread = false;
		bool selfInitiated = false;
	};

	// JUCE's GenericAudioProcessorEditor hides non-automatable parameters, so the
	// probe needs its own editor to make every parameter reachable from its GUI.
	// The attachments report gestures exactly like the product editor would.
	class ProbeEditor final : public juce::AudioProcessorEditor
	{
	public:
		explicit ProbeEditor(juce::AudioProcessor& _processor) : AudioProcessorEditor(_processor)
		{
			for(auto* const parameter : _processor.getParameters())
			{
				auto* const ranged = dynamic_cast<juce::RangedAudioParameter*>(parameter);
				if(!ranged)
					continue;
				auto& row = m_rows.emplace_back(std::make_unique<Row>());
				row->label.setText(ranged->getName(64) + (ranged->isAutomatable() ? "" : " (not automatable)"),
					juce::dontSendNotification);
				addAndMakeVisible(row->label);
				if(auto* const choice = dynamic_cast<juce::AudioParameterChoice*>(ranged))
				{
					row->comboBox.addItemList(choice->choices, 1);
					addAndMakeVisible(row->comboBox);
					row->comboAttachment = std::make_unique<juce::ComboBoxParameterAttachment>(*choice, row->comboBox);
				}
				else if(auto* const toggle = dynamic_cast<juce::AudioParameterBool*>(ranged))
				{
					addAndMakeVisible(row->button);
					row->buttonAttachment = std::make_unique<juce::ButtonParameterAttachment>(*toggle, row->button);
				}
				else
				{
					row->slider.setSliderStyle(juce::Slider::LinearHorizontal);
					row->slider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 80, 20);
					addAndMakeVisible(row->slider);
					row->sliderAttachment = std::make_unique<juce::SliderParameterAttachment>(*ranged, row->slider);
				}
			}
			setSize(560, static_cast<int>(m_rows.size()) * g_rowHeight + 16);
		}

		void paint(juce::Graphics& _g) override
		{
			_g.fillAll(getLookAndFeel().findColour(juce::ResizableWindow::backgroundColourId));
		}

		void resized() override
		{
			auto area = getLocalBounds().reduced(8);
			for(auto& row : m_rows)
			{
				auto line = area.removeFromTop(g_rowHeight).reduced(0, 4);
				row->label.setBounds(line.removeFromLeft(240));
				row->slider.setBounds(line);
				row->comboBox.setBounds(line);
				row->button.setBounds(line.removeFromLeft(40));
			}
		}

	private:
		static constexpr int g_rowHeight = 36;
		struct Row
		{
			juce::Label label;
			juce::Slider slider;
			juce::ComboBox comboBox;
			juce::ToggleButton button;
			std::unique_ptr<juce::SliderParameterAttachment> sliderAttachment;
			std::unique_ptr<juce::ComboBoxParameterAttachment> comboAttachment;
			std::unique_ptr<juce::ButtonParameterAttachment> buttonAttachment;
		};
		std::vector<std::unique_ptr<Row>> m_rows;
	};

	class ProbeProcessor final : public juce::AudioProcessor, juce::AudioProcessorParameter::Listener,
		juce::Timer
	{
	public:
		ProbeProcessor()
			: AudioProcessor(BusesProperties().withOutput("Output", juce::AudioChannelSet::stereo(), true))
		{
			addParameter(m_button = new juce::AudioParameterBool({"button", 1}, "Probe Button", false));
			addParameter(m_encoder128 = new juce::AudioParameterInt({"encoder128", 1}, "Probe Encoder 128", 0, 127, 64));
			addParameter(m_encoder16k = new juce::AudioParameterInt({"encoder16k", 1}, "Probe Encoder 16k", 0, 16383, 8192));
			addParameter(m_encoderContinuous = new juce::AudioParameterFloat({"encoderContinuous", 1},
				"Probe Encoder Continuous", 0.0f, 1.0f, 0.5f));
			addParameter(m_recentre = new juce::AudioParameterChoice({"recentre", 1}, "Probe Recentre Mode",
				{"Off", "Notify without gesture", "Notify with gesture"}, 0));
			// Probe v2: appended so v1 parameter indices and Live mappings stay valid.
			addParameter(m_encoderNoAutomation = new juce::AudioParameterInt({"encoder128NoAutomation", 2},
				"NOAUTO Encoder", 0, 127, 64,
				juce::AudioParameterIntAttributes().withAutomatable(false)));
			addParameter(m_recentreIdle = new juce::AudioParameterChoice({"recentreIdle", 2},
				"Probe Recentre Idle", {"0 ms", "50 ms", "100 ms", "300 ms", "1000 ms"}, 3));
			addParameter(m_recentreBand = new juce::AudioParameterChoice({"recentreBand", 2},
				"Probe Recentre Band", {"Always", "Outside middle third"}, 0));

			for(auto* const parameter : getParameters())
				parameter->addListener(this);

			m_logFile = juce::File::getSpecialLocation(juce::File::userHomeDirectory)
				.getChildFile("Library/Logs/GearmulatorHostProbe.log");
			m_logFile.getParentDirectory().createDirectory();
			m_logFile.appendText("\n# session " + juce::Time::getCurrentTime().toISO8601(true)
				+ " wrapper=" + juce::String(getWrapperTypeDescription(wrapperType))
				+ " probe=v2\n# ms parameter normalized plain thread origin\n");
			startTimer(10);
		}

		~ProbeProcessor() override
		{
			stopTimer();
			flushLog();
			for(auto* const parameter : getParameters())
				parameter->removeListener(this);
		}

		const juce::String getName() const override { return "Gearmulator Host Probe"; }
		bool acceptsMidi() const override { return true; }
		bool producesMidi() const override { return false; }
		double getTailLengthSeconds() const override { return 0.0; }
		int getNumPrograms() override { return 1; }
		int getCurrentProgram() override { return 0; }
		void setCurrentProgram(int) override {}
		const juce::String getProgramName(int) override { return {}; }
		void changeProgramName(int, const juce::String&) override {}
		void prepareToPlay(double, int) override {}
		void releaseResources() override {}
		void processBlock(juce::AudioBuffer<float>& _buffer, juce::MidiBuffer&) override { _buffer.clear(); }
		bool hasEditor() const override { return true; }
		juce::AudioProcessorEditor* createEditor() override { return new ProbeEditor(*this); }
		// Gesture state is deliberately not persisted, mirroring the concept's rule.
		void getStateInformation(juce::MemoryBlock&) override {}
		void setStateInformation(const void*, int) override {}

	private:
		void parameterValueChanged(const int _index, const float _value) override
		{
			const auto self = m_selfInitiatedParameter.load(std::memory_order_acquire) == _index;
			push({juce::Time::getMillisecondCounterHiRes(), _index, _value,
				juce::MessageManager::existsAndIsCurrentThread(), self});
			if(!self)
				m_lastExternalChange[static_cast<size_t>(_index)].store(
					juce::Time::getMillisecondCounterHiRes(), std::memory_order_release);
		}

		void parameterGestureChanged(const int _index, const bool _starting) override
		{
			m_pendingGestureLog[static_cast<size_t>(_index)].store(_starting ? 1 : 2,
				std::memory_order_release);
		}

		void push(const LogEntry& _entry)
		{
			int start1, size1, start2, size2;
			m_fifo.prepareToWrite(1, start1, size1, start2, size2);
			if(size1 > 0)
				m_entries[static_cast<size_t>(start1)] = _entry;
			else
				m_droppedEntries.fetch_add(1, std::memory_order_relaxed);
			m_fifo.finishedWrite(size1);
		}

		void timerCallback() override
		{
			recentreIdleEncoders();
			flushLog();
		}

		void recentreIdleEncoders()
		{
			const auto mode = static_cast<RecentreMode>(m_recentre->getIndex());
			if(mode == RecentreMode::Off)
				return;
			constexpr std::array<double, 5> idleMilliseconds{0.0, 50.0, 100.0, 300.0, 1000.0};
			const auto requiredIdle = idleMilliseconds[static_cast<size_t>(m_recentreIdle->getIndex())];
			const auto outsideBandOnly = m_recentreBand->getIndex() == 1;
			const auto now = juce::Time::getMillisecondCounterHiRes();
			for(auto* const parameter : {static_cast<juce::RangedAudioParameter*>(m_encoder128),
				static_cast<juce::RangedAudioParameter*>(m_encoder16k),
				static_cast<juce::RangedAudioParameter*>(m_encoderContinuous),
				static_cast<juce::RangedAudioParameter*>(m_encoderNoAutomation)})
			{
				const auto index = parameter->getParameterIndex();
				const auto idle = now - m_lastExternalChange[static_cast<size_t>(index)].load(std::memory_order_acquire);
				const auto centre = parameter->getDefaultValue();
				const auto value = parameter->getValue();
				if(idle < requiredIdle || value == centre)
					continue;
				if(outsideBandOnly && value > 1.0f / 3.0f && value < 2.0f / 3.0f)
					continue;
				m_selfInitiatedParameter.store(index, std::memory_order_release);
				if(mode == RecentreMode::NotifyWithGesture)
					parameter->beginChangeGesture();
				parameter->setValueNotifyingHost(centre);
				if(mode == RecentreMode::NotifyWithGesture)
					parameter->endChangeGesture();
				m_selfInitiatedParameter.store(-1, std::memory_order_release);
			}
		}

		void flushLog()
		{
			juce::String text;
			for(size_t index = 0; index < m_pendingGestureLog.size(); ++index)
			{
				const auto gesture = m_pendingGestureLog[index].exchange(0, std::memory_order_acq_rel);
				if(gesture != 0)
					text << "# gesture " << getParameters()[static_cast<int>(index)]->getName(64)
						<< (gesture == 1 ? " begin" : " end") << "\n";
			}
			int start1, size1, start2, size2;
			m_fifo.prepareToRead(m_fifo.getNumReady(), start1, size1, start2, size2);
			auto append = [&](const int _start, const int _size)
			{
				for(int i = 0; i < _size; ++i)
				{
					const auto& entry = m_entries[static_cast<size_t>(_start + i)];
					auto* const parameter = getParameters()[entry.parameter];
					text << juce::String(entry.milliseconds, 1) << ' ' << parameter->getName(64).quoted()
						<< ' ' << juce::String(entry.normalized, 6) << ' '
						<< parameter->getText(entry.normalized, 32).quoted() << ' '
						<< (entry.messageThread ? "message" : "other") << ' '
						<< (entry.selfInitiated ? "self" : "host") << "\n";
				}
			};
			append(start1, size1);
			append(start2, size2);
			m_fifo.finishedRead(size1 + size2);
			if(const auto dropped = m_droppedEntries.exchange(0, std::memory_order_relaxed))
				text << "# dropped " << static_cast<int>(dropped) << " entries\n";
			if(text.isNotEmpty())
				m_logFile.appendText(text);
		}

		static constexpr int g_capacity = 4096;
		juce::AbstractFifo m_fifo{g_capacity};
		std::array<LogEntry, g_capacity> m_entries{};
		std::atomic<uint32_t> m_droppedEntries{0};
		std::array<std::atomic<double>, 16> m_lastExternalChange{};
		std::array<std::atomic<int>, 16> m_pendingGestureLog{};
		std::atomic<int> m_selfInitiatedParameter{-1};
		juce::File m_logFile;

		juce::AudioParameterBool* m_button = nullptr;
		juce::AudioParameterInt* m_encoder128 = nullptr;
		juce::AudioParameterInt* m_encoder16k = nullptr;
		juce::AudioParameterFloat* m_encoderContinuous = nullptr;
		juce::AudioParameterChoice* m_recentre = nullptr;
		juce::AudioParameterInt* m_encoderNoAutomation = nullptr;
		juce::AudioParameterChoice* m_recentreIdle = nullptr;
		juce::AudioParameterChoice* m_recentreBand = nullptr;
	};
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
	return new ProbeProcessor();
}
