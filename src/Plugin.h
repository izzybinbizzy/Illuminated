// Luminous Arcana - SKSE plugin
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE.txt and the notice at the top of main.cpp.
//
// What the files share. Each block below names the file that defines it; a helper that only one
// file uses lives in that file and is not listed here.

#pragma once

#include "PCH.h"

namespace Plugin
{
	namespace fs = std::filesystem;

	// ------------------------------------------------------------------ rules more than one file reads
	constexpr std::string_view kOurFolder = "Luminous Arcana";
	constexpr std::string_view kCSFolder = "CS Light";

	// ------------------------------------------------------------------ Text.cpp: small text helpers
	std::string Lower(std::string_view a_text);
	std::string NormalPath(std::string_view a_path);
	bool        Contains(std::string_view a_text, std::string_view a_part);
	std::string Trim(std::string_view a_text);
	bool        ParseInt(std::string_view a_text, int& a_out);
	bool        ParseFloat(std::string_view a_text, float& a_out);

	// ------------------------------------------------------------------ EditorIDs.cpp: editor IDs, recorded as each form loads
	void        RememberEditorID(const RE::TESForm* a_form, const char* a_id);
	void        ForgetEditorIDs();  // once the passes have run: the names are not needed again
	std::string EditorID(const RE::TESForm* a_form);
	std::string Label(const RE::TESForm* a_form);

	template <class T>
	struct EditorIDHook
	{
		static bool thunk(RE::TESForm* a_this, const char* a_id)
		{
			RememberEditorID(a_this, a_id);
			return func(a_this, a_id);
		}
		static inline REL::Relocation<decltype(thunk)> func;
		static void                                    Install()
		{
			REL::Relocation<std::uintptr_t> vtbl{ T::VTABLE[0] };
			func = vtbl.write_vfunc(0x33, thunk);
		}
	};

	// ------------------------------------------------------------------ Settings.cpp: the menu's settings (Luminous Arcana only)
	struct Clause
	{
		std::string global;  // lower case
		int         value{ 0 };
	};

	struct Setting
	{
		std::string              id, ini, page, group, label;
		bool                     isChoice{ false }, isSlider{ false }, restart{ false }, autoAll{ false };
		std::vector<std::string> choices, tips, needs, autoPlugins;
		int                      defaultValue{ 0 }, value{ 0 };
		int                      minValue{ 0 }, maxValue{ 1 }, stepValue{ 1 };  // a slider's range and step (percent)
		RE::TESGlobal*           global{ nullptr };
	};

	struct MenuNote
	{
		std::string page, group, label, text;
	};

	// one in-memory copy of a light a config names; the sliders set its fade and radius (LightCopies.cpp)
	struct LightCopy
	{
		std::string         id, base;
		float               fade{ 0.0f };  // the config's own fade, or 0: the base light's
		int                 radius{ 0 };   // the config's own radius, or 0: the base light's
		RE::TESObjectLIGH*  form{ nullptr };
		float               startFade{ 0.0f };
		std::uint32_t       startRadius{ 0 };
	};

	struct SprayMarker
	{
		std::string                      name;
		std::vector<std::vector<Clause>> when;
		std::string                      values;  // key=value;key=value
	};

