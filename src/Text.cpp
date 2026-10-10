// Illuminated - SKSE plugin
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE.txt and the notice at the top of main.cpp.
//
// Small text helpers for paths (lower case is LightMath.h's LightKit::Lower; trimming and numbers are ConfigText.h's).

#include "Plugin.h"

namespace Plugin
{
	// ------------------------------------------------------------------ small text helpers
	std::string NormalPath(std::string_view a_path)
	{
		std::string out = Lower(a_path);
		std::ranges::replace(out, '/', '\\');
		return out;
	}

	bool Contains(std::string_view a_text, std::string_view a_part)
	{
		return a_text.find(a_part) != std::string_view::npos;
	}
}
