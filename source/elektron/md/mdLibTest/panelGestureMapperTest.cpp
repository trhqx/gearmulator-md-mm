#include "mdLib/mdpanelgesturemapper.h"
#include "mdLib/mdpanelparameters.h"

#include <cstdlib>
#include <iostream>
#include <optional>
#include <utility>
#include <vector>

namespace
{
	using md::MachineModel;
	using md::PanelControl;
	using md::PanelEncoder;
	using md::PanelEvent;
	using md::PanelGestureMapper;

	void expect(const bool _condition, const char* const _message)
	{
		if(_condition)
			return;
		std::cerr << "mdPanelGestureMapperTest: " << _message << '\n';
		std::exit(1);
	}

	std::vector<PanelEvent> events(const PanelGestureMapper::Output& _output)
	{
		return {_output.events.begin(), _output.events.begin() + static_cast<std::ptrdiff_t>(_output.count)};
	}

	std::vector<PanelEvent> repeat(const PanelEvent _event, const size_t _count)
	{
		return std::vector<PanelEvent>(_count, _event);
	}

	std::vector<PanelEvent> button(PanelGestureMapper& _mapper, const PanelControl _control, const bool _pressed)
	{
		PanelGestureMapper::Output output;
		_mapper.setButton(_control, _pressed, output);
		return events(output);
	}

	std::vector<PanelEvent> push(PanelGestureMapper& _mapper, const PanelEncoder _encoder, const bool _pressed)
	{
		PanelGestureMapper::Output output;
		_mapper.setEncoderPush(_encoder, _pressed, output);
		return events(output);
	}

	std::vector<PanelEvent> turn(const PanelGestureMapper& _mapper, const PanelEncoder _encoder, const int _steps)
	{
		PanelGestureMapper::Output output;
		_mapper.turnEncoder(_encoder, _steps, output);
		return events(output);
	}

	// Two distinct verified controls sharing a scan row, discovered from the
	// production table so the test follows any future packet verification.
	std::optional<std::pair<PanelControl, PanelControl>> findRowPair(const MachineModel _model)
	{
		for(uint8_t a = 0; a <= static_cast<uint8_t>(PanelControl::ClassicExtended); ++a)
		{
			const auto packetA = md::panelPacket(_model, static_cast<PanelControl>(a));
			if(!packetA)
				continue;
			for(uint8_t b = a + 1; b <= static_cast<uint8_t>(PanelControl::ClassicExtended); ++b)
			{
				const auto packetB = md::panelPacket(_model, static_cast<PanelControl>(b));
				if(packetB && packetB->row == packetA->row && (packetB->mask & packetA->mask) == 0)
					return std::make_pair(static_cast<PanelControl>(a), static_cast<PanelControl>(b));
			}
		}
		return std::nullopt;
	}

	void checkButtonPressRelease()
	{
		for(const auto model : {MachineModel::Machinedrum, MachineModel::Monomachine})
		{
			PanelGestureMapper mapper(model);
			const auto packet = md::panelPacket(model, PanelControl::Play);
			expect(packet.has_value(), "Play has no verified packet");
			expect(button(mapper, PanelControl::Play, true) == std::vector<PanelEvent>{{packet->row, packet->mask}},
				"press did not emit the control's row mask");
			expect(button(mapper, PanelControl::Play, false) == std::vector<PanelEvent>{{packet->row, 0}},
				"release did not clear the row");
		}
	}

	void checkButtonIdempotence()
	{
		PanelGestureMapper mapper(MachineModel::Machinedrum);
		expect(button(mapper, PanelControl::Play, false).empty(), "release without press emitted events");
		expect(!button(mapper, PanelControl::Play, true).empty(), "first press emitted nothing");
		expect(button(mapper, PanelControl::Play, true).empty(), "duplicate press emitted events");
		expect(!button(mapper, PanelControl::Play, false).empty(), "release emitted nothing");
		expect(button(mapper, PanelControl::Play, false).empty(), "duplicate release emitted events");
	}

