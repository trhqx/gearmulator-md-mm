#pragma once

#include "mdrom.h"

#include "synthLib/romLoader.h"

namespace md
{
	class RomLoader : synthLib::RomLoader
	{
	public:
		static Rom findROM();
		static Rom findROM(MachineModel _model);
		static bool isSupportedImage(size_t _size, uint64_t _fingerprint,
			MachineModel _model);
		// True for the stock image of the model only. findROM accepts nothing
		// else, so installed OS images never replace the base image.
		static bool isRomForModel(const std::vector<uint8_t>& _data, MachineModel _model);
		// True for the stock image or the stock image with another OS installed
		// (same bootloader). Used for images the caller supplies explicitly.
		static bool isImageForModel(const std::vector<uint8_t>& _data, MachineModel _model);
		// Load an explicitly chosen image; invalid if it is not an image for the
		// model. Never falls back to another image.
		static Rom loadImage(const std::vector<uint8_t>& _data, const std::string& _name,
			MachineModel _model);
		static uint64_t fingerprint(const uint8_t* _data, size_t _size);
	};
}
