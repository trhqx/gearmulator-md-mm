#include "mdController.h"

#include "mdPluginProcessor.h"
#include "mdLib/mdautomation.h"
#include "mdLib/mdpanelparameters.h"
#include "mdLib/mddevice.h"
#include "mdLib/mdsysexautomation.h"

#include <algorithm>
#include <chrono>
#include <set>

namespace mdJucePlugin
{
	namespace
	{
		constexpr uint64_t g_dumpRequestRetryMs = 2000;
		constexpr uint8_t g_snapshotVersion = 2;
		constexpr uint8_t g_snapshotComplete = 1u << 0;
		constexpr uint8_t g_snapshotEntryPending = 1u << 0;

		class AutomationParameter final : public pluginLib::Parameter
		{
		public:
			using pluginLib::Parameter::Parameter;

		protected:
			bool shouldSendRepeatedHostValues() const override { return true; }
		};

		pluginLib::SysEx toPluginSysex(
			const md::automation::sysex::Message& _message)
		{
			return pluginLib::SysEx(_message.begin(), _message.end());
		}
	}

	Controller::Controller(AudioPluginAudioProcessor& _p)
		: pluginLib::Controller(_p, _p.getModel() == md::MachineModel::Monomachine
			? "parameterDescriptions_mm.json" : "parameterDescriptions_md.json")
		, m_model(_p.getModel())
		, m_panelInput(_p.getModel())
		, m_panelMidiTranslator(_p.getModel())
	{
		setHostButtonsTap(_p.getConfig().getBoolValue(g_hostButtonsTapConfigKey, g_hostButtonsTapDefault));
		m_panelMidiMap = md::PanelMidiMap::fromText(
			_p.getConfig().getValue(g_panelMidiMapConfigKey).toStdString(), m_model);
		m_panelMidiTranslator.setMap(m_panelMidiMap);
		m_panelMidiChannel.store(m_panelMidiMap.channel, std::memory_order_release);
		registerParams(_p, [](const uint8_t _part, const bool _nonPartSensitive)
		{
			// The only global parameters are the front-panel controls.
			return _nonPartSensitive ? juce::String("Panel")
				: juce::String("Track ") + juce::String(_part + 1);
		});
		for(const auto& [address, parameters] : getExposedParameters())
		{
			(void)parameters;
			// Panel controls are gestures for the front panel, not firmware CCs.
			if(md::panelParameters::isPanelPage(address.page))
				continue;
			const Address automationAddress{address.page, address.partNum,
				address.paramNum};
			const auto slotIndex = m_automationSlots.size();
			m_automationSlots.emplace_back();
			auto& slot = m_automationSlots.back();
			slot.address = automationAddress;
			m_automationSlotIndices.emplace(automationAddress, slotIndex);
		}

		// Give mute (which is not part of a Kit dump) a defined initial cache value.
		// The other values are replaced by the firmware snapshot below.
		for(const auto& [address, parameters] : getExposedParameters())
		{
			for(auto* const parameter : parameters)
				parameter->setValueFromSynth(parameter->getDefault(),
					pluginLib::Parameter::Origin::PresetChange);
			if(auto* const slot = findAutomationSlot(
				{address.page, address.partNum, address.paramNum}))
			{
				slot->publication.store(createPublication(static_cast<uint8_t>(
					std::clamp(parameters.front()->getDefault(), 0, 127)), false),
					std::memory_order_release);
			}
		}

		requestAutomationState();
	}

	Controller::~Controller()
	{
		// Stop the base Timer while all derived synchronization members still exist.
		// Waiting until pluginLib::Controller's destructor would leave a teardown
		// window in which its callback can dispatch into partially destroyed state.
		stopControllerTimer();
	}

	uint8_t Controller::getPartCount() const
	{
		return m_model == md::MachineModel::Monomachine
			? md::automation::monomachine::TrackCount
			: md::automation::machinedrum::TrackCount;
	}

	pluginLib::Parameter* Controller::createParameter(
		pluginLib::Controller& _controller,
		const pluginLib::Description& _description, const uint8_t _part,
		const int _uid, const pluginLib::Parameter::PartFormatter& _formatter)
	{
		return new AutomationParameter(_controller, _description, _part, _uid,
			_formatter);
	}

	void Controller::onStateLoaded()
	{
		// Loading/replacing the emulated device establishes a new authoritative
		// baseline even when it selects the same numbered Kit as the old device.
		// Restored AUTO publications remain dirty and therefore still win when the
		// correlated Kit dump is applied.
		requestAutomationState(true);
		// Panel gestures are never part of a project: a load releases everything.
		resetPanelParameters();
	}

	void Controller::resetPanelParameters()
	{
		for(const auto& [address, parameters] : getExposedParameters())
		{
			if(!md::panelParameters::isPanelPage(address.page))
				continue;
			for(auto* const parameter : parameters)
				parameter->setValueFromSynth(parameter->getDefault(),
					pluginLib::Parameter::Origin::PresetChange);
		}
		m_panelInput.requestReset();
	}

	void Controller::setHostButtonsTap(const bool _tap)
	{
		m_panelInput.setHostButtonMode(_tap ? PanelHostInput::ButtonMode::Tap
			: PanelHostInput::ButtonMode::Momentary);
	}

	void Controller::returnTappedPanelButtonsToOff()
	{
		auto bits = m_panelInput.takeReturnToOffRequests();
		for(uint8_t slot = 0; bits != 0; ++slot, bits >>= 1)
		{
			if((bits & 1) == 0)
				continue;
			const auto address = PanelHostInput::addressOf(slot);
			for(auto* const parameter : findSynthParam(0, address.page, address.index))
				if(parameter->getUnnormalizedValue() != 0)
					parameter->setUnnormalizedValueNotifyingHost(0, pluginLib::Parameter::Origin::Ui);
		}
	}

