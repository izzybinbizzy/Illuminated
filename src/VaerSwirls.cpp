// Illuminated - SKSE plugin
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE.txt and the notice at the top of main.cpp.
//
// Pass 7: VAER Reborn's weapon swirls, when the VAER Reborn setting is on and VAEReborn.esp is loaded.
//
//  a. BRIGHTER STRANDS. The swirling particle strands VAER hangs on enchanted weapons are drawn four times brighter
//     (never bigger): this download carries VAER's own 36 swirl meshes with only their emissive multiplier raised,
//     under meshes\Illuminated\VAER\L3s\ so they replace nothing on disk. Each of VAER's art objects that names one
//     of those meshes is pointed at our copy - a copy that is not on disk is left alone.
//  b. THAUMATURGY'S OWN COPIES. Thaumaturgy points the Fear, Paralyze, Turn Undead, Banish, Silent Moons and second
//     Absorb enchantments at new magic effects of its own, and VAER only dresses the vanilla ones, so those weapons
//     had no swirl. Each copy is given the enchant art and enchant shader VAER made for the vanilla effect. Only
//     when Thaumaturgy.esp is loaded too.
//
// Read when the game loads: changing the setting takes effect the next time the game starts.
// The table below is written by lagen.py from gen.VAER_SWIRL_FORMS - the one list LTBG's patcher and the
// RELight - Spell Addon share. Never edit it here.

#include "Plugin.h"

namespace Plugin
{
	namespace
	{
		constexpr std::string_view kVaerSetting = "IlluminatedVAERReborn";
		constexpr std::string_view kVaerPlugin = "VAEReborn.esp";
		constexpr std::string_view kVaerMeshes = "magic\\l3s\\";
		constexpr std::string_view kOurMeshes = "Illuminated\\VAER\\L3s\\";

		struct SwirlCopy
		{
			std::string_view effectPlugin;
			RE::FormID       effect;
			std::string_view artPlugin;
			RE::FormID       art;
			std::string_view shaderPlugin;
			RE::FormID       shader;
			std::string_view name;
		};

		constexpr SwirlCopy kSwirlCopies[] = {
			{ "Thaumaturgy.esp", 0x1904A2, "VAEReborn.esp", 0x000807, "VAEReborn.esp", 0x00082C, "MAG_EnchFearFFContact" },
			{ "Thaumaturgy.esp", 0x1904A0, "VAEReborn.esp", 0x00080B, "VAEReborn.esp", 0x000823, "MAG_EnchParalyzeFFContact" },
			{ "Thaumaturgy.esp", 0x1904A3, "VAEReborn.esp", 0x00080E, "VAEReborn.esp", 0x00082F, "MAG_EnchTurnUndeadFFContact01" },
			{ "Thaumaturgy.esp", 0x30C4C4, "VAEReborn.esp", 0x00080E, "VAEReborn.esp", 0x00082F, "MAG_EnchTurnUndeadFFContact02" },
			{ "Thaumaturgy.esp", 0x30C4C5, "VAEReborn.esp", 0x00080E, "VAEReborn.esp", 0x00082F, "MAG_EnchTurnUndeadFFContact03" },
			{ "Thaumaturgy.esp", 0x1904A4, "VAEReborn.esp", 0x000803, "VAEReborn.esp", 0x000827, "MAG_EnchBanishFFContact01" },
			{ "Thaumaturgy.esp", 0x3115CA, "VAEReborn.esp", 0x00080A, "VAEReborn.esp", 0x000826, "MAG_EnchSilentMoonsEnchFFContact02" },
			{ "Thaumaturgy.esp", 0x427D9E, "VAEReborn.esp", 0x000801, "VAEReborn.esp", 0x00082A, "MAG_EnchAbsorbMagickaFFContact02" },
			{ "Thaumaturgy.esp", 0x42CEA1, "VAEReborn.esp", 0x000802, "VAEReborn.esp", 0x000826, "MAG_EnchAbsorbStaminaFFContact02" },
		};

		// loaded, not merely present: LookupModByName also finds a plugin that is installed but not enabled
		bool VaerPluginLoaded(std::string_view a_name)
		{
			return PluginLoaded(a_name);
		}

		std::size_t BrighterStrands()
		{
			std::size_t pointed = 0, absent = 0;
			auto* dh = RE::TESDataHandler::GetSingleton();
			for (auto* art : dh->GetFormArray<RE::BGSArtObject>()) {
				if (!art) {
					continue;
				}
				const auto* file = art->GetFile(0);
				if (!file || Lower(std::string(file->GetFilename())) != Lower(std::string(kVaerPlugin))) {
					continue;
				}
				const std::string model = art->GetModel() ? art->GetModel() : "";
				const std::string low = Lower(model);
				if (low.rfind(kVaerMeshes, 0) != 0) {
					continue;
				}
				const std::string ours = std::string(kOurMeshes) + model.substr(kVaerMeshes.size());
				std::error_code ec;
				if (!fs::exists(fs::path("Data") / "meshes" / ours, ec)) {
					++absent;
					continue;
				}
				art->SetModel(ours.c_str());
				const char* back = art->GetModel();
				if (back && Lower(back) == Lower(ours)) {
					++pointed;
				} else {
					SKSE::log::warn("[VAER] {:08X} did not take {}", art->GetFormID(), ours);
				}
			}
			SKSE::log::info("VAER strands: {} swirl art object(s) now use the brighter meshes, {} without our copy left alone",
				pointed, absent);
			return pointed;
		}

		std::size_t ThaumaturgyCopies()
		{
			std::size_t set = 0, missing = 0;
			auto* dh = RE::TESDataHandler::GetSingleton();
			for (const auto& c : kSwirlCopies) {
				auto* effect = dh->LookupForm<RE::EffectSetting>(c.effect, c.effectPlugin);
				auto* art = dh->LookupForm<RE::BGSArtObject>(c.art, c.artPlugin);
				auto* shader = dh->LookupForm<RE::TESEffectShader>(c.shader, c.shaderPlugin);
				if (!effect || !art || !shader) {
					++missing;
					SKSE::log::warn("[VAER] {}: a form is not in this load order (effect {}, art {}, shader {})", c.name,
						effect != nullptr, art != nullptr, shader != nullptr);
					continue;
				}
				effect->data.enchantEffectArt = art;
				effect->data.enchantShader = shader;
				if (effect->data.enchantEffectArt == art && effect->data.enchantShader == shader) {
					++set;
					SKSE::log::info("[VAER] {} now wears VAER's swirl {:08X} and shader {:08X}", c.name, art->GetFormID(),
						shader->GetFormID());
				}
			}
			SKSE::log::info("VAER on Thaumaturgy: {} of {} effect(s) given VAER's swirl, {} not found", set,
				std::size(kSwirlCopies), missing);
			return set;
		}
	}

	void VaerSwirls()
	{
		if (!VaerPluginLoaded(kVaerPlugin)) {
			SKSE::log::info("VAER: {} is not loaded; nothing to do", kVaerPlugin);
			return;
		}
		if (SettingValue(kVaerSetting, 0) == 0) {
			SKSE::log::info("VAER: the VAER Reborn setting is off; VAER's swirls are left as VAER made them");
			return;
		}
		BrighterStrands();
		if (VaerPluginLoaded("Thaumaturgy.esp")) {
			ThaumaturgyCopies();
		} else {
			SKSE::log::info("VAER: Thaumaturgy.esp is not loaded; no copies to dress");
		}
	}
}
