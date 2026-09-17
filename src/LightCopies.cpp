// Luminous Arcana - SKSE plugin
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE.txt and the notice at the top of main.cpp.
//
// The Brightness and Reach sliders. A Light Placer light that states no fade or radius of its own takes them from its
// light record, and reads them each time it makes a light (Light Placer 4.2.1, LightData.cpp GetFade/GetRadius). So
// every light the configs name is an in-memory copy made here - one per light record, fade and radius the configs
// used - and its fade and radius are the made-with values times the sliders. A copy is found by its editor ID, which
// Light Placer looks up when it reads its configs, after this runs. The copies are listed in the settings files
// ([light] blocks), written by lagen.py.

#include "Plugin.h"

namespace Plugin
{
	namespace
	{
		constexpr std::string_view kBrightness = "LuminousArcanaBrightness";
		constexpr std::string_view kReach = "LuminousArcanaReach";
	}

	void MakeLightCopies()
	{
		std::size_t made = 0, missing = 0, failed = 0;
		for (auto& c : LightCopies()) {
			auto* base = RE::TESForm::LookupByEditorID<RE::TESObjectLIGH>(c.base);
			if (!base) {
				++missing;
				SKSE::log::warn("[LIGHTCOPY-MISSING] {} | its light {} is not in this load order; the lights that name it stay dark", c.id, c.base);
				continue;
			}
			auto* copy = CopyLight(base);
			if (!copy || !RegisterEditorID(copy, c.id) || RE::TESForm::LookupByEditorID<RE::TESObjectLIGH>(c.id) != copy) {
				++failed;
				SKSE::log::warn("[LIGHTCOPY-FAILED] {} | could not make a findable copy of {}", c.id, c.base);
				continue;
			}
			c.form = copy;
			c.startFade = c.fade > 0.0f ? c.fade : base->fade;
			c.startRadius = c.radius > 0 ? static_cast<std::uint32_t>(c.radius) : base->data.radius;
			++made;
		}
		SKSE::log::info("light copies: {} made, {} whose light is not in this load order, {} failed", made, missing, failed);
		ApplyLightStrength(true);
	}

	void ApplyLightStrength(bool a_log)
	{
		const int brightness = SettingValue(kBrightness, 100);
		const int reach = SettingValue(kReach, 100);
		std::size_t set = 0;
		for (auto& c : LightCopies()) {
			if (!c.form) {
				continue;
			}
			c.form->fade = c.startFade * static_cast<float>(brightness) / 100.0f;
			c.form->data.radius = static_cast<std::uint32_t>(std::lround(static_cast<double>(c.startRadius) * reach / 100.0));
			++set;
		}
		SKSE::log::info("light strength: brightness {}%, reach {}%, on {} light copies", brightness, reach, set);
		if (a_log) {
			for (const auto& c : LightCopies()) {
				if (c.form) {
					SKSE::log::info("[LIGHTCOPY] {} | {} | fade {} radius {}", c.id, c.base, c.form->fade, c.form->data.radius);
				}
			}
		}
	}
}
