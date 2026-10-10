// Illuminated - SKSE plugin
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE.txt and the notice at the top of main.cpp.
//
// Pass 1: a magic effect whose casting art is lit loses the game's own casting light - for as long as the menu's
// settings keep that art lit. Each effect's own light is remembered, so switching an option off gives it back.
// Without Light Placer (RecordLights.cpp) the effect is pointed at the record light made for its art instead of losing
// its light, and an effect that had no casting light gets one.
// AUTOMATIC LIGHTS (his go-to pick 2026-10-07, "do all of them" 2026-10-08), without Light Placer: a spell whose casting
// art no config lights - another mod's spell Illuminated has no patch for - gets a light of ours too, behind the setting
// "Spells without a patch": in the color of its own casting light when it has one, else in its element's color (the
// color most of the tuned spells of that element wear: fire, frost, shock), at the middle strength and reach of the tuned
// hand lights. A spell with neither keeps what it has: no color is guessed.

#include "Plugin.h"

namespace Plugin
{
	// ------------------------------------------------------------------ pass 1: casting lights
	namespace
	{
		constexpr std::size_t                     kMaxCounterEffects = 512;
		constexpr std::array<std::string_view, 5> kSkipPrefixes{ "trap", "hazard", "voice", "ench", "test" };

		bool SkippedPrefix(std::string_view a_editorID)
		{
			auto low = Lower(a_editorID);
			if (low.size() > 4 && low.starts_with("dlc")) {
				low = low.substr(4);
			}
			return std::ranges::any_of(kSkipPrefixes, [&](std::string_view p) { return low.starts_with(p); });
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
			RE::TESObjectLIGH* autoLight{ nullptr };  // no config lights its art: the automatic light it gets while the setting is on
			int                element{ 0 };          // fire, frost or shock (RecordLights.cpp): its color setting
			bool               enbLight{ false };     // ENB Light changed the effect or its casting art: it steps aside on ENB
		};
		constexpr std::string_view kAutoSetting = "IlluminatedAutoLights";
		std::vector<CastingTarget> gCastingTargets;
		const Coverage*            gCastingCoverage = nullptr;
	}