	void Controller::setPanelControlFromUi(const md::panelParameters::Address _address, const bool _pressed)
	{
		const auto value = _pressed ? 1 : 0;
		// Post first so the edge is ordered before any later change, even if the
		// parameter's own (asynchronous) value notification coalesces press and release.
		m_panelInput.post(_address.page, _address.index, value,
			PanelHostInput::Source::Editor);
		for(auto* const parameter : findSynthParam(0, _address.page, _address.index))
			parameter->setUnnormalizedValueNotifyingHost(value, pluginLib::Parameter::Origin::Ui);
	}

	bool Controller::consumePanelMidi(const synthLib::SMidiEvent& _event)
	{
		if(!md::isPanelMidiMessage(m_panelMidiChannel.load(std::memory_order_acquire), _event.a))
			return false;
		// The panel channel belongs to the panel even when its queue is full.
		(void)m_panelMidiIngress.tryPush({_event.a, _event.b, _event.c});
		m_consumedPanelMidi.fetch_add(1, std::memory_order_acq_rel);
		return true;
	}

	void Controller::setPanelMidiMap(const md::PanelMidiMap& _map)
	{
		m_panelMidiMap = _map;
		m_panelMidiChannel.store(_map.channel, std::memory_order_release);
		auto& config = static_cast<AudioPluginAudioProcessor&>(getProcessor()).getConfig();
		config.setValue(g_panelMidiMapConfigKey, juce::String(_map.toText()));
		config.saveIfNeeded();
		publishPanelMidiMap();
		onPanelMidiMapChanged();
	}

	void Controller::learnPanelMidi(const md::PanelMidiTarget _target)
	{
		if(!_target.isSupported(m_model))
			return;
		m_panelMidiLearning = true;
		m_panelMidiLearnRequest.store(static_cast<uint16_t>(static_cast<uint16_t>(_target.type) << 8 | _target.index),
			std::memory_order_release);
	}

	void Controller::cancelPanelMidiLearn()
	{
		m_panelMidiLearnRequest.store(g_cancelPanelMidiLearn, std::memory_order_release);
		if(!m_panelMidiLearning)
			return;
		m_panelMidiLearning = false;
		onPanelMidiMapChanged();
	}

	void Controller::publishPanelMidiMap()
	{
		m_panelMidiMapPending = !m_panelMidiMapUpdates.tryPush(m_panelMidiMap);
	}

	void Controller::servicePanelMidi()
	{
		md::PanelMidiTranslator::Binding learned;
		bool changed = false;
		while(m_panelMidiLearned.tryPop(learned))
		{
			changed |= m_panelMidiMap.bind(learned.trigger, learned.target, m_model);
			m_panelMidiLearning = false;
		}
		if(changed)
			setPanelMidiMap(m_panelMidiMap);
		else if(m_panelMidiMapPending)
			publishPanelMidiMap();
	}

	void Controller::translatePanelMidi()
	{
		md::PanelMidiMap map;
		bool haveMap = false;
		while(m_panelMidiMapUpdates.tryPop(map))
			haveMap = true;
		if(haveMap)
			m_panelMidiTranslator.setMap(map);
		const auto learn = m_panelMidiLearnRequest.exchange(g_noPanelMidiLearn, std::memory_order_acq_rel);
		if(learn == g_cancelPanelMidiLearn)
			m_panelMidiTranslator.cancelLearn();
		else if(learn != g_noPanelMidiLearn)
			m_panelMidiTranslator.learn({static_cast<md::PanelMidiTarget::Type>(learn >> 8),
				static_cast<uint8_t>(learn & 0xff)});

		namespace pp = md::panelParameters;
		using Type = md::PanelMidiTarget::Type;
		PanelMidiMessage message;
		for(size_t i = 0; i < 512 && m_panelMidiIngress.tryPop(message); ++i)
		{
			const auto result = m_panelMidiTranslator.translate(message.status, message.data1, message.data2);
			if(result.learned && !m_panelMidiLearned.tryPush(*result.learned))
				m_panelMidiTranslator.learn(result.learned->target);	// retry with the next message
			if(!result.action)
				continue;
			const auto& [target, value] = *result.action;
			if(target.type == Type::Encoder)
				m_panelInput.turn(static_cast<md::PanelEncoder>(target.index), value, PanelHostInput::Source::PanelMidi);
			else
				m_panelInput.post(target.type == Type::Button ? pp::ButtonPage : pp::EncoderPushPage, target.index,
					value, PanelHostInput::Source::PanelMidi);
		}
	}

	void Controller::drainPanelInput()
	{
		auto& plugin = getProcessor().getPlugin();
		plugin.withDeviceLocked([this, &plugin](synthLib::Device* const _device)
		{
			auto* const device = dynamic_cast<md::Device*>(_device);
			translatePanelMidi();
			if(device == nullptr)
			{
				m_panelInput.discard();
				return;
			}
			// Gesture timing uses emulated machine time: it advances with processed
			// audio, so an offline render replays recorded recentres like realtime.
			const auto& hardware = device->getHardware();
			const PanelHostInput::DrainContext context{
				static_cast<double>(hardware.hostCurrentCycle()) * 1000.0 / static_cast<double>(md::g_ucClockHz),
				hardware.isFirmwareMidiReady(), device->hardwareEpoch()};
			auto& diagnostics = plugin.getRealtimeInstrumentation();
			const auto model = static_cast<uint32_t>(m_model);
			m_panelInput.drain(context, [&](const md::PanelEvent& _event)
			{
				const auto token = diagnostics.beginPanelInput(model, _event.command, _event.argument);
				const auto accepted = device->sendPanelEvent(_event.command, _event.argument);
				diagnostics.endPanelInput(token, model, _event.command, _event.argument, accepted);
				if(accepted)
					m_deliveredPanelEvents.fetch_add(1, std::memory_order_acq_rel);
			});
		});
	}

