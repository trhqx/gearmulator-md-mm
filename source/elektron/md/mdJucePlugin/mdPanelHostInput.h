#pragma once

#include "mdRealtimeQueue.h"

#include "mdLib/mdpanelgesturemapper.h"
#include "mdLib/mdpanelparameters.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>

namespace mdJucePlugin
{
	// Collects front-panel input from host parameters, the editor and panel MIDI
	// and feeds it to the PanelGestureMapper.
	//
	// Hosts may deliver the same change on several threads (VST3: message thread and
	// audio thread) and physical MIDI arrives on its own thread, so post() and turn()
	// are lock-free and callable from anywhere, while exactly one consumer - the
	// audio thread - calls drain(). Besides the ordered queue, the latest value of
	// every button is kept; a queue overflow marks the button dirty so its final
	// state is applied even if intermediate values are lost. A button can therefore
	// never stay held because a release was dropped. Encoder steps lost to an
	// overflow are accumulated and delivered after the queue.
	//
	// JUCE-free so it can be tested headless.
	class PanelHostInput
	{
	public:
		static constexpr size_t g_queueCapacity = 256;
		// Long enough for the firmware's panel scan to see a tap as a press.
		static constexpr double g_tapMilliseconds = 40.0;
		// Encoder ticks per encoder and drain. The machine's panel FIFO holds 512
		// packets and drops ticks that do not fit; the rest is sent next drain.
		static constexpr int g_maxTicksPerDrain = 64;

		enum class ButtonMode : uint8_t
		{
			// 1 holds the control, 0 releases it (CC buttons, automation).
			Momentary,
			// A change to 1 is one short press, after which the owner returns the
			// parameter to 0 (takeReturnToOffRequests). Hosts such as Ableton Live
			// toggle a note-mapped on/off parameter on each note-on, so every note
			// is one press and the host shows Off whenever the key is up.
			Tap
		};

		enum class Source : uint8_t
		{
			// Host parameters: hosts, automation and validators. Withheld until the
			// firmware is ready: keys held during boot open the startup menu (test
			// mode, resets, OS upgrade), which a validator setting every parameter
			// would otherwise reach.
			Host,
			// The user in the editor. Delivered at any time, as on the hardware.
			Editor,
			// A MIDI controller on the panel channel. Always momentary, withheld
			// while booting like host input, and never reflected in host parameters.
			PanelMidi
		};

		struct DrainContext
		{
			double nowMilliseconds = 0.0;	// machine time, see Controller
			bool firmwareReady = false;		// gates host and panel MIDI input
			uint64_t hardwareEpoch = 0;
		};

		explicit PanelHostInput(const md::MachineModel _model) : m_model(_model), m_mapper(_model)
		{
		}

		// Any thread. A button or encoder push; false if the address is not a panel
		// control of this model.
		bool post(const uint8_t _page, const uint8_t _index, const int _value, const Source _source = Source::Host)
		{
			const auto slot = slotFor(_page, _index);
			if(!slot)
				return false;
			const auto value = static_cast<uint8_t>(_value != 0 ? 1 : 0);
			m_latest[*slot].store(value, std::memory_order_release);
			m_latestSource[*slot].store(_source, std::memory_order_release);
			if(!m_queue.tryPush({Kind::Row, *slot, value, 0, _source}))
				m_dirty.fetch_or(uint64_t{1} << *slot, std::memory_order_acq_rel);
			return true;
		}

		// Any thread. Turns an endless encoder by _steps ticks; false if the model
		// has no such encoder.
		bool turn(const md::PanelEncoder _encoder, const int _steps, const Source _source = Source::PanelMidi)
		{
			const auto index = static_cast<uint8_t>(_encoder);
			if(index >= g_encoderCount || !md::PanelGestureMapper::supportsEncoder(m_model, _encoder))
				return false;
			if(_steps == 0)
				return true;
			const auto steps = static_cast<int16_t>(std::clamp(_steps, -g_maxQueuedSteps, g_maxQueuedSteps));
			if(!m_queue.tryPush({Kind::Turn, index, 0, steps, _source}))
				m_overflowSteps[index].fetch_add(steps, std::memory_order_acq_rel);
			return true;
		}

		// Any thread. Applies to host parameters only; the editor and panel MIDI
		// are always momentary.
		void setHostButtonMode(const ButtonMode _mode)
		{
			m_buttonMode.store(_mode, std::memory_order_release);
		}

		// Any thread. The next drain releases every held control, e.g. after a
		// project load.
		void requestReset()
		{
			m_resetRequested.store(true, std::memory_order_release);
		}

		// Any thread. One bit per slotOf() whose host parameter should be set back
		// to 0 after a tap; each request is reported once.
		uint64_t takeReturnToOffRequests()
		{
			return m_returnToOffRequests.exchange(0, std::memory_order_acq_rel);
		}

