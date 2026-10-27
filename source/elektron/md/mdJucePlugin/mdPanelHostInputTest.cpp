#include "mdPanelHostInput.h"

#include "mdLib/mdpanelparameters.h"

#include <cstdlib>
#include <iostream>
#include <thread>
#include <vector>

namespace
{
	using md::MachineModel;
	using md::PanelControl;
	using md::PanelEncoder;
	using md::PanelEvent;
	using mdJucePlugin::PanelHostInput;
	namespace pp = md::panelParameters;

	void expect(const bool _condition, const char* const _message)
	{
		if(_condition)
			return;
		std::cerr << "mdPanelHostInputTest: " << _message << '\n';
		std::exit(1);
	}

	struct Machine
	{
		bool ready = true;
		uint64_t epoch = 1;
		double now = 0.0;
	};

	std::vector<PanelEvent> drain(PanelHostInput& _input, const Machine& _machine)
	{
		std::vector<PanelEvent> events;
		_input.drain({_machine.now, _machine.ready, _machine.epoch},
			[&events](const PanelEvent& _event) { events.push_back(_event); });
		return events;
	}

	uint8_t button(const PanelControl _control) { return static_cast<uint8_t>(_control); }
	uint8_t encoder(const PanelEncoder _encoder) { return static_cast<uint8_t>(_encoder); }

	PanelEvent pressOf(const MachineModel _model, const PanelControl _control)
	{
		const auto packet = *md::panelPacket(_model, _control);
		return {packet.row, packet.mask};
	}

	void checkButtonsReachTheMapper()
	{
		const auto model = MachineModel::Machinedrum;
		PanelHostInput input(model);
		Machine machine;
		expect(input.post(pp::ButtonPage, button(PanelControl::Play), 1), "button post rejected");
		expect(drain(input, machine) == std::vector<PanelEvent>{pressOf(model, PanelControl::Play)},
			"press did not reach the panel");
		expect(input.post(pp::ButtonPage, button(PanelControl::Play), 0), "release post rejected");
		const auto release = drain(input, machine);
		expect(release.size() == 1 && release[0].argument == 0, "release did not clear the row");
		expect(input.takeReturnToOffRequests() == 0, "momentary mode changed the host value");
	}

	void checkDuplicateDeliveryIsIgnored()
	{
		// VST3 delivers each host change on the message thread and again on the
		// audio thread with the same value.
		const auto model = MachineModel::Machinedrum;
		PanelHostInput input(model);
		Machine machine;
		input.post(pp::ButtonPage, button(PanelControl::Stop), 1);
		input.post(pp::ButtonPage, button(PanelControl::Stop), 1);
		expect(drain(input, machine).size() == 1, "duplicate press produced two events");
	}

	void checkRejectsForeignAddresses()
	{
		PanelHostInput input(MachineModel::Monomachine);
		expect(!input.post(0, 0, 1), "track parameter accepted as panel input");
		expect(!input.post(pp::EncoderPage, encoder(PanelEncoder::DataEntryA), 1),
			"the retired encoder page accepted input");
		expect(!input.turn(PanelEncoder::SoundSelection, 1), "unsupported MM encoder accepted");
		expect(!input.post(pp::ButtonPage, 200, 1), "out-of-range button accepted");
	}

	void checkHostInputBeforeFirmwareReadinessIsWithheld()
	{
		// Keys held while the machine boots open its startup menu (test mode,
		// resets, OS upgrade). A host - or a validator setting every parameter -
		// must never get there, so host input waits for the firmware.
		const auto model = MachineModel::Machinedrum;
		PanelHostInput input(model);
		Machine machine;
		machine.ready = false;
		input.post(pp::ButtonPage, button(PanelControl::Record), 1);
		expect(drain(input, machine).empty(), "host input reached a booting machine");

		machine.ready = true;
		input.post(pp::ButtonPage, button(PanelControl::Record), 0);
		expect(drain(input, machine).empty(), "release of a withheld press reached the panel");
	}

	void checkEditorInputBeforeFirmwareReadinessIsDelivered()
	{
		// A user holding a key in the editor during boot means it, as on hardware.
		const auto model = MachineModel::Machinedrum;
		PanelHostInput input(model);
		Machine machine;
		machine.ready = false;
		input.post(pp::ButtonPage, button(PanelControl::Function), 1, PanelHostInput::Source::Editor);
		expect(drain(input, machine) == std::vector<PanelEvent>{pressOf(model, PanelControl::Function)},
			"an editor press during boot did not reach the panel");
		// The parameter's own notification echoes the value as host input.
		input.post(pp::ButtonPage, button(PanelControl::Function), 1);
		expect(drain(input, machine).empty(), "host echo of an editor press produced events");
		input.post(pp::ButtonPage, button(PanelControl::Function), 0, PanelHostInput::Source::Editor);
		const auto release = drain(input, machine);
		expect(release.size() == 1 && release[0].argument == 0, "editor release during boot was lost");
	}