	std::vector<uint8_t> Controller::createAutomationSnapshot() const
	{
		const auto epoch = m_synchronizationEpoch.load(std::memory_order_acquire);
		const auto complete = m_automationReady.load(std::memory_order_acquire)
			&& m_haveGlobal.load(std::memory_order_acquire)
			&& m_haveKit.load(std::memory_order_acquire)
			&& m_currentKit.load(std::memory_order_acquire) != 0xff;
		std::vector<uint64_t> publications;
		publications.reserve(m_automationSlots.size());
		bool hasPendingIntent = false;
		for(const auto& [address, parameters] : getExposedParameters())
		{
			if(md::panelParameters::isPanelPage(address.page))
				continue;
			if(parameters.empty())
				return {};
			const auto* const slot = findAutomationSlot(
				{address.page, address.partNum, address.paramNum});
			if(slot == nullptr)
				return {};
			const auto publication = slot->publication.load(std::memory_order_acquire);
			publications.push_back(publication);
			hasPendingIntent |= publicationIsDirty(publication);
		}
		// Before the first coherent firmware snapshot, persist only meaningful host/UI
		// intent. This avoids turning constructor defaults into writes merely because a
		// host saved while the machine was still booting.
		if(!complete && !hasPendingIntent)
			return {};
		std::vector<uint8_t> result;
		result.reserve(7 + m_automationSlots.size() * 5);
		result.push_back(g_snapshotVersion);
		result.push_back(static_cast<uint8_t>(m_model));
		result.push_back(getAutomationBaseChannel());
		result.push_back(m_currentKit.load(std::memory_order_acquire));
		const auto count = static_cast<uint16_t>(m_automationSlots.size());
		result.push_back(static_cast<uint8_t>(count >> 8));
		result.push_back(static_cast<uint8_t>(count & 0xff));
		result.push_back(complete ? g_snapshotComplete : 0);
		size_t publicationIndex = 0;
		for(const auto& [address, parameters] : getExposedParameters())
		{
			(void)parameters;
			if(md::panelParameters::isPanelPage(address.page))
				continue;
			const auto publication = publications[publicationIndex++];
			result.push_back(address.page);
			result.push_back(address.partNum);
			result.push_back(address.paramNum);
			result.push_back(publicationValue(publication));
			result.push_back(publicationIsDirty(publication)
				? g_snapshotEntryPending : 0);
		}
		if(complete && (!m_automationReady.load(std::memory_order_acquire)
			|| epoch != m_synchronizationEpoch.load(std::memory_order_acquire)))
			return {};
		return result;
	}

	bool Controller::restoreAutomationSnapshot(
		const std::vector<uint8_t>& _snapshot)
	{
		const std::lock_guard synchronizationLock(m_synchronizationLock);
		if(_snapshot.size() < 6 || (_snapshot[0] != 1
			&& _snapshot[0] != g_snapshotVersion)
			|| _snapshot[1] != static_cast<uint8_t>(m_model))
			return false;
		const auto version = _snapshot[0];
		const auto headerSize = version == 1 ? size_t{6} : size_t{7};
		const auto entrySize = version == 1 ? size_t{4} : size_t{5};
		if(_snapshot.size() < headerSize)
			return false;
		const auto count = static_cast<size_t>((_snapshot[4] << 8) | _snapshot[5]);
		const auto kitCount = m_model == md::MachineModel::Monomachine ? 128 : 64;
		const auto complete = version == 1
			|| (_snapshot[6] & g_snapshotComplete) != 0;
		if(_snapshot.size() != headerSize + count * entrySize
			|| count != m_automationSlots.size()
			|| (version == g_snapshotVersion && (_snapshot[6] & ~g_snapshotComplete))
			|| !(_snapshot[2] < 16 || _snapshot[2] == 0x7f)
			|| (complete && _snapshot[3] >= kitCount)
			|| (!complete && _snapshot[3] != 0xff && _snapshot[3] >= kitCount))
			return false;
		std::set<Address> addresses;
		bool hasPendingIntent = false;
		for(size_t position = headerSize; position < _snapshot.size();
			position += entrySize)
		{
			const Address address{_snapshot[position], _snapshot[position + 1],
				_snapshot[position + 2]};
			if(_snapshot[position + 3] > 127 || !addresses.insert(address).second
				|| (version == g_snapshotVersion
					&& (_snapshot[position + 4] & ~g_snapshotEntryPending))
				|| findAutomationSlot(address) == nullptr)
				return false;
			hasPendingIntent |= version == 1
				|| (_snapshot[position + 4] & g_snapshotEntryPending) != 0;
		}
		if(!complete && !hasPendingIntent)
			return false;

		m_automationReady.store(false, std::memory_order_release);
		m_synchronizationEpoch.fetch_add(1, std::memory_order_acq_rel);
		if(complete)
		{
			m_baseChannel.store(_snapshot[2], std::memory_order_release);
			m_currentKit.store(_snapshot[3], std::memory_order_release);
		}
		else
		{
			// A partial snapshot deliberately contains no baseline for clean entries.
			// Forget a prior session's Kit identity so the next correlated dump rebuilds
			// that baseline; dirty entries below still survive publishFirmwareValue().
			m_currentKit.store(0xff, std::memory_order_release);
		}
		for(size_t position = headerSize; position < _snapshot.size();
			position += entrySize)
		{
			const auto pending = complete || version == 1
				|| (_snapshot[position + 4] & g_snapshotEntryPending) != 0;
			if(!pending)
				continue;
			const Address address{_snapshot[position], _snapshot[position + 1],
				_snapshot[position + 2]};
			const auto& parameters = findSynthParam(_snapshot[position + 1],
				_snapshot[position], _snapshot[position + 2]);
			if(parameters.empty())
				return false;
			for(auto* const parameter : parameters)
				parameter->setValueFromSynth(_snapshot[position + 3],
					pluginLib::Parameter::Origin::PresetChange);
			publishAutomationIntent({address.page, address.track, address.index,
				_snapshot[position + 3]}, true);
		}
		// A direct restore is a complete state transition, not merely a parser
		// helper. Reset/request here so standalone and wrapper callers cannot leave
		// the controller with Ready trackers but invalidated snapshot flags.
		requestAutomationState();
		getProcessor().updateHostDisplay(
			juce::AudioProcessorListener::ChangeDetails().withProgramChanged(true));
		return true;
	}

