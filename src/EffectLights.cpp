// Luminous Arcana - SKSE plugin
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE.txt and the notice at the top of main.cpp.
//
// Pass 2: projectiles, explosions and hazards whose model is lit lose the game's own light.

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

	template <class T>
	void EffectLights(const Coverage& a_cov, std::string_view a_kind)
	{
		std::size_t scanned = 0, clean = 0, nulled = 0, coneKept = 0, poison = 0, forced = 0;
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
			form->data.light = nullptr;
			++nulled;
			SKSE::log::info("[FX-NULLED] {} {} | {}", a_kind, Label(form), model);
		}
		SKSE::log::info("{} lights: {} lit or named seen; {} nulled, {} already had none, {} cone/flame kept on purpose, "
						"{} poison sprays, {} named",
			a_kind, scanned, nulled, clean, coneKept, poison, forced);
	}

	// the three kinds main.cpp runs this pass for
	template void EffectLights<RE::BGSProjectile>(const Coverage& a_cov, std::string_view a_kind);
	template void EffectLights<RE::BGSExplosion>(const Coverage& a_cov, std::string_view a_kind);
	template void EffectLights<RE::BGSHazard>(const Coverage& a_cov, std::string_view a_kind);
}
