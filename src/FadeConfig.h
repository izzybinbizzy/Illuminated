// Illuminated - the fading module's own names
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE.txt and the notice at the top of main.cpp.
//
// The fading module is the same code in Illuminated and in RELight - Spell Addon (the two never run together, so each
// carries its own copy). This header and FadeDevBench.cpp are each mod's own: the mod's name and its folders here, its
// DevBench glue there. Every other Fade* file must be byte-identical in both (`PC Runner\fade copies check.py`, in
// preflight).
// Game-free: the fading tests include it.

#pragma once

namespace Fade::Mod
{
	inline constexpr const char* kName = "Illuminated";                                                                // the menu section and the log lines
	inline constexpr const char* kSettingsPath = "Data/SKSE/Plugins/Illuminated/Fading.ini";                           // the settings file
	inline constexpr const char* kRulesDir = "Data/SKSE/Plugins/Illuminated/Fading";                                   // the rule files (*.json)
	inline constexpr const char* kRulesDirText = "Data\\SKSE\\Plugins\\Illuminated\\Fading";                           // the same, as a player reads it
	inline constexpr const char* kItemsPath = "Data/SKSE/Plugins/Illuminated/Lights by Item.ini";                      // FadeItems.cpp (his order)
	inline constexpr const char* kAdvancedPath = "Data/SKSE/Plugins/Illuminated/Illuminated - Advanced Settings.ini";  // FadeTuning.cpp (his rule)
}