	void checkMissingDeviceDiscardsInput()
	{
		const auto model = MachineModel::Machinedrum;
		PanelHostInput input(model);
		Machine machine;
		machine.epoch = 0;	// no local device
		input.post(pp::ButtonPage, button(PanelControl::Play), 1);
		input.discard();
		machine.epoch = 1;
		input.post(pp::ButtonPage, button(PanelControl::Play), 0);
		expect(drain(input, machine).empty(), "input without a device reached a later device");
	}

	// Ableton Live toggles a note-mapped on/off parameter on every note-on and
	// ignores note-off. In tap mode every host value change is one short press.
	uint64_t slotBit(const PanelHostInput& _input, const uint8_t _page, const uint8_t _index)
	{
		const auto slot = _input.slotOf(_page, _index);
		expect(slot.has_value(), "no slot for a panel control");
		return uint64_t{1} << *slot;
	}

	// Ableton Live toggles a note-mapped on/off parameter on every note-on. In tap
	// mode a change to 1 is one short press, after which the plugin returns the
	// parameter to 0, so the next note toggles it to 1 again and the host shows
	// Off whenever the key is up.
	void checkTapModeTurnsToggledValuesIntoPresses()
	{
		const auto model = MachineModel::Machinedrum;
		PanelHostInput input(model);
		input.setHostButtonMode(PanelHostInput::ButtonMode::Tap);
		Machine machine;
		const auto press = pressOf(model, PanelControl::Play);
		const PanelEvent release{press.command, 0};
		constexpr double hold = PanelHostInput::g_tapMilliseconds;
		const auto play = button(PanelControl::Play);
		const auto bit = slotBit(input, pp::ButtonPage, play);

		input.post(pp::ButtonPage, play, 1);
		expect(drain(input, machine) == std::vector<PanelEvent>{press}, "a toggle to 1 did not press");
		expect(input.takeReturnToOffRequests() == 0, "returned to Off while the tap is still held");
		machine.now += hold - 1;
		expect(drain(input, machine).empty(), "tap released before the firmware could scan it");
		machine.now += 1;
		expect(drain(input, machine) == std::vector<PanelEvent>{release}, "tap was not released");
		expect(input.takeReturnToOffRequests() == bit, "a finished tap did not return to Off");
		expect(input.takeReturnToOffRequests() == 0, "the return request was not consumed");

		// The plugin's own return to 0 comes back as host input.
		input.post(pp::ButtonPage, play, 0);
		machine.now += 100;
		expect(drain(input, machine).empty(), "returning to Off pressed the key");

		input.post(pp::ButtonPage, play, 1);
		input.post(pp::ButtonPage, play, 1);
		expect(drain(input, machine) == std::vector<PanelEvent>{press}, "the next toggle did not press once");
		machine.now += hold;
		expect(drain(input, machine) == std::vector<PanelEvent>{release}, "second tap was not released");
	}

	void checkTapModeIgnoresTogglesToOff()
	{
		// If the host toggles to 0 before the plugin did, that is no press either.
		const auto model = MachineModel::Machinedrum;
		PanelHostInput input(model);
		input.setHostButtonMode(PanelHostInput::ButtonMode::Tap);
		Machine machine;
		input.post(pp::ButtonPage, button(PanelControl::Stop), 1);
		(void)drain(input, machine);
		machine.now += 10;
		input.post(pp::ButtonPage, button(PanelControl::Stop), 0);
		expect(drain(input, machine).empty(), "a toggle to 0 during a tap produced events");
		machine.now += PanelHostInput::g_tapMilliseconds;
		const auto released = drain(input, machine);
		expect(released.size() == 1 && released[0].argument == 0, "tap was not released");
		expect(input.takeReturnToOffRequests() == 0, "a parameter already at 0 was returned to Off");
	}

