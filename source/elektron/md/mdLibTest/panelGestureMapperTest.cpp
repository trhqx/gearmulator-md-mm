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

	constexpr double g_idle = PanelGestureMapper::g_recentreIdleMilliseconds;

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

	std::vector<PanelEvent> turn(PanelGestureMapper& _mapper, const PanelEncoder _encoder,
		const uint8_t _position, const double _now)
	{
		PanelGestureMapper::Output output;
		_mapper.setEncoderPosition(_encoder, _position, _now, output);
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

	void checkEncoderDeltas()
	{
		PanelGestureMapper mapper(MachineModel::Machinedrum);
		const auto command = *md::panelEncoderCommand(MachineModel::Machinedrum, PanelEncoder::DataEntryA);
		const PanelEvent up{command, 0x01};
		const PanelEvent down{command, 0xff};
		double now = 0.0;
		expect(turn(mapper, PanelEncoder::DataEntryA, 64, now).empty(), "centre position produced ticks");
		expect(turn(mapper, PanelEncoder::DataEntryA, 67, now += 10) == repeat(up, 3), "+3 did not emit three ticks");
		expect(turn(mapper, PanelEncoder::DataEntryA, 67, now += 1).empty(), "duplicate value emitted ticks");
		expect(turn(mapper, PanelEncoder::DataEntryA, 62, now += 10) == repeat(down, 5), "-5 did not emit five ticks");
		expect(turn(mapper, PanelEncoder::DataEntryA, 127, now += 10)
			== repeat(up, PanelGestureMapper::g_encoderBurstCap), "large jump was not capped");
	}

	void checkRecentreEchoIsIgnored()
	{
		PanelGestureMapper mapper(MachineModel::Machinedrum);
		const auto command = *md::panelEncoderCommand(MachineModel::Machinedrum, PanelEncoder::DataEntryB);
		double now = 0.0;
		(void)turn(mapper, PanelEncoder::DataEntryB, 90, now);
		// A recentre (self-initiated or replayed from automation) lands exactly on
		// the centre after the idle interval and moves by more than one step.
		expect(turn(mapper, PanelEncoder::DataEntryB, PanelGestureMapper::g_encoderCentre, now += g_idle).empty(),
			"jump back to centre after idle emitted counter-ticks");
		expect(turn(mapper, PanelEncoder::DataEntryB, 65, now += 10) == std::vector<PanelEvent>{{command, 0x01}},
			"turning after a recentre did not continue from the centre");
	}

	void checkRealMovesOntoCentreAreKept()
	{
		PanelGestureMapper mapper(MachineModel::Machinedrum);
		const auto command = *md::panelEncoderCommand(MachineModel::Machinedrum, PanelEncoder::DataEntryC);
		double now = 0.0;
		(void)turn(mapper, PanelEncoder::DataEntryC, 66, now);
		expect(turn(mapper, PanelEncoder::DataEntryC, 64, now += 10) == repeat({command, 0xff}, 2),
			"a fast move onto the centre was treated as a recentre");
		(void)turn(mapper, PanelEncoder::DataEntryC, 63, now += 10);
		expect(turn(mapper, PanelEncoder::DataEntryC, 64, now += 2 * g_idle) == std::vector<PanelEvent>{{command, 0x01}},
			"a single step onto the centre after a pause was dropped");
	}

	void checkRebase()
	{
		PanelGestureMapper mapper(MachineModel::Monomachine);
		const auto command = *md::panelEncoderCommand(MachineModel::Monomachine, PanelEncoder::Level);
		mapper.rebaseEncoder(PanelEncoder::Level, 20);
		expect(turn(mapper, PanelEncoder::Level, 20, 0.0).empty(), "rebased position produced ticks");
		expect(turn(mapper, PanelEncoder::Level, 22, 10.0) == repeat({command, 0x01}, 2),
			"delta was not measured from the rebased position");
	}

	void checkAdoptedPositionCountsAsMovement()
	{
		PanelGestureMapper mapper(MachineModel::Machinedrum);
		constexpr auto encoder = PanelEncoder::DataEntryF;
		mapper.adoptEncoderPosition(encoder, 110, 0.0);
		expect(!mapper.recentreTarget(encoder, g_idle / 2), "adopted position recentred too early");
		expect(mapper.recentreTarget(encoder, g_idle).has_value(), "adopted position was never recentred");
		expect(turn(mapper, encoder, PanelGestureMapper::g_encoderCentre, g_idle).empty(),
			"recentre after an adopted position emitted ticks");
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

	void checkRecentreTarget()
	{
		PanelGestureMapper mapper(MachineModel::Machinedrum);
		constexpr auto encoder = PanelEncoder::DataEntryD;
		double now = 0.0;
		expect(!mapper.recentreTarget(encoder, now + 10 * g_idle), "untouched encoder asked for a recentre");
		(void)turn(mapper, encoder, 70, now);
		expect(!mapper.recentreTarget(encoder, now + 10 * g_idle), "position inside the middle third was recentred");
		(void)turn(mapper, encoder, 100, now += 10);
		expect(!mapper.recentreTarget(encoder, now + g_idle / 2), "recentre requested before the idle interval");
		const auto target = mapper.recentreTarget(encoder, now + g_idle);
		expect(target && *target == PanelGestureMapper::g_encoderCentre, "idle encoder outside the band not recentred");
		(void)turn(mapper, encoder, 20, now += 10);
		expect(mapper.recentreTarget(encoder, now + g_idle).has_value(), "low end of the band was not recentred");
	}

	void checkUnsupportedControlsAreInert()
	{
		PanelGestureMapper mapper(MachineModel::Monomachine);
		expect(!md::panelEncoderCommand(MachineModel::Monomachine, PanelEncoder::SoundSelection),
			"test premise: MM has no sound-selection encoder");
		expect(!PanelGestureMapper::supportsEncoder(MachineModel::Monomachine, PanelEncoder::SoundSelection),
			"unsupported encoder reported as supported");
		expect(turn(mapper, PanelEncoder::SoundSelection, 100, 0.0).empty(), "unsupported encoder emitted ticks");
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
		const auto model = MachineModel::Machinedrum;
		const auto command = *md::panelEncoderCommand(model, PanelEncoder::DataEntryA);
		PanelGestureMapper mapper(model);
		(void)button(mapper, PanelControl::Play, true);
		(void)push(mapper, PanelEncoder::DataEntryA, true);
		(void)turn(mapper, PanelEncoder::DataEntryA, 100, 0.0);
		// A rebooted machine starts with released controls, so reset emits nothing.
		mapper.reset();
		expect(button(mapper, PanelControl::Play, false).empty(), "reset left a button held");
		expect(push(mapper, PanelEncoder::DataEntryA, false).empty(), "reset left a push held");
		expect(!mapper.recentreTarget(PanelEncoder::DataEntryA, 10000.0), "reset kept encoder history");
		expect(turn(mapper, PanelEncoder::DataEntryA, 65, 10.0) == std::vector<PanelEvent>{{command, 0x01}},
			"reset did not return the encoder to the centre");
	}
}

int main()
{
	checkButtonPressRelease();
	checkButtonIdempotence();
	checkRowMerging();
	checkEncoderPushSharesRowState();
	checkEncoderDeltas();
	checkRecentreEchoIsIgnored();
	checkRealMovesOntoCentreAreKept();
	checkRebase();
	checkRecentreTarget();
	checkParameterAddressForPacket();
	checkAdoptedPositionCountsAsMovement();
	checkUnsupportedControlsAreInert();
	checkReleaseAll();
	checkResetForgetsEverythingSilently();
	std::cout << "mdPanelGestureMapperTest passed\n";
	return 0;
}