		// The slot of a button or encoder push, as used by takeReturnToOffRequests.
		std::optional<uint8_t> slotOf(const uint8_t _page, const uint8_t _index) const
		{
			return slotFor(_page, _index);
		}

		static md::panelParameters::Address addressOf(const uint8_t _slot)
		{
			namespace pp = md::panelParameters;
			if(_slot >= g_firstPushSlot)
				return {pp::EncoderPushPage, static_cast<uint8_t>(_slot - g_firstPushSlot)};
			return {pp::ButtonPage, _slot};
		}

		// Audio thread only. Drops everything queued, e.g. while no local device
		// exists, so stale input cannot reach a machine created later.
		void discard()
		{
			Entry entry;
			for(size_t i = 0; i < g_queueCapacity && m_queue.tryPop(entry); ++i)
			{
			}
			m_dirty.store(0, std::memory_order_release);
			for(auto& steps : m_overflowSteps)
				steps.store(0, std::memory_order_release);
			forgetMachine();
		}

		// Audio thread only. Calls _sink(const md::PanelEvent&) for each event.
		template<typename Sink>
		void drain(const DrainContext& _context, Sink&& _sink)
		{
			Output output;
			auto flush = [&]()
			{
				for(size_t i = 0; i < output.count; ++i)
					_sink(output.events[i]);
				output.count = 0;
			};

			if(_context.hardwareEpoch != m_hardwareEpoch)
			{
				// A replaced machine boots with every control released.
				m_hardwareEpoch = _context.hardwareEpoch;
				forgetMachine();
			}
			if(m_resetRequested.exchange(false, std::memory_order_acq_rel))
			{
				m_mapper.releaseAll(output);
				flush();
				forgetMachine();
			}
			for(uint8_t slot = 0; slot < g_slotCount; ++slot)
			{
				if(m_tapReleaseAt[slot] < 0.0 || _context.nowMilliseconds < m_tapReleaseAt[slot])
					continue;
				m_tapReleaseAt[slot] = -1.0;
				setRowControl(slot, false, output);
				flush();
				requestReturnToOff(slot);
			}

			std::array<int, g_encoderCount> budget{};
			budget.fill(g_maxTicksPerDrain);
			// Accepted ticks that did not fit wait while the firmware is not ready.
			for(uint8_t e = 0; e < g_encoderCount && _context.firmwareReady; ++e)
			{
				const auto carried = m_carrySteps[e];
				m_carrySteps[e] = 0;
				emitTicks(e, carried, budget, output, flush);
			}

			Entry entry;
			for(size_t i = 0; i < g_queueCapacity && m_queue.tryPop(entry); ++i)
			{
				const auto withheld = isWithheld(entry.source, _context);
				if(entry.kind == Kind::Turn)
				{
					if(!withheld)
						emitTicks(entry.index, entry.steps, budget, output, flush);
					continue;
				}
				applyRowControl(entry.index, entry.value, entry.source, withheld, _context, output);
				flush();
			}

			auto bits = m_dirty.exchange(0, std::memory_order_acq_rel);
			while(bits != 0)
			{
				const auto slot = static_cast<uint8_t>(countTrailingZeros(bits));
				bits &= bits - 1;
				const auto source = m_latestSource[slot].load(std::memory_order_acquire);
				applyRowControl(slot, m_latest[slot].load(std::memory_order_acquire), source,
					isWithheld(source, _context), _context, output);
				flush();
			}
			for(uint8_t e = 0; e < g_encoderCount; ++e)
			{
				// The origin of overflowed steps is unknown; treat them as gated input.
				const auto steps = m_overflowSteps[e].exchange(0, std::memory_order_acq_rel);
				if(!_context.firmwareReady)
					continue;
				emitTicks(e, steps, budget, output, flush);
			}
		}

	private:
		using Output = md::PanelGestureMapper::Output;

		enum class Kind : uint8_t
		{
			Row,
			Turn
		};

		struct Entry
		{
			Kind kind = Kind::Row;
			uint8_t index = 0;		// slot for rows, md::PanelEncoder for turns
			uint8_t value = 0;
			int16_t steps = 0;
			Source source = Source::Host;
		};

		static constexpr int g_maxQueuedSteps = 1024;
		static constexpr uint8_t g_buttonCount = static_cast<uint8_t>(md::PanelControl::ClassicExtended) + 1;
		static constexpr uint8_t g_encoderCount = static_cast<uint8_t>(md::PanelEncoder::SoundSelection) + 1;
		static constexpr uint8_t g_pushCount = static_cast<uint8_t>(md::PanelEncoder::DataEntryH) + 1;
		static constexpr uint8_t g_firstPushSlot = g_buttonCount;
		static constexpr size_t g_slotCount = g_firstPushSlot + g_pushCount;
		static_assert(g_slotCount <= 64, "one dirty word holds every button");

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

