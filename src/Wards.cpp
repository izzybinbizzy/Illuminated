// Illuminated - SKSE plugin
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE.txt and the notice at the top of main.cpp.
//
// The wards: one dome per ward, and 360 Ward's sphere in the vanilla blue. Steps down when DynamicWards.dll is loaded,
// which dresses every ward itself (his call: "have the whole ward system step down if dynamic wards is present").
//
// ONE DOME. A ward spell's rank is the effect that accumulates Ward Power; every OTHER effect on the same spell that wears
// the vanilla ward art draws a second dome and a second hand light (vanilla's ShieldConcSelf, Odin's Ward Regeneration,
// Mysticism's Dummy rows - measured in his game). Those lose their art and their light. An effect on a mod's
// own ward art (a vampire's blood ward) is never touched.
//
// THE BLUE SPHERE. 360 Ward's own sphere is orange (its mesh uses the fire gradient); with 360 Ward loaded the vanilla
// dome art form and 360 Ward's hit flash point at our copies under `meshes\magic\Glow Wards\Vanilla\` (his report,
// "the 360 ward is orange"; the look below). Nothing is saved. The ward hand wears our copy under `Glow Wards\Blue\`, and
// its light row claims that copy beside the vanilla mesh (lagen.py WARD_HAND_COPIES), so it is lit on either route.
//
// THE WARD LOOK (his words 2026-10-09: "change the blue ward to vanilla blue. add transparent ward with vanilla outline as a
// preset for wards, no slider just pure transparent with outline."): the setting IlluminatedWardLook - 0 the vanilla ward's own
// blue (`Glow Wards\Vanilla\`: the sphere and flash retinted to gradwardcolor's hue; the old `Blue\` sphere was Dynamic Wards'
// deeper Adept Blue), 1 Transparent (`Glow Wards\Transparent\`: no fill, the rim glow as the outline; without 360 Ward our
// vanilla-blue dome with its bright layer turned into a rim). Applied at data load and on every settings refresh
// (ApplyWardLook): an art form's model is read when a ward is raised, so it shows on the next ward.

#include "Plugin.h"

namespace Plugin
{
	namespace
	{
		constexpr std::uint32_t    kShieldConcSelf = 0x0FCC62;  // Skyrim.esm
		constexpr std::uint32_t    kDomeArt = 0x018124;         // Skyrim.esm WardHitEffect: magic\wardbodyfx.nif
		constexpr std::uint32_t    kFlashArt = 0x000802;        // 360 Ward.esp: magic\wardShieldHitFX.nif
		constexpr const char*      k360Plugin = "360 Ward.esp";
		constexpr const char*      kEmptyModel = "Effects\\FXEmptyObject.nif";
		constexpr const char*      kBlueSphere = "Magic\\Glow Wards\\Vanilla\\wardbodyfx360.nif";
		constexpr const char*      kBlueFlash = "Magic\\Glow Wards\\Vanilla\\wardshieldhitfx.nif";
		constexpr const char*      kClearSphere = "Magic\\Glow Wards\\Transparent\\wardbodyfx360.nif";
		constexpr const char*      kClearDome = "Magic\\Glow Wards\\Transparent\\wardbodyfx.nif";
		constexpr std::string_view kWardLook = "IlluminatedWardLook";  // 0 vanilla blue, 1 transparent
		int                        gLookApplied = -1;                  // the look last put on the forms (-1 none yet)
		// his call: the dome (without 360 Ward) and the hand (always) wear OUR vanilla-blue copies (their own textures, so no
		// other mod's loose textures reach them)
		constexpr std::uint32_t kHandArt = 0x0253F1;  // Skyrim.esm: magic\wardinhandfx.nif
		constexpr const char*   kBlueDome = "Magic\\Glow Wards\\Blue\\wardbodyfx.nif";
		constexpr const char*   kBlueHand = "Magic\\Glow Wards\\Blue\\wardinhandfx.nif";

