#pragma once

#include <cstdint>
#include <optional>

#include "mdpanel.h"
#include "mdtypes.h"

namespace md::panelParameters
{
	// Global host parameters for the front panel (doc/mdmm_panel_host_parameters.md).
	// Page numbers are part of the JUCE parameter IDs ("page_0_index") and sit well
	// above the firmware track pages, which may still grow. The parameter index is
	// the PanelControl / PanelEncoder value. Never renumber either.
	constexpr uint8_t ButtonPage = 100;
	// Retired: encoders are not host parameters (their recentring polluted host
	// automation). Kept reserved so the page number is never reused.
	constexpr uint8_t EncoderPage = 101;
	constexpr uint8_t EncoderPushPage = 102;

	// Parameters added after the first release are registered in their own
	// version group so the host order of existing parameters stays unchanged.
	constexpr int Version = 1;

	constexpr bool isPanelPage(const uint8_t _page)
	{
		return _page >= ButtonPage && _page <= EncoderPushPage;
	}

	struct Address
	{
		uint8_t page = 0;
		uint8_t index = 0;
	};

	// The parameter that owns a single-control scan-row packet: a button or an
	// encoder push switch. Merged multi-control masks have no single owner.
	inline std::optional<Address> addressForPacket(const MachineModel _model, const PanelPacket& _packet)
	{
		for(uint8_t c = 0; c <= static_cast<uint8_t>(PanelControl::ClassicExtended); ++c)
			if(panelPacket(_model, static_cast<PanelControl>(c)) == _packet)
				return Address{ButtonPage, c};
		for(uint8_t e = 0; e <= static_cast<uint8_t>(PanelEncoder::DataEntryH); ++e)
			if(panelEncoderPressPacket(_model, static_cast<PanelEncoder>(e)) == _packet)
				return Address{EncoderPushPage, e};
		return std::nullopt;
	}
}
