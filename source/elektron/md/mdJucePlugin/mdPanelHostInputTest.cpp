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

	void checkInputBeforeFirmwareReadinessIsDelivered()
	{
		// The emulated panel UART accepts input during boot, as the hardware does
		// (e.g. keys held at power-on). Only recentring waits for readiness.
		const auto model = MachineModel::Machinedrum;
		PanelHostInput input(model);
		Machine machine;
		machine.ready = false;
		input.post(pp::ButtonPage, button(PanelControl::Function), 1);
		expect(drain(input, machine) == std::vector<PanelEvent>{pressOf(model, PanelControl::Function)},
			"a press during boot did not reach the panel");
		machine.ready = true;
		input.post(pp::ButtonPage, button(PanelControl::Function), 0);
		const auto release = drain(input, machine);
		expect(release.size() == 1 && release[0].argument == 0, "release after boot was lost");
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
	checkInputBeforeFirmwareReadinessIsDelivered();
	checkMissingDeviceDiscardsInput();
	checkEditorTurnsOnlyMoveTheBase();
	checkRebootForgetsHeldControls();
	checkResetReleasesHeldControls();
	checkRecentreRequests();
	checkNoRecentreBeforeReadiness();
	checkQueueOverflowKeepsFinalState();
	checkConcurrentProducers();
	std::cout << "mdPanelHostInputTest passed\n";
	return 0;
}
