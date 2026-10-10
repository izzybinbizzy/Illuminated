// Illuminated - SKSE plugin
// Copyright (C) 2026 izzydoingit
//
// This program is free software: you can redistribute it and/or modify it under the terms of the
// GNU General Public License as published by the Free Software Foundation, either version 3 of the
// License, or (at your option) any later version. It is distributed WITHOUT ANY WARRANTY; see the
// GNU General Public License in LICENSE.txt for details.
//
// What it does, once, when the game has finished loading its plugins:
//   0. its own copies of the game's magic lights, carrying the look its lights were made with (see pass 0);
//   1. a magic effect whose casting art Illuminated or CS Light lights loses the game's own
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
// Every choice is a setting in the menu (SKSE Menu Framework - the only menu this mod has).
// Passes 1 and 2 follow the settings live; pass 4 reads them when the game loads. The
// Brightness and Reach sliders set the fade, radius and cutoff of the light copies LightCopies.cpp makes;
// a light that is already lit follows at the next frame.
// Without Light Placer, passes 1 and 2 give the game's own light records Illuminated's lights instead of taking
// theirs off (RecordLights.cpp).
//
// THE FILES, AND WHAT EACH ONE IS FOR
//   main.cpp           this file - the passes in order, and the SKSE messages
//   Plugin.h           what the files share; LightKit.h the small helpers RELight - Spell Addon carries too
//   Text.cpp           lower case, paths, trimming, number parsing      EditorIDs.cpp  the passes' way to an editor ID
//   Settings.cpp       the settings files, the INI and the game globals Menu.cpp       the menu pages
//   DevBench.cpp       DevBench's inspect and control (optional)       Lighting.cpp   Community Shaders, ENB or Vanilla
//   Configs.cpp        what the Light Placer configs light              FormCopies.cpp copies of forms, in memory
//   LightSettings.cpp  pass 0      CastingLights.cpp  pass 1      EffectLights.cpp  pass 2 (and RefreshLights)
//   PoisonRune.cpp     pass 3      SprayLights.cpp    pass 4      Enchantments.cpp  pass 5
//   StreamLights.cpp   pass 6      VaerSwirls.cpp     pass 7      Wards.cpp         one dome per ward
//   SprayMarkers.cpp   the spray settings                         LightCopies.cpp   the sliders, and every hook
//   RecordLights.cpp   the game's own light records, lit without Light Placer
//   ArtLights.cpp      without Light Placer: lights of ours on lit art and weapon models (bound weapons)
//   Fade*.cpp, Fade*.h the fading module (identical in RELight - Spell Addon but for FadeConfig.h, FadeOwnLight.cpp
//                      and FadeDevBench.cpp - PC Runner\fade copies check.py)
//   SKSEMenuFramework.h, DevBenchAPI.*  those mods' own files; DevBenchGlue.h, MenuStyle.h, Translation.h are
//                      shared word for word with our other plugins

