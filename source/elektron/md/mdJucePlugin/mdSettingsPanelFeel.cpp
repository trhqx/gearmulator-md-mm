#include "mdSettingsPanelFeel.h"

#include "mdController.h"
#include "mdEditor.h"
#include "mdLcdInteractionModel.h"
#include "mdPixelPerfectPanel.h"
#include "mdPluginProcessor.h"

#include "jucePluginEditorLib/pluginProcessor.h"
#include "jucePluginEditorLib/settingsPlugin.h"

#include "juceRmlUi/rmlElemButton.h"
#include "juceRmlUi/rmlEventListener.h"
#include "juceRmlUi/rmlHelper.h"

#include "RmlUi/Core/Element.h"

namespace mdJucePlugin
{
	SettingsPanelFeel::SettingsPanelFeel(Editor& _editor, Rml::Element* _root) : m_editor(_editor)
	{
		jucePluginEditorLib::SettingsPlugin::createToggleButton(_root, "btPixelPerfectPanel",
			m_editor.getProcessor().getConfig(), PixelPerfectPanel::configKey, [this](bool)
			{
				m_editor.applyPixelPerfectPanel();
			}, PixelPerfectPanel::defaultEnabled);
		jucePluginEditorLib::SettingsPlugin::createToggleButton(_root, "btLcdRotaryInteraction",
			m_editor.getProcessor().getConfig(), lcdInteraction::configKey, [this](bool)
			{
				m_editor.applyLcdInteraction();
			}, lcdInteraction::defaultEnabled);
		jucePluginEditorLib::SettingsPlugin::createToggleButton(_root, "btHostButtonsTap",
			m_editor.getProcessor().getConfig(), Controller::g_hostButtonsTapConfigKey, [this](const bool _tap)
			{
				if(auto* const controller = dynamic_cast<Controller*>(&m_editor.getProcessor().getController()))
					controller->setHostButtonsTap(_tap);
			}, Controller::g_hostButtonsTapDefault);
		createPanelMidi(_root);
		bindGroup(_root, "btWheelSpeed", "panelWheelSpeedPercent");
		bindGroup(_root, "btEncoderSpeed", "panelEncoderSpeedPercent");

		m_ramRecordingComplete = juceRmlUi::helper::findChild(
			_root, "btRamRecordingComplete", false);
		m_ramRecordingOriginal = juceRmlUi::helper::findChild(
			_root, "btRamRecordingOriginal", false);
		if(m_ramRecordingComplete)
			juceRmlUi::EventListener::AddClick(m_ramRecordingComplete, [this]
			{
				static_cast<AudioPluginAudioProcessor&>(m_editor.getProcessor())
					.setRamRecordingMode(md::RamRecordingMode::CompleteTail);
				updateRamRecordingMode();
			});
		if(m_ramRecordingOriginal)
			juceRmlUi::EventListener::AddClick(m_ramRecordingOriginal, [this]
			{
				static_cast<AudioPluginAudioProcessor&>(m_editor.getProcessor())
					.setRamRecordingMode(md::RamRecordingMode::Original);
				updateRamRecordingMode();
			});
		updateRamRecordingMode();

		if(auto* const loadFactory = juceRmlUi::helper::findChild(
			_root, "btLoadInstalledFactoryStorage", false))
		{
			juceRmlUi::EventListener::AddClick(loadFactory, [this]
			{
				m_editor.loadInstalledFactoryStorage();
			});
		}
		if(auto* const chooseStorage = juceRmlUi::helper::findChild(
			_root, "btChooseStorageImage", false))
		{
			juceRmlUi::EventListener::AddClick(chooseStorage, [this]
			{
				m_editor.chooseStorageImage();
			});
		}
		m_restoreStorage = juceRmlUi::helper::findChild(
			_root, "btRestorePreviousStorage", false);
		if(m_restoreStorage)
		{
			juceRmlUi::EventListener::AddClick(m_restoreStorage, [this]
			{
				m_editor.restorePreviousStorage();
			});
			updateRestoreAvailability();
		}
		if(m_restoreStorage || m_ramRecordingComplete || m_ramRecordingOriginal || m_panelMidiChannelInfo)
			startTimerHz(2);
	}