	void checkRowMerging()
	{
		for(const auto model : {MachineModel::Machinedrum, MachineModel::Monomachine})
		{
			const auto pair = findRowPair(model);
			expect(pair.has_value(), "no two verified controls share a row");
			const auto a = *md::panelPacket(model, pair->first);
			const auto b = *md::panelPacket(model, pair->second);
			PanelGestureMapper mapper(model);
			expect(button(mapper, pair->first, true) == std::vector<PanelEvent>{{a.row, a.mask}}, "first press");
			expect(button(mapper, pair->second, true)
				== std::vector<PanelEvent>{{a.row, static_cast<uint8_t>(a.mask | b.mask)}},
				"second press in the same row did not merge masks");
			expect(button(mapper, pair->first, false) == std::vector<PanelEvent>{{a.row, b.mask}},
				"releasing one control cleared the other");
			expect(button(mapper, pair->second, false) == std::vector<PanelEvent>{{a.row, 0}},
				"releasing the last control left bits set");
		}
	}

	void checkEncoderPushSharesRowState()
	{
		for(const auto model : {MachineModel::Machinedrum, MachineModel::Monomachine})
		{
			const auto pushA = *md::panelEncoderPressPacket(model, PanelEncoder::DataEntryA);
			const auto pushB = *md::panelEncoderPressPacket(model, PanelEncoder::DataEntryB);
			PanelGestureMapper mapper(model);
			expect(push(mapper, PanelEncoder::DataEntryA, true) == std::vector<PanelEvent>{{pushA.row, pushA.mask}},
				"encoder push did not emit its packet");
			expect(push(mapper, PanelEncoder::DataEntryB, true)
				== std::vector<PanelEvent>{{pushA.row, static_cast<uint8_t>(pushA.mask | pushB.mask)}},
				"two encoder pushes did not merge");
			expect(push(mapper, PanelEncoder::DataEntryA, false) == std::vector<PanelEvent>{{pushA.row, pushB.mask}},
				"encoder release cleared another push");
			expect(push(mapper, PanelEncoder::DataEntryA, false).empty(), "duplicate push release emitted events");
			expect(push(mapper, PanelEncoder::Level, true).empty(), "Level has no push switch but emitted events");
		}
	}

	void checkEncoderTicks()
	{
		for(const auto model : {MachineModel::Machinedrum, MachineModel::Monomachine})
		{
			const PanelGestureMapper mapper(model);
			const auto command = *md::panelEncoderCommand(model, PanelEncoder::DataEntryA);
			expect(turn(mapper, PanelEncoder::DataEntryA, 0).empty(), "zero steps emitted ticks");
			expect(turn(mapper, PanelEncoder::DataEntryA, 3) == repeat({command, 0x01}, 3), "+3 did not emit three ticks");
			expect(turn(mapper, PanelEncoder::DataEntryA, -5) == repeat({command, 0xff}, 5), "-5 did not emit five ticks");
		}
	}

	void checkEncoderTicksRespectOutputSpace()
	{
		PanelGestureMapper mapper(MachineModel::Machinedrum);
		const auto command = *md::panelEncoderCommand(MachineModel::Machinedrum, PanelEncoder::DataEntryB);
		PanelGestureMapper::Output output;
		(void)mapper.setButton(PanelControl::Play, true, output);
		const auto room = static_cast<int>(PanelGestureMapper::Output::g_capacity - output.count);
		expect(mapper.turnEncoder(PanelEncoder::DataEntryB, -100, output) == -room,
			"a long turn did not report the steps that fit");
		expect(output.count == PanelGestureMapper::Output::g_capacity, "a long turn did not fill the output");
		expect(output.events.back() == PanelEvent{command, 0xff}, "a long turn emitted the wrong tick");
		expect(mapper.turnEncoder(PanelEncoder::DataEntryB, 1, output) == 0, "a full output accepted another tick");
	}