#include "Fade.h"
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

	void LoadEverything()
	{
		const auto loadStarted = std::chrono::steady_clock::now();
		if (OtherPluginLoaded()) {
			SKSE::log::info(
				"Let There Be Glow's plugin ({}) is loaded: this plugin changes nothing. "
				"Illuminated and Let There Be Glow are never used together.",
				kOtherPluginDll);
			return;
		}
		// the advanced settings file's own values (his rule 2026-10-10) - read with the fading module's, in Fade::OnDataLoaded
		Fade::Tuning::Register("Lights", "NearbyDistance", 2800.0f, 300.0f, 30000.0f, "Hand lights for - Everyone nearby: how far from you, in game units", gNearby);
		Fade::Tuning::Register("Vanilla and ENB", "GroundLights", 1.0f, 0.0f, 1.0f, "1: a hand light lights the ground too (its twin, made with land lighting on); 0: off", gGroundLightsOn);
		Fade::Tuning::Register("Vanilla and ENB", "ArtLights", 1.0f, 0.0f, 1.0f, "1: bound weapons and lit art and weapons get their light without Light Placer; 0: off", gArtLightsOn);
		LoadSettings();  // first: Light Placer reads the settings' globals in its conditions
		ReadLighting();  // before pass 0: on ENB and Vanilla its lights are made plain
		RegisterMenu();
		Fade::OnDataLoaded();  // the fading module: lights follow charge and magicka (its own settings, rules, hooks, menu pages)
		RegisterMenuTail();    // Praedy's Staves last, below the fading pages (his word 2026-10-09)
		LightSettings();
		MakeLightCopies();    // after pass 0 (the copies take its flags), before Light Placer reads its configs
		VaerSwirls();         // pass 7: VAER Reborn's swirls - the settings are loaded by now (HIS CALL 2026-09-22)
		Wards();              // before pass 1: an effect it silences must not be followed by the casting-light pass
		DecideRecordRoute();  // no Light Placer: the configs light the game's own light records instead (RecordLights.cpp)
		const auto& cov = ReadCoverage();
		SKSE::log::info("configs: {} file(s), {} lit model(s), {} shader name(s)", cov.files, cov.models.size(), cov.shaders.size());
		if (RecordRoute()) {
			MakeRecordLights();  // before passes 1 and 2, which point the records at them
		}
		if (cov.files == 0) {
			SKSE::log::warn("no Illuminated configs were found under Data\\LightPlacer; nothing was changed");
			StreamLights();  // pass 6: the lights that travel with a spray or a bolt, and its two hooks
			RecordTablesReady();
			Fade::ForgetPassEditorIDs();  // only what a fading rule file can name stays, as on the full path
			return;
		}
		CastingLights(cov);
		EffectLights<RE::BGSProjectile>(cov, "projectile");
		EffectLights<RE::BGSExplosion>(cov, "explosion");
		EffectLights<RE::BGSHazard>(cov, "hazard");
		ApplyEffectLights(true);
		PoisonRuneArt();
		SprayLights();
		ApplyLightStrength(false);  // the sliders onto pass 4's spray lights too
		StreamLights();             // pass 6, after pass 4: a spray's hand light is its stretched copy by now; and its two hooks
		if (!RecordRoute()) {       // pass 5 is about Light Placer's lit shaders: there are none without it
			DoubledEnchantments(cov);
		}
		if (AnyLitShaders()) {
			WatchCraftingMenu();
		}
		RecordTablesReady();          // last: the hooks installed earlier may read the record tables from here on
		Fade::ForgetPassEditorIDs();  // the passes have run: only what a fading rule file can name stays
		SKSE::log::info("done in {:.1f} ms",
			std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - loadStarted).count());
	}

	// an exception never leaves into the game: one thrown by the load work (a file it cannot read, memory) is logged, and
	// the passes that already ran keep what they did
	void OnDataLoaded()
	{
		try {
			LoadEverything();
		} catch (const std::exception& e) {
			SKSE::log::critical(
				"the data-load work stopped part way: {} - the passes before it keep their work, the rest is not done "
				"this session",
				e.what());
		}
	}

	void OnMessage(SKSE::MessagingInterface::Message* a_msg)
	{
		if (!a_msg) {
			return;
		}
		switch (a_msg->type) {
		case SKSE::MessagingInterface::kPostLoad:
			Fade::OnPluginLoad();                        // the one editor-ID recorder (weapons, enchantments, magic effects), before the plugins load
			Fade::RecordEditorIDs<RE::BGSProjectile>();  // and what the passes read besides
			Fade::RecordEditorIDs<RE::BGSExplosion>();
			Fade::RecordEditorIDs<RE::BGSHazard>();
			Fade::RecordEditorIDs<RE::TESObjectLIGH>();
			Fade::RecordEditorIDs<RE::TESEffectShader>();
			OfferToDevBench();
			Fade::OnPostLoad();
			break;
		case SKSE::MessagingInterface::kDataLoaded:
			OnDataLoaded();
			break;
		case SKSE::MessagingInterface::kSaveGame:
			// the save is written inside the call this message comes before; the copies return on the next frame
			if (AnyLitShaders()) {
				UseOriginals("saving");
				if (auto* tasks = SKSE::GetTaskInterface()) {
					tasks->AddTask([]() { UseQuiet("save written"); });
				} else {
					UseQuiet("save written");
				}
			}
			break;
		case SKSE::MessagingInterface::kPreLoadGame:
			DropArtLights();  // the art and weapons they hang on are the old game's
			Fade::OnGameLoading();
			// the enchantments made during play are about to be replaced by the save's; never touch them again
			ForgetCreatedEnchantments();
			break;
		case SKSE::MessagingInterface::kPostLoadGame:
		case SKSE::MessagingInterface::kNewGame:
			// a save holds the plugin's globals as they were when it was made; the settings file is the truth
			ApplyGlobals();
			if (a_msg->type == SKSE::MessagingInterface::kNewGame) {
				Fade::OnGameLoading();
			}
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
	SKSE::Init(a_skse, { .trampoline = true, .trampolineSize = 128 });
	// the editor-ID recorders are installed at SKSE's post-load (OnMessage), still before the game reads its plugins
	SKSE::GetMessagingInterface()->RegisterListener(OnMessage);
	SKSE::log::info("Illuminated plugin loaded; waiting for the game's data");
	return true;
}
