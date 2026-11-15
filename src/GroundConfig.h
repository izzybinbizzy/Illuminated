// Illuminated - the ground twins' own names and hooks (GroundLights.cpp is one file in Illuminated and RELight - Spell Lighting NG,
// master in Tools\Shared Plugin Code\; what differs between the two mods is here)
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE.txt and the notice at the top of main.cpp.

#pragma once

#include "Plugin.h"

namespace Plugin::Ground
{
	inline constexpr const char* kLightName = "IlluminatedGroundLight";

	// the twins stand on the record route only (no Light Placer): Vanilla, ENB, and Community Shaders without Light Placer
	[[nodiscard]] inline bool Wanted() { return RecordRoute(); }

	// Performance's switch: every light of the mod off
	[[nodiscard]] inline bool AllOff() { return LightsOff(); }

	// a casting light the mod keeps dark: none here (RELight darkens a switched-off option's light)
	[[nodiscard]] inline bool WantedDark(RE::NiLight*) { return false; }

	// a far caster's light kept out by a light budget: none here
	[[nodiscard]] inline bool KeptOut(RE::NiLight*) { return false; }

	// a mark a twin wears from its making: none here (RELight marks it for RE::Light's flicker prevention)
	inline void Mark(RE::NiPointLight*) {}

	// a casting light left hidden was let go: nothing to tell here (RELight's Keep.cpp sweeps)
	inline void LetGo() {}

	// anyone besides the player with hands out (Events.cpp's list holds the player first)
	[[nodiscard]] inline bool OthersDrawn() { return DrawnActors().size() > 1; }

	// the player first, then every actor with drawn hands (Events.cpp)
	template <class F>
	void ForEachDrawn(F&& a_visit)
	{
		for (auto* actor : DrawnActors()) {
			a_visit(actor);
		}
	}
}