	void checkTapModeKeepsEditorMomentary()
	{
		const auto model = MachineModel::Machinedrum;
		PanelHostInput input(model);
		input.setHostButtonMode(PanelHostInput::ButtonMode::Tap);
		Machine machine;
		input.post(pp::ButtonPage, button(PanelControl::Function), 1, PanelHostInput::Source::Editor);
		expect(drain(input, machine) == std::vector<PanelEvent>{pressOf(model, PanelControl::Function)},
			"editor press in tap mode");
		// The host echoes the editor's own notification back as host input.
		input.post(pp::ButtonPage, button(PanelControl::Function), 1);
		machine.now += 10 * PanelHostInput::g_tapMilliseconds;
		expect(drain(input, machine).empty(), "an editor hold was released by the tap timer");
		input.post(pp::ButtonPage, button(PanelControl::Function), 0, PanelHostInput::Source::Editor);
		input.post(pp::ButtonPage, button(PanelControl::Function), 0);
		const auto released = drain(input, machine);
		expect(released.size() == 1 && released[0].argument == 0,
			"the host echo of an editor release was taken as a tap");
	}

	void checkTapModeAppliesToEncoderPushes()
	{
		const auto model = MachineModel::Monomachine;
		PanelHostInput input(model);
		input.setHostButtonMode(PanelHostInput::ButtonMode::Tap);
		Machine machine;
		const auto packet = *md::panelEncoderPressPacket(model, PanelEncoder::DataEntryC);
		input.post(pp::EncoderPushPage, encoder(PanelEncoder::DataEntryC), 1);
		expect(drain(input, machine) == std::vector<PanelEvent>{{packet.row, packet.mask}}, "push tap press");
		machine.now += PanelHostInput::g_tapMilliseconds;
		const auto released = drain(input, machine);
		expect(released.size() == 1 && released[0].argument == 0, "push tap was not released");
	}

	void checkTapModeIgnoresTogglesWhileBooting()
	{
		const auto model = MachineModel::Machinedrum;
		PanelHostInput input(model);
		input.setHostButtonMode(PanelHostInput::ButtonMode::Tap);
		Machine machine;
		machine.ready = false;
		input.post(pp::ButtonPage, button(PanelControl::Play), 1);
		expect(drain(input, machine).empty(), "a toggle during boot pressed a key");
		expect(input.takeReturnToOffRequests() == slotBit(input, pp::ButtonPage, button(PanelControl::Play)),
			"a withheld toggle was not returned to Off");
		input.post(pp::ButtonPage, button(PanelControl::Play), 0);
		machine.ready = true;
		machine.now += 1000;
		expect(drain(input, machine).empty(), "a withheld toggle was delivered later");
		input.post(pp::ButtonPage, button(PanelControl::Play), 1);
		expect(drain(input, machine) == std::vector<PanelEvent>{pressOf(model, PanelControl::Play)},
			"the first toggle after boot did not press");
	}

	void checkRebootForgetsHeldControls()
	{
		const auto model = MachineModel::Machinedrum;
		PanelHostInput input(model);
		Machine machine;
		input.post(pp::ButtonPage, button(PanelControl::Function), 1);
		(void)drain(input, machine);
		++machine.epoch;
		expect(drain(input, machine).empty(), "a reboot emitted events");
		input.post(pp::ButtonPage, button(PanelControl::Function), 0);
		expect(drain(input, machine).empty(), "release after reboot reached the fresh machine");
		input.post(pp::ButtonPage, button(PanelControl::Function), 1);
		expect(drain(input, machine) == std::vector<PanelEvent>{pressOf(model, PanelControl::Function)},
			"press after reboot was lost");
	}

	void checkResetReleasesHeldControls()
	{
		const auto model = MachineModel::Machinedrum;
		PanelHostInput input(model);
		Machine machine;
		input.post(pp::ButtonPage, button(PanelControl::Function), 1);
		(void)drain(input, machine);
		input.requestReset();
		const auto released = drain(input, machine);
		expect(released.size() == 1 && released[0].argument == 0, "reset did not release the held button");
	}

	PanelEvent tick(const MachineModel _model, const PanelEncoder _encoder, const bool _up)
	{
		return {*md::panelEncoderCommand(_model, _encoder), static_cast<uint8_t>(_up ? 0x01 : 0xff)};
	}

	std::vector<PanelEvent> ticks(const MachineModel _model, const PanelEncoder _encoder, const int _steps)
	{
		return std::vector<PanelEvent>(static_cast<size_t>(std::abs(_steps)), tick(_model, _encoder, _steps > 0));
	}

