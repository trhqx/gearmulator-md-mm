#include "mdosinstaller.h"

#include <algorithm>
#include <chrono>
#include <cmath>

#include "mdhardware.h"
#include "mdpanel.h"
#include "mdromloader.h"

namespace md
{
	namespace
	{
		constexpr uint8_t g_dataCommand = 0x7e;
		constexpr uint8_t g_endCommand = 0x7f;

		// One MIDI byte (10 bits at 31250 baud) takes 320 us on the wire.
		constexpr double g_wireMsPerByte = 0.32;

		uint32_t framesFromMs(const double _ms)
		{
			return static_cast<uint32_t>(std::ceil(_ms * g_samplerate / 1000.0));
		}

		uint64_t ucCyclesFromMs(const double _ms)
		{
			return static_cast<uint64_t>(_ms * static_cast<double>(g_ucClockHz) / 1000.0);
		}

		bool cancelled(const std::atomic<bool>* _cancel)
		{
			return _cancel && _cancel->load(std::memory_order_relaxed);
		}

		OsInstaller::Result fail(const OsInstaller::Status _status, std::string _error)
		{
			OsInstaller::Result result;
			result.status = _status;
			result.error = std::move(_error);
			return result;
		}

		// Advance in slices so long waits stay cancellable.
		bool advanceFor(Hardware& _hardware, const double _ms, const std::atomic<bool>* _cancel)
		{
			auto remaining = framesFromMs(_ms);
			while(remaining)
			{
				if(cancelled(_cancel))
					return false;
				const auto chunk = std::min<uint32_t>(remaining, g_samplerate / 10);
				_hardware.advance(chunk);
				remaining -= chunk;
			}
			return true;
		}

		bool tap(Hardware& _hardware, const MachineModel _model, const PanelControl _control)
		{
			const auto packet = panelPacket(_model, _control);
			if(!packet || !_hardware.trySendPanelEvent(packet->row, packet->mask))
				return false;
			_hardware.advance(2048);
			return _hardware.trySendPanelEvent(packet->row, 0);
		}
	}

	const char* toString(const OsUpgradeFileError _error)
	{
		switch(_error)
		{
		case OsUpgradeFileError::None: return "Valid OS upgrade file.";
		case OsUpgradeFileError::Empty: return "The file is empty.";
		case OsUpgradeFileError::InvalidFraming: return "The file is not a complete SysEx file.";
		case OsUpgradeFileError::WrongModel: return "The OS upgrade is for another machine.";
		case OsUpgradeFileError::NotAnUpgrade: return "The file contains SysEx that is not an Elektron OS upgrade.";
		case OsUpgradeFileError::MissingEnd: return "The OS upgrade is incomplete: its end message is missing.";
		}
		return "The file could not be read.";
	}

	OsUpgradeFileError parseOsUpgradeFile(const std::vector<uint8_t>& _bytes,
		const MachineModel _model, OsUpgradeFile& _file)
	{
		_file.messages.clear();
		if(_bytes.empty())
			return OsUpgradeFileError::Empty;

		const uint8_t product = _model == MachineModel::Monomachine ? 3 : 2;
		bool ended = false;
		size_t dataMessages = 0;

		for(size_t offset = 0; offset < _bytes.size();)
		{
			if(_bytes[offset] != 0xf0)
				return OsUpgradeFileError::InvalidFraming;
			const auto end = std::find(_bytes.begin() + static_cast<std::ptrdiff_t>(offset) + 1,
				_bytes.end(), uint8_t{0xf7});
			if(end == _bytes.end())
				return OsUpgradeFileError::InvalidFraming;
			const auto size = static_cast<size_t>(end - _bytes.begin()) + 1 - offset;
			const auto* b = _bytes.data() + offset;

			if(size < 8 || b[1] != 0x00 || b[2] != 0x20 || b[3] != 0x3c)
				return OsUpgradeFileError::NotAnUpgrade;
			for(size_t i = 1; i < size - 1; ++i)
			{
				if(b[i] & 0x80)
					return OsUpgradeFileError::InvalidFraming;
			}
			if(b[6] != g_dataCommand && b[6] != g_endCommand)
				return OsUpgradeFileError::NotAnUpgrade;
			if(b[4] != product)
				return OsUpgradeFileError::WrongModel;
			// Nothing may follow the end message.
			if(ended)
				return OsUpgradeFileError::NotAnUpgrade;

			if(b[6] == g_endCommand)
				ended = true;
			else
				++dataMessages;

			_file.messages.emplace_back(b, b + size);
			offset += size;
		}

		if(!dataMessages)
			return OsUpgradeFileError::NotAnUpgrade;
		if(!ended)
			return OsUpgradeFileError::MissingEnd;
		return OsUpgradeFileError::None;
	}

	const char* toString(const OsInstaller::Status _status)
	{
		switch(_status)
		{
		case OsInstaller::Status::Ok: return "OS installed.";
		case OsInstaller::Status::InvalidBaseImage: return "The base firmware image is not valid for this machine.";
		case OsInstaller::Status::InvalidUpgradeFile: return "The file is not a valid OS upgrade for this machine.";
		case OsInstaller::Status::NoStartupMenu: return "The machine did not enter the EARLY STARTUP MENU.";
		case OsInstaller::Status::Cancelled: return "The installation was cancelled.";
		case OsInstaller::Status::NotFlashed: return "The bootloader did not program the flash. The upgrade was rejected.";
		case OsInstaller::Status::FlashTimeout: return "The bootloader did not finish programming the flash.";
		case OsInstaller::Status::VerifyFailed: return "The installed OS did not start.";
		}
		return "The installation failed.";
	}

