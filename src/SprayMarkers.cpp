// Luminous Arcana - SKSE plugin
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE.txt and the notice at the top of main.cpp.
//
// The spray settings: whether spray lights are on, and their reach, fade and colors. They were marker files an
// installer put down; now they are marker rows in the settings file, each on while the menu's settings say so.
// Read once when the game loads its data: the spray lights are copied then, so a change shows after a restart.

#include "Plugin.h"

namespace Plugin
{
	// ------------------------------------------------------------------ the spray markers
	std::map<std::string, std::string> ReadMarkerValues(std::string_view a_values)
	{
		std::map<std::string, std::string> out;
		std::size_t                        start = 0;
		while (start <= a_values.size()) {
			const auto end = a_values.find(';', start);
			const auto t = Trim(a_values.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start));
			if (const auto eq = t.find('='); !t.empty() && t[0] != '#' && eq != std::string::npos && eq > 0) {
				out[Lower(Trim(t.substr(0, eq)))] = Trim(t.substr(eq + 1));
			}
			if (end == std::string_view::npos) {
				break;
			}
			start = end + 1;
		}
		return out;
	}

	bool ParseRgb(std::string_view a_text, Rgb& a_out)
	{
		const auto c1 = a_text.find(',');
		const auto c2 = c1 == std::string_view::npos ? c1 : a_text.find(',', c1 + 1);
		if (c1 == std::string_view::npos || c2 == std::string_view::npos) {
			return false;
		}
		return ParseInt(a_text.substr(0, c1), a_out.r) && ParseInt(a_text.substr(c1 + 1, c2 - c1 - 1), a_out.g) &&
		       ParseInt(a_text.substr(c2 + 1), a_out.b);
	}

	SprayChoice ReadSprayChoice()
	{
		SprayChoice                                                     sc;
		std::vector<std::pair<std::string, std::map<std::string, std::string>>> markers;
		for (const auto& m : SprayMarkers()) {
			if (ConditionsHold(m.when)) {
				markers.emplace_back(m.name, ReadMarkerValues(m.values));
			}
		}
		// the base marker first, so every axis marker wins over it whatever order the file lists them
		for (auto& [name, kv] : markers) {
			if (name != "luminous arcana sprays.txt") {
				continue;
			}
			sc.on = true;
			sc.found += "on ";
			int   n = 0;
			float f = 0.0f;
			if (ParseInt(kv["radiuspc"], n) && n > 0) sc.radiusPc = n;
			if (ParseInt(kv["radius"], n) && n >= 0) sc.radiusAbs = n;
			if (ParseFloat(kv["fade"], f)) sc.fade = f;
			if (ParseFloat(kv["frostfade"], f)) sc.frostFade = f;
			if (ParseFloat(kv["falloff"], f)) sc.falloff = f;
		}
		for (auto& [name, kv] : markers) {
			float f = 0.0f;
			if (name == "luminous arcana sprays - reduced.txt") {
				sc.found += "reduced ";
				if (ParseFloat(kv["fade"], f)) sc.fade = f;
				if (ParseFloat(kv["frostfade"], f)) sc.frostFade = f;
			} else if (name == "luminous arcana sprays - frost.txt" && ParseRgb(kv["frost"], sc.frost)) {
				sc.frostSet = true;
				sc.found += "frost ";
			} else if (name == "luminous arcana sprays - shock.txt" && ParseRgb(kv["shock"], sc.shock)) {
				sc.shockSet = true;
				sc.found += "shock ";
			} else if (name == "luminous arcana sprays - fire.txt" && ParseRgb(kv["firedelta"], sc.fireDelta)) {
				sc.fireSet = true;
				sc.found += "fire ";
			}
		}
		return sc;
	}
}
