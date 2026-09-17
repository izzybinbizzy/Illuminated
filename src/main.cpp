// Luminous Arcana - SKSE plugin
// Copyright (C) 2026 izzydoingit
//
// This program is free software: you can redistribute it and/or modify it under the terms of the
// GNU General Public License as published by the Free Software Foundation, either version 3 of the
// License, or (at your option) any later version. It is distributed WITHOUT ANY WARRANTY; see the
// GNU General Public License in LICENSE.txt for details.
//
// What it does, once, when the game has finished loading its plugins:
//   0. its own copies of the game's magic lights, carrying the look its lights were made with (see pass 0);
//   1. a magic effect whose casting art Luminous Arcana or CS Light lights loses the game's own
//      casting light, so the hand does not carry two lights;
//   2. the same for projectiles, explosions and hazards whose model is lit - except cone and flame
//      projectiles, which keep their light, and poison sprays, which lose it whether lit or not;
//   3. the Dragonborn poison rune gets the casting art its lit hand needs;
//   4. with the Spray Lights setting on, each spray projectile gets a private copy of its own
//      light, stretched to cover the spray and colored from the settings' markers;
//   5. an enchantment carrying two or more lit shaders keeps the light of its first one only - the ones
//      plugins define and the ones made at the enchanting table, never letting a save hold a copy.
// If Let There Be Glow's own plugin is loaded, nothing is changed at all: the two mods are never
// installed together.
// Every choice is a setting in the menu (SKSE Menu Framework, and the MCM when Luminous Arcana.esp is
// installed). Passes 1 and 2 follow the settings live; pass 4 reads them when the game loads.
//
// Where each part lives: main.cpp (this file) runs the passes in order; Plugin.h lists what the files
// share; Text.cpp, EditorIDs.cpp, Configs.cpp, SprayMarkers.cpp and FormCopies.cpp are the helpers;
// Settings.cpp holds the settings and Menu.cpp the menu pages (SKSEMenuFramework.h is that mod's own header);
// CastingLights.cpp, EffectLights.cpp, PoisonRune.cpp, SprayLights.cpp and Enchantments.cpp are passes 1 to 5;
// LightSettings.cpp is pass 0.

#include "Plugin.h"

using namespace Plugin;

namespace
{
	// ------------------------------------------------------------------ rules
	constexpr std::string_view kOtherPluginDll = "LetThereBeGlow.dll";

	// ------------------------------------------------------------------ order of work
	bool OtherPluginLoaded()
	{
		return REX::W32::GetModuleHandleA(kOtherPluginDll.data()) != nullptr;
	}

	void OnDataLoaded()
	{
		const auto loadStarted = std::chrono::steady_clock::now();
		if (OtherPluginLoaded()) {
			SKSE::log::info("Let There Be Glow's plugin ({}) is loaded: this plugin changes nothing. "
							"Luminous Arcana and Let There Be Glow are never used together.",
				kOtherPluginDll);
			return;
		}
		LoadSettings();  // first: Light Placer reads the settings' globals in its conditions
		RegisterMenu();
		LightSettings();
		const auto& cov = ReadCoverage();
		SKSE::log::info("configs: {} file(s), {} lit model(s), {} shader name(s)", cov.files, cov.models.size(), cov.shaders.size());
		if (cov.files == 0) {
			SKSE::log::warn("no Luminous Arcana configs were found under Data\\LightPlacer; nothing was changed");
			return;
		}
		CastingLights(cov);
		EffectLights<RE::BGSProjectile>(cov, "projectile");
		EffectLights<RE::BGSExplosion>(cov, "explosion");
		EffectLights<RE::BGSHazard>(cov, "hazard");
		ApplyEffectLights(true);
		PoisonRuneArt();
		SprayLights();
		DoubledEnchantments(cov);
		if (AnyLitShaders()) {
			WatchCraftingMenu();
		}
		ForgetEditorIDs();
		SKSE::log::info("done in {:.1f} ms",
			std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - loadStarted).count());
	}

	void OnMessage(SKSE::MessagingInterface::Message* a_msg)
	{
		if (!a_msg) {
			return;
		}
		switch (a_msg->type) {
		case SKSE::MessagingInterface::kDataLoaded:
			OnDataLoaded();
			break;
		case SKSE::MessagingInterface::kSaveGame:
			// the save is written inside the call this message comes before; the copies return on the next frame
			if (AnyLitShaders()) {
				UseOriginals("saving");
				SKSE::GetTaskInterface()->AddTask([]() { UseQuiet("save written"); });
			}
			break;
		case SKSE::MessagingInterface::kPreLoadGame:
			// the enchantments made during play are about to be replaced by the save's; never touch them again
			ForgetCreatedEnchantments();
			break;
		case SKSE::MessagingInterface::kPostLoadGame:
		case SKSE::MessagingInterface::kNewGame:
			// a save holds the plugin's globals as they were when it was made; the settings file is the truth
			ApplyGlobals();
			if (AnyLitShaders()) {
				UseQuiet("game loaded");
			}
			break;
		default:
			break;
		}
	}
}

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
	SKSE::Init(a_skse);
	EditorIDHook<RE::EffectSetting>::Install();
	EditorIDHook<RE::BGSProjectile>::Install();
	EditorIDHook<RE::BGSExplosion>::Install();
	EditorIDHook<RE::BGSHazard>::Install();
	EditorIDHook<RE::TESObjectLIGH>::Install();
	EditorIDHook<RE::TESEffectShader>::Install();
	EditorIDHook<RE::EnchantmentItem>::Install();
	SKSE::GetMessagingInterface()->RegisterListener(OnMessage);
	InstallPapyrus();
	SKSE::log::info("Luminous Arcana plugin loaded; waiting for the game's data");
	return true;
}
