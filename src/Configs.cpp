// Luminous Arcana - SKSE plugin
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE.txt and the notice at the top of main.cpp.
//
// Reads the Light Placer configs and collects what they light: model paths and shader names.

#include "Plugin.h"

namespace Plugin
{
	// ------------------------------------------------------------------ what the configs light
	fs::path LightPlacerDir(std::string_view a_folder)
	{
		return fs::current_path() / "Data" / "LightPlacer" / fs::path(std::string(a_folder));
	}

	// A small reader for the one shape these configs have. It never fails a file: a string it cannot
	// place is simply not counted, which is the patcher's behaviour too.
	void ReadConfig(const fs::path& a_file, Coverage& a_cov)
	{
		std::ifstream in(a_file, std::ios::binary);
		if (!in) {
			return;
		}
		std::stringstream buf;
		buf << in.rdbuf();
		const std::string text = buf.str();
		++a_cov.files;

		std::string lastString;
		bool        lastWasKey = false;
		int         formIDsDepth = -1;  // bracket depth at which a formIDs array opened
		int         depth = 0;
		bool        pendingFormIDs = false;

		for (std::size_t i = 0; i < text.size(); ++i) {
			const char c = text[i];
			if (c == '"') {
				std::string s;
				for (++i; i < text.size() && text[i] != '"'; ++i) {
					if (text[i] == '\\' && i + 1 < text.size()) {
						++i;
						s.push_back(text[i] == 'n' ? '\n' : text[i] == 't' ? '\t' : text[i]);
					} else {
						s.push_back(text[i]);
					}
				}
				std::size_t j = i + 1;
				while (j < text.size() && std::isspace(static_cast<unsigned char>(text[j]))) {
					++j;
				}
				const bool isKey = j < text.size() && text[j] == ':';
				if (isKey) {
					pendingFormIDs = Lower(s) == "formids";
				} else {
					const auto low = NormalPath(s);
					if (low.size() > 4 && low.ends_with(".nif")) {
						a_cov.models.insert(low);
					} else if (formIDsDepth >= 0 && low.size() > 2) {
						a_cov.shaders.insert(low);
					}
				}
				lastString = std::move(s);
				lastWasKey = isKey;
				continue;
			}
			if (c == '[') {
				++depth;
				if (pendingFormIDs && formIDsDepth < 0) {
					formIDsDepth = depth;
				}
				pendingFormIDs = false;
			} else if (c == ']') {
				if (formIDsDepth == depth) {
					formIDsDepth = -1;
				}
				--depth;
			} else if (c == '{' || c == ',') {
				if (!lastWasKey) {
					pendingFormIDs = false;
				}
			}
		}
	}

	Coverage ReadCoverage()
	{
		Coverage cov;
		for (const auto folder : { kOurFolder, kCSFolder }) {
			std::error_code ec;
			const auto      dir = LightPlacerDir(folder);
			if (!fs::is_directory(dir, ec)) {
				continue;
			}
			for (const auto& entry : fs::recursive_directory_iterator(dir, ec)) {
				if (entry.is_regular_file() && Lower(entry.path().extension().string()) == ".json") {
					ReadConfig(entry.path(), cov);
				}
			}
		}
		return cov;
	}
}
