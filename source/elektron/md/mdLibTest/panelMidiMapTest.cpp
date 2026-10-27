#include "mdLib/mdpanelmidimap.h"

#include <cstdlib>
#include <iostream>
#include <optional>

namespace
{
	using md::MachineModel;
	using md::PanelControl;
	using md::PanelEncoder;
	using md::PanelMidiAction;
	using md::PanelMidiEncoding;
	using md::PanelMidiMap;
	using md::PanelMidiTarget;
	using md::PanelMidiTranslator;
	using md::PanelMidiTrigger;

	constexpr uint8_t g_channel = 15;	// MIDI channel 16
	constexpr uint8_t g_noteOn = 0x90 | g_channel;
	constexpr uint8_t g_noteOff = 0x80 | g_channel;
	constexpr uint8_t g_cc = 0xb0 | g_channel;

	void expect(const bool _condition, const char* const _message)
	{
		if(_condition)
			return;
		std::cerr << "mdPanelMidiMapTest: " << _message << '\n';
		std::exit(1);
	}

	PanelMidiTarget button(const PanelControl _control) { return PanelMidiTarget::button(_control); }
	PanelMidiTarget push(const PanelEncoder _encoder) { return PanelMidiTarget::encoderPush(_encoder); }
	PanelMidiTarget encoder(const PanelEncoder _encoder) { return PanelMidiTarget::encoder(_encoder); }

	PanelMidiMap mapOnChannel()
	{
		PanelMidiMap map;
		map.channel = g_channel;
		return map;
	}

	std::optional<PanelMidiAction> action(PanelMidiTranslator& _translator, const uint8_t _a, const uint8_t _b,
		const uint8_t _c)
	{
		return _translator.translate(_a, _b, _c).action;
	}

	void checkChannelFilter()
	{
		auto map = mapOnChannel();
		const MachineModel model = MachineModel::Machinedrum;
		expect(map.bind(PanelMidiTrigger::note(60), button(PanelControl::Play), model), "bind Play");
		PanelMidiTranslator translator(model);
		translator.setMap(map);
		expect(translator.translate(0x90, 60, 100).consumed == false, "another channel was consumed");
		expect(translator.translate(0xf8, 0, 0).consumed == false, "a realtime message was consumed");
		expect(translator.translate(g_noteOn, 61, 100).consumed, "an unbound note on the panel channel leaked");
		expect(!translator.translate(g_noteOn, 61, 100).action, "an unbound note produced an action");
		expect(translator.translate(0xe0 | g_channel, 0, 64).consumed, "pitch bend on the panel channel leaked");

		map.channel = PanelMidiMap::g_off;
		translator.setMap(map);
		expect(!translator.translate(g_noteOn, 60, 100).consumed, "a disabled panel channel consumed input");
		expect(!md::isPanelMidiMessage(map.channel, g_noteOn), "a disabled channel matched");
		expect(md::isPanelMidiMessage(g_channel, g_cc) && !md::isPanelMidiMessage(g_channel, 0xb0),
			"channel test");
	}

	void checkButtons()
	{
		const MachineModel model = MachineModel::Machinedrum;
		auto map = mapOnChannel();
		expect(map.bind(PanelMidiTrigger::note(36), button(PanelControl::Trigger1), model), "bind note");
		expect(map.bind(PanelMidiTrigger::controlChange(80), button(PanelControl::Function), model), "bind cc");
		expect(map.bind(PanelMidiTrigger::controlChange(81), push(PanelEncoder::DataEntryB), model), "bind push");
		PanelMidiTranslator translator(model);
		translator.setMap(map);

		const auto press = PanelMidiAction{button(PanelControl::Trigger1), 1};
		const auto release = PanelMidiAction{button(PanelControl::Trigger1), 0};
		expect(action(translator, g_noteOn, 36, 1) == press, "note-on did not press");
		expect(action(translator, g_noteOff, 36, 64) == release, "note-off did not release");
		expect(action(translator, g_noteOn, 36, 0) == release, "note-on with velocity 0 did not release");
		expect(action(translator, g_cc, 80, 127) == PanelMidiAction{button(PanelControl::Function), 1}, "cc press");
		expect(action(translator, g_cc, 80, 0) == PanelMidiAction{button(PanelControl::Function), 0}, "cc release");
		expect(action(translator, g_cc, 80, 64) == PanelMidiAction{button(PanelControl::Function), 1},
			"cc 64 is pressed");
		expect(action(translator, g_cc, 81, 100) == PanelMidiAction{push(PanelEncoder::DataEntryB), 1},
			"encoder push via cc");
	}

