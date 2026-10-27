// Installs an OS upgrade .syx through the emulated bootloader (EARLY STARTUP
// MENU > MIDI UPGRADE) and checks the resulting image. Needs vendor files that
// are never part of the repository:
//   GEARMULATOR_MD_FIRMWARE_BIN + GEARMULATOR_MD_OS_SYX
//   GEARMULATOR_MM_FIRMWARE_BIN + GEARMULATOR_MM_OS_SYX
// Skips (77) when neither pair is set.
#include "mdLib/mdosinstaller.h"
#include "mdLib/mdromloader.h"
#include "baseLib/filesystem.h"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace
{
	using md::MachineModel;

	const char* phaseName(const md::OsInstaller::Phase _phase)
	{
		switch(_phase)
		{
		case md::OsInstaller::Phase::Booting: return "booting into EARLY STARTUP MENU";
		case md::OsInstaller::Phase::Sending: return "sending";
		case md::OsInstaller::Phase::Flashing: return "bootloader programming flash";
		case md::OsInstaller::Phase::Verifying: return "verifying boot";
		}
		return "?";
	}

	// 0 = pass, 1 = fail, 77 = not configured
	int run(const MachineModel _model, const char* _binVar, const char* _syxVar)
	{
		const char* name = _model == MachineModel::Monomachine ? "MM" : "MD";
		const char* binPath = std::getenv(_binVar);
		const char* syxPath = std::getenv(_syxVar);
		if(!binPath || !*binPath || !syxPath || !*syxPath)
		{
			std::cout << name << ": SKIP (" << _binVar << " / " << _syxVar << " not set)\n";
			return 77;
		}

		std::vector<uint8_t> base, syx;
		if(!baseLib::filesystem::readFile(base, binPath) || !baseLib::filesystem::readFile(syx, syxPath))
		{
			std::cerr << name << ": FAIL could not read the firmware files\n";
			return 1;
		}
		if(!md::RomLoader::isRomForModel(base, _model))
		{
			std::cerr << name << ": FAIL base image is not the stock image\n";
			return 1;
		}

		md::OsInstaller::Options options;
		if(const char* gap = std::getenv("GEARMULATOR_OS_INSTALL_GAP_MS"))
			options.messageGapMs = std::atof(gap);

		const auto start = std::chrono::steady_clock::now();
		auto lastPhase = static_cast<md::OsInstaller::Phase>(0xff);
		size_t lastReported = 0;
		const auto result = md::OsInstaller::install(base, _model, syx, options,
			[&](const md::OsInstaller::Progress& _p)
			{
				if(_p.phase != lastPhase || _p.sentMessages >= lastReported + 2000)
				{
					lastPhase = _p.phase;
					lastReported = _p.sentMessages;
					const auto s = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
					std::cout << name << ": " << static_cast<int>(s) << "s " << phaseName(_p.phase)
						<< " " << _p.sentMessages << "/" << _p.totalMessages << std::endl;
				}
			});
		const auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

		if(result.status != md::OsInstaller::Status::Ok)
		{
			std::cerr << name << ": FAIL " << result.error << '\n';
			return 1;
		}
		if(result.image.size() != base.size())
		{
			std::cerr << name << ": FAIL image size\n";
			return 1;
		}
		for(size_t i = 0; i < base.size(); ++i)
		{
			const bool osRegion = i >= md::g_osRegionBegin && i < md::g_osRegionEnd;
			if(!osRegion && result.image[i] != base[i])
			{
				std::cerr << name << ": FAIL byte " << i << " outside the OS region changed\n";
				return 1;
			}
		}

		// The installer cleared the OS region before the upgrade, so a result that
		// equals the stock image means the bootloader wrote all of it.
		if(md::RomLoader::isRomForModel(result.image, _model))
			std::cout << name << ": PASS, the upgrade reproduced the stock image byte for byte ("
				<< static_cast<int>(seconds) << "s)\n";
		else
			std::cout << name << ": PASS, installed a different OS; it boots ("
				<< static_cast<int>(seconds) << "s)\n";
		return 0;
	}
}

int main()
{
	const auto md = run(MachineModel::Machinedrum, "GEARMULATOR_MD_FIRMWARE_BIN", "GEARMULATOR_MD_OS_SYX");
	const auto mm = run(MachineModel::Monomachine, "GEARMULATOR_MM_FIRMWARE_BIN", "GEARMULATOR_MM_OS_SYX");
	if(md == 1 || mm == 1)
		return 1;
	if(md == 77 && mm == 77)
		return 77;
	return 0;
}
