#pragma once

// Illuminated - SKSE plugin
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE.txt and the notice at the top of main.cpp.
//
// 360 Ward's sphere and hit flash: its own, or recolored in place by another mod (360 Ward - Other colors), and the color a
// recolored one shows. Game-free, so the off-game tests read it.

#include <algorithm>
#include <cmath>
#include <optional>
#include <span>

namespace WardColor
{
	struct Rgb
	{
		float r{ 0.0f };
		float g{ 0.0f };
		float b{ 0.0f };
	};

	// every block of 360 Ward's own sphere and flash that carries a hue wears this orange
	inline constexpr Rgb k360Orange{ 1.0f, 0.6f, 0.3f };

	[[nodiscard]] inline bool Near(const Rgb& a_a, const Rgb& a_b)
	{
		return std::abs(a_a.r - a_b.r) < 0.05f && std::abs(a_a.g - a_b.g) < 0.05f && std::abs(a_a.b - a_b.b) < 0.05f;
	}

	// a white or grey block carries no hue of the model's own
	[[nodiscard]] inline bool Colored(const Rgb& a_c)
	{
		const float hi = (std::max)({ a_c.r, a_c.g, a_c.b });
		const float lo = (std::min)({ a_c.r, a_c.g, a_c.b });
		return hi > 0.0f && (hi - lo) / hi > 0.2f;
	}

	// none = 360 Ward's own (a block in its orange) or nothing read; else the average of the colored blocks, or of every block
	// when the recolor is white
	[[nodiscard]] inline std::optional<Rgb> Recolor(std::span<const Rgb> a_blocks)
	{
		if (a_blocks.empty() || std::ranges::any_of(a_blocks, [](const Rgb& a_c) { return Near(a_c, k360Orange); })) {
			return std::nullopt;
		}
		const bool anyColored = std::ranges::any_of(a_blocks, Colored);
		Rgb        sum{};
		int        count = 0;
		for (const auto& c : a_blocks) {
			if (!anyColored || Colored(c)) {
				sum.r += c.r;
				sum.g += c.g;
				sum.b += c.b;
				++count;
			}
		}
		return Rgb{ sum.r / count, sum.g / count, sum.b / count };
	}
}