	void Controller::requestAutomationState()
	{
		requestAutomationState(false);
	}

	void Controller::requestAutomationState(const bool _forceApplyKitDump)
	{
		const std::lock_guard synchronizationLock(m_synchronizationLock);
		m_automationReady.store(false, std::memory_order_release);
		m_synchronizationEpoch.fetch_add(1, std::memory_order_acq_rel);
		m_haveGlobal.store(false, std::memory_order_release);
		m_haveKit.store(false, std::memory_order_release);
		m_currentGlobal.store(0xff, std::memory_order_release);
		// Retain the selected Kit identity. A request for the same slot returns the
		// stored Kit, not its unsaved live edit buffer, so applying it would roll
		// back host/front-panel changes already observed in this session.
		m_forceApplyRequestedKitDump.store(_forceApplyKitDump,
			std::memory_order_release);
		m_globalSynchronization.reset();
		m_kitSynchronization.reset();
		m_kitDumpRequestRevision.store(0, std::memory_order_release);
		sendMissingSynchronizationRequests();
	}

	void Controller::requestKitState()
	{
		const std::lock_guard synchronizationLock(m_synchronizationLock);
		m_automationReady.store(false, std::memory_order_release);
		m_synchronizationEpoch.fetch_add(1, std::memory_order_acq_rel);
		m_haveKit.store(false, std::memory_order_release);
		// Program/status changes explicitly reload the Kit and therefore make the
		// stored slot authoritative even when its number happens to be unchanged.
		m_forceApplyRequestedKitDump.store(true, std::memory_order_release);
		m_kitSynchronization.reset();
		m_kitDumpRequestRevision.store(0, std::memory_order_release);
		sendMissingSynchronizationRequests();
	}

	uint64_t Controller::milliseconds()
	{
		return std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count();
	}

	void Controller::sendMissingSynchronizationRequests()
	{
		const std::lock_guard synchronizationLock(m_synchronizationLock);
		// Do not accumulate status requests in the plug-in MIDI queue while the
		// firmware DSPs are still booting. Once consumed, those duplicates can yield
		// late Kit dumps that overwrite host writes after synchronization completed.
		if(!firmwareReadyForAutomation())
			return;
		const auto now = milliseconds();
		const auto recoverTimedOut = [this, now](
			md::automation::DumpRequestTracker& _tracker,
			std::atomic<bool>& _haveSnapshot)
		{
			if(!_tracker.recoverTimedOutDump(now, g_dumpRequestRetryMs))
				return;
			_haveSnapshot.store(false, std::memory_order_release);
			m_automationReady.store(false, std::memory_order_release);
			m_synchronizationEpoch.fetch_add(1, std::memory_order_acq_rel);
		};
		recoverTimedOut(m_globalSynchronization, m_haveGlobal);
		recoverTimedOut(m_kitSynchronization, m_haveKit);
		if(m_globalSynchronization.statusRequestDue(now, 500))
		{
			m_globalSynchronization.statusRequestSent(now);
			sendSynchronizationRequest(toPluginSysex(md::automation::sysex::statusRequest(m_model,
				md::automation::sysex::StatusParameter::Global)));
		}
		if(m_kitSynchronization.statusRequestDue(now, 500))
		{
			m_kitSynchronization.statusRequestSent(now);
			sendSynchronizationRequest(toPluginSysex(md::automation::sysex::statusRequest(m_model,
				md::automation::sysex::StatusParameter::Kit)));
		}
	}

	void Controller::sendSynchronizationRequest(const pluginLib::SysEx& _message) const
	{
		// Keep controller-generated state queries observable in firmware-test
		// diagnostics without changing their normal editor-to-device routing.
		synthLib::SMidiEvent event(synthLib::MidiEventSource::Editor);
		event.sysex = _message;
		m_synchronizationRequests.fetch_add(1, std::memory_order_relaxed);
		sendMidiEvent(event);
	}

	void Controller::onControllerTimer()
	{
		const std::lock_guard synchronizationLock(m_synchronizationLock);
		// The same protocol service also runs from an offline render callback, which
		// is non-realtime but is not necessarily JUCE's message thread. Keep editor
		// Value/listener work on the message thread while still allowing headless
		// renders to drain MIDI and advance synchronization below.
		const auto* const messageManager =
			juce::MessageManager::getInstanceWithoutCreating();
		if(messageManager != nullptr && messageManager->isThisTheMessageThread())
		{
			servicePanelMidi();
			returnTappedPanelButtonsToOff();
			for(const auto& [address, parameters] : getExposedParameters())
			{
				(void)address;
				for(auto* const parameter : parameters)
					parameter->flushRealtimeValueToUi();
			}
		}
		drainRealtimeParameterChanges(RealtimeAutomationCapacity, false);
		completeSynchronizationIfReady();
		const auto now = milliseconds();
		if(m_automationReady.load(std::memory_order_acquire))
		{
			// Firmware Global is authoritative for the MIDI channel, and a front-panel
			// kit selection bypasses the controller. Poll their tiny status messages;
			// only a changed kit number causes the much larger Kit dump.
			if(now - m_lastStatePollMs.load(std::memory_order_acquire) < 5000)
				return;
			m_lastStatePollMs.store(now, std::memory_order_release);
			// A controller-facing copy of firmware MIDI is deliberately allowed to
			// drop rather than block the audio thread. Retire a lost periodic status
			// response so one contention event cannot disable polling forever.
			m_globalSynchronization.recoverTimedOutStatus(
				now, g_dumpRequestRetryMs);
			m_kitSynchronization.recoverTimedOutStatus(
				now, g_dumpRequestRetryMs);
			// A status response identifies the active Global slot, but does not expose
			// edits to that Global's MIDI base channel. Refresh the selected Global too
			// so queued writes survive MIDI NONE and resume when a channel is enabled.
			if(m_globalSynchronization.canPollStatus())
			{
				m_globalSynchronization.statusRequestSent(now);
				sendSynchronizationRequest(toPluginSysex(
					md::automation::sysex::statusRequest(m_model,
						md::automation::sysex::StatusParameter::Global)));
			}
			if(m_kitSynchronization.canPollStatus())
			{
				m_kitSynchronization.statusRequestSent(now);
				sendSynchronizationRequest(toPluginSysex(
					md::automation::sysex::statusRequest(m_model,
						md::automation::sysex::StatusParameter::Kit)));
			}
			return;
		}
		sendMissingSynchronizationRequests();
	}