	void checkTurnsBecomeTicks()
	{
		for(const auto model : {MachineModel::Machinedrum, MachineModel::Monomachine})
		{
			PanelHostInput input(model);
			Machine machine;
			expect(input.turn(PanelEncoder::DataEntryA, 3), "turn rejected");
			expect(input.turn(PanelEncoder::DataEntryA, -2), "turn rejected");
			auto expected = ticks(model, PanelEncoder::DataEntryA, 3);
			const auto down = ticks(model, PanelEncoder::DataEntryA, -2);
			expected.insert(expected.end(), down.begin(), down.end());
			expect(drain(input, machine) == expected, "turns did not become ticks in order");
			expect(input.turn(PanelEncoder::Level, 0) && drain(input, machine).empty(), "zero steps emitted ticks");
		}
	}

	void checkLongTurnsAreSpreadOverDrains()
	{
		// The machine's panel FIFO drops ticks that do not fit, so a fast controller
		// burst is metered out instead of sent at once.
		const auto model = MachineModel::Machinedrum;
		PanelHostInput input(model);
		Machine machine;
		constexpr int steps = 2 * PanelHostInput::g_maxTicksPerDrain + 8;
		input.turn(PanelEncoder::DataEntryH, -steps);
		expect(drain(input, machine) == ticks(model, PanelEncoder::DataEntryH, -PanelHostInput::g_maxTicksPerDrain),
			"first drain exceeded the tick budget");
		expect(drain(input, machine).size() == PanelHostInput::g_maxTicksPerDrain, "second drain");
		expect(drain(input, machine) == ticks(model, PanelEncoder::DataEntryH, -8), "remainder was lost");
		expect(drain(input, machine).empty(), "steps were sent twice");
	}

	void checkGestureOrderIsKept()
	{
		// FUNCTION held while turning selects a different parameter on the machine.
		const auto model = MachineModel::Machinedrum;
		PanelHostInput input(model);
		Machine machine;
		using Source = PanelHostInput::Source;
		input.post(pp::ButtonPage, button(PanelControl::Function), 1, Source::PanelMidi);
		input.turn(PanelEncoder::DataEntryB, 2);
		input.post(pp::ButtonPage, button(PanelControl::Function), 0, Source::PanelMidi);
		const auto press = pressOf(model, PanelControl::Function);
		const auto up = tick(model, PanelEncoder::DataEntryB, true);
		expect(drain(input, machine) == std::vector<PanelEvent>{press, up, up, {press.command, 0}},
			"press, turn and release were reordered");
	}

	void checkPanelMidiIsWithheldWhileBooting()
	{
		const auto model = MachineModel::Machinedrum;
		PanelHostInput input(model);
		Machine machine;
		machine.ready = false;
		using Source = PanelHostInput::Source;
		input.post(pp::ButtonPage, button(PanelControl::Function), 1, Source::PanelMidi);
		input.turn(PanelEncoder::DataEntryA, 5);
		expect(drain(input, machine).empty(), "panel MIDI reached a booting machine");
		machine.ready = true;
		expect(drain(input, machine).empty(), "withheld panel MIDI was delivered later");
		input.post(pp::ButtonPage, button(PanelControl::Function), 0, Source::PanelMidi);
		expect(drain(input, machine).empty(), "release of a withheld press reached the panel");
	}

	void checkPanelMidiIsMomentaryInTapMode()
	{
		// A note gives a real release, so panel MIDI holds the key as long as the
		// note, and it never changes what counts as a host duplicate.
		const auto model = MachineModel::Machinedrum;
		PanelHostInput input(model);
		input.setHostButtonMode(PanelHostInput::ButtonMode::Tap);
		Machine machine;
		using Source = PanelHostInput::Source;
		input.post(pp::ButtonPage, button(PanelControl::Function), 1, Source::PanelMidi);
		expect(drain(input, machine) == std::vector<PanelEvent>{pressOf(model, PanelControl::Function)},
			"panel MIDI press");
		machine.now += 10 * PanelHostInput::g_tapMilliseconds;
		expect(drain(input, machine).empty(), "a held note was released by the tap timer");
		input.post(pp::ButtonPage, button(PanelControl::Function), 0, Source::PanelMidi);
		const auto released = drain(input, machine);
		expect(released.size() == 1 && released[0].argument == 0, "panel MIDI release");
		input.post(pp::ButtonPage, button(PanelControl::Function), 1);
		expect(drain(input, machine) == std::vector<PanelEvent>{pressOf(model, PanelControl::Function)},
			"a host toggle after panel MIDI input was taken as a duplicate");
	}

