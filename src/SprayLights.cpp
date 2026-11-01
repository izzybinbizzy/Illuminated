// Illuminated - SKSE plugin
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE.txt and the notice at the top of main.cpp.
//
// Pass 4: with the Spray Lights setting on (its marker row in the settings file, SprayMarkers.cpp), each spray projectile gets a
// copy of its light that carries the color its row of lights wears (StreamLights.cpp lights the length); the spray itself wears
// its own light, or that light in the art pick's color (ApplySprayColors). It answers to that setting only: his answer
// 2026-10-13 - Streams off does not touch the sprays Spray Lights lights, two settings, each doing one thing.

#include "Plugin.h"

namespace Plugin
{
	// ------------------------------------------------------------------ pass 4: spray lights
	namespace
	{
		constexpr std::uint32_t kSprayFlags = 0x2001;  // Dynamic | Portal-strict

		std::unordered_map<const RE::BGSProjectile*, RE::TESObjectLIGH*> gOriginal;  // each raised spray's light before it (data load)

		// each raised copy, colored again on every settings refresh: an art pick in the menu moves a spray's light at once (his
		// 2026-10-10 "needs to be live"; 2026-10-19 his Frostbite kept the old pick's color until a restart)
		struct SprayCopy
		{
			RE::BGSProjectile* proj{ nullptr };
			RE::TESObjectLIGH* original{ nullptr };  // the light the spray had before (worn again while all lights are off)
			RE::TESObjectLIGH* copy{ nullptr };
			Family             family{ Family::kNone };
			RE::Color          own{};                // the color of the light it was copied from
			bool               vanillaArt{ false };  // the game's own art, which the art picks recolor
			RE::TESObjectLIGH* tint{ nullptr };      // the original in the pick's color, worn in its place on the game's art
		};
		std::vector<SprayCopy> gCopies;

		// 64-bit: a marker's shift may be any int, and a channel plus INT_MAX must not overflow before the clamp (CodeRabbit, #26)
		std::uint8_t Clamp255(std::int64_t a_value)
		{
			return static_cast<std::uint8_t>(std::clamp<std::int64_t>(a_value, 0, 255));
		}

		// the copy's color for the settings as they are now: the frost / shock marker's, the fire marker's shift on its own color,
		// else its own color
		void Recolor(const SprayCopy& a_s, const SprayChoice& a_sc)
		{
			auto& color = a_s.copy->data.color;
			if (const auto hand = a_s.vanillaArt ? ArtHandColor(a_s.family) : std::nullopt) {
				color.red = hand->red;
				color.green = hand->green;
				color.blue = hand->blue;
			} else if (a_s.family == Family::kFrost && a_sc.frostSet) {
				color.red = Clamp255(a_sc.frost.r);
				color.green = Clamp255(a_sc.frost.g);
				color.blue = Clamp255(a_sc.frost.b);
			} else if (a_s.family == Family::kShock && a_sc.shockSet) {
				color.red = Clamp255(a_sc.shock.r);
				color.green = Clamp255(a_sc.shock.g);
				color.blue = Clamp255(a_sc.shock.b);
			} else if (a_s.family == Family::kFire && a_sc.fireSet) {
				color.red = Clamp255(std::int64_t{ a_s.own.red } + a_sc.fireDelta.r);
				color.green = Clamp255(std::int64_t{ a_s.own.green } + a_sc.fireDelta.g);
				color.blue = Clamp255(std::int64_t{ a_s.own.blue } + a_sc.fireDelta.b);
			} else {
				color.red = a_s.own.red;
				color.green = a_s.own.green;
				color.blue = a_s.own.blue;
			}
			color = VanillaLook(color);
		}

		constexpr std::string_view FamilyName(Family a_family)
		{
			switch (a_family) {
			case Family::kFire:
				return "fire";
			case Family::kFrost:
				return "frost";
			case Family::kShock:
				return "shock";
			default:
				return "";
			}
		}
	}

