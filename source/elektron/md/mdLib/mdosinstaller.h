#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "mdtypes.h"

namespace md
{
	// An Elektron OS upgrade file: data messages F0 00 20 3C <product> 00 7E ... F7
	// followed by one end message F0 00 20 3C <product> 00 7F ... F7. The bytes
	// are passed to the bootloader unchanged; nothing here decodes them.
	enum class OsUpgradeFileError : uint8_t
	{
		None,
		Empty,
		InvalidFraming,
		WrongModel,
		NotAnUpgrade,
		MissingEnd,
	};

	const char* toString(OsUpgradeFileError _error);

	struct OsUpgradeFile
	{
		// Complete messages including F0 and F7, end message last.
		std::vector<std::vector<uint8_t>> messages;
	};

	OsUpgradeFileError parseOsUpgradeFile(const std::vector<uint8_t>& _bytes,
		MachineModel _model, OsUpgradeFile& _file);

	// Installs an OS the way the hardware does: boot the base image with FUNCTION
	// held, choose 5 (MIDI UPGRADE) in the EARLY STARTUP MENU, send the upgrade
	// file over the emulated MIDI input and let the bootloader program the flash.
	// The OS region is cleared first, so every byte of it in the result was
	// written by the bootloader. The result is then booted on a fresh machine and
	// only accepted when the OS comes up.
	//
	// Runs the emulation synchronously on the calling thread; call it from a
	// worker thread. Takes a few minutes of CPU time.
	class OsInstaller
	{
	public:
		enum class Phase : uint8_t { Booting, Sending, Flashing, Verifying };

		struct Progress
		{
			Phase phase = Phase::Booting;
			size_t sentMessages = 0;
			size_t totalMessages = 0;
		};

		enum class Status : uint8_t
		{
			Ok,
			InvalidBaseImage,
			InvalidUpgradeFile,
			NoStartupMenu,
			Cancelled,
			NotFlashed,
			FlashTimeout,
			VerifyFailed,
		};

		struct Result
		{
			Status status = Status::InvalidBaseImage;
			std::string error;
			// Complete 8 MiB image: the base image with the installed OS region.
			std::vector<uint8_t> image;
		};

		using ProgressCallback = std::function<void(const Progress&)>;

		struct Options
		{
			// Pause after each message's wire time. Each message must at least
			// take its wire time; queued all at once the bootloader fails the
			// upgrade. 0 worked on the MM; 5 ms leaves a margin.
			double messageGapMs = 5.0;
		};

		static Result install(const std::vector<uint8_t>& _baseImage, MachineModel _model,
			const std::vector<uint8_t>& _upgradeFile, const ProgressCallback& _progress = {},
			const std::atomic<bool>* _cancel = nullptr);
		static Result install(const std::vector<uint8_t>& _baseImage, MachineModel _model,
			const std::vector<uint8_t>& _upgradeFile, const Options& _options,
			const ProgressCallback& _progress = {}, const std::atomic<bool>* _cancel = nullptr);
	};

	const char* toString(OsInstaller::Status _status);
}
