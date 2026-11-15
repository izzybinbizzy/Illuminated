// Illuminated - the fading module's own names
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE.txt and the notice at the top of main.cpp.
//
// The fading module is the same code in Illuminated and in RELight - Spell Lighting NG (the two never run together, so each
// carries its own copy). This header is each mod's own: the mod's name, its folders and its DevBench names. Every other Fade*
// file, FadeDevBench.cpp included, is one file with its master in Tools\Shared Plugin Code\ (`PC Runner\shared code sync.py`).
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
	inline constexpr const char* kHudArtDir = "Data/SKSE/Plugins/Illuminated/HUD/";                                    // FadeMenu.cpp's soul gem sheets
	// the mod's files under an old name, read while the new one is not there yet (SettingsText::ReadFrom) - none: Illuminated
	// has never renamed its settings files
	inline constexpr const char* kOldSettingsPath = "";
	inline constexpr const char* kOldItemsPath = "";
	inline constexpr const char* kOldAdvancedPath = "";
	// the fading module's DevBench names (FadeDevBench.cpp): its inspect kind and menu alias, its tool, its event
	inline constexpr const char* kDevBenchKey = "illuminatedfade";
	inline constexpr const char* kDevBenchTool = "illuminatedfade.control";
	inline constexpr const char* kDevBenchRulesEvent = "illuminatedfade.rulesLoaded";
}
