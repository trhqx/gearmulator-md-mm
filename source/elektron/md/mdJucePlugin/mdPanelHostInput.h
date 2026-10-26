#pragma once

#include "mdRealtimeQueue.h"

#include "mdLib/mdpanelgesturemapper.h"
#include "mdLib/mdpanelparameters.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace mdJucePlugin
{
	// Bridges front-panel host parameters to the PanelGestureMapper.
	//
	// Hosts may deliver the same change on several threads (VST3: message thread and
	// audio thread), so post() is lock-free and callable from anywhere, while
	// exactly one consumer - the audio thread - calls drain(). Besides the ordered
	// queue, the latest value of every control is kept; a queue overflow marks the
	// control dirty so its final state is applied even if intermediate values are
	// lost. A button can therefore never stay held because a release was dropped.
	//
	// JUCE-free so it can be tested headless.
	class PanelHostInput
	{
	public:
		static constexpr size_t g_queueCapacity = 256;

		struct DrainContext
		{
			double nowMilliseconds = 0.0;	// audio-sample clock, see Controller
			bool firmwareReady = false;
			uint64_t hardwareEpoch = 0;
		};

		explicit PanelHostInput(const md::MachineModel _model) : m_model(_model), m_mapper(_model)
		{
		}

		// Any thread. False if the address is not a panel control of this model.
		bool post(const uint8_t _page, const uint8_t _index, const int _value)
		{
			const auto slot = slotFor(_page, _index);
			if(!slot)
				return false;
			const auto value = static_cast<uint8_t>(std::clamp(_value, 0,
				static_cast<int>(md::PanelGestureMapper::g_encoderMaximum)));
			m_latest[*slot].store(value, std::memory_order_release);
			if(!m_queue.tryPush({*slot, value}))
				m_dirty[*slot / 64].fetch_or(uint64_t{1} << (*slot % 64), std::memory_order_acq_rel);
			return true;
		}

		// Any thread. The next drain releases every held control, e.g. after a
		// project load, and returns the encoders to the centre.
		void requestReset()
		{
			m_resetRequested.store(true, std::memory_order_release);
		}

		// Any thread. One bit per md::PanelEncoder whose host value should be moved
		// back to the centre; each request is reported once.
		uint32_t takeRecentreRequests()
		{
			return m_recentreRequests.exchange(0, std::memory_order_acq_rel);
		}

		// Audio thread only. Calls _sink(const md::PanelEvent&) for each event.
		template<typename Sink>
		void drain(const DrainContext& _context, Sink&& _sink)
		{
			Output output;
			auto flush = [&]()
			{
				if(_context.firmwareReady)
					for(size_t i = 0; i < output.count; ++i)
						_sink(output.events[i]);
				output.count = 0;
			};

			if(_context.hardwareEpoch != m_hardwareEpoch)
			{
				// A replaced machine boots with every control released.
				m_hardwareEpoch = _context.hardwareEpoch;
				m_mapper.reset();
				m_recentrePending = 0;
			}
			if(m_resetRequested.exchange(false, std::memory_order_acq_rel))
			{
				m_mapper.releaseAll(output);
				flush();
				m_mapper.reset();
				m_recentrePending = 0;
			}

			Entry entry;
			for(size_t i = 0; i < g_queueCapacity && m_queue.tryPop(entry); ++i)
			{
				apply(entry.slot, entry.value, _context, output);
				flush();
			}
			for(size_t word = 0; word < m_dirty.size(); ++word)
			{
				auto bits = m_dirty[word].exchange(0, std::memory_order_acq_rel);
				while(bits != 0)
				{
					const auto bit = static_cast<size_t>(countTrailingZeros(bits));
					bits &= bits - 1;
					const auto slot = static_cast<uint8_t>(word * 64 + bit);
					apply(slot, m_latest[slot].load(std::memory_order_acquire), _context, output);
					flush();
				}
			}

			if(!_context.firmwareReady)
				return;
			for(uint8_t e = 0; e < g_encoderCount; ++e)
			{
				const auto bit = 1u << e;
				if((m_recentrePending & bit) == 0
					&& m_mapper.recentreTarget(static_cast<md::PanelEncoder>(e), _context.nowMilliseconds))
				{
					m_recentrePending |= bit;
					m_recentreRequests.fetch_or(bit, std::memory_order_acq_rel);
				}
			}
		}

	private:
		using Output = md::PanelGestureMapper::Output;

		struct Entry
		{
			uint8_t slot = 0;
			uint8_t value = 0;
		};

		static constexpr uint8_t g_buttonCount = static_cast<uint8_t>(md::PanelControl::ClassicExtended) + 1;
		static constexpr uint8_t g_encoderCount = static_cast<uint8_t>(md::PanelEncoder::SoundSelection) + 1;
		static constexpr uint8_t g_pushCount = static_cast<uint8_t>(md::PanelEncoder::DataEntryH) + 1;
		static constexpr uint8_t g_firstEncoderSlot = g_buttonCount;
		static constexpr uint8_t g_firstPushSlot = g_firstEncoderSlot + g_encoderCount;
		static constexpr size_t g_slotCount = g_firstPushSlot + g_pushCount;

		static int countTrailingZeros(uint64_t _bits)
		{
			int count = 0;
			while((_bits & 1) == 0)
			{
				_bits >>= 1;
				++count;
			}
			return count;
		}

		std::optional<uint8_t> slotFor(const uint8_t _page, const uint8_t _index) const
		{
			namespace pp = md::panelParameters;
			using Mapper = md::PanelGestureMapper;
			if(_page == pp::ButtonPage && _index < g_buttonCount
				&& Mapper::supportsButton(m_model, static_cast<md::PanelControl>(_index)))
				return _index;
			if(_page == pp::EncoderPage && _index < g_encoderCount
				&& Mapper::supportsEncoder(m_model, static_cast<md::PanelEncoder>(_index)))
				return static_cast<uint8_t>(g_firstEncoderSlot + _index);
			if(_page == pp::EncoderPushPage && _index < g_pushCount
				&& Mapper::supportsEncoderPush(m_model, static_cast<md::PanelEncoder>(_index)))
				return static_cast<uint8_t>(g_firstPushSlot + _index);
			return std::nullopt;
		}

		void apply(const uint8_t _slot, const uint8_t _value, const DrainContext& _context, Output& _output)
		{
			if(_slot >= g_firstPushSlot)
			{
				// Never deliver a press the firmware cannot see: its release would
				// then be the only edge the machine receives.
				if(_context.firmwareReady)
					m_mapper.setEncoderPush(static_cast<md::PanelEncoder>(_slot - g_firstPushSlot), _value != 0, _output);
				return;
			}
			if(_slot >= g_firstEncoderSlot)
			{
				const auto encoder = static_cast<md::PanelEncoder>(_slot - g_firstEncoderSlot);
				m_recentrePending &= ~(1u << (_slot - g_firstEncoderSlot));
				if(_context.firmwareReady)
					m_mapper.setEncoderPosition(encoder, _value, _context.nowMilliseconds, _output);
				else
					m_mapper.rebaseEncoder(encoder, _value);
				return;
			}
			if(_context.firmwareReady)
				m_mapper.setButton(static_cast<md::PanelControl>(_slot), _value != 0, _output);
		}

		const md::MachineModel m_model;
		RealtimeQueue<Entry, g_queueCapacity> m_queue;
		std::array<std::atomic<uint8_t>, g_slotCount> m_latest{};
		std::array<std::atomic<uint64_t>, (g_slotCount + 63) / 64> m_dirty{};
		std::atomic<bool> m_resetRequested{false};
		std::atomic<uint32_t> m_recentreRequests{0};

		// Consumer-owned.
		md::PanelGestureMapper m_mapper;
		uint64_t m_hardwareEpoch = 0;
		uint32_t m_recentrePending = 0;
	};
}