	void SettingsPanelFeel::timerCallback()
	{
		updateRestoreAvailability();
		updateRamRecordingMode();
		updatePanelMidi();	// the machine's own channel may change in its global menu
	}

	Controller* SettingsPanelFeel::getController() const
	{
		return dynamic_cast<Controller*>(&m_editor.getProcessor().getController());
	}

	void SettingsPanelFeel::createPanelMidi(Rml::Element* _root)
	{
		auto* const controller = getController();
		if(!controller)
			return;
		const auto checkbox = [_root](const std::string& _id) -> Rml::Element*
		{
			auto* const row = juceRmlUi::helper::findChild(_root, _id, false);
			return row ? juceRmlUi::helper::findChild(row, "button", false) : nullptr;
		};
		for(size_t i = 0; i < m_panelMidiChannels.size(); ++i)
		{
			m_panelMidiChannels[i] = checkbox(i == 0 ? std::string("btPanelMidiChannelOff")
				: "btPanelMidiChannel" + std::to_string(i));
			if(!m_panelMidiChannels[i])
				continue;
			juceRmlUi::EventListener::AddClick(m_panelMidiChannels[i], [this, i]
			{
				if(auto* const c = getController())
				{
					auto map = c->getPanelMidiMap();
					map.channel = i == 0 ? md::PanelMidiMap::g_off : static_cast<int8_t>(i - 1);
					c->setPanelMidiMap(map);
				}
			});
		}
		constexpr const char* encodingIds[] = {"btPanelMidiEncodingTwosComplement", "btPanelMidiEncodingBinaryOffset",
			"btPanelMidiEncodingSignMagnitude", "btPanelMidiEncodingAbsolute"};
		for(size_t i = 0; i < m_panelMidiEncodings.size(); ++i)
		{
			m_panelMidiEncodings[i] = checkbox(encodingIds[i]);
			if(!m_panelMidiEncodings[i])
				continue;
			juceRmlUi::EventListener::AddClick(m_panelMidiEncodings[i], [this, i]
			{
				if(auto* const c = getController())
				{
					auto map = c->getPanelMidiMap();
					map.encoding = static_cast<md::PanelMidiEncoding>(i);
					c->setPanelMidiMap(map);
				}
			});
		}
		if(auto* const forget = juceRmlUi::helper::findChild(_root, "btPanelMidiForgetAll", false))
			juceRmlUi::EventListener::AddClick(forget, [this]
			{
				if(auto* const c = getController())
				{
					c->cancelPanelMidiLearn();
					auto map = c->getPanelMidiMap();
					map.clearBindings();
					c->setPanelMidiMap(map);
				}
			});
		m_panelMidiChannelInfo = juceRmlUi::helper::findChild(_root, "lbPanelMidiChannelInfo", false);
		m_panelMidiMappingCount = juceRmlUi::helper::findChild(_root, "lbPanelMidiMappingCount", false);
		m_onPanelMidiMapChanged = baseLib::EventListener<>(controller->onPanelMidiMapChanged, [this]
		{
			updatePanelMidi();
		});
		updatePanelMidi();
	}

