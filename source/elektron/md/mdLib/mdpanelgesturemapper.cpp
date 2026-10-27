#include "mdpanelgesturemapper.h"

#include <algorithm>
#include <cstdlib>

namespace md
{
	namespace
	{
		constexpr uint8_t g_firstRow = 0x20;
		constexpr uint8_t g_lastRow = 0x26;
	}

	PanelGestureMapper::PanelGestureMapper(const MachineModel _model) : m_model(_model)
	{
	}

	bool PanelGestureMapper::supportsButton(const MachineModel _model, const PanelControl _control)
	{
		return panelPacket(_model, _control).has_value();
	}

	bool PanelGestureMapper::supportsEncoder(const MachineModel _model, const PanelEncoder _encoder)
	{
		return panelEncoderCommand(_model, _encoder).has_value();
	}

	bool PanelGestureMapper::supportsEncoderPush(const MachineModel _model, const PanelEncoder _encoder)
	{
		return panelEncoderPressPacket(_model, _encoder).has_value();
	}

	bool PanelGestureMapper::setButton(const PanelControl _control, const bool _pressed, Output& _output)
	{
		const auto index = static_cast<size_t>(_control);
		if(index >= g_controlCount)
			return false;
		bool held = m_heldButtons.test(index);
		const auto changed = setRowControl(panelPacket(m_model, _control), _pressed, held, _output);
		m_heldButtons.set(index, held);
		return changed;
	}

	bool PanelGestureMapper::setEncoderPush(const PanelEncoder _encoder, const bool _pressed, Output& _output)
	{
		const auto index = static_cast<size_t>(_encoder);
		if(index >= g_encoderCount)
			return false;
		bool held = m_heldPushes.test(index);
		const auto changed = setRowControl(panelEncoderPressPacket(m_model, _encoder), _pressed, held, _output);
		m_heldPushes.set(index, held);
		return changed;
	}

	bool PanelGestureMapper::setRowControl(const std::optional<PanelPacket> _packet, const bool _pressed,
		bool& _held, Output& _output)
	{
		if(!_packet || _held == _pressed)
			return false;
		_held = _pressed;
		const auto combined = _pressed ? m_rows.press(*_packet) : m_rows.release(*_packet);
		append(_output, {combined.row, combined.mask});
		return true;
	}

	int PanelGestureMapper::turnEncoder(const PanelEncoder _encoder, const int _steps, Output& _output) const
	{
		const auto command = panelEncoderCommand(m_model, _encoder);
		if(!command || _steps == 0)
			return 0;
		const auto room = static_cast<int>(_output.events.size() - _output.count);
		const auto count = std::min(std::abs(_steps), room);
		const PanelEvent tick{*command, static_cast<uint8_t>(_steps > 0 ? 0x01 : 0xff)};
		for(int i = 0; i < count; ++i)
			append(_output, tick);
		return _steps > 0 ? count : -count;
	}

	void PanelGestureMapper::releaseAll(Output& _output)
	{
		for(auto row = g_firstRow; row <= g_lastRow; ++row)
		{
			if(m_rows.mask(row) != 0)
				append(_output, {row, 0});
		}
		m_rows.reset();
		m_heldButtons.reset();
		m_heldPushes.reset();
	}

	void PanelGestureMapper::reset()
	{
		m_rows.reset();
		m_heldButtons.reset();
		m_heldPushes.reset();
	}

	void PanelGestureMapper::append(Output& _output, const PanelEvent _event)
	{
		if(_output.count < _output.events.size())
			_output.events[_output.count++] = _event;
	}
}
