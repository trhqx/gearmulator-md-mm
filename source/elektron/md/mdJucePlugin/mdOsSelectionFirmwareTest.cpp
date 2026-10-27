// OS image selection through the processor: switching reboots the machine with
// the chosen image, carries patch RAM over, rejects foreign images without
// touching the running machine and reports a missing image instead of failing
// silently. Uses a temporary copy of the stock image as the "installed" OS.
#include "mdAutomationTestSupport.h"

#include "mdLib/mdromloader.h"

#include "baseLib/filesystem.h"

#include <iostream>

namespace
{
	using namespace mdAutomationTest;

	std::vector<uint8_t> patchRam(Harness& _harness)
	{
		std::vector<uint8_t> result;
		_harness.processor.getPlugin().withDeviceLocked([&result](synthLib::Device* const _device)
		{
			if(auto* const device = dynamic_cast<md::Device*>(_device))
				result = device->getHardware().copyPatchRam();
		});
		return result;
	}

	std::string romName(Harness& _harness)
	{
		std::string result;
		_harness.processor.getPlugin().withDeviceLocked([&result](synthLib::Device* const _device)
		{
			if(auto* const device = dynamic_cast<md::Device*>(_device))
				result = device->getHardware().getRom().getFilename();
		});
		return result;
	}

	void boot(Harness& _harness, const char* _what)
	{
		for(int block = 0; block < 40 * 48000 / BlockSize && !firmwareMidiReady(_harness); ++block)
			_harness.process(1);
		require(firmwareMidiReady(_harness), std::string("the machine did not boot ") + _what);
	}

	bool verifyModel(const md::MachineModel _model)
	{
		Harness harness(_model);
		if(!harness.hasLocalFirmware())
			return allowMissingFirmware("mdOsSelectionFirmwareTest", _model);
		harness.prepare();
		boot(harness, "with the stock OS");
		auto& processor = harness.processor;
		require(processor.getRunningOsDescription() == "Stock OS", "stock OS not reported");

		const auto stock = md::RomLoader::findROM(_model);
		require(stock.isValid(), "stock image not found");
		const auto folder = juce::File::getSpecialLocation(juce::File::tempDirectory)
			.getChildFile("mdOsSelectionFirmwareTest").getNonexistentSibling();
		require(folder.createDirectory().wasOk(), "temporary folder");
		struct Cleanup { juce::File f; ~Cleanup() { f.deleteRecursively(); } } cleanup{folder};

		const auto installed = folder.getChildFile("Installed OS.bin");
		require(baseLib::filesystem::writeFile(installed.getFullPathName().toStdString(), stock.data()),
			"write installed image");
		const auto foreign = folder.getChildFile("Foreign.bin");
		require(baseLib::filesystem::writeFile(foreign.getFullPathName().toStdString(),
			std::vector<uint8_t>(md::g_romSize, 0xff)), "write foreign image");

		// A foreign image is rejected and the running machine stays untouched.
		juce::String result;
		require(!processor.selectOsImage(foreign, result), "foreign image accepted");
		require(processor.getSelectedOsImage() == juce::File(), "rejected image was selected");
		require(firmwareMidiReady(harness), "rejecting an image disturbed the machine");

		// Switching keeps patch RAM, like battery-backed RAM on the hardware.
		const auto ram = patchRam(harness);
		require(!ram.empty(), "patch RAM unavailable");
		require(processor.selectOsImage(installed, result), "switching failed: " + result.toStdString());
		require(patchRam(harness) == ram, "patch RAM was not carried over");
		require(processor.getRunningOsDescription() == "Installed OS", "installed OS not reported: "
			+ processor.getRunningOsDescription());
		require(romName(harness) == installed.getFullPathName().toStdString(), "machine does not run the selected image");
		boot(harness, "with the installed OS");

		// A selected image that disappeared is reported, and the stock OS runs.
		require(installed.deleteFile(), "delete installed image");
		require(processor.rebootDevice(), "reboot with a missing image failed");
		require(processor.getRunningOsDescription().find("was not used") != std::string::npos,
			"missing image not reported: " + processor.getRunningOsDescription());
		boot(harness, "after its OS image disappeared");

		require(processor.selectOsImage({}, result), "switching back to the stock OS failed");
		require(processor.getRunningOsDescription() == "Stock OS", "stock OS not reported after switching back");
		boot(harness, "after switching back to the stock OS");

		std::cout << "mdOsSelectionFirmwareTest: " << (_model == md::MachineModel::Monomachine ? "MM" : "MD")
			<< " PASS\n";
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
		std::cerr << "mdOsSelectionFirmwareTest: " << _error.what() << '\n';
		return 1;
	}
}
