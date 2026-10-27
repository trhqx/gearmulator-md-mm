#pragma once

#include <array>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "mdpanel.h"
#include "mdtypes.h"

namespace md
{
	// One complete UART2 panel command as accepted by Hardware::trySendPanelEvent:
	// a scan-row state (row, mask) or an encoder tick (command, +1/-1).
	struct PanelEvent
	{
		uint8_t command = 0;
		uint8_t argument = 0;

		constexpr bool operator==(const PanelEvent& _other) const
		{
			return command == _other.command && argument == _other.argument;
		}
	};

	// Translates front-panel gestures into panel events.
	//
	// Buttons and encoder push switches are momentary bools; the mapper owns the
	// single scan-row state so simultaneous presses from any source merge correctly.
	// Encoders are endless: a signed step count becomes that many encoder ticks.
	// See doc/mdmm_panel_host_parameters.md.
	//
	// Not thread-safe: exactly one owner thread feeds it. Allocation-free.
	class PanelGestureMapper
	{
	public:
		struct Output
		{
			static constexpr size_t g_capacity = 16;
			std::array<PanelEvent, g_capacity> events{};
			size_t count = 0;
		};

		explicit PanelGestureMapper(MachineModel _model);

		static bool supportsButton(MachineModel _model, PanelControl _control);
		static bool supportsEncoder(MachineModel _model, PanelEncoder _encoder);
		static bool supportsEncoderPush(MachineModel _model, PanelEncoder _encoder);

		bool setButton(PanelControl _control, bool _pressed, Output& _output);
		bool setEncoderPush(PanelEncoder _encoder, bool _pressed, Output& _output);
		// Appends up to |_steps| ticks, limited by the free space in _output, and
		// returns the signed number of steps emitted.
		int turnEncoder(PanelEncoder _encoder, int _steps, Output& _output) const;

		// Release every held button and push switch, emitting one cleared row each.
		void releaseAll(Output& _output);
		// Forget all state without emitting events, for a freshly booted machine.
		void reset();

	private:
		static constexpr size_t g_controlCount = static_cast<size_t>(PanelControl::ClassicExtended) + 1;
		static constexpr size_t g_encoderCount = static_cast<size_t>(PanelEncoder::SoundSelection) + 1;

		bool setRowControl(std::optional<PanelPacket> _packet, bool _pressed, bool& _held, Output& _output);
		static void append(Output& _output, PanelEvent _event);

		const MachineModel m_model;
		PanelRowState m_rows;
		std::bitset<g_controlCount> m_heldButtons;
		std::bitset<g_encoderCount> m_heldPushes;
	};
}