	void checkTurnOverflowKeepsSteps()
	{
		const auto model = MachineModel::Machinedrum;
		PanelHostInput input(model);
		Machine machine;
		constexpr int turns = static_cast<int>(PanelHostInput::g_queueCapacity) + 20;
		for(int i = 0; i < turns; ++i)
			input.turn(PanelEncoder::DataEntryC, 1);
		size_t total = 0;
		for(int i = 0; i < 16; ++i)
			total += drain(input, machine).size();
		expect(total == static_cast<size_t>(turns), "steps were lost on overflow");
	}

	void checkCarriedTicksWaitForReadiness()
	{
		// A project-state restore takes the firmware out of readiness without a
		// new machine; the rest of a long turn waits instead of reaching it.
		const auto model = MachineModel::Machinedrum;
		PanelHostInput input(model);
		Machine machine;
		constexpr int budget = PanelHostInput::g_maxTicksPerDrain;
		input.turn(PanelEncoder::DataEntryE, 3 * budget);
		expect(drain(input, machine).size() == budget, "first part of a long turn");
		machine.ready = false;
		expect(drain(input, machine).empty(), "carried ticks reached a machine that is not ready");
		machine.ready = true;
		expect(drain(input, machine).size() == budget, "carried ticks were lost while not ready");
		expect(drain(input, machine).size() == budget, "last part of a long turn");
		expect(drain(input, machine).empty(), "a long turn was sent twice");
	}

	void checkRebootDropsPendingTicks()
	{
		const auto model = MachineModel::Machinedrum;
		PanelHostInput input(model);
		Machine machine;
		input.turn(PanelEncoder::DataEntryD, 3 * PanelHostInput::g_maxTicksPerDrain);
		(void)drain(input, machine);
		++machine.epoch;
		expect(drain(input, machine).empty(), "ticks for the old machine reached the new one");
	}

	void checkQueueOverflowKeepsFinalState()
	{
		const auto model = MachineModel::Machinedrum;
		PanelHostInput input(model);
		Machine machine;
		// Far more changes than the queue holds, ending released.
		for(size_t i = 0; i < 4 * PanelHostInput::g_queueCapacity + 1; ++i)
			input.post(pp::ButtonPage, button(PanelControl::Play), static_cast<int>(i & 1));
		const auto events = drain(input, machine);
		expect(!events.empty(), "overflowing changes produced nothing");
		expect(events.back().argument == 0, "the final released state was lost on overflow");
		input.post(pp::ButtonPage, button(PanelControl::Play), 0);
		expect(drain(input, machine).empty(), "Play is still considered held after overflow");
	}

	void checkConcurrentProducers()
	{
		const auto model = MachineModel::Machinedrum;
		PanelHostInput input(model);
		Machine machine;
		std::thread first([&input] { for(int i = 0; i < 1000; ++i) input.post(pp::ButtonPage, button(PanelControl::Up), 1); });
		std::thread second([&input] { for(int i = 0; i < 1000; ++i) input.post(pp::ButtonPage, button(PanelControl::Up), 1); });
		first.join();
		second.join();
		expect(drain(input, machine) == std::vector<PanelEvent>{pressOf(model, PanelControl::Up)},
			"concurrent duplicate presses were not reduced to one press");
	}
}

int main()
{
	checkButtonsReachTheMapper();
	checkDuplicateDeliveryIsIgnored();
	checkRejectsForeignAddresses();
	checkHostInputBeforeFirmwareReadinessIsWithheld();
	checkEditorInputBeforeFirmwareReadinessIsDelivered();
	checkMissingDeviceDiscardsInput();
	checkTapModeTurnsToggledValuesIntoPresses();
	checkTapModeIgnoresTogglesToOff();
	checkTapModeKeepsEditorMomentary();
	checkTapModeAppliesToEncoderPushes();
	checkTapModeIgnoresTogglesWhileBooting();
	checkRebootForgetsHeldControls();
	checkResetReleasesHeldControls();
	checkTurnsBecomeTicks();
	checkLongTurnsAreSpreadOverDrains();
	checkGestureOrderIsKept();
	checkPanelMidiIsWithheldWhileBooting();
	checkPanelMidiIsMomentaryInTapMode();
	checkTurnOverflowKeepsSteps();
	checkCarriedTicksWaitForReadiness();
	checkRebootDropsPendingTicks();
	checkQueueOverflowKeepsFinalState();
	checkConcurrentProducers();
	std::cout << "mdPanelHostInputTest passed\n";
	return 0;
}