	void checkRelativeEncodings()
	{
		const MachineModel model = MachineModel::Monomachine;
		auto map = mapOnChannel();
		expect(map.bind(PanelMidiTrigger::controlChange(16), encoder(PanelEncoder::DataEntryA), model), "bind");
		PanelMidiTranslator translator(model);
		const auto steps = [&](const PanelMidiEncoding _encoding, const uint8_t _value) -> int
		{
			map.encoding = _encoding;
			translator.setMap(map);
			const auto result = action(translator, g_cc, 16, _value);
			expect(!result || result->target == encoder(PanelEncoder::DataEntryA), "wrong encoder");
			return result ? result->value : 0;
		};
		expect(steps(PanelMidiEncoding::TwosComplement, 1) == 1, "2's complement +1");
		expect(steps(PanelMidiEncoding::TwosComplement, 5) == 5, "2's complement +5");
		expect(steps(PanelMidiEncoding::TwosComplement, 127) == -1, "2's complement -1");
		expect(steps(PanelMidiEncoding::TwosComplement, 123) == -5, "2's complement -5");
		expect(steps(PanelMidiEncoding::BinaryOffset, 65) == 1, "binary offset +1");
		expect(steps(PanelMidiEncoding::BinaryOffset, 63) == -1, "binary offset -1");
		expect(steps(PanelMidiEncoding::BinaryOffset, 64) == 0, "binary offset 64 is no movement");
		expect(steps(PanelMidiEncoding::SignMagnitude, 3) == 3, "sign magnitude +3");
		expect(steps(PanelMidiEncoding::SignMagnitude, 67) == -3, "sign magnitude -3");
		expect(steps(PanelMidiEncoding::TwosComplement, 0) == 0, "2's complement 0 is no movement");
	}

	void checkAbsoluteKnobs()
	{
		// A non-endless knob: the change since the last value turns the encoder.
		const MachineModel model = MachineModel::Machinedrum;
		auto map = mapOnChannel();
		map.encoding = PanelMidiEncoding::Absolute;
		expect(map.bind(PanelMidiTrigger::controlChange(20), encoder(PanelEncoder::Level), model), "bind");
		PanelMidiTranslator translator(model);
		translator.setMap(map);
		expect(!action(translator, g_cc, 20, 70), "first absolute value turned the encoder");
		expect(action(translator, g_cc, 20, 73) == PanelMidiAction{encoder(PanelEncoder::Level), 3}, "+3");
		expect(action(translator, g_cc, 20, 60) == PanelMidiAction{encoder(PanelEncoder::Level), -13}, "-13");
		translator.setMap(map);
		expect(!action(translator, g_cc, 20, 0), "a new map kept the old reference value");
	}

	void checkBindingRules()
	{
		const MachineModel model = MachineModel::Monomachine;
		PanelMidiMap map = mapOnChannel();
		expect(!map.bind(PanelMidiTrigger::note(40), encoder(PanelEncoder::DataEntryA), model),
			"an encoder was bound to a note");
		expect(!map.bind(PanelMidiTrigger::controlChange(10), encoder(PanelEncoder::SoundSelection), model),
			"an encoder the model lacks was bound");
		expect(!map.bind(PanelMidiTrigger::controlChange(200), button(PanelControl::Play), model),
			"an invalid controller number was bound");

		expect(map.bind(PanelMidiTrigger::note(40), button(PanelControl::Play), model), "bind Play");
		expect(map.bind(PanelMidiTrigger::note(41), button(PanelControl::Play), model), "rebind Play");
		expect(map.find(PanelMidiTrigger::note(40)) == PanelMidiTarget{}, "a rebound control kept its old trigger");
		expect(map.findTrigger(button(PanelControl::Play)) == PanelMidiTrigger::note(41), "trigger lookup");

		expect(map.bind(PanelMidiTrigger::note(41), button(PanelControl::Stop), model), "steal note 41");
		expect(!map.findTrigger(button(PanelControl::Play)), "a stolen trigger still drives its old control");
		map.unbind(button(PanelControl::Stop));
		expect(map.find(PanelMidiTrigger::note(41)) == PanelMidiTarget{}, "unbind");
	}

