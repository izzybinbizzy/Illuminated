// Luminous Arcana - SKSE plugin
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE.txt and the notice at the top of main.cpp.
//
// Pass 2: projectiles, explosions and hazards whose model is lit lose the game's own light - for as long as the
// menu's settings keep that model lit. Each one's own light is remembered, so switching an option off gives it back.
// Also RefreshLights, which runs passes 1 and 2 again (and puts the sliders onto the light copies) whenever a setting changes.

#include "Plugin.h"

namespace Plugin
{
	// ------------------------------------------------------------------ pass 2: projectiles, explosions, hazards
	const std::set<std::string> kForceNullProjectiles{ "tvr_geist_projectile" };

	bool IsPoisonSpray(const std::string& a_editorID, const std::string& a_model)
	{
		const auto id = Lower(a_editorID);
		return Contains(a_model, "spray") && (Contains(id, "poison") || Contains(id, "poision"));
	}

	struct EffectTarget
	{
		RE::TESObjectLIGH** slot;  // the form's light
		RE::TESObjectLIGH*  own;
		std::string         model;
		std::string         label;
		bool                always;  // named or a poison spray: dark whatever the settings say
	};
	std::vector<EffectTarget> gEffectTargets;
	const Coverage*           gEffectCoverage = nullptr;

	void ApplyEffectLights(bool a_log)
	{
		std::size_t nulled = 0, restored = 0;
		for (auto& t : gEffectTargets) {
			const bool lit = t.always || (gEffectCoverage && gEffectCoverage->ModelLit(t.model));
			if (lit && *t.slot) {
				*t.slot = nullptr;
				++nulled;
				if (a_log) {
					SKSE::log::info("[FX-NULLED] {} | {}", t.label, t.model);
				}
			} else if (!lit && *t.slot != t.own) {
				*t.slot = t.own;
				++restored;
				if (a_log) {
					SKSE::log::info("[FX-KEPT] {} | {} | no setting lights it now", t.label, t.model);
				}
			}
		}
		SKSE::log::info("projectile, explosion and hazard lights for the settings as they are: {} taken off, {} given back, {} followed",
			nulled, restored, gEffectTargets.size());
	}

	template <class T>
	void EffectLights(const Coverage& a_cov, std::string_view a_kind)
	{
		gEffectCoverage = &a_cov;
		std::size_t scanned = 0, clean = 0, followed = 0, coneKept = 0, poison = 0, forced = 0;
		for (auto* form : RE::TESDataHandler::GetSingleton()->GetFormArray<T>()) {
			if (!form) {
				continue;
			}
			const auto model = NormalPath(form->GetModel() ? form->GetModel() : "");
			if (model.empty()) {
				continue;
			}
			const auto id = EditorID(form);
			const bool force = kForceNullProjectiles.contains(Lower(id));
			const bool poisonSpray = IsPoisonSpray(id, model);
			if (!a_cov.models.contains(model) && !force && !poisonSpray) {
				continue;
			}
			if (force) {
				++forced;
				SKSE::log::info("[FX-FORCE] {} {} | {}", a_kind, Label(form), model);
			}
			// decided at compile time: an explosion or a hazard has no projectile type to read
			if constexpr (std::is_same_v<T, RE::BGSProjectile>) {
				if (!poisonSpray && form->data.types.any(RE::BGSProjectileData::Type::kFlamethrower, RE::BGSProjectileData::Type::kCone)) {
					++coneKept;
					continue;
				}
			}
			if (poisonSpray) {
				++poison;
				SKSE::log::info("[FX-POISON-SPRAY] {} {} | {}", a_kind, Label(form), model);
			}
			++scanned;
			if (!form->data.light) {
				++clean;
				SKSE::log::info("[FX-CLEAN] {} {} | {}", a_kind, Label(form), model);
				continue;
			}
			gEffectTargets.push_back({ &form->data.light, form->data.light, model, std::string(a_kind) + " " + Label(form), force || poisonSpray });
			++followed;
		}
		SKSE::log::info("{} lights: {} lit or named seen; {} followed, {} already had none, {} cone/flame kept on purpose, "
						"{} poison sprays, {} named",
			a_kind, scanned, followed, clean, coneKept, poison, forced);
	}

	// the three kinds main.cpp runs this pass for
	template void EffectLights<RE::BGSProjectile>(const Coverage& a_cov, std::string_view a_kind);
	template void EffectLights<RE::BGSExplosion>(const Coverage& a_cov, std::string_view a_kind);
	template void EffectLights<RE::BGSHazard>(const Coverage& a_cov, std::string_view a_kind);

	void RefreshLights()
	{
		const auto started = std::chrono::steady_clock::now();
		ApplyLightStrength(false);
		ApplyCastingLights(false);
		ApplyEffectLights(false);
		SKSE::log::info("lights refreshed for the settings in {:.1f} ms",
			std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count());
	}
}
