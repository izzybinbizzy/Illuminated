// Illuminated - SKSE plugin
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE.txt and the notice at the top of main.cpp.
//
// Pass 2: projectiles, explosions and hazards whose model is lit lose the game's own light - for as long as the
// menu's settings keep that model lit. Each one's own light is remembered, so switching an option off gives it back.
// Without Light Placer (RecordLights.cpp) each is pointed at the record light made for its model instead, and one that
// had no light gets one.
// Also RefreshLights, which runs passes 1, 2 and 6 again (and puts the sliders and the record lights' colors onto the
// lights) whenever a setting changes.

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
		bool                always;        // named or a poison spray: dark whatever the settings say
		int                 element{ 0 };  // the element of the effects that fire it (RecordLights.cpp): its color setting
	};
	std::vector<EffectTarget> gEffectTargets;
	const Coverage*           gEffectCoverage = nullptr;

	void ApplyEffectLights(bool a_log)
	{
		std::size_t nulled = 0, restored = 0, recorded = 0;
		for (auto& t : gEffectTargets) {
			if (RecordRoute()) {
				// the record light its model's rows give it now; else its own (a named one or a poison spray: none)
				auto* const record = YieldToRELight() ? nullptr : ElementLight(RecordLightFor(t.model), t.element);
				auto* const want = record ? record : t.always ? nullptr :
				                                                t.own;
				if (*t.slot != want) {
					*t.slot = want;
					++(record ? recorded : want ? restored :
												  nulled);
					if (a_log && record) {
						SKSE::log::info("[FX-RECORD] {} | {} | {}", t.label, t.model, t.own ? "its own light replaced" : "had none");
					}
				}
				continue;
			}
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
		SKSE::log::info(
			"projectile, explosion and hazard lights for the settings as they are: {} taken off, {} pointed at a record light, {} given back, "
			"{} followed",
			nulled, recorded, restored, gEffectTargets.size());
	}

	// A projectile a Light-archetype effect fires (Magelight) IS the lamp that stays where it lands: it keeps the game's
	// light, the same rule as the effect itself (a user's report, 2026-09-24: "magelight is not producing proper
	// illumination once the projectile hits a surface or target" - its light had been taken off here and in the patcher).
	// A Magelight that lands on terrain places a HAZARD instead (vanilla Skyrim.esm 0x03FA51, and the copies overhauls
	// make of it). Nothing links that hazard to the spell, so it is known by its mesh, the light-spell lamp, and keeps
	// its light the same way (measured 2026-09-25: with its light taken off, the landed lamp was dark).
	constexpr std::string_view kLightSpellLampMesh = "lightspellhazard.nif";

	bool IsLightSpellLamp(const std::string& a_model)
	{
		return a_model == kLightSpellLampMesh || a_model.ends_with("\\" + std::string(kLightSpellLampMesh));
	}
	std::set<const RE::BGSProjectile*> LightSpellProjectiles()
	{
		std::set<const RE::BGSProjectile*> out;
		for (const auto* effect : RE::TESDataHandler::GetSingleton()->GetFormArray<RE::EffectSetting>()) {
			if (effect && effect->data.archetype == RE::EffectArchetypes::ArchetypeID::kLight && effect->data.projectileBase) {
				out.insert(effect->data.projectileBase);
			}
		}
		return out;
	}

	template <class T>
	void EffectLights(const Coverage& a_cov, std::string_view a_kind)
	{
		gEffectCoverage = &a_cov;
		std::size_t                        scanned = 0, clean = 0, followed = 0, coneKept = 0, poison = 0, forced = 0, lamps = 0;
		std::set<const RE::BGSProjectile*> lightSpells;
		if constexpr (std::is_same_v<T, RE::BGSProjectile>) {
			lightSpells = LightSpellProjectiles();
		}
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
			if constexpr (std::is_same_v<T, RE::BGSHazard>) {
				if (IsLightSpellLamp(model)) {
					++lamps;
					SKSE::log::info("[FX-LIGHT-SPELL] {} {} | {} | a Light spell's landed lamp keeps its light", a_kind, Label(form), model);
					continue;
				}
			}
			// decided at compile time: an explosion or a hazard has no projectile type to read
			if constexpr (std::is_same_v<T, RE::BGSProjectile>) {
				if (lightSpells.contains(form)) {
					++lamps;
					SKSE::log::info("[FX-LIGHT-SPELL] {} {} | {} | a Light spell's lamp keeps its light", a_kind, Label(form), model);
					continue;
				}
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
				if (!RecordRoute()) {
					SKSE::log::info("[FX-CLEAN] {} {} | {}", a_kind, Label(form), model);
					continue;
				}
				// without Light Placer it is followed too: its model's record light is the first light it ever had
			}
			gEffectTargets.push_back({ &form->data.light, form->data.light, model, std::string(a_kind) + " " + Label(form), force || poisonSpray });
			if (RecordRoute()) {
				if (const int e = ElementOfForm(form); e > 0) {
					gEffectTargets.back().element = e;
					PrepareElementLightsFor(model, e);
				}
			}
			++followed;
		}
		SKSE::log::info(
			"{} lights: {} lit or named seen; {} followed, {} had none of their own, {} cone/flame kept on purpose, "
			"{} poison sprays, {} named, {} Light spell lamps kept",
			a_kind, scanned, followed, clean, coneKept, poison, forced, lamps);
	}

	// the three kinds main.cpp runs this pass for
	template void EffectLights<RE::BGSProjectile>(const Coverage& a_cov, std::string_view a_kind);
	template void EffectLights<RE::BGSExplosion>(const Coverage& a_cov, std::string_view a_kind);
	template void EffectLights<RE::BGSHazard>(const Coverage& a_cov, std::string_view a_kind);

	void RefreshLights()
	{
		const auto started = std::chrono::steady_clock::now();
		ApplyRecordColors();
		ApplyLightStrength(false);
		ApplyStreamLights(false);  // switched off: the hand lights come back first, and pass 2 then has its say over them
		ApplyCastingLights(false);
		ApplyEffectLights(false);
		ApplyStreamLights(false);  // still on: a light pass 2 just gave back comes off again (measured 2026-10-01: 102 did not)
		SKSE::log::info("lights refreshed for the settings in {:.1f} ms",
			std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count());
	}
}