	void Controller::sendParameterChange(const pluginLib::Parameter& _parameter,
		const pluginLib::ParamValue _value, const pluginLib::Parameter::Origin _origin)
	{
		const auto& description = _parameter.getDescription();
		// Panel gestures are delivered to the emulated front panel, never as CCs.
		if(md::panelParameters::isPanelPage(description.page))
		{
			m_panelInput.post(description.page, description.index, static_cast<int>(_value));
			return;
		}
		const md::automation::ParameterChange change{
			description.page,
			_parameter.getPart(),
			description.index,
			static_cast<uint8_t>(std::clamp<pluginLib::ParamValue>(_value, 0, 127))
		};
		publishAutomationIntent(change,
			_origin != pluginLib::Parameter::Origin::HostAutomation
				|| !m_automationReady.load(std::memory_order_acquire));
		// UI changes use exactly the same ordered publication path as host
		// automation. A non-realtime caller may drain immediately, while a host
		// callback only performs the bounded publication and returns.
		if(_origin != pluginLib::Parameter::Origin::HostAutomation)
			drainRealtimeParameterChanges(RealtimeAutomationCapacity, false);
	}

	uint8_t Controller::publicationValue(const uint64_t _publication)
	{
		return static_cast<uint8_t>(_publication & PublicationValueMask);
	}

	uint64_t Controller::publicationRevision(const uint64_t _publication)
	{
		return (_publication & PublicationRevisionMask) >> 8;
	}

	bool Controller::publicationIsDirty(const uint64_t _publication)
	{
		return (_publication & PublicationDirty) != 0;
	}

	uint64_t Controller::createPublication(const uint8_t _value, const bool _dirty)
	{
		const auto revision = m_nextAutomationRevision.fetch_add(
			1, std::memory_order_relaxed);
		return (_dirty ? PublicationDirty : 0)
			| ((revision << 8) & PublicationRevisionMask)
			| (_value & PublicationValueMask);
	}

	void Controller::publishAutomationIntent(
		const md::automation::ParameterChange& _change,
		const bool _supersedeEarlier)
	{
		const auto found = m_automationSlotIndices.find(
			{_change.page, _change.track, _change.index});
		if(found == m_automationSlotIndices.end())
			return;

		const auto publication = createPublication(_change.value, true);
		auto& slot = m_automationSlots[found->second];
		slot.publication.exchange(publication, std::memory_order_acq_rel);
		const auto advanceDeliveryFloor = [&slot](const uint64_t _revision)
		{
			// Keep the realtime producer strictly bounded: a fixed number of strong
			// attempts can only all fail under sustained same-address contention.
			// Failure to advance merely permits an extra stale value before the latest;
			// it cannot lose or overwrite the authoritative publication.
			auto floor = slot.deliveryFloorRevision.load(std::memory_order_acquire);
			for(size_t attempt = 0; attempt < 8 && floor < _revision; ++attempt)
			{
				if(slot.deliveryFloorRevision.compare_exchange_strong(floor, _revision,
					std::memory_order_release, std::memory_order_acquire))
					return;
			}
		};
		if(_supersedeEarlier)
			advanceDeliveryFloor(publicationRevision(publication));
		if(!m_realtimeAutomationChanges.tryPush(
			{_change, found->second, publication}))
		{
			// The atomic slot remains authoritative. A bounded slot scan will deliver
			// it even when queue capacity or producer contention drops this hint. Once
			// a hint is missing, older queued values can no longer form a complete FIFO
			// stream, so explicitly coalesce them behind the recovered latest value.
			advanceDeliveryFloor(publicationRevision(publication));
			slot.scanPublication.store(publication, std::memory_order_release);
			m_realtimeAutomationOverflows.fetch_add(1, std::memory_order_relaxed);
		}
	}

	Controller::AutomationSlot* Controller::findAutomationSlot(
		const Address& _address)
	{
		const auto found = m_automationSlotIndices.find(_address);
		return found == m_automationSlotIndices.end()
			? nullptr : &m_automationSlots[found->second];
	}

	const Controller::AutomationSlot* Controller::findAutomationSlot(
		const Address& _address) const
	{
		const auto found = m_automationSlotIndices.find(_address);
		return found == m_automationSlotIndices.end()
			? nullptr : &m_automationSlots[found->second];
	}

	int Controller::getLastFirmwareKitValue(
		const pluginLib::Parameter& _parameter) const
	{
		const auto& description = _parameter.getDescription();
		const auto* const slot = findAutomationSlot({description.page,
			_parameter.getPart(), description.index});
		if(slot == nullptr)
			return -1;
		const auto value = slot->lastFirmwareKitValue.load(std::memory_order_acquire);
		return value <= 127 ? static_cast<int>(value) : -1;
	}

