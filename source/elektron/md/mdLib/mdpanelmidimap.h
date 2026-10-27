#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>

#include "mdpanel.h"
#include "mdtypes.h"

namespace md
{
	// Front-panel control a MIDI controller can drive.
	struct PanelMidiTarget
	{
		enum class Type : uint8_t
		{
			None,
			Button,			// md::PanelControl
			EncoderPush,	// md::PanelEncoder
			Encoder			// md::PanelEncoder
		};

		Type type = Type::None;
		uint8_t index = 0;

		static constexpr PanelMidiTarget button(const PanelControl _control)
		{
			return {Type::Button, static_cast<uint8_t>(_control)};
		}
		static constexpr PanelMidiTarget encoderPush(const PanelEncoder _encoder)
		{
			return {Type::EncoderPush, static_cast<uint8_t>(_encoder)};
		}
		static constexpr PanelMidiTarget encoder(const PanelEncoder _encoder)
		{
			return {Type::Encoder, static_cast<uint8_t>(_encoder)};
		}

		bool isSupported(MachineModel _model) const;

		constexpr bool operator==(const PanelMidiTarget& _other) const
		{
			return type == _other.type && (type == Type::None || index == _other.index);
		}
		constexpr bool operator!=(const PanelMidiTarget& _other) const { return !(*this == _other); }
	};

	struct PanelMidiTrigger
	{
		enum class Type : uint8_t
		{
			Note,
			ControlChange
		};

		Type type = Type::Note;
		uint8_t number = 0;

		static constexpr PanelMidiTrigger note(const uint8_t _note) { return {Type::Note, _note}; }
		static constexpr PanelMidiTrigger controlChange(const uint8_t _cc) { return {Type::ControlChange, _cc}; }

		constexpr bool operator==(const PanelMidiTrigger& _other) const
		{
			return type == _other.type && number == _other.number;
		}
	};

	// How a controller reports the movement of an encoder in a CC value.
	enum class PanelMidiEncoding : uint8_t
	{
		TwosComplement,	// 1..63 up, 127..65 down (Ableton "Relative (2's Comp.)")
		BinaryOffset,	// 65.. up, 63.. down
		SignMagnitude,	// 1..63 up, 65..127 down
		Absolute		// a knob with end stops; its change turns the encoder
	};

	// The bindings of one machine's panel to a MIDI channel. Every channel message
	// on that channel belongs to the panel and never reaches the machine. Each
	// trigger drives at most one control and each control has at most one trigger.
	//
	// Trivially copyable so it can be handed to the audio thread through a queue.
	class PanelMidiMap
	{
	public:
		static constexpr int8_t g_off = -1;

		int8_t channel = g_off;	// zero-based, or g_off
		PanelMidiEncoding encoding = PanelMidiEncoding::TwosComplement;

		// False if the target does not exist on _model or cannot use the trigger
		// (encoders need a CC). Replaces earlier bindings of either side.
		bool bind(PanelMidiTrigger _trigger, PanelMidiTarget _target, MachineModel _model);
		void unbind(PanelMidiTarget _target);
		void clearBindings();

		PanelMidiTarget find(PanelMidiTrigger _trigger) const;
		std::optional<PanelMidiTrigger> findTrigger(PanelMidiTarget _target) const;

		// Line-based, human-readable; see doc/mdmm_panel_host_parameters.md. Lines that
		// cannot be applied to _model are skipped.
		std::string toText() const;
		static PanelMidiMap fromText(const std::string& _text, MachineModel _model);

		bool operator==(const PanelMidiMap& _other) const;

	private:
		std::array<PanelMidiTarget, 128>& table(PanelMidiTrigger::Type _type);
		const std::array<PanelMidiTarget, 128>& table(PanelMidiTrigger::Type _type) const;

		std::array<PanelMidiTarget, 128> m_notes{};
		std::array<PanelMidiTarget, 128> m_controlChanges{};
	};

	// A channel voice message on _channel (zero-based; g_off never matches).
	constexpr bool isPanelMidiMessage(const int8_t _channel, const uint8_t _status)
	{
		return _channel >= 0 && _status >= 0x80 && _status < 0xf0 && (_status & 0x0f) == _channel;
	}

	// One press/release (value 1/0) of a button or push, or a turn by value steps.
	struct PanelMidiAction
	{
		PanelMidiTarget target;
		int value = 0;

		bool operator==(const PanelMidiAction& _other) const
		{
			return target == _other.target && value == _other.value;
		}
	};

	// Turns panel-channel MIDI into panel actions and records learned bindings.
	// Not thread-safe: one owner thread. Allocation-free.
	class PanelMidiTranslator
	{
	public:
		struct Binding
		{
			PanelMidiTrigger trigger;
			PanelMidiTarget target;
		};

		struct Result
		{
			bool consumed = false;					// the message belongs to the panel
			std::optional<PanelMidiAction> action;
			std::optional<Binding> learned;			// now part of map()
		};

		explicit PanelMidiTranslator(MachineModel _model);

		void setMap(const PanelMidiMap& _map);
		const PanelMidiMap& map() const { return m_map; }

		// The next suitable message on the panel channel is bound to _target
		// instead of driving the panel. For an encoder, a CC binds the turn and a
		// note binds its push switch.
		void learn(PanelMidiTarget _target);
		void cancelLearn();
		bool isLearning() const { return m_learnTarget.type != PanelMidiTarget::Type::None; }

		Result translate(uint8_t _status, uint8_t _data1, uint8_t _data2);

	private:
		std::optional<PanelMidiAction> toAction(PanelMidiTrigger _trigger, PanelMidiTarget _target, uint8_t _value,
			bool _pressed);
		int encoderSteps(uint8_t _controller, uint8_t _value);

		static constexpr int16_t g_noValue = -1;

		const MachineModel m_model;
		PanelMidiMap m_map;
		PanelMidiTarget m_learnTarget;
		std::array<int16_t, 128> m_lastAbsolute{};
	};
}
