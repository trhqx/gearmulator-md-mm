#include "mdpanelmidimap.h"

#include <cstring>
#include <sstream>

namespace md
{
	namespace
	{
		constexpr uint8_t g_lastControl = static_cast<uint8_t>(PanelControl::ClassicExtended);
		constexpr uint8_t g_lastEncoder = static_cast<uint8_t>(PanelEncoder::SoundSelection);

		constexpr const char* g_encodingNames[] = {"twos-complement", "binary-offset", "sign-magnitude", "absolute"};

		const char* typeName(const PanelMidiTarget::Type _type)
		{
			switch(_type)
			{
			case PanelMidiTarget::Type::Button:			return "button";
			case PanelMidiTarget::Type::EncoderPush:	return "push";
			case PanelMidiTarget::Type::Encoder:		return "encoder";
			case PanelMidiTarget::Type::None:			break;
			}
			return "";
		}

		const char* targetName(const PanelMidiTarget _target)
		{
			if(_target.type == PanelMidiTarget::Type::Button)
				return panelControlName(static_cast<PanelControl>(_target.index));
			return panelEncoderName(static_cast<PanelEncoder>(_target.index));
		}

		std::optional<PanelMidiTarget> parseTarget(const std::string& _type, const std::string& _name)
		{
			if(_type == "button")
			{
				for(uint8_t c = 0; c <= g_lastControl; ++c)
					if(_name == panelControlName(static_cast<PanelControl>(c)))
						return PanelMidiTarget::button(static_cast<PanelControl>(c));
				return std::nullopt;
			}
			PanelMidiTarget::Type type;
			if(_type == "push")
				type = PanelMidiTarget::Type::EncoderPush;
			else if(_type == "encoder")
				type = PanelMidiTarget::Type::Encoder;
			else
				return std::nullopt;
			for(uint8_t e = 0; e <= g_lastEncoder; ++e)
				if(_name == panelEncoderName(static_cast<PanelEncoder>(e)))
					return PanelMidiTarget{type, e};
			return std::nullopt;
		}

		std::optional<uint8_t> parseNumber(const std::string& _text, const int _minimum, const int _maximum)
		{
			if(_text.empty() || _text.size() > 3)
				return std::nullopt;
			int value = 0;
			for(const auto c : _text)
			{
				if(c < '0' || c > '9')
					return std::nullopt;
				value = value * 10 + (c - '0');
			}
			if(value < _minimum || value > _maximum)
				return std::nullopt;
			return static_cast<uint8_t>(value);
		}
	}

	bool PanelMidiTarget::isSupported(const MachineModel _model) const
	{
		switch(type)
		{
		case Type::Button:
			return index <= g_lastControl && panelPacket(_model, static_cast<PanelControl>(index)).has_value();
		case Type::EncoderPush:
			return index <= g_lastEncoder && panelEncoderPressPacket(_model, static_cast<PanelEncoder>(index)).has_value();
		case Type::Encoder:
			return index <= g_lastEncoder && panelEncoderCommand(_model, static_cast<PanelEncoder>(index)).has_value();
		case Type::None:
			break;
		}
		return false;
	}

	bool PanelMidiMap::bind(const PanelMidiTrigger _trigger, const PanelMidiTarget _target, const MachineModel _model)
	{
		if(_trigger.number > 127 || !_target.isSupported(_model))
			return false;
		// An endless encoder needs the value of a CC; a note only knows on and off.
		if(_target.type == PanelMidiTarget::Type::Encoder && _trigger.type != PanelMidiTrigger::Type::ControlChange)
			return false;
		unbind(_target);
		table(_trigger.type)[_trigger.number] = _target;
		return true;
	}

	void PanelMidiMap::unbind(const PanelMidiTarget _target)
	{
		for(auto* t : {&m_notes, &m_controlChanges})
			for(auto& entry : *t)
				if(entry == _target)
					entry = {};
	}

	void PanelMidiMap::clearBindings()
	{
		m_notes.fill({});
		m_controlChanges.fill({});
	}

	PanelMidiTarget PanelMidiMap::find(const PanelMidiTrigger _trigger) const
	{
		if(_trigger.number > 127)
			return {};
		return table(_trigger.type)[_trigger.number];
	}

	std::optional<PanelMidiTrigger> PanelMidiMap::findTrigger(const PanelMidiTarget _target) const
	{
		if(_target.type == PanelMidiTarget::Type::None)
			return std::nullopt;
		for(uint8_t n = 0; n < 128; ++n)
		{
			if(m_notes[n] == _target)
				return PanelMidiTrigger::note(n);
			if(m_controlChanges[n] == _target)
				return PanelMidiTrigger::controlChange(n);
		}
		return std::nullopt;
	}

	std::string PanelMidiMap::toText() const
	{
		std::ostringstream out;
		out << "channel ";
		if(channel == g_off)
			out << "off";
		else
			out << static_cast<int>(channel) + 1;
		out << "\nencoding " << g_encodingNames[static_cast<size_t>(encoding)] << '\n';
		for(const auto type : {PanelMidiTrigger::Type::Note, PanelMidiTrigger::Type::ControlChange})
		{
			const auto& entries = table(type);
			for(size_t n = 0; n < entries.size(); ++n)
			{
				if(entries[n].type == PanelMidiTarget::Type::None)
					continue;
				out << typeName(entries[n].type) << ' ' << targetName(entries[n]) << ' '
					<< (type == PanelMidiTrigger::Type::Note ? "note " : "cc ") << n << '\n';
			}
		}
		return out.str();
	}

