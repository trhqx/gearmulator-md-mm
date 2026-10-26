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

	// Translates host-parameter values for front-panel controls into panel events.
	//
	// Buttons and encoder push switches are momentary bools; the mapper owns the
	// single scan-row state so simultaneous presses from any source merge correctly.
	// Encoders are 0..127 positions driven by a host in relative mode: the delta to
	// the previous position becomes encoder ticks. Because a host saturates at the
	// ends of the range, the owner periodically moves an idle encoder back to the
	// centre (recentreTarget). A jump onto the centre after the idle interval is a
	// recentre - self-initiated or replayed from recorded automation - and only
	// moves the base. See doc/mdmm_panel_host_parameters.md.
	//
	// Not thread-safe: exactly one owner thread feeds it. Allocation-free.
	class PanelGestureMapper
	{
	public:
		static constexpr uint8_t g_encoderCentre = 64;
		static constexpr uint8_t g_encoderMaximum = 127;
		static constexpr int g_encoderBurstCap = 8;
		static constexpr double g_recentreIdleMilliseconds = 300.0;
		// Positions inside the middle third are left alone; recentring there would
		// only fight a host that is still turning.
		static constexpr uint8_t g_recentreBandLow = 43;
		static constexpr uint8_t g_recentreBandHigh = 84;

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
		bool setEncoderPosition(PanelEncoder _encoder, uint8_t _position, double _nowMilliseconds,
			Output& _output);

		// Adopt a position without emitting ticks, e.g. after a state restore.
		void rebaseEncoder(PanelEncoder _encoder, uint8_t _position);
		std::optional<uint8_t> recentreTarget(PanelEncoder _encoder, double _nowMilliseconds) const;

		// Release every held button and push switch, emitting one cleared row each.
		void releaseAll(Output& _output);
		// Forget all state without emitting events, for a freshly booted machine.
		void reset();

	private:
		static constexpr size_t g_controlCount = static_cast<size_t>(PanelControl::ClassicExtended) + 1;
		static constexpr size_t g_encoderCount = static_cast<size_t>(PanelEncoder::SoundSelection) + 1;

		struct EncoderState
		{
			uint8_t position = g_encoderCentre;
			bool touched = false;
			double lastChangeMilliseconds = 0.0;
		};

		bool setRowControl(std::optional<PanelPacket> _packet, bool _pressed, bool& _held, Output& _output);
		static void append(Output& _output, PanelEvent _event);

		const MachineModel m_model;
		PanelRowState m_rows;
		std::bitset<g_controlCount> m_heldButtons;
		std::bitset<g_encoderCount> m_heldPushes;
		std::array<EncoderState, g_encoderCount> m_encoders{};
	};
}
