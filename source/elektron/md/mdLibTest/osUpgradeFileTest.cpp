#include "mdLib/mdosinstaller.h"
#include "mdLib/mdromloader.h"

#include <cstdlib>
#include <initializer_list>
#include <iostream>
#include <vector>

namespace
{
	using md::MachineModel;
	using md::OsUpgradeFileError;

	void expect(const bool _condition, const char* const _message)
	{
		if(_condition)
			return;
		std::cerr << "mdOsUpgradeFileTest: " << _message << '\n';
		std::exit(1);
	}

	// Synthetic frames only; no vendor data.
	std::vector<uint8_t> message(const uint8_t _product, const uint8_t _command,
		std::initializer_list<uint8_t> _payload = {0x01, 0x02, 0x03})
	{
		std::vector<uint8_t> m{0xf0, 0x00, 0x20, 0x3c, _product, 0x00, _command};
		m.insert(m.end(), _payload);
		m.push_back(0xf7);
		return m;
	}

	std::vector<uint8_t> join(std::initializer_list<std::vector<uint8_t>> _messages)
	{
		std::vector<uint8_t> bytes;
		for(const auto& m : _messages)
			bytes.insert(bytes.end(), m.begin(), m.end());
		return bytes;
	}

	OsUpgradeFileError parse(const std::vector<uint8_t>& _bytes, const MachineModel _model,
		md::OsUpgradeFile* _file = nullptr)
	{
		md::OsUpgradeFile file;
		const auto error = md::parseOsUpgradeFile(_bytes, _model, file);
		if(_file)
			*_file = std::move(file);
		return error;
	}

	constexpr uint8_t g_md = 2;
	constexpr uint8_t g_mm = 3;
	constexpr uint8_t g_data = 0x7e;
	constexpr uint8_t g_end = 0x7f;

	void testValid()
	{
		md::OsUpgradeFile file;
		const auto bytes = join({message(g_md, g_data), message(g_md, g_data), message(g_md, g_end, {})});
		expect(parse(bytes, MachineModel::Machinedrum, &file) == OsUpgradeFileError::None,
			"valid MD upgrade rejected");
		expect(file.messages.size() == 3, "message count");
		expect(file.messages.front() == message(g_md, g_data), "messages must be passed unchanged");
		expect(file.messages.back() == message(g_md, g_end, {}), "end message must be last");

		expect(parse(join({message(g_mm, g_data), message(g_mm, g_end)}), MachineModel::Monomachine)
			== OsUpgradeFileError::None, "valid MM upgrade rejected");
	}

	void testRejected()
	{
		expect(parse({}, MachineModel::Machinedrum) == OsUpgradeFileError::Empty, "empty");
		expect(parse(join({message(g_mm, g_data), message(g_mm, g_end)}), MachineModel::Machinedrum)
			== OsUpgradeFileError::WrongModel, "MM upgrade accepted for MD");
		expect(parse(join({message(g_md, g_data), message(g_md, g_end)}), MachineModel::Monomachine)
			== OsUpgradeFileError::WrongModel, "MD upgrade accepted for MM");
		expect(parse(join({message(g_md, g_data)}), MachineModel::Machinedrum)
			== OsUpgradeFileError::MissingEnd, "upgrade without end message");
		expect(parse(join({message(g_md, g_end)}), MachineModel::Machinedrum)
			== OsUpgradeFileError::NotAnUpgrade, "end message without data");
		expect(parse(join({message(g_md, g_data), message(g_md, g_end), message(g_md, g_data)}),
			MachineModel::Machinedrum) == OsUpgradeFileError::NotAnUpgrade, "data after the end message");

		// A kit dump (user data) is not an OS upgrade.
		expect(parse(join({message(g_md, 0x52), message(g_md, g_end)}), MachineModel::Machinedrum)
			== OsUpgradeFileError::NotAnUpgrade, "user data accepted as an upgrade");
		auto otherManufacturer = message(g_md, g_data);
		otherManufacturer[3] = 0x3d;
		expect(parse(join({otherManufacturer, message(g_md, g_end)}), MachineModel::Machinedrum)
			== OsUpgradeFileError::NotAnUpgrade, "other manufacturer");

		auto truncated = join({message(g_md, g_data), message(g_md, g_end)});
		truncated.pop_back();
		expect(parse(truncated, MachineModel::Machinedrum) == OsUpgradeFileError::InvalidFraming,
			"missing F7");
		expect(parse(join({message(g_md, g_data, {0x01, 0x80}), message(g_md, g_end)}),
			MachineModel::Machinedrum) == OsUpgradeFileError::InvalidFraming, "status byte in payload");
		auto junk = join({message(g_md, g_data), message(g_md, g_end)});
		junk.insert(junk.begin(), 0x00);
		expect(parse(junk, MachineModel::Machinedrum) == OsUpgradeFileError::InvalidFraming,
			"bytes outside SysEx");
	}

	void testImageAcceptance()
	{
		// Without the stock bootloader no 8 MiB image is accepted, and loadImage
		// never substitutes another image for a rejected one.
		const std::vector<uint8_t> blank(md::g_romSize, 0xff);
		for(const auto model : {MachineModel::Machinedrum, MachineModel::Monomachine})
		{
			expect(!md::RomLoader::isImageForModel(blank, model), "foreign bootloader accepted");
			expect(!md::RomLoader::loadImage(blank, "blank.bin", model).isValid(),
				"loadImage returned an image for a rejected one");
			expect(!md::RomLoader::isImageForModel(std::vector<uint8_t>(md::g_osRegionBegin), model),
				"short image accepted");
		}

		const auto result = md::OsInstaller::install(blank, MachineModel::Machinedrum,
			join({message(g_md, g_data), message(g_md, g_end)}));
		expect(result.status == md::OsInstaller::Status::InvalidBaseImage, "installer accepted a foreign base image");
	}
}

int main()
{
	testValid();
	testRejected();
	testImageAcceptance();
	std::cout << "mdOsUpgradeFileTest: PASS\n";
	return 0;
}
