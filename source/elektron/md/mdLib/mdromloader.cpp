#include "mdromloader.h"

namespace md
{
	Rom RomLoader::findROM()
	{
		return findROM(MachineModel::Machinedrum);
	}

	Rom RomLoader::findROM(const MachineModel _model)
	{
		const auto files = findFiles(".bin", g_romSize, g_romSize);

		if(files.empty())
			return {};

		for (const auto& file : files)
		{
			auto rom = Rom(file);
			if(rom.isValid() && isRomForModel(rom.data(), _model))
				return rom;
		}
		return {};
	}

	bool RomLoader::isSupportedImage(const size_t _size,
		const uint64_t _fingerprint, const MachineModel _model)
	{
		if(_size != g_romSize)
			return false;
		return _model == MachineModel::Monomachine
			? _fingerprint == g_mmOs132bFingerprint
			: _fingerprint == g_mdOs163Fingerprint;
	}

	bool RomLoader::isRomForModel(const std::vector<uint8_t>& _data,
		const MachineModel _model)
	{
		if(_data.size() != g_romSize)
			return false;

		// Accept only a supported image for the requested product.
		return isSupportedImage(_data.size(), fingerprint(_data.data(), _data.size()), _model);
	}

	bool RomLoader::isImageForModel(const std::vector<uint8_t>& _data,
		const MachineModel _model)
	{
		if(_data.size() != g_romSize)
			return false;
		const auto bootloader = fingerprint(_data.data(), g_osRegionBegin);
		return _model == MachineModel::Monomachine
			? bootloader == g_mmBootloaderFingerprint
			: bootloader == g_mdBootloaderFingerprint;
	}

	Rom RomLoader::loadImage(const std::vector<uint8_t>& _data, const std::string& _name,
		const MachineModel _model)
	{
		// Rom() would search the disk; an empty image is explicitly invalid.
		if(!isImageForModel(_data, _model))
			return Rom(std::vector<uint8_t>{}, _name);
		return Rom(_data, _name);
	}

	uint64_t RomLoader::fingerprint(const uint8_t* const _data, const size_t _size)
	{
		uint64_t fingerprint = 14695981039346656037ull;
		for(size_t i = 0; i < _size; ++i)
		{
			fingerprint ^= _data[i];
			fingerprint *= 1099511628211ull;
		}
		return fingerprint;
	}
}
