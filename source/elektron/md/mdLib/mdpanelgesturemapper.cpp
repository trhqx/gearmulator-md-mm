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

	bool PanelGestureMapper::setEncoderPosition(const PanelEncoder _encoder, const uint8_t _position,
		const double _nowMilliseconds, Output& _output)
	{
		const auto index = static_cast<size_t>(_encoder);
		const auto command = panelEncoderCommand(m_model, _encoder);
		if(index >= g_encoderCount || !command)
			return false;

		auto& state = m_encoders[index];
		const auto position = std::min(_position, g_encoderMaximum);
		if(position == state.position)
			return false;

		const auto delta = static_cast<int>(position) - static_cast<int>(state.position);
		const auto idle = !state.touched
			|| _nowMilliseconds - state.lastChangeMilliseconds >= g_recentreIdleMilliseconds;
		const auto isRecentre = position == g_encoderCentre && std::abs(delta) > 1 && idle;

		state.position = position;
		state.touched = true;
		state.lastChangeMilliseconds = _nowMilliseconds;
		if(isRecentre)
			return false;

		const PanelEvent tick{*command, static_cast<uint8_t>(delta > 0 ? 0x01 : 0xff)};
		const auto count = std::min(std::abs(delta), g_encoderBurstCap);
		for(int i = 0; i < count; ++i)
			append(_output, tick);
		return true;
	}

	void PanelGestureMapper::rebaseEncoder(const PanelEncoder _encoder, const uint8_t _position)
	{
		const auto index = static_cast<size_t>(_encoder);
		if(index >= g_encoderCount)
			return;
		m_encoders[index].position = std::min(_position, g_encoderMaximum);
	}

	std::optional<uint8_t> PanelGestureMapper::recentreTarget(const PanelEncoder _encoder,
		const double _nowMilliseconds) const
	{
		const auto index = static_cast<size_t>(_encoder);
		if(index >= g_encoderCount || !supportsEncoder(m_model, _encoder))
			return std::nullopt;
		const auto& state = m_encoders[index];
		if(!state.touched || (state.position >= g_recentreBandLow && state.position <= g_recentreBandHigh))
			return std::nullopt;
		if(_nowMilliseconds - state.lastChangeMilliseconds < g_recentreIdleMilliseconds)
			return std::nullopt;
		return g_encoderCentre;
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
		m_encoders = {};
	}

	void PanelGestureMapper::append(Output& _output, const PanelEvent _event)
	{
		if(_output.count < _output.events.size())
			_output.events[_output.count++] = _event;
	}
}