	PanelMidiMap PanelMidiMap::fromText(const std::string& _text, const MachineModel _model)
	{
		PanelMidiMap map;
		std::istringstream in(_text);
		std::string line;
		while(std::getline(in, line))
		{
			std::istringstream words(line);
			std::string first, second, third, fourth;
			words >> first >> second >> third >> fourth;
			if(first == "channel")
			{
				if(second == "off")
					map.channel = g_off;
				else if(const auto value = parseNumber(second, 1, 16))
					map.channel = static_cast<int8_t>(*value - 1);
				continue;
			}
			if(first == "encoding")
			{
				for(size_t i = 0; i < std::size(g_encodingNames); ++i)
					if(second == g_encodingNames[i])
						map.encoding = static_cast<PanelMidiEncoding>(i);
				continue;
			}
			const auto target = parseTarget(first, second);
			const auto number = parseNumber(fourth, 0, 127);
			if(!target || !number || (third != "note" && third != "cc"))
				continue;
			map.bind(third == "note" ? PanelMidiTrigger::note(*number) : PanelMidiTrigger::controlChange(*number),
				*target, _model);
		}
		return map;
	}

	bool PanelMidiMap::operator==(const PanelMidiMap& _other) const
	{
		return channel == _other.channel && encoding == _other.encoding && m_notes == _other.m_notes
			&& m_controlChanges == _other.m_controlChanges;
	}

	std::array<PanelMidiTarget, 128>& PanelMidiMap::table(const PanelMidiTrigger::Type _type)
	{
		return _type == PanelMidiTrigger::Type::Note ? m_notes : m_controlChanges;
	}

	const std::array<PanelMidiTarget, 128>& PanelMidiMap::table(const PanelMidiTrigger::Type _type) const
	{
		return _type == PanelMidiTrigger::Type::Note ? m_notes : m_controlChanges;
	}

	PanelMidiTranslator::PanelMidiTranslator(const MachineModel _model) : m_model(_model)
	{
		m_lastAbsolute.fill(g_noValue);
	}

	void PanelMidiTranslator::setMap(const PanelMidiMap& _map)
	{
		m_map = _map;
		m_lastAbsolute.fill(g_noValue);
	}

	void PanelMidiTranslator::learn(const PanelMidiTarget _target)
	{
		m_learnTarget = _target.isSupported(m_model) ? _target : PanelMidiTarget{};
	}

	void PanelMidiTranslator::cancelLearn()
	{
		m_learnTarget = {};
	}

	PanelMidiTranslator::Result PanelMidiTranslator::translate(const uint8_t _status, const uint8_t _data1,
		const uint8_t _data2)
	{
		Result result;
		if(!isPanelMidiMessage(m_map.channel, _status))
			return result;
		result.consumed = true;

		const auto kind = _status & 0xf0;
		std::optional<PanelMidiTrigger> trigger;
		bool pressed = false;
		if(kind == 0x90 || kind == 0x80)
		{
			trigger = PanelMidiTrigger::note(_data1 & 0x7f);
			pressed = kind == 0x90 && _data2 != 0;
		}
		else if(kind == 0xb0)
		{
			trigger = PanelMidiTrigger::controlChange(_data1 & 0x7f);
			pressed = _data2 >= 64;
		}
		if(!trigger)
			return result;

		if(isLearning())
		{
			// A note only starts with its note-on; releases belong to earlier presses.
			if(trigger->type == PanelMidiTrigger::Type::Note && !pressed)
				return result;
			auto target = m_learnTarget;
			// An encoder learns its turn from a CC and its push switch from a note.
			if(target.type == PanelMidiTarget::Type::Encoder && trigger->type == PanelMidiTrigger::Type::Note)
				target.type = PanelMidiTarget::Type::EncoderPush;
			if(!m_map.bind(*trigger, target, m_model))
				return result;
			result.learned = Binding{*trigger, target};
			m_learnTarget = {};
			if(trigger->type == PanelMidiTrigger::Type::ControlChange)
				m_lastAbsolute[trigger->number] = _data2;
			return result;
		}

		const auto target = m_map.find(*trigger);
		if(target.type != PanelMidiTarget::Type::None)
			result.action = toAction(*trigger, target, _data2, pressed);
		return result;
	}

	std::optional<PanelMidiAction> PanelMidiTranslator::toAction(const PanelMidiTrigger _trigger,
		const PanelMidiTarget _target, const uint8_t _value, const bool _pressed)
	{
		if(_target.type != PanelMidiTarget::Type::Encoder)
			return PanelMidiAction{_target, _pressed ? 1 : 0};
		const auto steps = encoderSteps(_trigger.number, _value & 0x7f);
		if(steps == 0)
			return std::nullopt;
		return PanelMidiAction{_target, steps};
	}

	int PanelMidiTranslator::encoderSteps(const uint8_t _controller, const uint8_t _value)
	{
		switch(m_map.encoding)
		{
		case PanelMidiEncoding::TwosComplement:
			return _value < 64 ? _value : _value - 128;
		case PanelMidiEncoding::BinaryOffset:
			return _value - 64;
		case PanelMidiEncoding::SignMagnitude:
			return (_value & 0x40) ? -(_value & 0x3f) : (_value & 0x3f);
		case PanelMidiEncoding::Absolute:
			{
				const auto last = m_lastAbsolute[_controller];
				m_lastAbsolute[_controller] = _value;
				return last == g_noValue ? 0 : _value - last;
			}
		}
		return 0;
	}
}