	uint8_t Controller::publishFirmwareValue(const Address& _address,
		const uint8_t _value, const uint64_t _kitRequestRevision)
	{
		auto* const slot = findAutomationSlot(_address);
		if(slot == nullptr)
			return _value;

		const auto desired = createPublication(_value, false);
		auto observed = slot->publication.load(std::memory_order_acquire);
		for(;;)
		{
			// An undelivered DAW/UI intent always survives a dump. For Kit dumps,
			// direct changes observed after the request watermark survive as well.
			// The watermark is valid because both the read request and later editor/host
			// MIDI enter synthLib::Plugin's FIFO in publication order; processBlock
			// appends its bounded realtime insertions after general ingress already
			// queued for that block. Firmware therefore cannot observe the later change
			// before the earlier request.
			if(publicationIsDirty(observed)
				|| (_kitRequestRevision != 0
					&& publicationRevision(observed) > _kitRequestRevision))
				return publicationValue(observed);
			if(slot->publication.compare_exchange_weak(observed, desired,
				std::memory_order_release, std::memory_order_acquire))
				return _value;
		}
	}

	void Controller::transmitParameterChange(
		const md::automation::ParameterChange& _change)
	{
		if(const auto message = md::automation::encodeParameterChange(
			m_model, _change, getAutomationBaseChannel()))
		{
			auto digest = m_transmittedAutomationDigest.load(std::memory_order_relaxed);
			for(const auto byte : *message)
			{
				digest ^= byte;
				digest *= 1099511628211ull;
			}
			m_transmittedAutomationDigest.store(digest, std::memory_order_release);
			m_transmittedAutomationChanges.fetch_add(1, std::memory_order_release);
			sendMidiEvent((*message)[0], (*message)[1], (*message)[2]);
		}
	}

	bool Controller::transmitRealtimeParameterChange(
		const md::automation::ParameterChange& _change)
	{
		const auto message = md::automation::encodeParameterChange(
			m_model, _change, getAutomationBaseChannel());
		if(!message)
			return true;
		const synthLib::SMidiEvent event(synthLib::MidiEventSource::Editor,
			(*message)[0], (*message)[1], (*message)[2]);
		if(!getProcessor().tryAddRealtimeMidiEvent(event))
			return false;

		auto digest = m_transmittedAutomationDigest.load(std::memory_order_relaxed);
		for(const auto byte : *message)
		{
			digest ^= byte;
			digest *= 1099511628211ull;
		}
		m_transmittedAutomationDigest.store(digest, std::memory_order_release);
		m_transmittedAutomationChanges.fetch_add(1, std::memory_order_release);
		return true;
	}

	bool Controller::deliverAutomationPublication(
		const QueuedAutomationChange& _queued, const bool _realtime)
	{
		if(_queued.slotIndex >= m_automationSlots.size())
			return true;
		auto& slot = m_automationSlots[_queued.slotIndex];
		auto observed = slot.publication.load(std::memory_order_acquire);
		const auto queuedRevision = publicationRevision(_queued.publication);
		if(queuedRevision < slot.deliveryFloorRevision.load(std::memory_order_acquire))
			return true;
		if(observed != _queued.publication)
		{
			// A later ordinary DAW publication does not invalidate this FIFO entry.
			// Transmit the older value now and leave the latest dirty publication for
			// its own hint. If the latest was already delivered, its clean state proves
			// this reordered/stale hint must instead be ignored.
			if(!publicationIsDirty(observed)
				|| publicationRevision(observed) <= queuedRevision)
				return true;
		}
		else if(!publicationIsDirty(observed))
			return true;
		if(!m_automationReady.load(std::memory_order_acquire)
			|| getAutomationBaseChannel() == 0x7f)
			return true;

		if(_realtime)
		{
			if(!transmitRealtimeParameterChange(_queued.change))
				return false;
		}
		else
		{
			transmitParameterChange(_queued.change);
		}

		if(observed == _queued.publication)
		{
			// Clear the delivery bit only if no newer publication replaced this one
			// while MIDI was being queued. A failed CAS leaves that newer value dirty.
			const auto delivered = observed & ~PublicationDirty;
			slot.publication.compare_exchange_strong(observed, delivered,
				std::memory_order_release, std::memory_order_acquire);
		}
		return true;
	}

