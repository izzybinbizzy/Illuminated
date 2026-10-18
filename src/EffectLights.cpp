// Illuminated - SKSE plugin
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE.txt and the notice at the top of main.cpp.
//
// Pass 2: projectiles, explosions and hazards whose model is lit lose the game's own light - for as long as the
// menu's settings keep that model lit. Each one's own light is remembered, so switching an option off gives it back.
// Without Light Placer (RecordLights.cpp) each is pointed at the record light made for its model instead, and one that
// had no light gets one.
// Sprays, streams and bolts (flame, cone and beam projectiles) are never touched here: pass 6 lights each by its own light
// record made bigger and brighter (his stream rule - never take the projectile's own light off).
// Also RefreshLights, which runs passes 1, 2 and 6 again (and puts the sliders onto the lights) whenever a setting changes.

#include "Plugin.h"

namespace Plugin
{
	// ------------------------------------------------------------------ pass 2: projectiles, explosions, hazards
	namespace
	{
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
	}

	namespace
	{
		struct EffectCounts
		{
			std::size_t nulled{ 0 }, restored{ 0 }, recorded{ 0 };
		};

		// without Light Placer: each followed form points at the record light its model's rows give it now, else its own (a named
		// one or a poison spray: none)
		void ApplyEffectRecordRoute(bool a_log, EffectCounts& a_n)
		{
			for (auto& t : gEffectTargets) {
				auto* const        record = RecordLightFor(t.model);
				RE::TESObjectLIGH* want = record;
				if (!want && (!t.always || LightsOff())) {  // all lights off: an always-dark form gets its own light back too
					want = t.own;
				}
				if (*t.slot == want) {
					continue;
				}
				*t.slot = want;
				if (record) {
					++a_n.recorded;
				} else if (want) {
					++a_n.restored;
				} else {
					++a_n.nulled;
				}
				if (a_log && record) {
					SKSE::log::info("[FX-RECORD] {} | {} | {}", t.label, t.model, t.own ? "its own light replaced" : "had none");
				}
			}
		}

		// with Light Placer: a form whose model a setting lights now loses the game's light (Light Placer's takes its place),
		// any other gets its own back
		void ApplyEffectLightPlacerRoute(bool a_log, EffectCounts& a_n)
		{
			for (auto& t : gEffectTargets) {
				const bool lit = (t.always && !LightsOff()) || (gEffectCoverage && gEffectCoverage->ModelLit(t.model));  // all off: its own light back too
				if (lit && *t.slot) {
					*t.slot = nullptr;
					++a_n.nulled;
					if (a_log) {
						SKSE::log::info("[FX-NULLED] {} | {}", t.label, t.model);
					}
				} else if (!lit && *t.slot != t.own) {
					*t.slot = t.own;
					++a_n.restored;
					if (a_log) {
						SKSE::log::info("[FX-KEPT] {} | {} | no setting lights it now", t.label, t.model);
					}
				}
			}
		}
	}

	void ApplyEffectLights(bool a_log)
	{
		EffectCounts n;
		if (RecordRoute()) {
			ApplyEffectRecordRoute(a_log, n);
		} else {
			ApplyEffectLightPlacerRoute(a_log, n);
		}
		if (a_log) {
			SKSE::log::info(
				"projectile, explosion and hazard lights for the settings as they are: {} taken off, {} pointed at a record light, {} given back, "
				"{} followed",
				n.nulled, n.recorded, n.restored, gEffectTargets.size());
		}
	}

	// A projectile a Light-archetype effect fires (Magelight) IS the lamp that stays where it lands: it keeps the game's
	// light, the same rule as the effect itself (a user's report: "magelight is not producing proper
	// illumination once the projectile hits a surface or target" - its light had been taken off here and in the patcher).
	// A Magelight that lands on terrain places a HAZARD instead (vanilla Skyrim.esm 0x03FA51, and the copies overhauls
	// make of it). Nothing links that hazard to the spell, so it is known by its mesh, the light-spell lamp, and keeps
	// its light the same way (measured: with its light taken off, the landed lamp was dark).
	namespace
	{
		constexpr std::string_view kLightSpellLampMesh = "lightspellhazard.nif";

		bool IsLightSpellLamp(std::string_view a_model)
		{
			return a_model.ends_with(kLightSpellLampMesh) &&
			       (a_model.size() == kLightSpellLampMesh.size() || a_model[a_model.size() - kLightSpellLampMesh.size() - 1] == '\\');
		}
		std::set<const RE::BGSProjectile*> LightSpellProjectiles()
		{
			std::set<const RE::BGSProjectile*> out;
			for (const auto* effect : LightKit::FormsOf<RE::EffectSetting>()) {
				if (effect && effect->data.archetype == RE::EffectArchetypes::ArchetypeID::kLight && effect->data.projectileBase) {
					out.insert(effect->data.projectileBase);
				}
			}
			return out;
		}
	}

	template <class T>
	void EffectLights(const Coverage& a_cov, std::string_view a_kind)
	{
		gEffectCoverage = &a_cov;
		const bool                         record = RecordRoute();  // the route, decided once for the pass
		std::size_t                        scanned = 0, clean = 0, followed = 0, coneKept = 0, poison = 0, forced = 0, lamps = 0;
		std::set<const RE::BGSProjectile*> lightSpells;
		if constexpr (std::is_same_v<T, RE::BGSProjectile>) {
			lightSpells = LightSpellProjectiles();
		}
		for (auto* form : LightKit::FormsOf<T>()) {
			if (!form) {
				continue;
			}
			auto model = NormalPath(form->GetModel() ? form->GetModel() : "");
			if (model.empty()) {
				continue;
			}
			const auto id = EditorID(form);
			const bool force = kForceNullProjectiles.contains(Lower(id));
			const bool poisonSpray = IsPoisonSpray(id, model);
			if (!a_cov.models.contains(model) && !force && !poisonSpray) {
				// without Light Placer a config may name the form itself (Explosions.json's formIDs entries): its rows are kept
				// under its editor ID's key (Configs.cpp), which stands in for the model from here on
				// a vanilla record a mod renames is named by its id instead (Configs.cpp, gen.RENAMED_BASE_FORMS)
				auto key = id.empty() ? std::string() : FormKey(Lower(id));
				if (key.empty() || !a_cov.modelTests.contains(key)) {
					key = FormIdKey(form->GetFormID());
				}
				if (!record || !a_cov.modelTests.contains(key)) {
					continue;
				}
				model = key;
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
				using Type = RE::BGSProjectileData::Type;
				if (!poisonSpray && form->data.types.any(Type::kFlamethrower, Type::kCone, Type::kBeam)) {  // pass 6's
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
				if (!record) {
					SKSE::log::info("[FX-CLEAN] {} {} | {}", a_kind, Label(form), model);
					continue;
				}
				// without Light Placer it is followed too: its model's record light is the first light it ever had
			}
			gEffectTargets.push_back({ &form->data.light, form->data.light, model, std::string(a_kind) + " " + Label(form), force || poisonSpray });
			++followed;
		}
		SKSE::log::info(
			"{} lights: {} lit or named seen; {} followed, {} had none of their own, {} spray / stream / bolt left to pass 6, "
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
		ApplyLightStrength(false);
		// passes 1, 2 and 6 write different forms (pass 2 leaves every spray, stream and bolt to pass 6), so their order is free
		ApplyCastingLights(false);
		ApplyEffectLights(false);
		ApplyStreamLights(false);
		SKSE::log::info("lights refreshed for the settings in {:.1f} ms",
			std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count());
	}
}
