// Illuminated - SKSE plugin
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE.txt and the notice at the top of main.cpp.
//
// Pass 4: with the Spray Lights setting on (its marker row in the settings file, SprayMarkers.cpp), each spray projectile gets a
// stretched copy of its light. It answers to that setting only: his answer 2026-10-13 - Streams off does not touch the sprays
// Spray Lights brightens, two settings, each doing one thing (pass 6, StreamLights.cpp, leaves these copies alone).

#include "Plugin.h"

namespace Plugin
{
	// ------------------------------------------------------------------ pass 4: spray lights
	namespace
	{
		constexpr std::uint32_t kSprayFlags = 0x2001;  // Dynamic | Portal-strict

		std::unordered_map<const RE::BGSProjectile*, RE::TESObjectLIGH*> gOriginal;  // each raised spray's light before it (data load)

		std::uint8_t Clamp255(int a_value)
		{
			return static_cast<std::uint8_t>(std::clamp(a_value, 0, 255));
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
			const int range = static_cast<int>(std::lround(proj->data.range));
			if (range <= 0) {
				++failed;
				SKSE::log::warn("[SPRAY-FAILED] {} | could not read its range", Label(proj));
				continue;
			}
			const int  radius = sc.radiusAbs > 0 ? sc.radiusAbs : static_cast<int>(std::lround(range * sc.radiusPc / 100.0));
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
			if (family == Family::kFrost && sc.frostSet) {
				copy->data.color.red = Clamp255(sc.frost.r);
				copy->data.color.green = Clamp255(sc.frost.g);
				copy->data.color.blue = Clamp255(sc.frost.b);
			} else if (family == Family::kShock && sc.shockSet) {
				copy->data.color.red = Clamp255(sc.shock.r);
				copy->data.color.green = Clamp255(sc.shock.g);
				copy->data.color.blue = Clamp255(sc.shock.b);
			} else if (family == Family::kFire && sc.fireSet) {
				copy->data.color.red = Clamp255(copy->data.color.red + sc.fireDelta.r);
				copy->data.color.green = Clamp255(copy->data.color.green + sc.fireDelta.g);
				copy->data.color.blue = Clamp255(copy->data.color.blue + sc.fireDelta.b);
			}
			proj->data.light = copy;
			if (proj->data.light != copy || copy->data.radius != static_cast<std::uint32_t>(radius)) {
				++failed;
				SKSE::log::warn("[SPRAY-FAILED] {} | the new light did not take", Label(proj));
				continue;
			}
			gOriginal[proj] = bulb;
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

	RE::TESObjectLIGH* SprayOriginal(const RE::BGSProjectile* a_projectile)
	{
		const auto it = gOriginal.find(a_projectile);
		return it == gOriginal.end() ? nullptr : it->second;
	}
}
