#include "jucePluginLib/parameterdescriptions.h"
#include "mdLib/mdautomation.h"
#include "mdLib/mdpanelgesturemapper.h"
#include "mdLib/mdpanelparameters.h"
#include "mdRealtimeQueue.h"

#include <array>
#include <atomic>
#include <cstdlib>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace
{
	void require(const bool _condition, const std::string& _message)
	{
		if(_condition)
			return;
		std::cerr << "mdAutomationParameterTest: " << _message << '\n';
		std::exit(1);
	}

	pluginLib::ParameterDescriptions load(const char* const _filename)
	{
		const std::string path = std::string(MD_AUTOMATION_PARAMETER_DIR) + "/" + _filename;
		std::ifstream file(path, std::ios::binary);
		require(file.good(), "could not open " + path);
		return pluginLib::ParameterDescriptions(
			std::string(std::istreambuf_iterator<char>(file), {}));
	}

	void hashByte(uint64_t& _hash, const uint8_t _value)
	{
		_hash ^= _value;
		_hash *= 1099511628211ull;
	}

	void hashInteger(uint64_t& _hash, const int64_t _value)
	{
		for(unsigned int shift = 0; shift < 64; shift += 8)
			hashByte(_hash, static_cast<uint8_t>(
				static_cast<uint64_t>(_value) >> shift));
	}

	void hashString(uint64_t& _hash, const std::string& _value)
	{
		hashInteger(_hash, static_cast<int64_t>(_value.size()));
		for(const auto character : _value)
			hashByte(_hash, static_cast<uint8_t>(character));
	}

	uint64_t hostContractFingerprint(
		const pluginLib::ParameterDescriptions& _descriptions,
		const uint8_t _trackCount)
	{
		uint64_t hash = 14695981039346656037ull;
		size_t hostIndex = 0;
		for(uint8_t track = 0; track < _trackCount; ++track)
		{
			for(const auto& description : _descriptions.getDescriptions())
			{
				// Panel parameters are global and live in their own version group;
				// they are verified separately so this contract stays untouched.
				if(md::panelParameters::isPanelPage(description.page))
					continue;
				// This mirrors Parameter::genId() and Controller::registerParams(). Any
				// change here is a DAW project/automation compatibility decision.
				const auto id = std::to_string(description.page) + "_"
					+ std::to_string(track) + "_"
					+ std::to_string(description.index);
				const auto name = "Track " + std::to_string(track + 1) + " "
					+ description.displayName;
				hashInteger(hash, static_cast<int64_t>(hostIndex++));
				hashString(hash, id);
				hashInteger(hash, description.version);
				hashString(hash, name);
				hashInteger(hash, description.range.getStart());
				hashInteger(hash, description.range.getEnd());
				hashInteger(hash,
					description.defaultValue == pluginLib::Description::NoDefaultValue
						? 0 : description.defaultValue);
				hashInteger(hash, description.isDiscrete);
				hashInteger(hash, description.isBool);
				hashInteger(hash, description.isBipolar);
				hashInteger(hash, description.step);
			}
		}
		return hash;
	}

	const pluginLib::Description* findDescription(
		const pluginLib::ParameterDescriptions& _descriptions, const uint8_t _page,
		const uint8_t _index)
	{
		for(const auto& description : _descriptions.getDescriptions())
			if(description.page == _page && description.index == _index)
				return &description;
		return nullptr;
	}

	void requirePanelParameter(const pluginLib::Description* const _description,
		const bool _supported, const bool _isBool, const std::string& _what)
	{
		if(!_supported)
		{
			require(_description == nullptr, _what + " has a parameter but no verified panel packet");
			return;
		}
		require(_description != nullptr, _what + " is verified but has no parameter");
		require(_description->isNonPartSensitive(), _what + " must be a single global parameter");
		require(_description->version == md::panelParameters::Version,
			_what + " must be registered in the panel version group");
		require(_description->isPublic, _what + " must be visible to the host");
		require(_description->isDiscrete, _what + " must be discrete");
		require(_description->isBool == _isBool, _what + " has the wrong bool flag");
		require(_description->range.getStart() == 0, _what + " must start at 0");
		require(_description->range.getEnd() == 1, _what + " must be 0/1");
		require(_description->defaultValue == 0, _what + " must default to released");
		require(_description->displayName.rfind("Panel ", 0) != 0,
			_what + " display name must not repeat the Panel prefix");
	}

	void verifyPanel(const char* const _filename, const md::MachineModel _model,
		const pluginLib::ParameterDescriptions& _descriptions,
		const size_t _expectedPanelParameterCount)
	{
		using md::PanelControl;
		using md::PanelEncoder;
		using Mapper = md::PanelGestureMapper;
		namespace pp = md::panelParameters;

		size_t expected = 0;
		for(uint8_t c = 0; c <= static_cast<uint8_t>(PanelControl::ClassicExtended); ++c)
		{
			const auto control = static_cast<PanelControl>(c);
			const auto supported = Mapper::supportsButton(_model, control);
			expected += supported ? 1 : 0;
			requirePanelParameter(findDescription(_descriptions, pp::ButtonPage, c), supported, true,
				std::string(_filename) + " button " + md::panelControlName(control));
		}
		for(uint8_t e = 0; e <= static_cast<uint8_t>(PanelEncoder::SoundSelection); ++e)
		{
			const auto encoder = static_cast<PanelEncoder>(e);
			const auto push = Mapper::supportsEncoderPush(_model, encoder);
			expected += push ? 1 : 0;
			// Encoders have no host parameter: a relative control mapped to an
			// absolute host value has to be recentred, and every recentre would show
			// up in the host's automation. Their effect is recorded on the real track
			// parameters the firmware reports instead.
			requirePanelParameter(findDescription(_descriptions, pp::EncoderPage, e), false, false,
				std::string(_filename) + " encoder " + md::panelEncoderName(encoder));
			requirePanelParameter(findDescription(_descriptions, pp::EncoderPushPage, e), push, true,
				std::string(_filename) + " encoder push " + md::panelEncoderName(encoder));
		}

		size_t panelCount = 0;
		std::set<std::string> displayNames;
		for(const auto& description : _descriptions.getDescriptions())
		{
			if(!pp::isPanelPage(description.page))
				continue;
			++panelCount;
			require(displayNames.insert(description.displayName).second,
				std::string(_filename) + " duplicate panel display name " + description.displayName);
		}
		require(panelCount == expected, std::string(_filename) + " has unexpected panel parameters");
		if(panelCount != _expectedPanelParameterCount)
			std::cerr << _filename << " panel parameter count is " << panelCount << '\n';
		require(panelCount == _expectedPanelParameterCount,
			"panel parameter count changed; this is a host compatibility decision");
	}

	void verify(const char* const _filename, const md::MachineModel _model,
		const size_t _trackParameterCount, const uint8_t _trackCount,
		const size_t _expectedHostParameterCount,
		const uint64_t _expectedHostContractFingerprint,
		const size_t _expectedPanelParameterCount)
	{
		const auto descriptions = load(_filename);
		require(descriptions.isValid(), descriptions.getErrors());

		size_t trackDescriptionCount = 0;
		std::set<std::pair<uint8_t, uint8_t>> addresses;
		for(const auto& description : descriptions.getDescriptions())
		{
			if(md::panelParameters::isPanelPage(description.page))
				continue;
			++trackDescriptionCount;
			require(!description.isNonPartSensitive(),
				"automation parameters must belong to firmware tracks");

			if(_model == md::MachineModel::Monomachine
				&& description.page == md::automation::monomachine::Effects)
			{
				if(description.index == 3)
					require(description.name == "EffectsDelayTime",
						"MM CC 83 must be labelled Delay Time");
				if(description.index == 4)
					require(description.name == "EffectsDelaySend",
						"MM CC 84 must be labelled Delay Send");
			}

			require(addresses.emplace(description.page, description.index).second,
				"duplicate track parameter address");
			for(uint8_t track = 0; track < _trackCount; ++track)
			{
				const md::automation::ParameterChange change{
					description.page, track, description.index, 0};
				require(md::automation::encodeParameterChange(_model, change, 0).has_value(),
					"host parameter has no MIDI mapping");
			}
		}

		require(trackDescriptionCount == _trackParameterCount,
			std::string(_filename) + " has the wrong track description count");
		require(_trackParameterCount * _trackCount
			== _expectedHostParameterCount, "host parameter count changed");
		const auto fingerprint = hostContractFingerprint(descriptions, _trackCount);
		if(fingerprint != _expectedHostContractFingerprint)
		{
			std::cerr << _filename << " host contract fingerprint is "
				<< fingerprint << '\n';
			require(false, "host parameter IDs/order/names/ranges/defaults changed");
		}
		verifyPanel(_filename, _model, descriptions, _expectedPanelParameterCount);
	}

	void verifyRealtimeQueue()
	{
		mdJucePlugin::RealtimeQueue<uint32_t, 8> bounded;
		for(uint32_t value = 0; value < 8; ++value)
			require(bounded.tryPush(value), "bounded queue filled too early");
		require(!bounded.tryPush(8), "bounded queue accepted an over-capacity item");
		for(uint32_t value = 0; value < 8; ++value)
		{
			uint32_t observed = 99;
			require(bounded.tryPop(observed) && observed == value,
				"bounded queue changed FIFO order");
		}
		uint32_t empty = 0;
		require(!bounded.tryPop(empty), "bounded queue popped from empty");

		constexpr uint32_t ProducerCount = 4;
		constexpr uint32_t ItemsPerProducer = 2000;
		constexpr uint32_t ItemCount = ProducerCount * ItemsPerProducer;
		mdJucePlugin::RealtimeQueue<uint32_t, 1024> concurrent;
		std::array<std::atomic<uint8_t>, ItemCount> seen{};
		std::array<std::thread, ProducerCount> producers;
		for(uint32_t producer = 0; producer < ProducerCount; ++producer)
		{
			producers[producer] = std::thread([&, producer]
			{
				for(uint32_t ordinal = 0; ordinal < ItemsPerProducer; ++ordinal)
				{
					const auto value = producer * ItemsPerProducer + ordinal;
					while(!concurrent.tryPush(value))
						std::this_thread::yield();
				}
			});
		}
		for(uint32_t count = 0; count < ItemCount;)
		{
			uint32_t value = 0;
			if(!concurrent.tryPop(value))
			{
				std::this_thread::yield();
				continue;
			}
			require(value < ItemCount, "concurrent queue returned invalid data");
			require(seen[value].fetch_add(1, std::memory_order_relaxed) == 0,
				"concurrent queue returned an item twice");
			++count;
		}
		for(auto& producer : producers)
			producer.join();
		for(const auto& count : seen)
			require(count.load(std::memory_order_relaxed) == 1,
				"concurrent queue lost an item");
	}
}

int main()
{
	verifyRealtimeQueue();
	verify("parameterDescriptions_md.json", md::MachineModel::Machinedrum,
		26, md::automation::machinedrum::TrackCount, 416, 6612241820543455307ull, 45);
	verify("parameterDescriptions_mm.json", md::MachineModel::Monomachine,
		58, md::automation::monomachine::TrackCount, 348, 15680169437425968391ull, 52);
	std::cout << "mdAutomationParameterTest: PASS\n";
	return 0;
}