	void SettingsPanelFeel::updatePanelMidi()
	{
		auto* const controller = getController();
		if(!controller)
			return;
		const auto& map = controller->getPanelMidiMap();
		for(size_t i = 0; i < m_panelMidiChannels.size(); ++i)
			if(m_panelMidiChannels[i])
				juceRmlUi::ElemButton::setChecked(m_panelMidiChannels[i],
					static_cast<int>(i) == map.channel + 1);
		for(size_t i = 0; i < m_panelMidiEncodings.size(); ++i)
			if(m_panelMidiEncodings[i])
				juceRmlUi::ElemButton::setChecked(m_panelMidiEncodings[i],
					static_cast<size_t>(map.encoding) == i);

		if(m_panelMidiChannelInfo)
		{
			// MD listens on four consecutive channels, MM on six.
			const auto base = controller->getAutomationBaseChannel();
			const int span = static_cast<AudioPluginAudioProcessor&>(m_editor.getProcessor()).getModel()
				== md::MachineModel::Monomachine ? 6 : 4;
			std::string text;
			if(base < 16)
			{
				const int first = base + 1;
				const int last = std::min(16, first + span - 1);
				text = "The machine listens on channels " + std::to_string(first) + "-" + std::to_string(last) + ".";
				if(map.channel != md::PanelMidiMap::g_off && map.channel + 1 >= first && map.channel + 1 <= last)
					text += " The panel channel overlaps them: notes and CCs on it no longer reach the machine.";
			}
			if(text != m_panelMidiChannelInfoText)
			{
				m_panelMidiChannelInfoText = text;
				m_panelMidiChannelInfo->SetInnerRML(text);
			}
		}
		if(m_panelMidiMappingCount)
		{
			size_t count = 0;
			for(uint8_t n = 0; n < 128; ++n)
			{
				count += map.find(md::PanelMidiTrigger::note(n)).type != md::PanelMidiTarget::Type::None ? 1 : 0;
				count += map.find(md::PanelMidiTrigger::controlChange(n)).type != md::PanelMidiTarget::Type::None ? 1 : 0;
			}
			auto text = std::to_string(count) + " panel controls mapped.";
			if(text != m_panelMidiMappingCountText)
			{
				m_panelMidiMappingCountText = text;
				m_panelMidiMappingCount->SetInnerRML(text);
			}
		}
	}

	void SettingsPanelFeel::updateRamRecordingMode()
	{
		if(!m_ramRecordingComplete && !m_ramRecordingOriginal)
			return;
		auto& processor = static_cast<AudioPluginAudioProcessor&>(m_editor.getProcessor());
		const auto mode = processor.getRamRecordingMode();
		const bool available = processor.isRamRecordingModeAvailable();
		if(m_ramRecordingComplete)
		{
			if(auto* const button = juceRmlUi::helper::findChild(
				m_ramRecordingComplete, "button", false))
				juceRmlUi::ElemButton::setChecked(button,
					mode == md::RamRecordingMode::CompleteTail);
			juceRmlUi::helper::setEnabled(m_ramRecordingComplete, available);
		}
		if(m_ramRecordingOriginal)
		{
			if(auto* const button = juceRmlUi::helper::findChild(
				m_ramRecordingOriginal, "button", false))
				juceRmlUi::ElemButton::setChecked(button,
					mode == md::RamRecordingMode::Original);
			juceRmlUi::helper::setEnabled(m_ramRecordingOriginal, available);
		}
	}

	void SettingsPanelFeel::updateRestoreAvailability()
	{
		if(m_restoreStorage)
			juceRmlUi::helper::setEnabled(m_restoreStorage,
				m_editor.hasStorageRecoveryImage());
	}

	void SettingsPanelFeel::bindGroup(Rml::Element* _root, const char* _idPrefix, const char* _configKey)
	{
		auto& config = m_editor.getProcessor().getConfig();

		std::vector<Rml::Element*> checkboxes(std::size(Editor::g_panelSpeedPercents), nullptr);

		for (size_t i = 0; i < std::size(Editor::g_panelSpeedPercents); ++i)
		{
			auto* row = juceRmlUi::helper::findChild(_root,
				_idPrefix + std::to_string(Editor::g_panelSpeedPercents[i]), false);
			if (row)
				checkboxes[i] = juceRmlUi::helper::findChild(row, "button");
		}

		const auto updateChecked = [checkboxes, &config, _configKey]
		{
			const auto current = config.getIntValue(_configKey, 100);
			for (size_t i = 0; i < checkboxes.size(); ++i)
			{
				if (checkboxes[i])
					juceRmlUi::ElemButton::setChecked(checkboxes[i], Editor::g_panelSpeedPercents[i] == current);
			}
		};

		updateChecked();

		for (const auto percent : Editor::g_panelSpeedPercents)
		{
			auto* row = juceRmlUi::helper::findChild(_root, _idPrefix + std::to_string(percent), false);
			if (!row)
				continue;

			juceRmlUi::EventListener::AddClick(row, [this, updateChecked, &config, _configKey, percent]
			{
				config.setValue(_configKey, percent);
				config.saveIfNeeded();
				updateChecked();
				m_editor.applyPanelSpeeds();
			});
		}
	}
}
