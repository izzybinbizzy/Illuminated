// Luminous Arcana - SKSE plugin
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE.txt and the notice at the top of main.cpp.
//
// Reads the installer's spray marker files: whether spray lights are on, and their settings.

#include "Plugin.h"

namespace Plugin
{
	// ------------------------------------------------------------------ the installer's spray markers
	std::map<std::string, std::string> ReadMarker(const fs::path& a_file)
	{
		std::map<std::string, std::string> out;
		std::ifstream                      in(a_file);
		std::string                        line;
		while (std::getline(in, line)) {
			const auto t = Trim(line);
			if (t.empty() || t[0] == '#') {
				continue;
			}
			const auto eq = t.find('=');
			if (eq == std::string::npos || eq == 0) {
				continue;
			}
			out[Lower(Trim(t.substr(0, eq)))] = Trim(t.substr(eq + 1));
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
		SprayChoice                                   sc;
		std::vector<std::pair<std::string, fs::path>> markers;
		for (const auto folder : { kOurFolder, kCSFolder }) {
			std::error_code ec;
			const auto      dir = LightPlacerDir(folder);
			if (!fs::is_directory(dir, ec)) {
				continue;
			}
			for (const auto& entry : fs::recursive_directory_iterator(dir, ec)) {
				if (entry.is_regular_file() && Lower(entry.path().extension().string()) == ".txt") {
					markers.emplace_back(Lower(entry.path().filename().string()), entry.path());
				}
			}
		}
		// the base marker first, so every axis marker wins over it whatever order the folder lists them
		for (const auto& [name, path] : markers) {
			if (name != "luminous arcana sprays.txt") {
				continue;
			}
			sc.on = true;
			sc.found += "on ";
			auto kv = ReadMarker(path);
			int  n = 0;
			float f = 0.0f;
			if (ParseInt(kv["radiuspc"], n) && n > 0) sc.radiusPc = n;
			if (ParseInt(kv["radius"], n) && n >= 0) sc.radiusAbs = n;
			if (ParseFloat(kv["fade"], f)) sc.fade = f;
			if (ParseFloat(kv["frostfade"], f)) sc.frostFade = f;
			if (ParseFloat(kv["falloff"], f)) sc.falloff = f;
		}
		for (const auto& [name, path] : markers) {
			auto kv = ReadMarker(path);
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