	void SprayLights()
	{
		const auto sc = ReadSprayChoice();
		gOriginal.clear();
		gCopies.clear();
		if (!sc.on) {
			SKSE::log::info("spray lights: off (the Spray Lights setting's marker does not hold); no spray light is touched");
			return;
		}
		SKSE::log::info("spray lights: the markers say {}| radius {} | fade {} | frost fade {} | falloff {}", sc.found,
			sc.radiusAbs, sc.fade, sc.frostFade, sc.falloff);
		std::size_t seen = 0, raised = 0, noLight = 0, poison = 0, named = 0, failed = 0;
		for (auto* proj : LightKit::FormsOf<RE::BGSProjectile>()) {
			if (!proj) {
				continue;
			}
			const auto model = NormalPath(proj->GetModel() ? proj->GetModel() : "");
			if (!Contains(model, "spray")) {
				continue;
			}
			++seen;
			const auto id = EditorID(proj);
			const auto low = Lower(id);
			if (Contains(low, "poison") || Contains(low, "poision")) {
				++poison;  // kept dark by pass 2, which also follows the settings for it
				continue;
			}
			if (std::ranges::find(kNotSpellProjectiles, low) != kNotSpellProjectiles.end()) {
				++named;
				continue;
			}
			auto* bulb = proj->data.light;
			if (!bulb) {
				++noLight;
				continue;
			}
			const int range = std::isfinite(proj->data.range) ? static_cast<int>(std::lround(std::clamp(proj->data.range, 0.0f, 1.0e7f))) : 0;
			if (range <= 0) {
				++failed;
				SKSE::log::warn("[SPRAY-FAILED] {} | could not read its range", Label(proj));
				continue;
			}
			const int  radius = sc.radiusAbs > 0 ? sc.radiusAbs :
			                                       static_cast<int>(std::lround(std::clamp(static_cast<double>(range) * sc.radiusPc / 100.0, 0.0, 1.0e7)));
			const auto family = FamilyOf(id, false);
			auto*      copy = CopyLight(bulb);
			if (!copy) {
				++failed;
				SKSE::log::warn("[SPRAY-FAILED] {} | could not copy its light", Label(proj));
				continue;
			}
			copy->data.radius = static_cast<std::uint32_t>(radius);
			copy->fade = family == Family::kFrost ? sc.frostFade : sc.fade;
			copy->data.flags = static_cast<RE::TES_LIGHT_FLAGS>(kSprayFlags);
			copy->data.fallofExponent = sc.falloff;
			SprayCopy sprayed{ proj, bulb, copy, family, bulb->data.color, VanillaArtModel(model) };
			if (sprayed.vanillaArt && family != Family::kNone) {
				sprayed.tint = CopyLight(bulb);
				if (const auto lc = std::ranges::find(LightCopies(), bulb, &LightCopy::form); sprayed.tint && lc != LightCopies().end()) {
					auto entry = *lc;
					entry.id = "tint " + id;
					entry.form = sprayed.tint;
					LightCopies().push_back(std::move(entry));
				}
			}
			Recolor(sprayed, sc);     // and again on every refresh (ApplySprayColors)
			proj->data.light = copy;  // read by pass 6 as this spray's row color, then the original goes back (ApplySprayColors)
			if (proj->data.light != copy || copy->data.radius != static_cast<std::uint32_t>(radius)) {
				++failed;
				SKSE::log::warn("[SPRAY-FAILED] {} | the new light did not take", Label(proj));
				continue;
			}
			gOriginal[proj] = bulb;
			gCopies.push_back(sprayed);
			LightCopies().push_back({ .id = "spray " + EditorID(proj), .base = EditorID(bulb), .form = copy, .startFade = copy->fade, .startRadius = copy->data.radius });
			++raised;
			SKSE::log::info("[SPRAY-RAISED] {} | range {} | radius {} | from {} | family {} | color {},{},{}", Label(proj), range,
				radius, EditorID(bulb), FamilyName(family), copy->data.color.red, copy->data.color.green, copy->data.color.blue);
		}
		SKSE::log::info(
			"spray lights: {} spray projectiles seen; {} raised, {} with no light of their own, {} poison left to pass 2, "
			"{} named as not spray spells, {} failed",
			seen, raised, noLight, poison, named, failed);
	}

	void ApplySprayColors()
	{
		if (gCopies.empty()) {
			return;
		}
		const auto sc = ReadSprayChoice();
		// every spray wears the game's own light: its length is lit by a row of small lights along its aim (StreamLights.cpp,
		// his 2026-10-20 "Do the flame sprays and cones in Illuminated first" - the one stretched copy flooded the scene, his
		// "the spray lighting in the background is ruining all of the feature images"); the copy carries the row's color
		// (on Performance there are no rows either, and the spray keeps the game's own light - the raised copy flooded the scene)
		for (const auto& s : gCopies) {
			HoldFlicker(s.original, FlickerOn());  // Flicker off: no flicker on the spray either (the record itself, so the sliders reach it)
			Recolor(s, sc);
			// the record's own color is vanilla's: on the game's art the spray wears it in the copy's color, unless all lights are off
			if (s.tint && !LightsOff()) {
				s.tint->data.color = s.copy->data.color;
				HoldFlicker(s.tint, FlickerOn());
				s.proj->data.light = s.tint;
			} else {
				s.proj->data.light = s.original;
			}
		}
#ifdef FADE_DEBUG_PAGE
		// a held Frostbite after a Frost Art switch still wore the old pick's color in game while every refresh ran: one line
		// per family says what its copies hold now, so the log tells "the copy did not change" from "the game did not take
		// the copy's new color" (the testing DLL: every slider step refreshes)
		for (const auto family : { Family::kFire, Family::kFrost, Family::kShock }) {
			const auto first = std::ranges::find(gCopies, family, &SprayCopy::family);
			if (first == gCopies.end()) {
				continue;
			}
			const auto  n = std::ranges::count(gCopies, family, &SprayCopy::family);
			const auto& c = first->copy->data.color;
			const bool  held = family == Family::kFrost ? sc.frostSet : family == Family::kShock ? sc.shockSet :
			                                                                                       sc.fireSet;
			SKSE::log::info("[SPRAY-COLOR] {} | {} copies | now {},{},{} | own {},{},{} | marker {}", FamilyName(family), n, c.red,
				c.green, c.blue, first->own.red, first->own.green, first->own.blue, held ? "holds" : "does not hold");
		}
#endif
	}

	RE::TESObjectLIGH* SprayOriginal(const RE::BGSProjectile* a_projectile)
	{
		const auto it = gOriginal.find(a_projectile);
		return it == gOriginal.end() ? nullptr : it->second;
	}

	const RE::TESObjectLIGH* SprayCopyOf(const RE::BGSProjectile* a_projectile)
	{
		const auto it = std::ranges::find(gCopies, a_projectile, &SprayCopy::proj);
		return it == gCopies.end() ? nullptr : it->copy;
	}
}