	void Controller::drainRealtimeParameterChanges(const size_t _maximumChanges,
		const bool _realtime)
	{
		if(_maximumChanges == 0
			|| m_realtimeAutomationDrain.test_and_set(std::memory_order_acquire))
			return;
		struct ClearFlag
		{
			std::atomic_flag& flag;
			~ClearFlag() { flag.clear(std::memory_order_release); }
		} clear{m_realtimeAutomationDrain};

		const auto routable = m_automationReady.load(std::memory_order_acquire)
			&& getAutomationBaseChannel() != 0x7f && !m_automationSlots.empty();
		// A successful hint is the only way to preserve an exact DAW sequence. Do not
		// consume it while firmware routing is unavailable; synchronization completion
		// (or a later MIDI-channel enable) will drain the intact FIFO. Failed hints have
		// their separate recovery marker and remain safe if this queue fills meanwhile.
		if(!routable)
			return;
		// Always reserve part of a routable callback for recovery-marked slots.
		// A full queue can contain thousands of stale hints for one address; without
		// this reservation, an unhinted latest value could wait for all of them.
		size_t reservedScan = routable ? std::min(m_automationSlots.size(),
			std::max<size_t>(1, _maximumChanges / 4)) : size_t{0};
		if(routable && _maximumChanges == 1)
		{
			// A one-event caller cannot serve the FIFO and recovery scan in one pass.
			// Alternate them so neither source can starve; larger budgets serve both.
			reservedScan = m_minimumBudgetRecoveryTurn ? 1 : 0;
			m_minimumBudgetRecoveryTurn = !m_minimumBudgetRecoveryTurn;
		}
		const auto queueLimit = _maximumChanges - reservedScan;
		size_t processed = 0;
		bool queueEmpty = false;
		while(processed < queueLimit)
		{
			QueuedAutomationChange queued;
			if(!m_realtimeAutomationChanges.tryPop(queued))
			{
				queueEmpty = true;
				break;
			}
			if(!deliverAutomationPublication(queued, _realtime))
				return;
			++processed;
		}

		// Queue overflow and producer contention only drop hints. Scan a bounded
		// rotating slice for explicit recovery markers, even while hints remain. Do
		// not deliver ordinary dirty slots from the scan: jumping their healthy FIFO
		// hints would incorrectly coalesce explicit host writes. If the queue
		// empties early, spend the unused budget on the scan. Thus a dirty slot is
		// reconsidered within ceil(slot-count / reserved-scan) callbacks regardless
		// of stale queue depth, while total callback work never exceeds the caller's
		// explicit maximum.
		const auto scanLimit = std::min(m_automationSlots.size(),
			queueEmpty ? _maximumChanges - processed : reservedScan);
		size_t inspected = 0;
		while(inspected < scanLimit)
		{
			if(m_dirtyScanPosition >= m_automationSlots.size())
				m_dirtyScanPosition = 0;
			const auto slotIndex = m_dirtyScanPosition++;
			auto& slot = m_automationSlots[slotIndex];
			auto recovery = slot.scanPublication.load(std::memory_order_acquire);
			if(recovery != 0)
			{
				const auto publication = slot.publication.load(std::memory_order_acquire);
				if(publicationIsDirty(publication))
				{
					const auto& address = slot.address;
					if(!deliverAutomationPublication({
						{address.page, address.track, address.index,
							publicationValue(publication)}, slotIndex, publication}, _realtime))
						return;
				}
				// Retain the marker until the FIFO has actually been observed empty, so a
				// later same-slot host write cannot disappear behind the known stale backlog.
				// CAS still prevents an older scan from clearing a replacement marker.
				if(queueEmpty && !publicationIsDirty(
					slot.publication.load(std::memory_order_acquire)))
					slot.scanPublication.compare_exchange_strong(recovery, 0,
						std::memory_order_release, std::memory_order_acquire);
			}
			++inspected;
		}
	}

	void Controller::processRealtimeParameterChanges(const size_t _maximumChanges)
	{
		drainRealtimeParameterChanges(_maximumChanges, true);
		drainPanelInput();
	}

	void Controller::applyKitParameters(
		const std::vector<md::automation::ParameterChange>& _changes)
	{
		const auto requestRevision = m_kitDumpRequestRevision.load(
			std::memory_order_acquire);
		for(const auto& change : _changes)
		{
			if(auto* const slot = findAutomationSlot(
				{change.page, change.track, change.index}))
				slot->lastFirmwareKitValue.store(change.value,
					std::memory_order_release);
			const auto value = publishFirmwareValue(
				{change.page, change.track, change.index}, change.value,
				requestRevision);
			const auto& parameters = findSynthParam(change.track, change.page,
				change.index);
			for(auto* const parameter : parameters)
				parameter->setValueFromSynth(value,
					pluginLib::Parameter::Origin::PresetChange);
		}
		getProcessor().updateHostDisplay(
			juce::AudioProcessorListener::ChangeDetails().withProgramChanged(true));
	}

	void Controller::completeSynchronizationIfReady()
	{
		// Requests are withheld while the DSPs boot or project restore is pending.
		// Once both strictly correlated replies arrive, those replies themselves are
		// the readiness proof; consulting asynchronous hardware state again here can
		// only delay publication of an otherwise coherent snapshot.
		if(!m_haveGlobal.load(std::memory_order_acquire)
			|| !m_haveKit.load(std::memory_order_acquire))
			return;
		if(!m_automationReady.load(std::memory_order_acquire))
		{
			// This function also services every ready timer tick. Only a transition
			// starts the poll interval; moving it on every tick starves polling.
			m_lastStatePollMs.store(milliseconds(), std::memory_order_release);
			m_automationReady.store(true, std::memory_order_release);
		}
		if(getAutomationBaseChannel() == 0x7f)
		{
			// MIDI NONE is a valid firmware setting. The cache may become ready for
			// reads, but pending DAW intent must remain intact until a routable Global
			// dump is observed.
			return;
		}

		// Once ready is visible, one serialized non-realtime drain delivers both
		// queued hints and every dirty slot missed because the queue was full.
		drainRealtimeParameterChanges(
			RealtimeAutomationCapacity + m_automationSlots.size(), false);
	}

	bool Controller::firmwareReadyForAutomation() const
	{
		if(m_syntheticFirmwareReadyForTests)
			return true;
		bool ready = true;
		getProcessor().getPlugin().withDeviceLocked(
			[&ready](synthLib::Device* const _device)
			{
				if(const auto* const device = dynamic_cast<md::Device*>(_device))
					ready = !device->isProjectStateRestorePending()
						&& device->getHardware().isFirmwareMidiReady();
			});
		return ready;
	}