	OsInstaller::Result OsInstaller::install(const std::vector<uint8_t>& _baseImage,
		const MachineModel _model, const std::vector<uint8_t>& _upgradeFile,
		const ProgressCallback& _progress, const std::atomic<bool>* _cancel)
	{
		return install(_baseImage, _model, _upgradeFile, Options{}, _progress, _cancel);
	}

	OsInstaller::Result OsInstaller::install(const std::vector<uint8_t>& _baseImage,
		const MachineModel _model, const std::vector<uint8_t>& _upgradeFile,
		const Options& _options, const ProgressCallback& _progress,
		const std::atomic<bool>* _cancel)
	{
		if(!RomLoader::isImageForModel(_baseImage, _model))
			return fail(Status::InvalidBaseImage, toString(Status::InvalidBaseImage));

		OsUpgradeFile file;
		if(const auto error = parseOsUpgradeFile(_upgradeFile, _model, file);
			error != OsUpgradeFileError::None)
			return fail(Status::InvalidUpgradeFile, toString(error));

		Progress progress;
		progress.totalMessages = file.messages.size();
		const auto report = [&](const Phase _phase)
		{
			progress.phase = _phase;
			if(_progress)
				_progress(progress);
		};

		std::vector<uint8_t> image;
		{
			// Clear the OS region so nothing of the base OS can survive into the
			// result. The bootloader below it is all that runs until the upgrade.
			auto working = _baseImage;
			std::fill(working.begin() + g_osRegionBegin, working.begin() + g_osRegionEnd, uint8_t{0});

			Hardware hardware(working, "OS installer", _model);
			if(!hardware.isValid())
				return fail(Status::InvalidBaseImage, toString(Status::InvalidBaseImage));
			hardware.setBootHoldFunction(true);

			report(Phase::Booting);
			bool handshake = false;
			for(int i = 0; i < 50 && !handshake; ++i)
			{
				if(!advanceFor(hardware, 100, _cancel))
					return fail(Status::Cancelled, toString(Status::Cancelled));
				handshake = hardware.getUC().isPanelHandshakeComplete();
			}
			if(!advanceFor(hardware, 2000, _cancel))
				return fail(Status::Cancelled, toString(Status::Cancelled));
			// A normal boot would bring the OS up; the menu leaves the DSPs idle.
			if(!handshake || hardware.isFirmwareMidiReady())
				return fail(Status::NoStartupMenu, toString(Status::NoStartupMenu));

			// 5.. MIDI UPGRADE
			if(!tap(hardware, _model, PanelControl::Trigger5)
				|| !advanceFor(hardware, 500, _cancel))
				return fail(Status::Cancelled, toString(Status::Cancelled));

			auto& uc = hardware.getUC();
			const auto sendStart = uc.getCycles();
			const auto writtenSinceStart = [&]
			{
				return uc.flashDirty() && uc.getCycles() - uc.flashIdleCycles() > sendStart;
			};

			report(Phase::Sending);
			for(const auto& message : file.messages)
			{
				if(cancelled(_cancel))
					return fail(Status::Cancelled, toString(Status::Cancelled));
				synthLib::SMidiEvent event(synthLib::MidiEventSource::Host);
				event.sysex = message;
				hardware.sendMidi(event);
				hardware.advance(framesFromMs(static_cast<double>(message.size()) * g_wireMsPerByte
					+ _options.messageGapMs));
				if((++progress.sentMessages % 64) == 0)
					report(Phase::Sending);
			}
			report(Phase::Sending);

			// The bootloader buffers the upgrade and programs the flash once the
			// end message arrives. Right after that it resets the machine. The
			// emulated DSPs cannot follow that reset: a halted DSP sleeps on every
			// scheduler step, so a single advance() could take minutes. Advance
			// in tiny steps and stop when the flash is quiet or a step suddenly
			// crawls; the verification boot below decides whether the image is
			// complete.
			report(Phase::Flashing);
			constexpr uint32_t stepFrames = 8;
			const auto quiet = ucCyclesFromMs(250);
			const auto maxSteps = framesFromMs(60000) / stepFrames;
			bool finished = false;
			for(uint32_t i = 0; i < maxSteps && !finished; ++i)
			{
				if(cancelled(_cancel))
					return fail(Status::Cancelled, toString(Status::Cancelled));
				const auto begin = std::chrono::steady_clock::now();
				hardware.advance(stepFrames);
				const auto wall = std::chrono::steady_clock::now() - begin;
				if(!writtenSinceStart())
					continue;
				finished = uc.flashIdleCycles() >= quiet || wall > std::chrono::milliseconds(100);
			}
			if(!writtenSinceStart())
				return fail(Status::NotFlashed, toString(Status::NotFlashed));
			if(!finished)
				return fail(Status::FlashTimeout, toString(Status::FlashTimeout));

			const auto flash = hardware.copyFlashData();
			if(flash.size() != _baseImage.size())
				return fail(Status::NotFlashed, toString(Status::NotFlashed));

			// Take only the OS region; everything else stays as in the base image.
			image = _baseImage;
			std::copy(flash.begin() + g_osRegionBegin, flash.begin() + g_osRegionEnd,
				image.begin() + g_osRegionBegin);
		}

		report(Phase::Verifying);
		{
			Hardware verify(image, "installed OS", _model);
			if(!verify.isValid())
				return fail(Status::VerifyFailed, toString(Status::VerifyFailed));
			bool ready = false;
			for(int i = 0; i < 160 && !ready; ++i)
			{
				if(!advanceFor(verify, 250, _cancel))
					return fail(Status::Cancelled, toString(Status::Cancelled));
				ready = verify.isFirmwareMidiReady();
			}
			if(!ready)
				return fail(Status::VerifyFailed, toString(Status::VerifyFailed));
		}

		Result result;
		result.status = Status::Ok;
		result.image = std::move(image);
		return result;
	}
}
