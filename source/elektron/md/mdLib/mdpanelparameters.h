#pragma once

#include <cstdint>

namespace md::panelParameters
{
	// Global host parameters for the front panel (doc/mdmm_panel_host_parameters.md).
	// Page numbers are part of the JUCE parameter IDs ("page_0_index") and sit well
	// above the firmware track pages, which may still grow. The parameter index is
	// the PanelControl / PanelEncoder value. Never renumber either.
	constexpr uint8_t ButtonPage = 100;
	constexpr uint8_t EncoderPage = 101;
	constexpr uint8_t EncoderPushPage = 102;

	// Parameters added after the first release are registered in their own
	// version group so the host order of existing parameters stays unchanged.
	constexpr int Version = 1;

	constexpr bool isPanelPage(const uint8_t _page)
	{
		return _page >= ButtonPage && _page <= EncoderPushPage;
	}
}