	void checkLearn()
	{
		const MachineModel model = MachineModel::Machinedrum;
		PanelMidiTranslator translator(model);
		translator.setMap(mapOnChannel());

		translator.learn(encoder(PanelEncoder::DataEntryC));
		expect(translator.isLearning(), "learn did not start");
		auto result = translator.translate(0xb0, 30, 1);
		expect(!result.consumed && translator.isLearning(), "learn took a message from another channel");
		result = translator.translate(0xe0 | g_channel, 0, 64);
		expect(result.consumed && !result.learned && translator.isLearning(), "pitch bend was learned");
		result = translator.translate(g_cc, 30, 1);
		expect(result.consumed && !result.action, "the learning message also turned the encoder");
		expect(result.learned && result.learned->trigger == PanelMidiTrigger::controlChange(30)
			&& result.learned->target == encoder(PanelEncoder::DataEntryC), "learn result");
		expect(!translator.isLearning(), "learn did not finish");
		expect(action(translator, g_cc, 30, 2) == PanelMidiAction{encoder(PanelEncoder::DataEntryC), 2},
			"the learned binding was not used right away");

		translator.learn(button(PanelControl::Record));
		result = translator.translate(g_noteOff, 52, 0);
		expect(!result.learned && translator.isLearning(), "a note-off was learned");
		result = translator.translate(g_noteOn, 52, 0);
		expect(!result.learned && translator.isLearning(), "a note-on with velocity 0 was learned");
		result = translator.translate(g_noteOn, 52, 90);
		expect(result.learned && result.learned->trigger == PanelMidiTrigger::note(52), "button learn");
		expect(action(translator, g_noteOff, 52, 0) == PanelMidiAction{button(PanelControl::Record), 0},
			"the release of the learning note was dropped");

		// Learning an encoder with a note binds its push switch.
		translator.learn(encoder(PanelEncoder::DataEntryD));
		result = translator.translate(g_noteOn, 53, 90);
		expect(result.learned && result.learned->target == push(PanelEncoder::DataEntryD)
			&& result.learned->trigger == PanelMidiTrigger::note(53), "a note learned on an encoder");
		translator.learn(encoder(PanelEncoder::Level));
		result = translator.translate(g_noteOn, 54, 90);
		expect(!result.learned && translator.isLearning(), "a note was learned for an encoder without a push");

		translator.learn(button(PanelControl::Stop));
		translator.cancelLearn();
		expect(!translator.isLearning(), "cancel");
	}

	void checkTextRoundTrip()
	{
		const MachineModel model = MachineModel::Monomachine;
		auto map = mapOnChannel();
		map.encoding = PanelMidiEncoding::SignMagnitude;
		map.bind(PanelMidiTrigger::note(60), button(PanelControl::Play), model);
		map.bind(PanelMidiTrigger::controlChange(16), encoder(PanelEncoder::DataEntryA), model);
		map.bind(PanelMidiTrigger::controlChange(17), push(PanelEncoder::DataEntryH), model);
		const auto text = map.toText();
		const auto parsed = PanelMidiMap::fromText(text, model);
		expect(parsed == map, "text round trip");
		expect(text.find("button Play note 60") != std::string::npos, "readable button line");
		expect(text.find("channel 16") != std::string::npos, "channel is written one-based");

		const auto lenient = PanelMidiMap::fromText(
			"channel off\nencoding binary-offset\nbutton NoSuchButton note 1\nencoder DataEntryB note 3\n"
			"garbage\nbutton Stop cc 99\n", model);
		expect(lenient.channel == PanelMidiMap::g_off, "channel off");
		expect(lenient.encoding == PanelMidiEncoding::BinaryOffset, "encoding");
		expect(lenient.find(PanelMidiTrigger::controlChange(99)) == button(PanelControl::Stop),
			"a valid line after invalid ones was lost");
		expect(lenient.find(PanelMidiTrigger::note(3)) == PanelMidiTarget{}, "an invalid binding was loaded");
		expect(PanelMidiMap::fromText("", model) == PanelMidiMap{}, "empty text");
	}
}

int main()
{
	checkChannelFilter();
	checkButtons();
	checkRelativeEncodings();
	checkAbsoluteKnobs();
	checkBindingRules();
	checkLearn();
	checkTextRoundTrip();
	std::cout << "mdPanelMidiMapTest passed\n";
	return 0;
}
