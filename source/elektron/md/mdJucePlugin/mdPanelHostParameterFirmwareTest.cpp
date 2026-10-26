// Firmware-backed check that front-panel host parameters drive the emulated panel
// without an editor (doc/mdmm_panel_host_parameters.md).

#include "mdAutomationTestSupport.h"

#include "mdLib/mdfrontpanel.h"

#include <iostream>
#include <string>
#include <vector>

namespace
{
	using namespace mdAutomationTest;

	pluginLib::Parameter& panelParameter(mdJucePlugin::Controller& _controller, const std::string& _name)
	{
		auto* const result = _controller.getParameter(_name, 0);
		require(result != nullptr, _name + " was not registered");
		return *result;
	}

	md::FrontPanel frontPanel(Harness& _harness)
	{
		md::FrontPanel panel;
		_harness.processor.getPlugin().withDeviceLocked([&panel](synthLib::Device* const _device)
		{
			if(auto* const device = dynamic_cast<md::Device*>(_device))
				panel = device->getFrontPanelSnapshot();
		});
		return panel;
	}

	uint64_t lcdFingerprint(const md::FrontPanel& _panel)
	{
		uint64_t hash = 14695981039346656037ull;
		for(uint32_t half = 0; half < 2; ++half)
			for(uint32_t page = 0; page < 8; ++page)
				for(uint32_t column = 0; column < 64; ++column)
				{
					hash ^= _panel.getLcdVram(half, page, column);
					hash *= 1099511628211ull;
				}
		return hash;
	}

	void tap(Harness& _harness, pluginLib::Parameter& _button)
	{
		hostWrite(_button, 1);
		_harness.process(16);
		hostWrite(_button, 0);
		_harness.process(64);
	}

	void verifyRecordButton(Harness& _harness)
	{
		auto& controller = _harness.controller;
		auto& record = panelParameter(controller, "PanelRecord");
		const auto initial = frontPanel(_harness).getModeLed(md::FrontPanel::ModeLed::Record);

		const auto beforeTap = controller.getDeliveredPanelEventCount();
		tap(_harness, record);
		require(controller.getDeliveredPanelEventCount() == beforeTap + 2,
			"a host press and release did not deliver exactly two panel events");
		require(frontPanel(_harness).getModeLed(md::FrontPanel::ModeLed::Record) != initial,
			"host-parameter RECORD press did not toggle the Record LED");

		const auto beforeDuplicate = controller.getDeliveredPanelEventCount();
		hostWrite(record, 0);
		_harness.process(16);
		require(controller.getDeliveredPanelEventCount() == beforeDuplicate,
			"a repeated release reached the panel");

		tap(_harness, record);
		require(frontPanel(_harness).getModeLed(md::FrontPanel::ModeLed::Record) == initial,
			"second host-parameter RECORD press did not toggle the LED back");
	}

	// Ableton Live toggles a note-mapped on/off parameter on each note-on, so in
	// tap mode every value change must be exactly one press of the button.
	void verifyTapMode(Harness& _harness)
	{
		auto& controller = _harness.controller;
		auto& record = panelParameter(controller, "PanelRecord");
		controller.setHostButtonsTap(true);
		const auto initial = frontPanel(_harness).getModeLed(md::FrontPanel::ModeLed::Record);

		hostWrite(record, 1);
		_harness.process(64);
		require(frontPanel(_harness).getModeLed(md::FrontPanel::ModeLed::Record) != initial,
			"a toggled-on parameter did not press RECORD in tap mode");
		hostWrite(record, 0);
		_harness.process(64);
		require(frontPanel(_harness).getModeLed(md::FrontPanel::ModeLed::Record) == initial,
			"a toggled-off parameter did not press RECORD again in tap mode");
		controller.setHostButtonsTap(false);
	}

