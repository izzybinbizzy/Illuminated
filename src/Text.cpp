// Luminous Arcana - SKSE plugin
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE.txt and the notice at the top of main.cpp.
//
// Small text helpers: lower case, paths, trimming and number parsing.

#include "Plugin.h"

namespace Plugin
{
	// ------------------------------------------------------------------ small text helpers
	std::string Lower(std::string_view a_text)
	{
		std::string out(a_text);
		for (auto& c : out) {
			c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		}
		return out;
	}

	std::string NormalPath(std::string_view a_path)
	{
		std::string out = Lower(a_path);
		std::replace(out.begin(), out.end(), '/', '\\');
		return out;
	}

	bool Contains(std::string_view a_text, std::string_view a_part)
	{
		return a_text.find(a_part) != std::string_view::npos;
	}

	std::string Trim(std::string_view a_text)
	{
		const auto first = a_text.find_first_not_of(" \t\r\n");
		if (first == std::string_view::npos) {
			return {};
		}
		const auto last = a_text.find_last_not_of(" \t\r\n");
		return std::string(a_text.substr(first, last - first + 1));
	}

	bool ParseInt(std::string_view a_text, int& a_out)
	{
		const auto t = Trim(a_text);
		const auto r = std::from_chars(t.data(), t.data() + t.size(), a_out);
		return r.ec == std::errc() && r.ptr == t.data() + t.size();
	}

	bool ParseFloat(std::string_view a_text, float& a_out)
	{
		const auto t = Trim(a_text);
		const auto r = std::from_chars(t.data(), t.data() + t.size(), a_out);
		return r.ec == std::errc() && r.ptr == t.data() + t.size();
	}
}
