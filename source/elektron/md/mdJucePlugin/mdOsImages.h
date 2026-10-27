#pragma once

#include "mdLib/mdosinstaller.h"
#include "mdLib/mdtypes.h"

#include "juce_core/juce_core.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace mdJucePlugin
{
	// Installed OS images: complete 8 MiB images derived from an OS upgrade .syx
	// by md::OsInstaller and stored as <data folder>/os/<syx name>.bin. They are
	// ordinary firmware images and can be kept or copied like the base .bin.
	//
	// Installation runs on a worker thread owned by this object, so it continues
	// when the editor closes; the destructor cancels and joins it.
	class OsImages
	{
	public:
		struct InstallStatus
		{
			bool running = false;
			md::OsInstaller::Phase phase = md::OsInstaller::Phase::Booting;
			size_t sentMessages = 0;
			size_t totalMessages = 0;
			// Increments whenever an installation finishes; 0 = none finished yet.
			uint32_t finishedCount = 0;
			bool success = false;
			std::string message;
			juce::File installedImage;
		};

		OsImages(md::MachineModel _model, juce::File _folder);
		~OsImages();

		OsImages(const OsImages&) = delete;
		OsImages& operator=(const OsImages&) = delete;

		const juce::File& getFolder() const { return m_folder; }

		// Reads and checks an image for this machine (stock or installed OS).
		bool readImage(const juce::File& _file, std::vector<uint8_t>& _image, std::string& _error) const;

		// Starts installing _upgrade on top of _baseImage (the stock image).
		bool startInstall(const juce::File& _upgrade, std::vector<uint8_t> _baseImage, std::string& _error);
		void cancelInstall();
		InstallStatus getInstallStatus() const;

	private:
		void run(juce::File _upgrade, std::vector<uint8_t> _baseImage);
		void finish(bool _success, std::string _message, juce::File _image = {});

		const md::MachineModel m_model;
		const juce::File m_folder;

		std::thread m_thread;
		std::atomic<bool> m_cancel{false};
		std::atomic<bool> m_running{false};
		std::atomic<uint8_t> m_phase{0};
		std::atomic<size_t> m_sent{0};
		std::atomic<size_t> m_total{0};

		mutable std::mutex m_resultMutex;
		uint32_t m_finishedCount = 0;
		bool m_success = false;
		std::string m_message;
		juce::File m_installedImage;
	};
}