	void verifyEncoder(Harness& _harness)
	{
		auto& controller = _harness.controller;
		auto& encoder = panelParameter(controller, "PanelEncoderDataEntryA");
		const auto before = lcdFingerprint(frontPanel(_harness));
		const auto beforeTicks = controller.getDeliveredPanelEventCount();

		// Turn up, then down past the start, so a parameter at either end of its
		// range still produces a visible change.
		hostWrite(encoder, 70);
		_harness.process(64);
		const auto afterUp = lcdFingerprint(frontPanel(_harness));
		hostWrite(encoder, 58);
		_harness.process(64);
		const auto afterDown = lcdFingerprint(frontPanel(_harness));

		require(controller.getDeliveredPanelEventCount() == beforeTicks + 6 + 8,
			"encoder positions did not become the expected capped tick count");
		require(afterUp != before || afterDown != afterUp,
			"host-parameter encoder turns did not change the LCD");
	}

	void verifyStateLoadReleasesHeldControls(Harness& _harness)
	{
		auto& controller = _harness.controller;
		auto& function = panelParameter(controller, "PanelFunction");
		hostWrite(function, 1);
		_harness.process(16);

		// Gestures are never part of a project; loading one releases them.
		juce::MemoryBlock state;
		_harness.audioProcessor.getStateInformation(state);
		_harness.audioProcessor.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
		require(_harness.synchronize(), "state restore did not resynchronize");
		_harness.process(64);
		require(function.getUnnormalizedValue() == 0, "state load left a panel button held");

		const auto before = controller.getDeliveredPanelEventCount();
		hostWrite(function, 0);
		_harness.process(16);
		require(controller.getDeliveredPanelEventCount() == before,
			"release after a state load reached the panel");
	}

	std::vector<pluginLib::Parameter*> panelParameters(Harness& _harness)
	{
		std::vector<pluginLib::Parameter*> result;
		for(auto* const audioParameter : _harness.audioProcessor.getParameters())
			if(auto* const parameter = dynamic_cast<pluginLib::Parameter*>(audioParameter))
				if(md::panelParameters::isPanelPage(parameter->getDescription().page))
					result.push_back(parameter);
		return result;
	}

	bool verifyModel(const md::MachineModel _model)
	{
		Harness harness(_model);
		if(!harness.hasLocalFirmware())
			return allowMissingFirmware("mdPanelHostParameterFirmwareTest", _model);
		harness.prepare();
		harness.controller.setHostButtonsTap(false);
		// auval and other validators set every parameter right after loading.
		// Held from power-on, panel keys would open the startup menu (test mode,
		// resets, OS upgrade) and the machine would never boot normally.
		for(auto* const parameter : panelParameters(harness))
			hostWrite(*parameter, parameter->getDescription().range.getEnd());
		require(harness.synchronize(), "the machine did not boot with every panel parameter held ("
			+ harness.firmwareReadiness() + ")");
		for(auto* const parameter : panelParameters(harness))
			hostWrite(*parameter, parameter->getDescription().range.getStart());
		harness.process(16);
		// MIDI synchronization completes while the firmware still shows its startup
		// screens, which ignore panel input. Give the UI the same boot time as the
		// mdLib panel firmware tests before pressing anything.
		harness.process(25 * 48000 / BlockSize);

		if(_model == md::MachineModel::Machinedrum)
		{
			verifyRecordButton(harness);
			verifyTapMode(harness);
		}
		verifyEncoder(harness);
		verifyStateLoadReleasesHeldControls(harness);

		std::cout << "mdPanelHostParameterFirmwareTest: " << modelName(_model) << " PASS\n";
		return true;
	}
}

int main()
{
	juce::ScopedJuceInitialiser_GUI juce;
	try
	{
		const auto ranMd = verifyModel(md::MachineModel::Machinedrum);
		const auto ranMm = verifyModel(md::MachineModel::Monomachine);
		return ranMd || ranMm ? 0 : SkipReturnCode;
	}
	catch(const std::exception& _error)
	{
		std::cerr << "mdPanelHostParameterFirmwareTest: " << _error.what() << '\n';
		return 1;
	}
}
