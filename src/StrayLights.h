// Illuminated - SKSE plugin
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE.txt and the notice at the top of main.cpp.
//
// Which lights are ours among Light Placer's, by the name Light Placer gives them (HerShadow.cpp moves only ours). Tested off the game.

#pragma once

#include "LightMath.h"

#include <string_view>

namespace StrayLights
{
	// every light Light Placer makes from one of our files is named LP_Light[Illuminated\<folder>\<file>|<id>]#n; another mod's
	// Light Placer light never starts this way
	inline constexpr std::string_view kOurs = "LP_Light[Illuminated\\";

	// without case: Light Placer names the folder as the mod manager merged it, "illuminated" as well as "Illuminated"
	[[nodiscard]] inline bool IsOurs(std::string_view a_name) noexcept { return LightKit::SameText(a_name.substr(0, kOurs.size()), kOurs); }
}
