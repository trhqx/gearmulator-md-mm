#pragma once

#include "jucePluginEditorLib/settingsDeviceSpecific.h"

#include "juce_events/juce_events.h"

#include "baseLib/event.h"

#include <array>
#include <string>

namespace Rml
{
	class Element;
}

namespace mdJucePlugin
{
	class Controller;
	class Editor;

	// Device-specific GUI settings: MM storage selection plus panel encoder feel.
	class SettingsPanelFeel : public jucePluginEditorLib::SettingsDeviceSpecific,
		private juce::Timer
	{
	public:
		SettingsPanelFeel(Editor& _editor, Rml::Element* _root);

	private:
		void timerCallback() override;
		void bindGroup(Rml::Element* _root, const char* _idPrefix, const char* _configKey);
		void updateRestoreAvailability();
		void updateRamRecordingMode();
		void createPanelMidi(Rml::Element* _root);
		void updatePanelMidi();
		Controller* getController() const;

		Editor& m_editor;
		Rml::Element* m_restoreStorage = nullptr;
		Rml::Element* m_ramRecordingComplete = nullptr;
		Rml::Element* m_ramRecordingOriginal = nullptr;
		std::array<Rml::Element*, 17> m_panelMidiChannels{};	// Off, 1..16
		std::array<Rml::Element*, 4> m_panelMidiEncodings{};
		Rml::Element* m_panelMidiChannelInfo = nullptr;
		Rml::Element* m_panelMidiMappingCount = nullptr;
		std::string m_panelMidiChannelInfoText;
		std::string m_panelMidiMappingCountText;
		baseLib::EventListener<> m_onPanelMidiMapChanged;
	};
}
