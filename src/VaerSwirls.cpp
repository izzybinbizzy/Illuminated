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
//  c. VAER'S OWN EFFECTS PUT BACK (ReLight's fix of 2026-09-28 late night, ported 2026-09-30). A plugin loaded after
//     VAEReborn.esp that edits the same vanilla effect (Thaumaturgy, Artificer, ...) carries the effect's old art, so VAER's
//     swirl is lost - 21 of VAER's 46 in his game, measured. Each gets VAER's Enchant Art and Enchant Shader back
//     (gen.VAER_VANILLA_FORMS, read off VAEReborn.esp). And two of VAER's shaders (Stendarr's Hammer, Dawnfang's Bloodthirst)
//     name their fill texture "....dds.dds", a file that does not exist, so they draw nothing: such a name is pointed at
//     the real file.
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

		// an empty art plugin: VAER leaves that effect's art alone and sets only the shader
		constexpr SwirlCopy kVaerOwn[] = {
			{ "Skyrim.esm", 0x0F23F9, "VAEReborn.esp", 0x000817, "VAEReborn.esp", 0x000848, "C06BladeOfYsgramorEnchEffect" },
			{ "ccbgssse004-ruinsedge.esl", 0x000802, "VAEReborn.esp", 0x000833, "VAEReborn.esp", 0x00083C, "ccBGSSSE004_BowBaseEffect" },
			{ "ccbgssse006-stendarshammer.esl", 0x000808, "VAEReborn.esp", 0x000834, "VAEReborn.esp", 0x00083D, "ccBGSSSE006_EnchDamageFFContact" },
			{ "ccbgssse007-chrysamere.esl", 0x000801, "VAEReborn.esp", 0x000835, "VAEReborn.esp", 0x00083E, "ccBGSSSE007_Chrysamere_Effect" },
			{ "ccbgssse013-dawnfang.esl", 0x000D18, "VAEReborn.esp", 0x000836, "VAEReborn.esp", 0x00083F, "ccBGSSSE013_EnchBloodthirst" },
			{ "ccbgssse013-dawnfang.esl", 0x000803, "VAEReborn.esp", 0x000837, "VAEReborn.esp", 0x000840, "ccBGSSSE013_EnchDawnfangDescriptionFX" },
			{ "ccbgssse013-dawnfang.esl", 0x000804, "VAEReborn.esp", 0x000837, "VAEReborn.esp", 0x000840, "ccBGSSSE013_EnchDawnfangKillIncrementFX" },
			{ "ccbgssse013-dawnfang.esl", 0x000819, "ccbgssse013-dawnfang.esl", 0x00080B, "VAEReborn.esp", 0x000840, "ccBGSSSE013_EnchFireDamageFFContact" },
			{ "ccbgssse013-dawnfang.esl", 0x00081A, "VAEReborn.esp", 0x000837, "VAEReborn.esp", 0x000840, "ccBGSSSE013_EnchFrostDamageFFContact" },
			{ "ccbgssse016-umbra.esm", 0x00C831, "VAEReborn.esp", 0x000838, "VAEReborn.esp", 0x000841, "ccBGSSSE016_EnchAbsorbHealthFFContact_SoulTrapFX" },
			{ "ccbgssse020-graycowl.esl", 0x00086F, "VAEReborn.esp", 0x000839, "VAEReborn.esp", 0x000842, "ccBGSSSE020_EnchTurnUndeadFFContact" },
			{ "ccbgssse067-daedinv.esm", 0x17094E, "VAEReborn.esp", 0x00083A, "VAEReborn.esp", 0x000844, "ccBGSSSE067_EnchBanishFFContact" },
			{ "ccbgssse067-daedinv.esm", 0x175AC0, "VAEReborn.esp", 0x00083B, "VAEReborn.esp", 0x000843, "ccBGSSSE067_EnchFireDamageFFContact_NoShader" },
			{ "Skyrim.esm", 0x106617, "VAEReborn.esp", 0x000814, "Skyrim.esm", 0x10A043, "ChillrendEnchFrostDamageFFContact" },
			{ "Skyrim.esm", 0x106616, "VAEReborn.esp", 0x000814, "Skyrim.esm", 0x10A043, "ChillrendParalysisFFContact" },
			{ "Skyrim.esm", 0x091AE5, "VAEReborn.esp", 0x00081A, "VAEReborn.esp", 0x00082E, "DA07MehrunesRazorMagicEffect" },
			{ "Skyrim.esm", 0x10FAF1, "VAEReborn.esp", 0x000811, "VAEReborn.esp", 0x000828, "DA08EnchAbsorbHealthFFContact" },
			{ "Skyrim.esm", 0x0FEE38, "", 0x000000, "VAEReborn.esp", 0x00084B, "DA09EncDawnbreakeScriptEffect" },
			{ "Skyrim.esm", 0x0FEFBC, "", 0x000000, "VAEReborn.esp", 0x00084B, "DA09EnchDawnbreakerEnchFireDamageFFContact" },
			{ "Dawnguard.esm", 0x016696, "VAEReborn.esp", 0x00080C, "VAEReborn.esp", 0x00082D, "DLC1DawnguardRuneAxeDamageEffect" },
			{ "Dawnguard.esm", 0x016695, "VAEReborn.esp", 0x00080C, "VAEReborn.esp", 0x00082D, "DLC1DawnguardRuneAxeIncrementKills" },
			{ "Dawnguard.esm", 0x006924, "VAEReborn.esp", 0x00080C, "VAEReborn.esp", 0x00082D, "DLC1DawnguardRuneVisualsEffect" },
			{ "Dawnguard.esm", 0x015719, "VAEReborn.esp", 0x00080C, "VAEReborn.esp", 0x00082D, "DLC1EnchSunDamage" },
			{ "Dawnguard.esm", 0x01571B, "VAEReborn.esp", 0x00080C, "VAEReborn.esp", 0x00082D, "DLC1EnchSunDamageUndead" },
			{ "Dawnguard.esm", 0x014557, "VAEReborn.esp", 0x00080C, "VAEReborn.esp", 0x00082D, "DLC1RuneHammerVisualEffect" },
			{ "Dragonborn.esm", 0x03570C, "VAEReborn.esp", 0x000804, "VAEReborn.esp", 0x00082B, "DLC2EnchAbsorbHealthFFContact50" },
			{ "Dragonborn.esm", 0x03570E, "VAEReborn.esp", 0x000804, "VAEReborn.esp", 0x00082B, "DLC2EnchAbsorbMagickaFFContact50" },
			{ "Dragonborn.esm", 0x03570F, "VAEReborn.esp", 0x000804, "VAEReborn.esp", 0x00082B, "DLC2EnchAbsorbStaminaFFContact50" },
			{ "Dragonborn.esm", 0x02C46B, "VAEReborn.esp", 0x000804, "VAEReborn.esp", 0x00082B, "DLC2EnchFireDamageFFContact50" },
			{ "Dragonborn.esm", 0x02C46D, "VAEReborn.esp", 0x000804, "VAEReborn.esp", 0x00082B, "DLC2EnchFrostDamageFFContact50" },
			{ "Dragonborn.esm", 0x02C46C, "VAEReborn.esp", 0x000804, "VAEReborn.esp", 0x00082B, "DLC2EnchShockDamageFFContact50" },
			{ "Skyrim.esm", 0x03B0B1, "VAEReborn.esp", 0x00080A, "VAEReborn.esp", 0x000826, "dunSilentMoonsEnchFFContact" },
			{ "Skyrim.esm", 0x0AA155, "VAEReborn.esp", 0x000800, "VAEReborn.esp", 0x000828, "EnchAbsorbHealthFFContact" },
			{ "Skyrim.esm", 0x0AA156, "VAEReborn.esp", 0x000801, "VAEReborn.esp", 0x00082A, "EnchAbsorbMagickaFFContact" },
			{ "Skyrim.esm", 0x0AA157, "VAEReborn.esp", 0x000802, "VAEReborn.esp", 0x000826, "EnchAbsorbStaminaFFContact" },
			{ "Skyrim.esm", 0x0ACBB5, "VAEReborn.esp", 0x000803, "VAEReborn.esp", 0x000827, "EnchBanishFFContact" },
			{ "Skyrim.esm", 0x04605A, "VAEReborn.esp", 0x000808, "VAEReborn.esp", 0x000824, "EnchFireDamageFFContact" },
			{ "Skyrim.esm", 0x04605B, "VAEReborn.esp", 0x000809, "VAEReborn.esp", 0x000825, "EnchFrostDamageFFContact" },
			{ "Skyrim.esm", 0x05B451, "VAEReborn.esp", 0x000807, "VAEReborn.esp", 0x00082C, "EnchInfluenceConfDownFFContactLow" },
			{ "Skyrim.esm", 0x05B44F, "VAEReborn.esp", 0x000805, "VAEReborn.esp", 0x000830, "EnchMagickaDamageFFContact" },
			{ "Skyrim.esm", 0x0ACBB6, "VAEReborn.esp", 0x00080B, "VAEReborn.esp", 0x000823, "EnchParalysisFFContact" },
			{ "Skyrim.esm", 0x04605C, "VAEReborn.esp", 0x00080D, "VAEReborn.esp", 0x000829, "EnchShockDamageFFContact" },
			{ "Skyrim.esm", 0x05B452, "VAEReborn.esp", 0x00080F, "VAEReborn.esp", 0x00082E, "EnchSoulTrapFFContact" },
			{ "Skyrim.esm", 0x05B450, "VAEReborn.esp", 0x000806, "VAEReborn.esp", 0x000831, "EnchStaminaDamageFFContact" },
			{ "Skyrim.esm", 0x05B46B, "VAEReborn.esp", 0x00080E, "VAEReborn.esp", 0x00082F, "EnchTurnUndeadFFContact" },
			{ "Skyrim.esm", 0x0F1AC2, "VAEReborn.esp", 0x000810, "VAEReborn.esp", 0x000829, "MQ203DragonDamageFFContact" },
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

		std::size_t VaerOwnBack()
		{
			// a plugin that is not loaded (a Creation Club file the player does not have) is simply skipped
			std::size_t back = 0, already = 0, absent = 0;
			auto*       dh = RE::TESDataHandler::GetSingleton();
			for (const auto& c : kVaerOwn) {
				auto* effect = dh->LookupForm<RE::EffectSetting>(c.effect, c.effectPlugin);
				auto* art = c.artPlugin.empty() ? nullptr : dh->LookupForm<RE::BGSArtObject>(c.art, c.artPlugin);
				auto* shader = dh->LookupForm<RE::TESEffectShader>(c.shader, c.shaderPlugin);
				if (!effect || (!c.artPlugin.empty() && !art) || !shader) {
					++absent;
					continue;
				}
				if ((c.artPlugin.empty() || effect->data.enchantEffectArt == art) && effect->data.enchantShader == shader) {
					++already;
					continue;
				}
				if (!c.artPlugin.empty()) {
					effect->data.enchantEffectArt = art;
				}
				effect->data.enchantShader = shader;
				++back;
				SKSE::log::info("[VAER] {} had lost VAER's swirl; given back", c.name);
			}
			SKSE::log::info("VAER: {} of VAER's effect(s) given their swirl back, {} still had it, {} not in this load order", back,
				already, absent);
			std::size_t typos = 0;
			for (const auto& c : kVaerOwn) {
				auto*             shader = dh->LookupForm<RE::TESEffectShader>(c.shader, c.shaderPlugin);
				const char*       tex = shader ? shader->fillTexture.textureName.c_str() : nullptr;
				const std::size_t len = tex ? std::strlen(tex) : 0;
				if (len > 8 && _stricmp(tex + len - 8, ".dds.dds") == 0) {
					const std::string fixed(tex, len - 4);
					SKSE::log::info("[VAER] {}: shader texture {} -> {}", c.name, tex, fixed);
					shader->fillTexture.textureName = fixed;
					++typos;
				}
			}
			SKSE::log::info("VAER: {} shader texture name(s) ending .dds.dds fixed", typos);
			return back;
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
		VaerOwnBack();
		if (VaerPluginLoaded("Thaumaturgy.esp")) {
			ThaumaturgyCopies();
		} else {
			SKSE::log::info("VAER: Thaumaturgy.esp is not loaded; no copies to dress");
		}
	}
}
