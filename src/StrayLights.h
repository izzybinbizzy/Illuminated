// Illuminated - SKSE plugin
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE.txt and the notice at the top of main.cpp.
//
// Which of our Light Placer lights have lost their node: clearing a spell's effects while its casting art is up tears the art down
// without Light Placer taking its light out of the scene, and the light stays lit with no parent. The game-free part of the sweep
// that puts such a light out (LightPlacerQueue.cpp); tested off the game.

#pragma once

#include <algorithm>
#include <cstddef>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace StrayLights
{
	// every light Light Placer makes from one of our files is named LP_Light[Illuminated\<folder>\<file>|<id>]#n; another mod's
	// Light Placer light never starts this way
	inline constexpr std::string_view kOurs = "LP_Light[Illuminated\\";
	// looks in a row a light must stand without a node before it goes: Light Placer detaches one a moment before the scene removes it
	inline constexpr int kLooks = 2;

	[[nodiscard]] constexpr bool IsOurs(std::string_view a_name) noexcept { return a_name.starts_with(kOurs); }

	struct Seen
	{
		const void*      key{ nullptr };  // the NiLight, however many registrations the scene holds of it
		std::string_view name;
		bool             hasParent{ true };
		bool             removalQueued{ false };  // Light Placer or the scene is taking it out already
	};

	class Watch
	{
	public:
		// the lights seen now -> the indices of those to put out; a light that is not stray in this look is forgotten
		[[nodiscard]] std::vector<std::size_t> Look(std::span<const Seen> a_now)
		{
			std::vector<std::pair<const void*, int>> next;
			std::vector<std::size_t>                 out;
			for (std::size_t i = 0; i < a_now.size(); ++i) {
				const auto& s = a_now[i];
				if (!s.key || s.hasParent || s.removalQueued || !IsOurs(s.name) || Counted(next, out, a_now, s.key)) {
					continue;
				}
				int looks = 1;
				for (const auto& [key, n] : counts_) {
					if (key == s.key) {
						looks = n + 1;
						break;
					}
				}
				if (looks >= kLooks) {
					out.push_back(i);
				} else {
					next.emplace_back(s.key, looks);
				}
			}
			counts_ = std::move(next);
			return out;
		}

		void Clear() { counts_.clear(); }

	private:
		// the scene may list one light twice (a light and its shadow registration): it is counted once a look
		static bool Counted(const std::vector<std::pair<const void*, int>>& a_next, const std::vector<std::size_t>& a_out, std::span<const Seen> a_now, const void* a_key)
		{
			return std::ranges::any_of(a_next, [a_key](const auto& a_kv) { return a_kv.first == a_key; }) ||
			       std::ranges::any_of(a_out, [&](std::size_t a_i) { return a_now[a_i].key == a_key; });
		}

		std::vector<std::pair<const void*, int>> counts_;
	};
}
