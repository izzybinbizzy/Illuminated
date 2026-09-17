// Luminous Arcana - SKSE plugin
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE.txt and the notice at the top of main.cpp.
//
// Pass 1: a magic effect whose casting art is lit loses the game's own casting light - for as long as the menu's
// settings keep that art lit. Each effect's own light is remembered, so switching an option off gives it back.

#include "Plugin.h"

namespace Plugin
{
	// ------------------------------------------------------------------ pass 1: casting lights
	constexpr std::size_t kMaxCounterEffects = 512;
	const std::vector<std::string> kSkipPrefixes{ "trap", "hazard", "voice", "ench", "test" };

	bool SkippedPrefix(const std::string& a_editorID)
	{
		auto low = Lower(a_editorID);
		if (low.size() > 4 && low.starts_with("dlc")) {
			low = low.substr(4);
		}
		return std::any_of(kSkipPrefixes.begin(), kSkipPrefixes.end(), [&](const std::string& p) { return low.starts_with(p); });
	}

	std::size_t CounterEffectCount(RE::EffectSetting* a_effect)
	{
		std::size_t listed = 0;
		for ([[maybe_unused]] auto* e : a_effect->counterEffects) {
			++listed;
		}
		const std::size_t declared = static_cast<std::uint16_t>(a_effect->data.numCounterEffects);
		// (std::max) in brackets: the Windows headers define a max macro
		return (std::max)(listed, declared);
	}

	struct CastingTarget
	{
		RE::EffectSetting* effect;
		RE::TESObjectLIGH* own;  // the light the effect had before this plugin touched it (after pass 0)
		std::string        model;
	};
	std::vector<CastingTarget> gCastingTargets;
	const Coverage*            gCastingCoverage = nullptr;

	void ApplyCastingLights(bool a_log)
	{
		std::size_t nulled = 0, restored = 0;
		for (auto& t : gCastingTargets) {
			const bool lit = gCastingCoverage && gCastingCoverage->ModelLit(t.model);
			if (lit && t.effect->data.light) {
				t.effect->data.light = nullptr;
				++nulled;
				if (a_log) {
					SKSE::log::info("[CAST-NULLED] {} | {}", Label(t.effect), t.model);
				}
			} else if (!lit && t.effect->data.light != t.own) {
				t.effect->data.light = t.own;
				++restored;
				if (a_log) {
					SKSE::log::info("[CAST-KEPT] {} | {} | no setting lights it now", Label(t.effect), t.model);
				}
			}
		}
		SKSE::log::info("casting lights for the settings as they are: {} taken off, {} given back, {} effects followed", nulled, restored,
			gCastingTargets.size());
	}

	void CastingLights(const Coverage& a_cov)
	{
		gCastingCoverage = &a_cov;
		gCastingTargets.clear();
		std::size_t scanned = 0, constant = 0, archetype = 0, prefix = 0, clean = 0, bloated = 0;
		for (auto* effect : RE::TESDataHandler::GetSingleton()->GetFormArray<RE::EffectSetting>()) {
			if (!effect || !effect->data.castingArt) {
				continue;
			}
			const auto model = NormalPath(effect->data.castingArt->GetModel() ? effect->data.castingArt->GetModel() : "");
			if (model.empty() || !a_cov.models.contains(model)) {
				continue;
			}
			++scanned;
			if (effect->data.castingType == RE::MagicSystem::CastingType::kConstantEffect) {
				++constant;
				continue;
			}
			if (effect->data.archetype == RE::EffectArchetypes::ArchetypeID::kLight) {
				++archetype;
				continue;
			}
			if (SkippedPrefix(EditorID(effect))) {
				++prefix;
				continue;
			}
			if (!effect->data.light) {
				++clean;
				SKSE::log::info("[CAST-CLEAN] {} | {}", Label(effect), model);
				continue;
			}
			if (CounterEffectCount(effect) > kMaxCounterEffects) {
				++bloated;
				continue;
			}
			gCastingTargets.push_back({ effect, effect->data.light, model });
		}
		SKSE::log::info("casting lights: {} lit effects seen; {} followed, {} already had none, skipped: {} constant effect, "
						"{} light archetype, {} editor ID prefix, {} oversized",
			scanned, gCastingTargets.size(), clean, constant, archetype, prefix, bloated);
		ApplyCastingLights(true);
	}
}