	void ApplyCastingLights(bool a_log)
	{
		std::size_t nulled = 0, restored = 0, recorded = 0;
		// the settings every target reads, once for the pass
		const bool autoOn = SettingValue(kAutoSetting, 1) != 0;
		const bool yield = RecordRoute() && YieldToENBLight();
		for (auto& t : gCastingTargets) {
			if (RecordRoute()) {
				// the record light its art's rows give it now (an automatic light while that setting is on), or its own when no
				// row lights the art; ENB Light's spells keep its light while Illuminated steps aside for it
				RE::TESObjectLIGH* lit = t.autoLight ? (autoOn ? t.autoLight : nullptr) : RecordLightFor(t.model);
				auto* const        record = t.enbLight && yield ?
				                                nullptr :
				                                ElementLight(lit, t.element);
				auto* const        want = record ? record : t.own;
				if (t.effect->data.light != want) {
					t.effect->data.light = want;
					++(record ? recorded : restored);
					if (a_log && record) {
						SKSE::log::info("[CAST-RECORD] {} | {} | {}", Label(t.effect), t.model, t.own ? "its own light replaced" : "had none");
					}
				}
				continue;
			}
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
		SKSE::log::info("casting lights for the settings as they are: {} taken off, {} pointed at a record light, {} given back, {} effects followed",
			nulled, recorded, restored, gCastingTargets.size());
	}

	namespace
	{
		// the spells no config lights: an automatic light each, where a color can be read off the spell
		void AutoCastingLights(const Coverage& a_cov)
		{
			if (!RecordRoute()) {
				return;
			}
			// what the tuned spells wear now: the hand lights in the middle of which an automatic light sits, and the color most
			// of the tuned spells of each element wear
			std::vector<const RE::TESObjectLIGH*>                          tuned;
			std::map<RE::ActorValue, std::map<std::uint32_t, std::size_t>> byElement;
			const auto                                                     pack = [](const RE::Color& c) { return (static_cast<std::uint32_t>(c.red) << 16) | (static_cast<std::uint32_t>(c.green) << 8) | c.blue; };
			for (const auto& t : gCastingTargets) {
				if (auto* record = RecordLightFor(t.model)) {
					tuned.push_back(record);
					++byElement[t.effect->data.resistVariable][pack(record->data.color)];
				}
			}
			const auto elementColor = [&](RE::ActorValue a_av) -> std::optional<RE::Color> {
				const auto it = byElement.find(a_av);
				if (a_av == RE::ActorValue::kNone || it == byElement.end() || it->second.empty()) {
					return std::nullopt;
				}
				const auto best = std::ranges::max_element(it->second, {}, [](const auto& kv) { return kv.second; })->first;
				return RE::Color(static_cast<std::uint8_t>(best >> 16), static_cast<std::uint8_t>(best >> 8), static_cast<std::uint8_t>(best), 0);
			};
			std::size_t own = 0, element = 0, noColor = 0;
			for (auto* effect : LightKit::FormsOf<RE::EffectSetting>()) {
				if (!effect || !effect->data.castingArt) {
					continue;
				}
				const auto model = NormalPath(effect->data.castingArt->GetModel() ? effect->data.castingArt->GetModel() : "");
				if (model.empty() || a_cov.models.contains(model) || effect->data.castingType == RE::MagicSystem::CastingType::kConstantEffect ||
					effect->data.archetype == RE::EffectArchetypes::ArchetypeID::kLight || SkippedPrefix(EditorID(effect)) ||
					CounterEffectCount(effect) > kMaxCounterEffects) {
					continue;
				}
				std::optional<RE::Color> color;
				if (const auto* l = effect->data.light) {
					color = l->data.color;
					++own;
				} else if ((color = elementColor(effect->data.resistVariable))) {
					++element;
				} else {
					++noColor;
					continue;
				}
				auto* light = AutoLight(*color, tuned);
				if (!light) {
					continue;
				}
				gCastingTargets.push_back({ effect, effect->data.light, model, light });
				SKSE::log::info("[CAST-AUTO] {} | {} | {},{},{} from {}", Label(effect), model, color->red, color->green, color->blue,
					effect->data.light ? "its own light" : "its element");
			}
			SKSE::log::info(
				"automatic lights: {} spell(s) no config lights - {} in their own light's color, {} in their element's, {} left as they are "
				"(no color to read); {} light(s) made",
				own + element + noColor, own, element, noColor, AutoLightCount());
		}
	}

	std::size_t AutoCastingCount()
	{
		return static_cast<std::size_t>(std::ranges::count_if(gCastingTargets, [](const CastingTarget& t) { return t.autoLight != nullptr; }));
	}

	void CastingLights(const Coverage& a_cov)
	{
		gCastingCoverage = &a_cov;
		gCastingTargets.clear();
		std::size_t scanned = 0, constant = 0, archetype = 0, prefix = 0, clean = 0, bloated = 0;
		for (auto* effect : LightKit::FormsOf<RE::EffectSetting>()) {
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
				if (!RecordRoute()) {
					SKSE::log::info("[CAST-CLEAN] {} | {}", Label(effect), model);
					continue;
				}
				// without Light Placer it is followed too: its art's record light is the first casting light it ever had
			}
			if (CounterEffectCount(effect) > kMaxCounterEffects) {
				++bloated;
				continue;
			}
			gCastingTargets.push_back({ effect, effect->data.light, model });
		}
		SKSE::log::info(
			"casting lights: {} lit effects seen; {} followed, {} had none of their own, skipped: {} constant effect, "
			"{} light archetype, {} editor ID prefix, {} oversized",
			scanned, gCastingTargets.size(), clean, constant, archetype, prefix, bloated);
		AutoCastingLights(a_cov);
		if (RecordRoute()) {
			for (auto& t : gCastingTargets) {
				t.enbLight = TouchedByENBLight(t.effect) || TouchedByENBLight(t.effect->data.castingArt);
				t.element = ElementOf(t.effect);
				if (t.element > 0) {
					if (t.autoLight) {
						PrepareElementLights(t.autoLight, t.element);
					} else {
						PrepareElementLightsFor(t.model, t.element);
					}
				}
			}
		}
		ApplyCastingLights(true);
	}
}
