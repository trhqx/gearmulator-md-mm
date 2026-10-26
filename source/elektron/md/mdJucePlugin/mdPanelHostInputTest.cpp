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

	constexpr double g_idle = md::PanelGestureMapper::g_recentreIdleMilliseconds;

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
		expect(!input.post(pp::EncoderPage, encoder(PanelEncoder::SoundSelection), 70),
			"unsupported MM encoder accepted");
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
		input.post(pp::EncoderPage, encoder(PanelEncoder::DataEntryA), 70);
		expect(drain(input, machine).empty(), "host input reached a booting machine");

		machine.ready = true;
		input.post(pp::ButtonPage, button(PanelControl::Record), 0);
		expect(drain(input, machine).empty(), "release of a withheld press reached the panel");
		input.post(pp::EncoderPage, encoder(PanelEncoder::DataEntryA), 71);
		const auto command = *md::panelEncoderCommand(model, PanelEncoder::DataEntryA);
		expect(drain(input, machine) == std::vector<PanelEvent>{{command, 0x01}},
			"encoder moved while booting was not adopted as the new base");
	}

	void checkEditorInputBeforeFirmwareReadinessIsDelivered()
	{
		// A user holding a key in the editor during boot means it, as on hardware.
		const auto model = MachineModel::Machinedrum;
		PanelHostInput input(model);
		Machine machine;
		machine.ready = false;
		input.post(pp::ButtonPage, button(PanelControl::Function), 1, PanelHostInput::Delivery::Mapped,
			PanelHostInput::Source::Editor);
		expect(drain(input, machine) == std::vector<PanelEvent>{pressOf(model, PanelControl::Function)},
			"an editor press during boot did not reach the panel");
		// The parameter's own notification echoes the value as host input.
		input.post(pp::ButtonPage, button(PanelControl::Function), 1);
		expect(drain(input, machine).empty(), "host echo of an editor press produced events");
		input.post(pp::ButtonPage, button(PanelControl::Function), 0, PanelHostInput::Delivery::Mapped,
			PanelHostInput::Source::Editor);
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

	void checkEditorTurnsOnlyMoveTheBase()
	{
		// The editor sends its encoder ticks itself and moves the host parameter
		// only so a host can learn it; the mapper must not send those ticks again.
		const auto model = MachineModel::Machinedrum;
		PanelHostInput input(model);
		Machine machine;
		const auto knob = encoder(PanelEncoder::DataEntryE);
		expect(input.post(pp::EncoderPage, knob, 70, PanelHostInput::Delivery::AlreadyDelivered),
			"editor encoder post rejected");
		expect(drain(input, machine).empty(), "editor-delivered ticks were sent again");
		input.post(pp::EncoderPage, knob, 70);
		expect(drain(input, machine).empty(), "host echo of the editor value produced ticks");
		input.post(pp::EncoderPage, knob, 71);
		const auto command = *md::panelEncoderCommand(model, PanelEncoder::DataEntryE);
		expect(drain(input, machine) == std::vector<PanelEvent>{{command, 0x01}},
			"a later host turn did not continue from the editor position");
	}

	// Ableton Live toggles a note-mapped on/off parameter on every note-on and
	// ignores note-off. In tap mode every host value change is one short press.
	void checkTapModeTurnsToggledValuesIntoPresses()
	{
		const auto model = MachineModel::Machinedrum;
		PanelHostInput input(model);
		input.setHostButtonMode(PanelHostInput::ButtonMode::Tap);
		Machine machine;
		const auto press = pressOf(model, PanelControl::Trigger1);
		const PanelEvent release{press.command, 0};
		constexpr double hold = PanelHostInput::g_tapMilliseconds;

		input.post(pp::ButtonPage, button(PanelControl::Trigger1), 1);
		expect(drain(input, machine) == std::vector<PanelEvent>{press}, "first toggle did not press");
		machine.now += hold - 1;
		expect(drain(input, machine).empty(), "tap released before the firmware could scan it");
		machine.now += 1;
		expect(drain(input, machine) == std::vector<PanelEvent>{release}, "tap was not released");

		// The next note-on toggles the value back to 0: that is another press.
		machine.now += 100;
		input.post(pp::ButtonPage, button(PanelControl::Trigger1), 0);
		expect(drain(input, machine) == std::vector<PanelEvent>{press}, "toggle back to 0 did not press");
		input.post(pp::ButtonPage, button(PanelControl::Trigger1), 0);
		machine.now += hold;
		expect(drain(input, machine) == std::vector<PanelEvent>{release},
			"duplicate delivery of a toggle produced a second press");
	}

	void checkTapModeKeepsEditorMomentary()
	{
		const auto model = MachineModel::Machinedrum;
		PanelHostInput input(model);
		input.setHostButtonMode(PanelHostInput::ButtonMode::Tap);
		Machine machine;
		input.post(pp::ButtonPage, button(PanelControl::Function), 1, PanelHostInput::Delivery::Mapped,
			PanelHostInput::Source::Editor);
		expect(drain(input, machine) == std::vector<PanelEvent>{pressOf(model, PanelControl::Function)},
			"editor press in tap mode");
		// The host echoes the editor's own notification back as host input.
		input.post(pp::ButtonPage, button(PanelControl::Function), 1);
		machine.now += 10 * PanelHostInput::g_tapMilliseconds;
		expect(drain(input, machine).empty(), "an editor hold was released by the tap timer");
		input.post(pp::ButtonPage, button(PanelControl::Function), 0, PanelHostInput::Delivery::Mapped,
			PanelHostInput::Source::Editor);
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
		machine.ready = true;
		machine.now += 1000;
		expect(drain(input, machine).empty(), "a withheld toggle was delivered later");
		input.post(pp::ButtonPage, button(PanelControl::Play), 0);
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
		input.post(pp::EncoderPage, encoder(PanelEncoder::DataEntryB), 90);
		(void)drain(input, machine);
		input.requestReset();
		const auto released = drain(input, machine);
		expect(released.size() == 1 && released[0].argument == 0, "reset did not release the held button");
		input.post(pp::EncoderPage, encoder(PanelEncoder::DataEntryB), 65);
		const auto command = *md::panelEncoderCommand(model, PanelEncoder::DataEntryB);
		expect(drain(input, machine) == std::vector<PanelEvent>{{command, 0x01}},
			"reset did not return the encoder base to the centre");
	}

	void checkRecentreRequests()
	{
		const auto model = MachineModel::Machinedrum;
		PanelHostInput input(model);
		Machine machine;
		constexpr auto knob = PanelEncoder::DataEntryC;
		const auto bit = 1u << encoder(knob);
		input.post(pp::EncoderPage, encoder(knob), 100);
		(void)drain(input, machine);
		expect(input.takeRecentreRequests() == 0, "recentre requested while still turning");

		machine.now += g_idle;
		(void)drain(input, machine);
		expect(input.takeRecentreRequests() == bit, "idle encoder outside the band was not recentred");
		expect(input.takeRecentreRequests() == 0, "recentre request was not consumed");
		machine.now += g_idle;
		(void)drain(input, machine);
		expect(input.takeRecentreRequests() == 0, "recentre requested twice before the echo arrived");

		// The plugin publishes the centre to the host, which echoes it back.
		input.post(pp::EncoderPage, encoder(knob), md::PanelGestureMapper::g_encoderCentre);
		machine.now += 10.0;
		expect(drain(input, machine).empty(), "recentre echo produced counter-ticks");
		input.post(pp::EncoderPage, encoder(knob), 20);
		machine.now += 10.0;
		(void)drain(input, machine);
		machine.now += g_idle;
		(void)drain(input, machine);
		expect(input.takeRecentreRequests() == bit, "a later turn could not be recentred again");
	}

	void checkNoRecentreBeforeReadiness()
	{
		PanelHostInput input(MachineModel::Machinedrum);
		Machine machine;
		machine.ready = false;
		input.post(pp::EncoderPage, encoder(PanelEncoder::DataEntryD), 120);
		(void)drain(input, machine);
		machine.now += 10 * g_idle;
		(void)drain(input, machine);
		expect(input.takeRecentreRequests() == 0, "recentre requested for a machine that is not ready");
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
	checkEditorTurnsOnlyMoveTheBase();
	checkTapModeTurnsToggledValuesIntoPresses();
	checkTapModeKeepsEditorMomentary();
	checkTapModeAppliesToEncoderPushes();
	checkTapModeIgnoresTogglesWhileBooting();
	checkRebootForgetsHeldControls();
	checkResetReleasesHeldControls();
	checkRecentreRequests();
	checkNoRecentreBeforeReadiness();
	checkQueueOverflowKeepsFinalState();
	checkConcurrentProducers();
	std::cout << "mdPanelHostInputTest passed\n";
	return 0;
}
