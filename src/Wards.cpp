// Illuminated - SKSE plugin
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE.txt and the notice at the top of main.cpp.
//
// The wards: one dome per ward, and 360 Ward's sphere in the vanilla blue. Steps down when DynamicWards.dll is loaded,
// which dresses every ward itself (his call, 2026-09-24: "have the whole ward system step down if dynamic wards is present").
//
// ONE DOME. A ward spell's rank is the effect that accumulates Ward Power; every OTHER effect on the same spell that wears
// the vanilla ward art draws a second dome and a second hand light (vanilla's ShieldConcSelf, Odin's Ward Regeneration,
// Mysticism's Dummy rows - measured in his game 2026-09-24). Those lose their art and their light. An effect on a mod's
// own ward art (a vampire's blood ward) is never touched.
//
// THE BLUE SPHERE. 360 Ward's own sphere is orange (its mesh uses the fire gradient); with 360 Ward loaded the vanilla
// dome art form and 360 Ward's hit flash point at our blue copies under `meshes\magic\Glow Wards\Blue\` (his report,
// 2026-09-24: "the 360 ward is orange"). Nothing is saved; a Light Placer config on the ward hand still lights the hand.

#include "Plugin.h"

namespace Plugin
{
	namespace
	{
		constexpr std::uint32_t kShieldConcSelf = 0x0FCC62;  // Skyrim.esm
		constexpr std::uint32_t kDomeArt = 0x018124;         // Skyrim.esm WardHitEffect: magic\wardbodyfx.nif
		constexpr std::uint32_t kFlashArt = 0x000802;        // 360 Ward.esp: magic\wardShieldHitFX.nif
		constexpr const char*   k360Plugin = "360 Ward.esp";
		constexpr const char*   kEmptyModel = "Effects\\FXEmptyObject.nif";
		constexpr const char*   kBlueSphere = "Magic\\Glow Wards\\Blue\\wardbodyfx360.nif";
		constexpr const char*   kBlueFlash = "Magic\\Glow Wards\\Blue\\wardshieldhitfx.nif";

		bool DynamicWardsLoaded() { return REX::W32::GetModuleHandleA("DynamicWards.dll") != nullptr; }

		// the bare mesh name, lower case: "Magic\WardBodyFX.nif" -> "wardbodyfx"
		std::string MeshName(const char* a_model)
		{
			auto path = NormalPath(a_model ? a_model : "");
			if (const auto slash = path.find_last_of("\\/"); slash != std::string::npos) {
				path.erase(0, slash + 1);
			}
			if (const auto dot = path.rfind('.'); dot != std::string::npos) {
				path.erase(dot);
			}
			return path;
		}

		bool IsVanillaWardArt(const RE::BGSArtObject* a_art)
		{
			if (!a_art) {
				return false;
			}
			const auto name = MeshName(a_art->GetModel());
			return name == "wardinhandfx" || name == "wardbodyfx" || name == "wardbodyfx360";
		}

		bool WearsWardArt(const RE::EffectSetting* a_e)
		{
			return a_e && (IsVanillaWardArt(a_e->data.castingArt) || IsVanillaWardArt(a_e->data.hitEffectArt));
		}

		bool IsRank(const RE::EffectSetting* a_e)
		{
			return a_e->data.archetype == RE::EffectArchetypes::ArchetypeID::kAccumulateMagnitude &&
			       a_e->data.primaryAV == RE::ActorValue::kWardPower;
		}
	}

	void Wards()
	{
		if (DynamicWardsLoaded()) {
			SKSE::log::info("wards: Dynamic Wards is loaded - it dresses the wards, so this pass leaves them alone");
			return;
		}
		auto* dh = RE::TESDataHandler::GetSingleton();

		// one dome
		std::set<RE::EffectSetting*> extras;
		if (auto* shield = dh->LookupForm<RE::EffectSetting>(kShieldConcSelf, "Skyrim.esm"); shield && WearsWardArt(shield)) {
			extras.insert(shield);
		}
		for (auto* spell : dh->GetFormArray<RE::SpellItem>()) {
			if (!spell) {
				continue;
			}
			bool                            rank = false;
			std::vector<RE::EffectSetting*> others;
			for (auto* effect : spell->effects) {
				auto* base = effect ? effect->baseEffect : nullptr;
				if (!WearsWardArt(base)) {
					continue;
				}
				if (IsRank(base)) {
					rank = true;
				} else {
					others.push_back(base);
				}
			}
			if (rank) {
				extras.insert(others.begin(), others.end());
			}
		}
		RE::BGSArtObject* empty = extras.empty() ? nullptr : NewForm<RE::BGSArtObject>();
		if (empty) {
			empty->SetModel(kEmptyModel);
			for (auto* e : extras) {
				e->data.castingArt = empty;
				e->data.hitEffectArt = empty;
				e->data.enchantEffectArt = nullptr;
				e->data.light = nullptr;  // the second hand light goes with the second dome
				SKSE::log::info("[WARD-SILENT] {}", Label(e));
			}
		}
		SKSE::log::info("wards: {} effect(s) wear the ward art beside a ward's rank and show no second dome", extras.size());

		// the blue sphere
		// loaded, not merely present: LookupModByName also finds a plugin that is installed but not enabled
		if (PluginLoaded(k360Plugin)) {
			if (auto* dome = dh->LookupForm<RE::BGSArtObject>(kDomeArt, "Skyrim.esm")) {
				dome->SetModel(kBlueSphere);
			}
			if (auto* flash = dh->LookupForm<RE::BGSArtObject>(kFlashArt, k360Plugin)) {
				flash->SetModel(kBlueFlash);
			}
			SKSE::log::info("wards: 360 Ward is loaded - its sphere and flash wear the vanilla blue ({})", kBlueSphere);
		}
	}
}