		// HIS REPORT (Spellbreaker - the old fix, lost in a handoff purge): a WORN ward - Spellbreaker, the Divine
		// Crusader shield, the Shield of Reman - never drives a cast ward's transitions, so the vanilla meshes sit idle: only
		// the swirl, and the dome flashes as the ward ends. They wear copies of our dome and hand on Magic\\BeginLoopEnd.hkx
		// (`wardgen.py warddomes`), in art forms of their own, so the cast wards keep theirs.
		struct WornRow
		{
			const char* plugin;
			RE::FormID  id;
		};
		constexpr WornRow kWorn[] = {
			{ "Skyrim.esm", 0x07DCDB },                        // Spellbreaker's Ward - Deflection
			{ "ccmtysse001-knightsofthenine.esl", 0x00081D },  // the Divine Crusader shield's
			{ "LegacyoftheDragonborn.esm", 0x124E5B },         // Reman's Ward
		};
		RE::BGSArtObject* gWornDome = nullptr;
		RE::BGSArtObject* gWornHand = nullptr;

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

	// Dynamic Wards 2.0 and later is a DLL only (no plugin); it dresses every ward itself
	bool DynamicWardsLoaded()
	{
		static const bool loaded = REX::W32::GetModuleHandleA("DynamicWards.dll") != nullptr;
		return loaded;
	}

	void ApplyWardLook()
	{
		if (DynamicWardsLoaded()) {
			return;
		}
		auto* dh = RE::TESDataHandler::GetSingleton();
		auto* dome = dh ? dh->LookupForm<RE::BGSArtObject>(kDomeArt, "Skyrim.esm") : nullptr;
		if (!dome) {
			return;
		}
		const int look = std::clamp(SettingValue(kWardLook, 0), 0, 1);
		if (look == gLookApplied) {
			return;
		}
		gLookApplied = look;
		// loaded, not merely present: LookupModByName also finds a plugin that is installed but not enabled
		if (PluginLoaded(k360Plugin)) {
			dome->SetModel(look == 1 ? kClearSphere : kBlueSphere);
			if (auto* flash = dh->LookupForm<RE::BGSArtObject>(kFlashArt, k360Plugin)) {
				flash->SetModel(kBlueFlash);
			}
			SKSE::log::info("wards: 360 Ward is loaded - its sphere {} ({}), its flash in the vanilla blue",
				look == 1 ? "transparent" : "in the vanilla blue", look == 1 ? kClearSphere : kBlueSphere);
		} else {
			dome->SetModel(look == 1 ? kClearDome : kBlueDome);
			SKSE::log::info("wards: the dome {} ({})", look == 1 ? "transparent" : "in our vanilla blue", look == 1 ? kClearDome : kBlueDome);
		}
	}

	void Wards()
	{
		if (DynamicWardsLoaded()) {
			SKSE::log::info("wards: Dynamic Wards is loaded - it dresses the wards, so this pass leaves them alone");
			return;
		}
		auto* dh = RE::TESDataHandler::GetSingleton();
		if (!dh) {
			return;
		}

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

		// the sphere / dome: the ward look
		gLookApplied = -1;
		ApplyWardLook();
		if (auto* hand = dh->LookupForm<RE::BGSArtObject>(kHandArt, "Skyrim.esm")) {
			hand->SetModel(kBlueHand);
			SKSE::log::info("wards: the hand wears our vanilla blue ({})", kBlueHand);
		}
		// the worn wards: their own looping hand always, their own looping dome unless 360 Ward's sphere is the dome
		if (!gWornDome) {
			gWornDome = NewForm<RE::BGSArtObject>();
		}
		if (!gWornHand) {
			gWornHand = NewForm<RE::BGSArtObject>();
		}
		if (gWornDome && gWornHand) {
			gWornDome->SetModel("Magic\\Glow Wards\\Blue\\wardbodyfxworn.nif");
			gWornHand->SetModel("Magic\\Glow Wards\\Blue\\wardinhandfxworn.nif");
			const bool  has360 = PluginLoaded(k360Plugin);
			auto*       dome = dh->LookupForm<RE::BGSArtObject>(kDomeArt, "Skyrim.esm");
			std::size_t worn = 0;
			for (const auto& r : kWorn) {
				if (auto* e = dh->LookupForm<RE::EffectSetting>(r.id, r.plugin)) {
					e->data.castingArt = gWornHand;
					e->data.hitEffectArt = has360 ? dome : gWornDome;
					++worn;
				}
			}
			SKSE::log::info("wards: {} worn ward(s) (Spellbreaker, Crusader, Reman) wear the looping copies", worn);
		}
	}
}
