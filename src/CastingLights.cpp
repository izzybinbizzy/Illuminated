// Illuminated - SKSE plugin
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE.txt and the notice at the top of main.cpp.
//
// Pass 1: a magic effect whose casting art is lit loses the game's own casting light - for as long as the menu's
// settings keep that art lit. Each effect's own light is remembered, so switching an option off gives it back.
// Without Light Placer (RecordLights.cpp) the effect is pointed at the record light made for its art instead of losing
// its light, and an effect that had no casting light gets one.
// AUTOMATIC LIGHTS (his go-to pick, "do all of them"), without Light Placer: a spell whose casting
// art no config lights - another mod's spell Illuminated has no patch for - gets a light of ours too, behind the setting
// "Spells without a patch", at the middle strength and reach of the tuned hand lights. Its color, in his order of 2026-10-18
// ("i want it for all spells ... the fact that we provide a way for them to change and save their colors should suffice"):
// its own casting light's, else the light of the projectile it fires, else its art's (the emissive colors of the art's
// effect shaders, read once at load), else the color most of the tuned spells of its resistance group wear (every group).
// A spell with none of them keeps what it has: no color is guessed.

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
			RE::EffectSetting* effect{ nullptr };
			RE::TESObjectLIGH* own{ nullptr };  // the light the effect had before this plugin touched it (after pass 0)
			std::string        model;
			RE::TESObjectLIGH* autoLight{ nullptr };  // no config lights its art: the automatic light it gets while the setting is on
			bool               enbLight{ false };     // ENB Light changed the effect or its casting art: it steps aside on ENB
		};
		constexpr std::string_view kAutoSetting = "IlluminatedAutoLights";
		std::vector<CastingTarget> gCastingTargets;
		const Coverage*            gCastingCoverage = nullptr;
	}

	namespace
	{
		struct CastingCounts
		{
			std::size_t nulled{ 0 }, restored{ 0 }, recorded{ 0 };
		};

		// without Light Placer: the record light its art's rows give it now (an automatic light while that setting is on), or
		// its own when no row lights the art; ENB Light's spells keep its light while Illuminated steps aside for it
		void ApplyRecordRoute(CastingTarget& a_t, bool a_autoOn, bool a_yield, bool a_log, CastingCounts& a_n)
		{
			RE::TESObjectLIGH* lit = a_t.autoLight ? (a_autoOn ? a_t.autoLight : nullptr) : RecordLightFor(a_t.model);
			auto* const        record = a_t.enbLight && a_yield ? nullptr : lit;
			auto* const        want = record ? record : a_t.own;
			if (a_t.effect->data.light != want) {
				a_t.effect->data.light = want;
				++(record ? a_n.recorded : a_n.restored);
				if (a_log && record) {
					SKSE::log::info("[CAST-RECORD] {} | {} | {}", Label(a_t.effect), a_t.model, a_t.own ? "its own light replaced" : "had none");
				}
			}
		}

		// with Light Placer: a lit art's casting light comes off; it is given back when no setting lights the art
		void ApplyLightPlacerRoute(CastingTarget& a_t, bool a_log, CastingCounts& a_n)
		{
			const bool lit = gCastingCoverage && gCastingCoverage->ModelLit(a_t.model);
			if (lit && a_t.effect->data.light) {
				a_t.effect->data.light = nullptr;
				++a_n.nulled;
				if (a_log) {
					SKSE::log::info("[CAST-NULLED] {} | {}", Label(a_t.effect), a_t.model);
				}
			} else if (!lit && a_t.effect->data.light != a_t.own) {
				a_t.effect->data.light = a_t.own;
				++a_n.restored;
				if (a_log) {
					SKSE::log::info("[CAST-KEPT] {} | {} | no setting lights it now", Label(a_t.effect), a_t.model);
				}
			}
		}
	}

	void ApplyCastingLights(bool a_log)
	{
		CastingCounts n;
		// the route and the settings every target reads, once for the pass
		const bool record = RecordRoute();
		const bool autoOn = SettingValue(kAutoSetting, 1) != 0 && !LightsOff();
		const bool yield = record && YieldToENBLight();
		for (auto& t : gCastingTargets) {
			if (record) {
				ApplyRecordRoute(t, autoOn, yield, a_log, n);
			} else {
				ApplyLightPlacerRoute(t, a_log, n);
			}
		}
		if (a_log) {
			SKSE::log::info("casting lights for the settings as they are: {} taken off, {} pointed at a record light, {} given back, {} effects followed",
				n.nulled, n.recorded, n.restored, gCastingTargets.size());
		}
	}

	namespace
	{
		// the spells no config lights: an automatic light each, where a color can be read off the spell
		void AutoCastingLights(const Coverage& a_cov)
		{
			if (!RecordRoute()) {
				return;
			}
			// what the tuned spells wear in the data (each art's first row, whatever the settings at load - a switch or "all lights
			// off" then changes no group's color; CodeRabbit, RELight #27): the hand lights in the middle of which an automatic
			// light sits, and the color most of the tuned spells of each group wear
			std::vector<const RE::TESObjectLIGH*>                          tuned;
			std::map<RE::ActorValue, std::map<std::uint32_t, std::size_t>> byElement;
			const auto                                                     pack = [](const RE::Color& c) { return (static_cast<std::uint32_t>(c.red) << 16) | (static_cast<std::uint32_t>(c.green) << 8) | c.blue; };
			for (const auto& t : gCastingTargets) {
				if (auto* record = RecordLightOfData(t.model)) {
					tuned.push_back(record);
					++byElement[t.effect->data.resistVariable][pack(record->data.color)];
				}
			}
			// HIS RULING 2026-10-18 (it reverses his 2026-10-13 "fire, frost and shock only"): every resistance group's color; a
			// spell no resistance applies to (healing, wards, utility) is in no group and keeps the game's own light
			const auto groupColor = [&](RE::ActorValue a_av) -> std::optional<RE::Color> {
				if (a_av == RE::ActorValue::kNone) {
					return std::nullopt;
				}
				const auto it = byElement.find(a_av);
				if (it == byElement.end() || it->second.empty()) {
					return std::nullopt;
				}
				const auto best = std::ranges::max_element(it->second, {}, [](const auto& kv) { return kv.second; })->first;
				return RE::Color(static_cast<std::uint8_t>(best >> 16), static_cast<std::uint8_t>(best >> 8), static_cast<std::uint8_t>(best), 0);
			};
			std::size_t own = 0, projectile = 0, art = 0, group = 0, noColor = 0, dark = 0;
			for (auto* effect : LightKit::FormsOf<RE::EffectSetting>()) {
				if (!effect || !effect->data.castingArt) {
					continue;
				}
				const auto model = NormalPath(effect->data.castingArt->GetModel() ? effect->data.castingArt->GetModel() : "");
				// an unpatched Light-archetype spell gets no automatic light: a mod may keep its orb's light in that slot, and a hand
				// light in the orb's color would be new (Candlelight / Magelight are covered - CastingLights below follows them).
				// A ward (its effect raises Ward Power) and any spell on a ward's hand art (named wardinhandfx - the game's, Dynamic
				// Wards' per rank, ours) are the wards' own: Dynamic Wards dresses every ward when it is installed, Wards.cpp when it is
				// not (measured 2026-10-19 on a Vanilla copy: 32 ward effects on Dynamic Wards' art took its uncolored meshes' orange /
				// red - it recolors them in game)
				if (model.empty() || a_cov.models.contains(model) || effect->data.castingType == RE::MagicSystem::CastingType::kConstantEffect ||
					effect->data.archetype == RE::EffectArchetypes::ArchetypeID::kLight || SkippedPrefix(EditorID(effect)) ||
					CounterEffectCount(effect) > kMaxCounterEffects || effect->data.primaryAV == RE::ActorValue::kWardPower ||
					model.contains("wardinhandfx")) {
					continue;
				}
				std::optional<RE::Color> color;
				std::string_view         from;
				const auto*              shot = effect->data.projectileBase ? effect->data.projectileBase->data.light : nullptr;
				if (const auto* l = effect->data.light) {
					color = l->data.color;
					from = "its own light";
					++own;
				} else if (shot) {
					color = shot->data.color;
					from = "its projectile's light";
					++projectile;
				} else if ((color = LightKit::ArtColor(effect->data.castingArt->GetModel()))) {
					from = "its art";
					++art;
				} else if ((color = groupColor(effect->data.resistVariable))) {
					from = "its resistance group";
					++group;
				} else {
					++noColor;
					continue;
				}
				// a black light is an author's dark on purpose (Colorful Bound Weapons' black summon): black stays dark
				if ((std::max)({ color->red, color->green, color->blue }) < LightKit::kDarkTop) {
					++dark;
					continue;
				}
				// a group's color comes off a record that already wears Vanilla's look
				auto* light = AutoLight(from == "its resistance group" ? *color : VanillaLook(*color), tuned);
				if (!light) {
					continue;
				}
				gCastingTargets.push_back({ effect, effect->data.light, model, light });
				SKSE::log::info("[CAST-AUTO] {} | {} | {},{},{} from {}", Label(effect), model, color->red, color->green, color->blue, from);
			}
			SKSE::log::info(
				"automatic lights: {} spell(s) no config lights - {} in their own light's color, {} in their projectile's, {} in their "
				"art's, {} in their resistance group's, {} left as they are (no color to read), {} left dark (a black color); {} light(s) made",
				own + projectile + art + group + noColor, own, projectile, art, group, noColor, dark, AutoLightCount());
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
			// A Light-archetype spell (Candlelight, Magelight) summons its orb through the archetype's association; its own light
			// slot is the HAND's casting light like any spell's (read 2026-10-12: Candlelight's casting light is
			// MAG_LightSpellHand01, reach 32, its orb ODN_Alt_Light_Light_MagicalLight). Skipped, it kept that 32-unit light on
			// Vanilla / ENB and its ground light lit nothing (his "i think candlelight hand light isn't working", measured: ground
			// +0 where every other spell lifted +125 .. +700). So on the record route it is followed like any spell; with Light
			// Placer it stays skipped as before.
			if (effect->data.archetype == RE::EffectArchetypes::ArchetypeID::kLight && !RecordRoute()) {
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
			"casting lights: {} lit effects seen; {} followed ({} of them Light spells), {} had none of their own, skipped: {} "
			"constant effect, {} light archetype, {} editor ID prefix, {} oversized",
			scanned, gCastingTargets.size(),
			std::ranges::count_if(gCastingTargets, [](const CastingTarget& t) { return t.effect->data.archetype == RE::EffectArchetypes::ArchetypeID::kLight; }),
			clean, constant, archetype, prefix, bloated);
		AutoCastingLights(a_cov);
		if (RecordRoute()) {
			for (auto& t : gCastingTargets) {
				t.enbLight = TouchedByENBLight(t.effect) || TouchedByENBLight(t.effect->data.castingArt);
			}
		}
		ApplyCastingLights(true);
	}
}