	bool Controller::parseSysexMessage(const pluginLib::SysEx& _message,
		synthLib::MidiEventSource)
	{
		const std::lock_guard synchronizationLock(m_synchronizationLock);
		if(const auto status = md::automation::sysex::parseStatusResponse(
			m_model, _message))
		{
			switch(status->parameter)
			{
			case md::automation::sysex::StatusParameter::Global:
			{
				const auto now = milliseconds();
				const auto observation =
					m_globalSynchronization.observeStatus(status->value);
				if(!observation.accepted)
					return true;
				m_currentGlobal.store(status->value, std::memory_order_release);
				if(observation.requestDump)
				{
					m_automationReady.store(false, std::memory_order_release);
					m_synchronizationEpoch.fetch_add(1, std::memory_order_acq_rel);
					m_haveGlobal.store(false, std::memory_order_release);
					m_globalSynchronization.dumpRequestSent(now);
					sendSynchronizationRequest(toPluginSysex(
						md::automation::sysex::globalRequest(m_model, status->value)));
				}
				return true;
			}
			case md::automation::sysex::StatusParameter::Kit:
			{
				const auto now = milliseconds();
				const auto observation =
					m_kitSynchronization.observeStatus(status->value);
				if(!observation.accepted)
					return true;
				const auto previousKit = m_currentKit.exchange(status->value,
					std::memory_order_acq_rel);
				if(observation.requestDump)
				{
					const auto forceApply = m_forceApplyRequestedKitDump.exchange(
						false, std::memory_order_acq_rel);
					// A status barrier after a timeout must not turn an unapplied
					// baseline/reload into a same-slot inspection. Retain its purpose
					// until an accepted dump consumes it, not merely a status reply.
					m_applyRequestedKitDump.store(
						m_applyRequestedKitDump.load(std::memory_order_acquire)
						|| previousKit == 0xff
						|| previousKit != status->value
						|| forceApply, std::memory_order_release);
					m_automationReady.store(false, std::memory_order_release);
					m_synchronizationEpoch.fetch_add(1, std::memory_order_acq_rel);
					m_haveKit.store(false, std::memory_order_release);
					// Keep the original watermark on same-slot retries as well: a
					// direct edit observed since that request must still beat the stored
					// dump. A new selection or explicit resync starts a new watermark.
					if(previousKit != status->value
						|| m_kitDumpRequestRevision.load(std::memory_order_acquire) == 0)
					{
						const auto next = m_nextAutomationRevision.load(
							std::memory_order_acquire);
						m_kitDumpRequestRevision.store(next > 0 ? next - 1 : 0,
							std::memory_order_release);
					}
					m_kitSynchronization.dumpRequestSent(now);
					sendSynchronizationRequest(toPluginSysex(
						md::automation::sysex::kitRequest(m_model, status->value)));
				}
				return true;
			}
			case md::automation::sysex::StatusParameter::Pattern:
				return true;
			}
		}

		if(const auto global = md::automation::sysex::parseGlobalDump(
			m_model, _message))
		{
			if(!m_globalSynchronization.acceptDump(global->slot))
				return true;
			// A periodic refresh may update the base channel while an otherwise-ready
			// snapshot is being serialized. Invalidate first so the snapshot either
			// observes the old generation in full or is rejected and retried.
			m_automationReady.store(false, std::memory_order_release);
			m_synchronizationEpoch.fetch_add(1, std::memory_order_acq_rel);
			m_baseChannel.store(global->baseChannel, std::memory_order_release);
			m_haveGlobal.store(true, std::memory_order_release);
			completeSynchronizationIfReady();
			return true;
		}

		if(const auto kit = md::automation::sysex::parseKitDump(
			m_model, _message))
		{
			if(!m_kitSynchronization.acceptDump(kit->slot))
				return true;
			if(m_applyRequestedKitDump.exchange(false, std::memory_order_acq_rel))
				applyKitParameters(kit->parameters);
			else
			{
				// Even when the stored dump must not replace the live cache, retain its
				// raw values for firmware-backed diagnostics.
				for(const auto& change : kit->parameters)
				{
					if(auto* const slot = findAutomationSlot(
						{change.page, change.track, change.index}))
						slot->lastFirmwareKitValue.store(change.value,
							std::memory_order_release);
				}
			}
			m_haveKit.store(true, std::memory_order_release);
			m_kitDumpRequestRevision.store(0, std::memory_order_release);
			completeSynchronizationIfReady();
			return true;
		}

		// External SET STATUS messages can change the active Global, selected Kit,
		// or Pattern without going through the controller. Refresh after the firmware
		// consumes the same queued MIDI event.
		if(const auto status = md::automation::sysex::parseSetStatus(m_model, _message))
		{
			if(status->parameter == md::automation::sysex::StatusParameter::Global)
			{
				m_automationReady.store(false, std::memory_order_release);
				m_synchronizationEpoch.fetch_add(1, std::memory_order_acq_rel);
				m_haveGlobal.store(false, std::memory_order_release);
				m_globalSynchronization.reset();
				sendMissingSynchronizationRequests();
			}
			else if(status->parameter == md::automation::sysex::StatusParameter::Kit
				|| status->parameter == md::automation::sysex::StatusParameter::Pattern)
			{
				requestKitState();
			}
		}
		return false;
	}

	bool Controller::parseControllerMessage(const synthLib::SMidiEvent& _event)
	{
		const std::lock_guard synchronizationLock(m_synchronizationLock);
		const auto change = md::automation::decodeParameterChange(m_model,
			{_event.a, _event.b, _event.c}, getAutomationBaseChannel());
		if(!change)
			return false;

		const auto& parameters = findSynthParam(change->track, change->page,
			change->index);
		if(parameters.empty())
			return false;
		const auto value = publishFirmwareValue(
			{change->page, change->track, change->index}, change->value);
		const auto origin = midiEventSourceToParameterOrigin(_event.source);
		for(auto* const parameter : parameters)
			parameter->setValueFromSynth(value, origin);
		return true;
	}

	bool Controller::parseMidiMessage(const synthLib::SMidiEvent& _event)
	{
		const std::lock_guard synchronizationLock(m_synchronizationLock);
		const auto handled = pluginLib::Controller::parseMidiMessage(_event);
		// Device-origin Program Change is outgoing firmware MIDI (for example from
		// an MM MIDI machine), not an instruction selecting the plug-in's Kit.
		if(_event.source != synthLib::MidiEventSource::Device
			&& _event.sysex.empty() && (_event.a & 0xf0) == 0xc0)
			requestKitState();
		return handled;
	}
}
