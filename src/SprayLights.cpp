// Luminous Arcana - SKSE plugin
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE.txt and the notice at the top of main.cpp.
//
// Pass 4: with the Spray Lights option installed, each spray projectile gets a stretched copy of its light.

#include "Plugin.h"

namespace Plugin
{
	// ------------------------------------------------------------------ pass 4: spray lights
	const std::set<std::string> kSprayNotSpells{ "trapspotlightprojectile", "sum_any_projectile_defaultcloakprojectile" };
	constexpr std::uint32_t kSprayFlags = 0x2001;  // Dynamic | Portal-strict

	std::string SprayFamily(const std::string& a_editorID)
	{
		const auto id = Lower(a_editorID);
		if (Contains(id, "frost") || Contains(id, "ice")) return "frost";
		if (Contains(id, "flame") || Contains(id, "fire")) return "fire";
		if (Contains(id, "shock") || Contains(id, "lightning")) return "shock";
		return {};
	}

	std::uint8_t Clamp255(int a_value)
	{
		return static_cast<std::uint8_t>(std::clamp(a_value, 0, 255));
	}

	void SprayLights()
	{
		const auto sc = ReadSprayChoice();
		if (!sc.on) {
			SKSE::log::info("spray lights: off (no Luminous Arcana Sprays.txt installed); no spray light is touched");
			return;
		}
		SKSE::log::info("spray lights: installer says {}| radius {} | fade {} | frost fade {} | falloff {}", sc.found,
			sc.radiusAbs, sc.fade, sc.frostFade, sc.falloff);
		std::size_t seen = 0, raised = 0, noLight = 0, poison = 0, named = 0, failed = 0;
		for (auto* proj : RE::TESDataHandler::GetSingleton()->GetFormArray<RE::BGSProjectile>()) {
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
			if (Contains(low, "poison")) {
				++poison;
				proj->data.light = nullptr;
				continue;
			}
			if (kSprayNotSpells.contains(low)) {
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
			const int radius = sc.radiusAbs > 0 ? sc.radiusAbs : static_cast<int>(std::lround(range * sc.radiusPc / 100.0));
			const auto family = SprayFamily(id);
			auto*      copy = CopyLight(bulb);
			if (!copy) {
				++failed;
				SKSE::log::warn("[SPRAY-FAILED] {} | could not copy its light", Label(proj));
				continue;
			}
			copy->data.radius = static_cast<std::uint32_t>(radius);
			copy->fade = family == "frost" ? sc.frostFade : sc.fade;
			copy->data.flags = static_cast<RE::TES_LIGHT_FLAGS>(kSprayFlags);
			copy->data.fallofExponent = sc.falloff;
			if (family == "frost" && sc.frostSet) {
				copy->data.color.red = Clamp255(sc.frost.r);
				copy->data.color.green = Clamp255(sc.frost.g);
				copy->data.color.blue = Clamp255(sc.frost.b);
			} else if (family == "shock" && sc.shockSet) {
				copy->data.color.red = Clamp255(sc.shock.r);
				copy->data.color.green = Clamp255(sc.shock.g);
				copy->data.color.blue = Clamp255(sc.shock.b);
			} else if (family == "fire" && sc.fireSet) {
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
			++raised;
			SKSE::log::info("[SPRAY-RAISED] {} | range {} | radius {} | from {} | family {} | colour {},{},{}", Label(proj), range,
				radius, EditorID(bulb), family, copy->data.color.red, copy->data.color.green, copy->data.color.blue);
		}
		SKSE::log::info("spray lights: {} spray projectiles seen; {} raised, {} with no light of their own, {} poison left dark, "
						"{} named as not spray spells, {} failed",
			seen, raised, noLight, poison, named, failed);
	}
}
