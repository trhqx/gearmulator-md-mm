// Firmware-backed check that front-panel host parameters drive the emulated panel
// without an editor (doc/mdmm_panel_host_parameters.md).

#include "mdAutomationTestSupport.h"

#include "mdLib/mdfrontpanel.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace
{
	using namespace mdAutomationTest;

	pluginLib::Parameter& panelParameter(mdJucePlugin::Controller& _controller, const std::string& _name)
	{
		auto* const result = _controller.getParameter(_name, 0);
		require(result != nullptr, _name + " was not registered");
		return *result;
	}

	md::FrontPanel frontPanel(Harness& _harness)
	{
		md::FrontPanel panel;
		_harness.processor.getPlugin().withDeviceLocked([&panel](synthLib::Device* const _device)
		{
			if(auto* const device = dynamic_cast<md::Device*>(_device))
				panel = device->getFrontPanelSnapshot();
		});
		return panel;
	}

	uint64_t lcdFingerprint(const md::FrontPanel& _panel)
	{
		uint64_t hash = 14695981039346656037ull;
		for(uint32_t half = 0; half < 2; ++half)
			for(uint32_t page = 0; page < 8; ++page)
				for(uint32_t column = 0; column < 64; ++column)
				{
					hash ^= _panel.getLcdVram(half, page, column);
					hash *= 1099511628211ull;
				}
		return hash;
	}

	void tap(Harness& _harness, pluginLib::Parameter& _button)
	{
		hostWrite(_button, 1);
		_harness.process(16);
		hostWrite(_button, 0);
		_harness.process(64);
	}

	void verifyRecordButton(Harness& _harness)
	{
		auto& controller = _harness.controller;
		auto& record = panelParameter(controller, "PanelRecord");
		const auto initial = frontPanel(_harness).getModeLed(md::FrontPanel::ModeLed::Record);

		const auto beforeTap = controller.getDeliveredPanelEventCount();
		tap(_harness, record);
		require(controller.getDeliveredPanelEventCount() == beforeTap + 2,
			"a host press and release did not deliver exactly two panel events");
		require(frontPanel(_harness).getModeLed(md::FrontPanel::ModeLed::Record) != initial,
			"host-parameter RECORD press did not toggle the Record LED");

		const auto beforeDuplicate = controller.getDeliveredPanelEventCount();
		hostWrite(record, 0);
		_harness.process(16);
		require(controller.getDeliveredPanelEventCount() == beforeDuplicate,
			"a repeated release reached the panel");

		tap(_harness, record);
		require(frontPanel(_harness).getModeLed(md::FrontPanel::ModeLed::Record) == initial,
			"second host-parameter RECORD press did not toggle the LED back");
	}

	// Ableton Live toggles a note-mapped on/off parameter on each note-on, so in
	// tap mode every value change must be exactly one press of the button.
	void verifyTapMode(Harness& _harness)
	{
		auto& controller = _harness.controller;
		auto& record = panelParameter(controller, "PanelRecord");
		controller.setHostButtonsTap(true);
		const auto initial = frontPanel(_harness).getModeLed(md::FrontPanel::ModeLed::Record);

		hostWrite(record, 1);
		_harness.process(64);
		require(frontPanel(_harness).getModeLed(md::FrontPanel::ModeLed::Record) != initial,
			"a toggled-on parameter did not press RECORD in tap mode");
		hostWrite(record, 0);
		_harness.process(64);
		require(frontPanel(_harness).getModeLed(md::FrontPanel::ModeLed::Record) == initial,
			"a toggled-off parameter did not press RECORD again in tap mode");
		controller.setHostButtonsTap(false);
	}

	// Turning a panel encoder must reach the host as the real parameter under it
	// (here machine/synthesis parameter 1 of track 1 on the default page): the
	// firmware reports the change and the controller publishes it, so a host
	// records absolute values on that parameter instead of encoder movements.
	void verifyEncoderTurnRecordsTrackParameter(Harness& _harness, const md::MachineModel _model)
	{
		auto& controller = _harness.controller;
		auto* const target = controller.getParameter(
			_model == md::MachineModel::Monomachine ? "SynthesisA" : "MachineParameter1", 0);
		require(target != nullptr, "track 1 parameter 1 is missing");
		require(controller.getParameter("PanelEncoderDataEntryA", 0) == nullptr,
			"encoders must not be host parameters");
		const auto command = *md::panelEncoderCommand(_model, md::PanelEncoder::DataEntryA);
		const auto before = target->getUnnormalizedValue();
		const auto turn = [&](const uint8_t _direction)
		{
			_harness.processor.getPlugin().withDeviceLocked([&](synthLib::Device* const _device)
			{
				if(auto* const device = dynamic_cast<md::Device*>(_device))
					for(int i = 0; i < 6; ++i)
						device->sendPanelEvent(command, _direction);
			});
			_harness.process(200);
		};
		turn(before < 100 ? 0x01 : 0xff);
		const auto after = target->getUnnormalizedValue();
		require(after != before, "a panel encoder turn did not reach the track parameter ("
			+ std::to_string(before) + " -> " + std::to_string(after) + ")");
		require(std::abs(after - before) == 6, "the track parameter did not follow the encoder exactly");
	}

	void verifyStateLoadReleasesHeldControls(Harness& _harness)
	{
		auto& controller = _harness.controller;
		auto& function = panelParameter(controller, "PanelFunction");
		hostWrite(function, 1);
		_harness.process(16);

		// Gestures are never part of a project; loading one releases them.
		juce::MemoryBlock state;
		_harness.audioProcessor.getStateInformation(state);
		_harness.audioProcessor.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
		require(_harness.synchronize(), "state restore did not resynchronize");
		_harness.process(64);
		require(function.getUnnormalizedValue() == 0, "state load left a panel button held");

		const auto before = controller.getDeliveredPanelEventCount();
		hostWrite(function, 0);
		_harness.process(16);
		require(controller.getDeliveredPanelEventCount() == before,
			"release after a state load reached the panel");
	}

	uint64_t midiRxConsumed(Harness& _harness)
	{
		uint64_t count = 0;
		_harness.processor.getPlugin().withDeviceLocked([&count](synthLib::Device* const _device)
		{
			if(auto* const device = dynamic_cast<md::Device*>(_device))
				count = device->getHardware().midiRxConsumedCount();
		});
		return count;
	}

	// A MIDI controller on the panel channel plays the front panel directly: it
	// never reaches the machine's MIDI input and never shows up as a host parameter.
	void verifyPanelMidi(Harness& _harness, const md::MachineModel _model)
	{
		constexpr int channel = 16;	// one-based, as in juce::MidiMessage
		auto& controller = _harness.controller;
		require(controller.getAutomationBaseChannel() + 6 < channel - 1,
			"test premise: the panel channel is outside the machine's channels");
		md::PanelMidiMap map;
		map.channel = channel - 1;
		map.encoding = md::PanelMidiEncoding::TwosComplement;
		require(map.bind(md::PanelMidiTrigger::controlChange(16),
			md::PanelMidiTarget::encoder(md::PanelEncoder::DataEntryA), _model), "bind encoder");
		require(map.bind(md::PanelMidiTrigger::note(60),
			md::PanelMidiTarget::button(md::PanelControl::Record), _model), "bind Record");
		controller.setPanelMidiMap(map);
		_harness.process(4);

		auto* const target = controller.getParameter(
			_model == md::MachineModel::Monomachine ? "SynthesisA" : "MachineParameter1", 0);
		require(target != nullptr, "track 1 parameter 1 is missing");
		const auto before = target->getUnnormalizedValue();
		const auto rxBefore = midiRxConsumed(_harness);
		const auto consumedBefore = controller.getConsumedPanelMidiCount();
		const uint8_t step = before < 100 ? 3 : 125;	// +3 or -3
		_harness.processMidi({juce::MidiMessage::controllerEvent(channel, 16, step),
			juce::MidiMessage::controllerEvent(channel, 16, step)});
		_harness.process(200);
		const auto after = target->getUnnormalizedValue();
		require(std::abs(after - before) == 6, "panel MIDI did not turn the encoder by six steps ("
			+ std::to_string(before) + " -> " + std::to_string(after) + ")");

		if(_model == md::MachineModel::Machinedrum)
		{
			const auto initial = frontPanel(_harness).getModeLed(md::FrontPanel::ModeLed::Record);
			_harness.processMidi({juce::MidiMessage::noteOn(channel, 60, static_cast<uint8_t>(100))});
			_harness.process(16);
			_harness.processMidi({juce::MidiMessage::noteOff(channel, 60)});
			_harness.process(64);
			require(frontPanel(_harness).getModeLed(md::FrontPanel::ModeLed::Record) != initial,
				"a panel MIDI note did not press RECORD");
			_harness.processMidi({juce::MidiMessage::noteOn(channel, 60, static_cast<uint8_t>(100)),
				juce::MidiMessage::noteOff(channel, 60)});
			_harness.process(64);
			require(frontPanel(_harness).getModeLed(md::FrontPanel::ModeLed::Record) == initial,
				"a second panel MIDI note did not press RECORD again");
		}

		require(midiRxConsumed(_harness) == rxBefore, "panel MIDI reached the machine's MIDI input");
		require(controller.getConsumedPanelMidiCount() > consumedBefore, "panel MIDI was not consumed");
		require(controller.getParameter("PanelEncoderDataEntryA", 0) == nullptr, "encoders became host parameters");

		// Other channels still reach the machine.
		_harness.processMidi({juce::MidiMessage::controllerEvent(controller.getAutomationBaseChannel() + 1, 120, 0)});
		_harness.process(32);
		require(midiRxConsumed(_harness) > rxBefore, "MIDI on the machine's channel no longer reached it");

		map.channel = md::PanelMidiMap::g_off;
		controller.setPanelMidiMap(map);
		_harness.process(4);
	}

	std::vector<pluginLib::Parameter*> panelParameters(Harness& _harness)
	{
		std::vector<pluginLib::Parameter*> result;
		for(auto* const audioParameter : _harness.audioProcessor.getParameters())
			if(auto* const parameter = dynamic_cast<pluginLib::Parameter*>(audioParameter))
				if(md::panelParameters::isPanelPage(parameter->getDescription().page))
					result.push_back(parameter);
		return result;
	}

	bool verifyModel(const md::MachineModel _model)
	{
		Harness harness(_model);
		if(!harness.hasLocalFirmware())
			return allowMissingFirmware("mdPanelHostParameterFirmwareTest", _model);
		harness.prepare();
		harness.controller.setHostButtonsTap(false);
		// auval and other validators set every parameter right after loading.
		// Held from power-on, panel keys would open the startup menu (test mode,
		// resets, OS upgrade) and the machine would never boot normally.
		for(auto* const parameter : panelParameters(harness))
			hostWrite(*parameter, parameter->getDescription().range.getEnd());
		require(harness.synchronize(), "the machine did not boot with every panel parameter held ("
			+ harness.firmwareReadiness() + ")");
		for(auto* const parameter : panelParameters(harness))
			hostWrite(*parameter, parameter->getDescription().range.getStart());
		harness.process(16);
		// MIDI synchronization completes while the firmware still shows its startup
		// screens, which ignore panel input. Give the UI the same boot time as the
		// mdLib panel firmware tests before pressing anything.
		harness.process(25 * 48000 / BlockSize);

		if(_model == md::MachineModel::Machinedrum)
		{
			verifyRecordButton(harness);
			verifyTapMode(harness);
		}
		verifyEncoderTurnRecordsTrackParameter(harness, _model);
		verifyPanelMidi(harness, _model);
		verifyStateLoadReleasesHeldControls(harness);

		std::cout << "mdPanelHostParameterFirmwareTest: " << modelName(_model) << " PASS\n";
		return true;
	}
}

int main()
{
	juce::ScopedJuceInitialiser_GUI juce;
	try
	{
		const auto ranMd = verifyModel(md::MachineModel::Machinedrum);
		const auto ranMm = verifyModel(md::MachineModel::Monomachine);
		return ranMd || ranMm ? 0 : SkipReturnCode;
	}
	catch(const std::exception& _error)
	{
		std::cerr << "mdPanelHostParameterFirmwareTest: " << _error.what() << '\n';
		return 1;
	}
}
