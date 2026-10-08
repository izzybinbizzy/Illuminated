// Illuminated - SKSE plugin
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
	constexpr std::string_view kOurFolder = "Illuminated";
	constexpr std::string_view kCSFolder = "CS Light";

	// ------------------------------------------------------------------ Text.cpp: small text helpers
	std::string Lower(std::string_view a_text);
	std::string NormalPath(std::string_view a_path);
	bool        Contains(std::string_view a_text, std::string_view a_part);
	std::string Trim(std::string_view a_text);
	bool        ParseInt(std::string_view a_text, int& a_out);
	bool        ParseFloat(std::string_view a_text, float& a_out);

	// ------------------------------------------------------------------ Lighting.cpp: which lighting the game draws with
	enum class Lighting : int
	{
		kShaders = 0,  // Community Shaders: inverse square lights, as the configs are made
		kEnb = 1,      // an ENB: the game's own lighting, every light drawn plain
		kVanilla = 2,  // the game's own lighting, every light drawn plain
	};

	struct PlainLight
	{
		float fade{ 0.0f }, radius{ 0.0f };
	};

	void        ReadLighting();  // data load, before pass 0
	Lighting    LightingPick();
	const char* LightingName(Lighting a_pick);
	bool        InverseSquare();                                       // lights are drawn inverse square (Community Shaders)
	float       IslReach(float a_fade, float a_cutoff, float a_size);  // how far an inverse-square light reaches
	PlainLight  Plain(float a_fade, float a_radius, float a_reach);    // that light, drawn by the game's own lighting
	std::string LightingReport();

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

	// ------------------------------------------------------------------ Settings.cpp: the menu's settings (Illuminated only)
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

	// one in-memory copy of a light a config names; the sliders set its fade, radius and cutoff (LightCopies.cpp)
	struct LightCopy
	{
		std::string        id, base;
		float              fade{ 0.0f };      // the config's own fade, or 0: the base light's
		int                radius{ 0 };       // the config's own radius, or 0: the base light's
		float              cutoff{ 0.0f };    // the config's own inverse-square cutoff, or 0: the base light's
		bool               flicker{ false };  // its lights flicker (a fadeController): Brightness is applied as they are drawn
		RE::TESObjectLIGH* form{ nullptr };
		float              startFade{ 0.0f };
		std::uint32_t      startRadius{ 0 };
		float              startCutoff{ 0.0f };  // 0: not an inverse-square light, its falloff is left alone
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
	void                             SaveSettings();
	void                             SetSetting(std::size_t a_index, int a_value);
	std::vector<Setting>&            Settings();
	const std::vector<MenuNote>&     Notes();
	const std::vector<SprayMarker>&  SprayMarkers();
	bool                             PluginLoaded(std::string_view a_plugin);
	bool                             SettingAvailable(const Setting& a_setting);
	int                              AllowedValue(const Setting& a_setting, int a_value);  // clamped, and on a slider's step
	int                              SettingValue(std::string_view a_id, int a_fallback);
	bool                             RegisterEditorID(RE::TESForm* a_form, const std::string& a_id);
	std::vector<LightCopy>&          LightCopies();
	void                             MakeLightCopies();                                                              // LightCopies.cpp
	void                             StreamLights();                                                                 // StreamLights.cpp: pass 6, lights that travel with a spray or bolt
	void                             ApplyStreamLights(bool a_log);                                                  // StreamLights.cpp: the setting, on or off
	void                             StreamLightsFrame();                                                            // StreamLights.cpp: once a frame, what a loader thread left to do
	void                             ApplyLightStrength(bool a_log);                                                 // LightCopies.cpp: the sliders onto the copies (a flicker's Brightness as it is drawn)
	void                             RequestRefresh();                                                               // LightCopies.cpp: RefreshLights at the next frame, on the main thread
	void                             NoteFadeWrite(const RE::NiPointLight* a_light, float a_before, float a_after);  // LightCopies.cpp: the fading module wrote it (never scaled twice)
	void                             RegisterMenu();                                                                 // Menu.cpp
	void                             OfferToDevBench();                                                              // DevBench.cpp: the settings and the light copies, for a test bench
	void                             RefreshLights();                                                                // EffectLights.cpp: the sliders and passes 1, 2 and 6 again, for the settings as they are now

	// ------------------------------------------------------------------ Configs.cpp: each config light, for the game's own light records
	struct ConfigLight
	{
		std::string                          light;  // the light copy it names (an IlluminatedLight editor ID)
		std::uint8_t                         r{ 0 }, g{ 0 }, b{ 0 };
		bool                                 hasColor{ false };      // false: the copy's own color
		bool                                 portalStrict{ false };  // kept inside its cell, as Light Placer keeps it
		bool                                 controller{ false };    // a fadeController drives its fade
		bool                                 flash{ false };         // that controller is a one-off flash (it ends at 0), not a flicker
		float                                meanFade{ 0.0f };       // the controller's fade, averaged over the time it runs
		std::vector<std::pair<float, float>> keys;                   // a flicker's keys (time, fade), sorted - empty for a flash or none
		std::uint8_t                         interpolation{ 1 };     // 0 step, 1 linear, 2 cubic (the controller's "interpolation")
		std::vector<std::vector<Clause>>     test;                   // the settings it waits on (ParseConditions)
	};
	using ConfigEntry = std::vector<ConfigLight>;
	using ConfigLightMap = std::unordered_map<std::string, std::vector<std::shared_ptr<const ConfigEntry>>>;  // model -> its entries
	void                  KeepConfigLights(bool a_keep);                                                      // before ReadCoverage: keep every light row too, per model
	const ConfigLightMap& ConfigLights();
	void                  DropConfigLights();  // once the record lights are made

	// ------------------------------------------------------------------ RecordLights.cpp: the game's own light records (no Light Placer)
	void                             DecideRecordRoute();  // before ReadCoverage
	[[nodiscard]] bool               RecordRoute();
	void                             MakeRecordLights();                             // after MakeLightCopies and ReadCoverage, before pass 1
	void                             ApplyRecordColors();                            // the Light colors setting onto every record light
	void                             RecordFlicker(RE::ActorMagicCaster* a_caster);  // after a caster's update: its hand light's flicker
	void                             AdvanceRecordFlicker(float a_delta);            // once a frame, on the main thread
	[[nodiscard]] RE::TESObjectLIGH* RecordLightFor(const std::string& a_model);     // nullptr: no row lights that model now
	// automatic lights (his go-to pick, 2026-10-07): a light of ours in a_color, at the middle strength and reach of the tuned
	// hand lights a_tuned (made once per color, at data load; follows the sliders and the Light colors setting)
	[[nodiscard]] RE::TESObjectLIGH* AutoLight(RE::Color a_color, const std::vector<const RE::TESObjectLIGH*>& a_tuned);
	[[nodiscard]] std::size_t        AutoLightCount();
	[[nodiscard]] std::size_t        AutoCastingCount();  // CastingLights.cpp: the spells given an automatic light
	// stepping aside for other light mods (RecordLights.cpp)
	[[nodiscard]] bool YieldToENBLight();  // its switch on, ENB Light.esp loaded, the lighting ENB
	[[nodiscard]] bool TouchedByENBLight(const RE::TESForm* a_form);
	// per-element colors (RecordLights.cpp): 0 none, 1 fire, 2 frost, 3 shock
	[[nodiscard]] int                ElementOf(const RE::EffectSetting* a_effect);
	[[nodiscard]] int                ElementOfForm(const RE::TESForm* a_form);                                // a projectile or explosion
	void                             PrepareElementLights(const RE::TESObjectLIGH* a_record, int a_element);  // data load
	void                             PrepareElementLightsFor(const std::string& a_model, int a_element);      // data load: every light the model can wear
	[[nodiscard]] RE::TESObjectLIGH* ElementLight(RE::TESObjectLIGH* a_record, int a_element);                // the element's colored copy, or a_record on Auto
	[[nodiscard]] std::string        RecordReport();                                                          // for DevBench

	// ------------------------------------------------------------------ Configs.cpp: what the configs light
	struct Coverage
	{
		std::set<std::string> models;   // lowercased .nif paths
		std::set<std::string> shaders;  // lowercased tokens from "formIDs" arrays
		std::size_t           files{ 0 };
		// every light row's settings tests, per model: a model is lit while the tests of any of its rows hold. An entry's
		// tests are kept once and shared by all of its models (never copied per model)
		using Tests = std::vector<std::vector<std::vector<Clause>>>;  // one entry: each light's settings test
		std::unordered_map<std::string, std::vector<std::shared_ptr<const Tests>>> modelTests;
		bool                                                                       ModelLit(const std::string& a_model) const;
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
		bool        on{ false };
		int         radiusAbs{ 1200 };
		int         radiusPc{ 216 };
		float       fade{ 1.7f };
		float       frostFade{ 0.8f };
		float       falloff{ 2.0f };
		bool        frostSet{ false }, shockSet{ false }, fireSet{ false };
		Rgb         frost, shock, fireDelta;
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
	void LightSettings();                       // LightSettings.cpp
	void CastingLights(const Coverage& a_cov);  // CastingLights.cpp
	template <class T>
	void EffectLights(const Coverage& a_cov, std::string_view a_kind);  // EffectLights.cpp
	void PoisonRuneArt();                                               // PoisonRune.cpp
	void SprayLights();                                                 // SprayLights.cpp
	void ApplyCastingLights(bool a_log);                                // CastingLights.cpp: pass 1 again, for the settings as they are now
	void ApplyEffectLights(bool a_log);                                 // EffectLights.cpp: pass 2 again
	void VaerSwirls();                                                  // VaerSwirls.cpp: pass 7, VAER Reborn's brighter strands and Thaumaturgy's copies
	void Wards();                                                       // Wards.cpp: one dome per ward, 360 Ward's sphere in the vanilla blue

	// ------------------------------------------------------------------ Enchantments.cpp
	void DoubledEnchantments(const Coverage& a_cov);
	bool AnyLitShaders();
	void WatchCraftingMenu();
	void ForgetCreatedEnchantments();
	void UseOriginals(std::string_view a_why);
	void UseQuiet(std::string_view a_why);
}