	std::vector<std::vector<Clause>> ParseConditions(const std::vector<std::string>& a_conditions);
	bool                             ConditionsHold(const std::vector<std::vector<Clause>>& a_tests);
	void                             LoadSettings();
	void                             ApplyGlobals();
	bool                             ReloadSettingsIni();
	void                             SaveSettings();
	void                             SetSetting(std::size_t a_index, int a_value);
	std::vector<Setting>&            Settings();
	const std::vector<MenuNote>&     Notes();
	const std::vector<SprayMarker>&  SprayMarkers();
	std::size_t                      GlobalsFromPlugin();
	std::size_t                      GlobalsMadeInMemory();
	bool                             PluginLoaded(std::string_view a_plugin);
	bool                             SettingAvailable(const Setting& a_setting);
	int                              AllowedValue(const Setting& a_setting, int a_value);  // clamped, and on a slider's step
	int                              SettingValue(std::string_view a_id, int a_fallback);
	bool                             RegisterEditorID(RE::TESForm* a_form, const std::string& a_id);
	std::vector<LightCopy>&          LightCopies();
	void                             MakeLightCopies();                // LightCopies.cpp
	void                             WatchCamera();                    // CameraWatch.cpp: what the camera does for 20 s after a load
	void                             StreamLights();                   // StreamLights.cpp: pass 6, lights that travel with a spray or bolt
	void                             ApplyStreamLights(bool a_log);    // StreamLights.cpp: the setting, on or off
	void                             ApplyLightStrength(bool a_log);  // LightCopies.cpp: the sliders onto the copies
	void                             InstallPapyrus();
	void                             RegisterMenu();   // Menu.cpp
	void                             RefreshLights();  // CastingLights.cpp: passes 1 and 2 again, for the settings as they are now

	// ------------------------------------------------------------------ Configs.cpp: what the configs light
	struct Coverage
	{
		std::set<std::string> models;   // lowercased .nif paths
		std::set<std::string> shaders;  // lowercased tokens from "formIDs" arrays
		std::size_t           files{ 0 };
		// every light row's settings tests, per model: a model is lit while the tests of any of its rows hold
		std::unordered_map<std::string, std::vector<std::vector<std::vector<Clause>>>> modelTests;
		bool ModelLit(const std::string& a_model) const;
	};

	fs::path        LightPlacerDir(std::string_view a_folder);
	const Coverage& ReadCoverage();

	// ------------------------------------------------------------------ SprayMarkers.cpp: the installer's spray markers
	struct Rgb
	{
		int r{ 0 }, g{ 0 }, b{ 0 };
	};

	struct SprayChoice
	{
		bool  on{ false };
		int   radiusAbs{ 1200 };
		int   radiusPc{ 216 };
		float fade{ 1.7f };
		float frostFade{ 0.8f };
		float falloff{ 2.0f };
		bool  frostSet{ false }, shockSet{ false }, fireSet{ false };
		Rgb   frost, shock, fireDelta;
		std::string found;
	};

	SprayChoice ReadSprayChoice();

	// ------------------------------------------------------------------ FormCopies.cpp: making copies in memory
	template <class T>
	T* NewForm()
	{
		// Create() is not const, so the factory pointer must not be either
		auto* factory = RE::IFormFactory::GetConcreteFormFactoryByType<T>();
		return factory ? factory->Create() : nullptr;
	}

	RE::TESObjectLIGH*   CopyLight(const RE::TESObjectLIGH* a_src);
	RE::TESEffectShader* CopyShader(const RE::TESEffectShader* a_src);
	RE::EffectSetting*   CopyEffect(RE::EffectSetting* a_src);

	// ------------------------------------------------------------------ the passes, in the order they run
	void LightSettings();  // LightSettings.cpp
	void CastingLights(const Coverage& a_cov);  // CastingLights.cpp
	template <class T>
	void EffectLights(const Coverage& a_cov, std::string_view a_kind);  // EffectLights.cpp
	void PoisonRuneArt();  // PoisonRune.cpp
	void SprayLights();  // SprayLights.cpp
	void ApplyCastingLights(bool a_log);  // CastingLights.cpp: pass 1 again, for the settings as they are now
	void ApplyEffectLights(bool a_log);   // EffectLights.cpp: pass 2 again

	// ------------------------------------------------------------------ Enchantments.cpp
	void DoubledEnchantments(const Coverage& a_cov);
	bool AnyLitShaders();
	void WatchCraftingMenu();
	void ForgetCreatedEnchantments();
	void UseOriginals(std::string_view a_why);
	void UseQuiet(std::string_view a_why);
}