		static bool isWithheld(const Source _source, const DrainContext& _context)
		{
			return _source != Source::Editor && !_context.firmwareReady;
		}

		std::optional<uint8_t> slotFor(const uint8_t _page, const uint8_t _index) const
		{
			namespace pp = md::panelParameters;
			using Mapper = md::PanelGestureMapper;
			if(_page == pp::ButtonPage && _index < g_buttonCount
				&& Mapper::supportsButton(m_model, static_cast<md::PanelControl>(_index)))
				return _index;
			if(_page == pp::EncoderPushPage && _index < g_pushCount
				&& Mapper::supportsEncoderPush(m_model, static_cast<md::PanelEncoder>(_index)))
				return static_cast<uint8_t>(g_firstPushSlot + _index);
			return std::nullopt;
		}

		bool setRowControl(const uint8_t _slot, const bool _pressed, Output& _output)
		{
			if(_slot >= g_firstPushSlot)
				return m_mapper.setEncoderPush(static_cast<md::PanelEncoder>(_slot - g_firstPushSlot), _pressed, _output);
			return m_mapper.setButton(static_cast<md::PanelControl>(_slot), _pressed, _output);
		}

		template<typename Flush>
		void emitTicks(const uint8_t _encoder, int _steps, std::array<int, g_encoderCount>& _budget,
			Output& _output, Flush& _flush)
		{
			const auto encoder = static_cast<md::PanelEncoder>(_encoder);
			while(_steps != 0 && _budget[_encoder] > 0)
			{
				const auto request = std::clamp(_steps, -_budget[_encoder], _budget[_encoder]);
				const auto sent = m_mapper.turnEncoder(encoder, request, _output);
				_flush();
				if(sent == 0)
					break;
				_steps -= sent;
				_budget[_encoder] -= std::abs(sent);
			}
			m_carrySteps[_encoder] = std::clamp(m_carrySteps[_encoder] + _steps, -g_maxQueuedSteps, g_maxQueuedSteps);
		}

		void requestReturnToOff(const uint8_t _slot)
		{
			// The host value is already 0 if the host toggled it back during the tap.
			if(m_lastHostValue[_slot] != 0)
				m_returnToOffRequests.fetch_or(uint64_t{1} << _slot, std::memory_order_acq_rel);
		}

		void forgetMachine()
		{
			m_mapper.reset();
			m_tapReleaseAt.fill(-1.0);
			m_carrySteps.fill(0);
		}

		void applyRowControl(const uint8_t _slot, const uint8_t _value, const Source _source,
			const bool _withheld, const DrainContext& _context, Output& _output)
		{
			if(_source == Source::PanelMidi)
			{
				if(!_withheld)
					setRowControl(_slot, _value != 0, _output);
				return;
			}
			if(_source == Source::Editor)
			{
				// The host echoes the editor's notification; make that a duplicate.
				m_lastHostValue[_slot] = _value;
				setRowControl(_slot, _value != 0, _output);
				return;
			}
			const auto changed = m_lastHostValue[_slot] != _value;
			m_lastHostValue[_slot] = _value;
			const auto tap = m_buttonMode.load(std::memory_order_acquire) == ButtonMode::Tap;
			if(_withheld)
			{
				if(tap && changed && _value != 0)
					requestReturnToOff(_slot);
				return;
			}
			if(!tap)
			{
				setRowControl(_slot, _value != 0, _output);
				return;
			}
			// Only a change to 1 presses; a tap still in progress absorbs a toggle
			// arriving within the hold time.
			if(!changed || _value == 0 || m_tapReleaseAt[_slot] >= 0.0)
				return;
			if(setRowControl(_slot, true, _output))
				m_tapReleaseAt[_slot] = _context.nowMilliseconds + g_tapMilliseconds;
		}

		const md::MachineModel m_model;
		RealtimeQueue<Entry, g_queueCapacity> m_queue;
		std::array<std::atomic<uint8_t>, g_slotCount> m_latest{};
		std::array<std::atomic<Source>, g_slotCount> m_latestSource{};
		std::atomic<uint64_t> m_dirty{0};
		std::array<std::atomic<int32_t>, g_encoderCount> m_overflowSteps{};
		std::atomic<bool> m_resetRequested{false};
		std::atomic<uint64_t> m_returnToOffRequests{0};
		std::atomic<ButtonMode> m_buttonMode{ButtonMode::Momentary};

		// Consumer-owned.
		md::PanelGestureMapper m_mapper;
		uint64_t m_hardwareEpoch = 0;
		std::array<uint8_t, g_slotCount> m_lastHostValue{};
		std::array<int, g_encoderCount> m_carrySteps{};
		std::array<double, g_slotCount> m_tapReleaseAt = makeIdleTaps();

		static std::array<double, g_slotCount> makeIdleTaps()
		{
			std::array<double, g_slotCount> taps{};
			taps.fill(-1.0);
			return taps;
		}
	};
}
