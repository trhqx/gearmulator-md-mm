#include "mdOsImages.h"

#include "mdLib/mdromloader.h"

#include "baseLib/filesystem.h"

#ifdef __APPLE__
#include <pthread/qos.h>
#endif

namespace mdJucePlugin
{
	namespace
	{
		// The largest OS upgrade files are under 2 MB; refuse anything absurd
		// before reading it into memory.
		constexpr juce::int64 g_maxUpgradeFileSize = 16 * 1024 * 1024;
	}

	OsImages::OsImages(const md::MachineModel _model, juce::File _folder)
		: m_model(_model), m_folder(std::move(_folder))
	{
	}

	OsImages::~OsImages()
	{
		m_cancel = true;
		if(m_thread.joinable())
			m_thread.join();
	}

	bool OsImages::readImage(const juce::File& _file, std::vector<uint8_t>& _image,
		std::string& _error) const
	{
		_image.clear();
		if(!_file.existsAsFile() || _file.getSize() != md::g_romSize)
		{
			_error = "The OS image must be an existing 8 MiB firmware image.";
			return false;
		}
		if(!baseLib::filesystem::readFile(_image, _file.getFullPathName().toStdString()))
		{
			_error = "The OS image could not be read.";
			return false;
		}
		if(!md::RomLoader::isImageForModel(_image, m_model))
		{
			_image.clear();
			_error = "The OS image does not belong to this machine's firmware.";
			return false;
		}
		return true;
	}

	bool OsImages::startInstall(const juce::File& _upgrade, std::vector<uint8_t> _baseImage,
		std::string& _error)
	{
		if(m_running)
		{
			_error = "An OS installation is already running.";
			return false;
		}
		if(!_upgrade.existsAsFile() || _upgrade.getSize() <= 0 || _upgrade.getSize() > g_maxUpgradeFileSize)
		{
			_error = "Choose an Elektron OS upgrade .syx file.";
			return false;
		}
		if(!md::RomLoader::isRomForModel(_baseImage, m_model))
		{
			_error = "The stock firmware image (.bin) is needed as the base for an OS installation.";
			return false;
		}
		if(m_thread.joinable())
			m_thread.join();

		m_cancel = false;
		m_sent = 0;
		m_total = 0;
		m_phase = static_cast<uint8_t>(md::OsInstaller::Phase::Booting);
		m_running = true;
		m_thread = std::thread([this, _upgrade, base = std::move(_baseImage)]() mutable
		{
			run(_upgrade, std::move(base));
		});
		return true;
	}

	void OsImages::cancelInstall()
	{
		m_cancel = true;
	}

	OsImages::InstallStatus OsImages::getInstallStatus() const
	{
		InstallStatus status;
		status.running = m_running;
		status.phase = static_cast<md::OsInstaller::Phase>(m_phase.load());
		status.sentMessages = m_sent;
		status.totalMessages = m_total;
		const std::scoped_lock lock(m_resultMutex);
		status.finishedCount = m_finishedCount;
		status.success = m_success;
		status.message = m_message;
		status.installedImage = m_installedImage;
		return status;
	}

	void OsImages::run(juce::File _upgrade, std::vector<uint8_t> _baseImage)
	{
#ifdef __APPLE__
		// Background work: never compete with the audio thread.
		pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
#endif
		std::vector<uint8_t> upgrade;
		if(!baseLib::filesystem::readFile(upgrade, _upgrade.getFullPathName().toStdString()))
			return finish(false, "The OS upgrade file could not be read.");

		const auto result = md::OsInstaller::install(_baseImage, m_model, upgrade,
			[this](const md::OsInstaller::Progress& _progress)
			{
				m_phase = static_cast<uint8_t>(_progress.phase);
				m_sent = _progress.sentMessages;
				m_total = _progress.totalMessages;
			}, &m_cancel);

		if(result.status != md::OsInstaller::Status::Ok)
			return finish(false, result.error);

		if(!m_folder.isDirectory() && !m_folder.createDirectory().wasOk())
			return finish(false, "The OS folder could not be created: " + m_folder.getFullPathName().toStdString());

		const auto target = m_folder.getChildFile(_upgrade.getFileNameWithoutExtension() + ".bin");
		if(!baseLib::filesystem::writeFileAtomic(target.getFullPathName().toStdString(),
			result.image.data(), result.image.size()))
			return finish(false, "The OS image could not be saved to " + target.getFullPathName().toStdString());

		finish(true, "Installed " + _upgrade.getFileName().toStdString() + " as "
			+ target.getFullPathName().toStdString(), target);
	}

	void OsImages::finish(const bool _success, std::string _message, juce::File _image)
	{
		{
			const std::scoped_lock lock(m_resultMutex);
			++m_finishedCount;
			m_success = _success;
			m_message = std::move(_message);
			m_installedImage = std::move(_image);
		}
		m_running = false;
	}
}