	void checkParameterAddressForPacket()
	{
		namespace pp = md::panelParameters;
		for(const auto model : {MachineModel::Machinedrum, MachineModel::Monomachine})
		{
			for(uint8_t c = 0; c <= static_cast<uint8_t>(PanelControl::ClassicExtended); ++c)
			{
				const auto packet = md::panelPacket(model, static_cast<PanelControl>(c));
				if(!packet)
					continue;
				const auto address = pp::addressForPacket(model, *packet);
				expect(address && address->page == pp::ButtonPage && address->index == c,
					"button packet did not map back to its parameter");
			}
			for(uint8_t e = 0; e <= static_cast<uint8_t>(PanelEncoder::DataEntryH); ++e)
			{
				const auto packet = *md::panelEncoderPressPacket(model, static_cast<PanelEncoder>(e));
				const auto address = pp::addressForPacket(model, packet);
				expect(address && address->page == pp::EncoderPushPage && address->index == e,
					"encoder push packet did not map back to its parameter");
			}
			expect(!pp::addressForPacket(model, {0x20, 0x03}), "a multi-bit packet mapped to one control");
		}
	}

	void checkUnsupportedControlsAreInert()
	{
		PanelGestureMapper mapper(MachineModel::Monomachine);
		expect(!md::panelEncoderCommand(MachineModel::Monomachine, PanelEncoder::SoundSelection),
			"test premise: MM has no sound-selection encoder");
		expect(!PanelGestureMapper::supportsEncoder(MachineModel::Monomachine, PanelEncoder::SoundSelection),
			"unsupported encoder reported as supported");
		expect(turn(mapper, PanelEncoder::SoundSelection, 4).empty(), "unsupported encoder emitted ticks");
		for(uint8_t c = 0; c <= static_cast<uint8_t>(PanelControl::ClassicExtended); ++c)
		{
			const auto control = static_cast<PanelControl>(c);
			for(const auto model : {MachineModel::Machinedrum, MachineModel::Monomachine})
			{
				const auto supported = PanelGestureMapper::supportsButton(model, control);
				expect(supported == md::panelPacket(model, control).has_value(),
					"button support differs from the verified packet table");
				PanelGestureMapper fresh(model);
				expect(button(fresh, control, true).empty() == !supported,
					"unsupported button emitted events or supported button emitted none");
			}
		}
	}

	void checkReleaseAll()
	{
		const auto model = MachineModel::Machinedrum;
		const auto pair = *findRowPair(model);
		PanelGestureMapper mapper(model);
		(void)button(mapper, pair.first, true);
		(void)button(mapper, pair.second, true);
		(void)button(mapper, PanelControl::Play, true);
		(void)push(mapper, PanelEncoder::DataEntryA, true);

		PanelGestureMapper::Output output;
		mapper.releaseAll(output);
		const auto released = events(output);
		expect(!released.empty(), "releaseAll emitted nothing while controls were held");
		for(const auto& event : released)
			expect(event.argument == 0, "releaseAll left a row mask set");
		expect(button(mapper, PanelControl::Play, false).empty(), "control still held after releaseAll");
		expect(!button(mapper, PanelControl::Play, true).empty(), "control could not be pressed after releaseAll");

		PanelGestureMapper::Output empty;
		PanelGestureMapper idle(model);
		idle.releaseAll(empty);
		expect(empty.count == 0, "releaseAll emitted events with nothing held");
	}

	void checkResetForgetsEverythingSilently()
	{
		PanelGestureMapper mapper(MachineModel::Machinedrum);
		(void)button(mapper, PanelControl::Play, true);
		(void)push(mapper, PanelEncoder::DataEntryA, true);
		// A rebooted machine starts with released controls, so reset emits nothing.
		mapper.reset();
		expect(button(mapper, PanelControl::Play, false).empty(), "reset left a button held");
		expect(push(mapper, PanelEncoder::DataEntryA, false).empty(), "reset left a push held");
	}
}

int main()
{
	checkButtonPressRelease();
	checkButtonIdempotence();
	checkRowMerging();
	checkEncoderPushSharesRowState();
	checkEncoderTicks();
	checkEncoderTicksRespectOutputSpace();
	checkParameterAddressForPacket();
	checkUnsupportedControlsAreInert();
	checkReleaseAll();
	checkResetForgetsEverythingSilently();
	std::cout << "mdPanelGestureMapperTest passed\n";
	return 0;
}
