// Illuminated - SKSE plugin
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE.txt and the notice at the top of main.cpp.
//
// Small text helpers: paths, and the fire / frost / shock word read off an editor ID (lower case is LightMath.h's
// LightKit::Lower; trimming and numbers are ConfigText.h's).

#include "Plugin.h"

namespace Plugin
{
	// ------------------------------------------------------------------ small text helpers
	std::string NormalPath(std::string_view a_path)
	{
		std::string out = Lower(a_path);
		std::ranges::replace(out, '/', '\\');
		// a VFX pack's copy (VfxPacks.cpp: meshes\VFX Packs\<pack>\...) reads as the model it stands in for, so the passes and the
		// configs still find a spell the switcher dressed
		constexpr std::string_view kPacks = "vfx packs\\";
		const std::size_t          at = out.starts_with(kPacks)                ? std::size_t{ 0 } :
		                                out.starts_with("meshes\\vfx packs\\") ? std::size_t{ 7 } :
		                                                                         std::string::npos;
		if (at != std::string::npos) {
			if (const auto end = out.find('\\', at + kPacks.size()); end != std::string::npos) {
				out.erase(at, end + 1 - at);
			}
		}
		return out;
	}

	bool Contains(std::string_view a_text, std::string_view a_part)
	{
		return a_text.find(a_part) != std::string_view::npos;
	}

	Family FamilyOf(std::string_view a_editorID, bool a_sparkStorm)
	{
		auto id = Lower(a_editorID);
		for (auto at = id.find("voice"); at != std::string::npos; at = id.find("voice")) {
			id.erase(at, 5);  // a shout's prefix: "voice" holds "ice"
		}
		if (Contains(id, "frost") || Contains(id, "ice")) {
			return Family::kFrost;
		}
		if (Contains(id, "flame") || Contains(id, "fire")) {
			return Family::kFire;
		}
		if (Contains(id, "shock") || Contains(id, "lightning") || (a_sparkStorm && (Contains(id, "spark") || Contains(id, "storm")))) {
			return Family::kShock;
		}
		return Family::kNone;
	}
}
