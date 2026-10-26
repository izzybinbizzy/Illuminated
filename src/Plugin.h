// Illuminated - SKSE plugin
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE.txt and the notice at the top of main.cpp.
//
// What the files share. Each block below names the file that defines it; a helper that only one
// file uses lives in that file and is not listed here.

#pragma once

#include "ConfigText.h"
#include "LightKit.h"

namespace Plugin
{
	namespace fs = std::filesystem;

	// ------------------------------------------------------------------ rules more than one file reads
	constexpr std::string_view kOurFolder = "Illuminated";
	// Data\SKSE\Plugins\Illuminated: the settings, the presets, the translation and the Streams rows
	[[nodiscard]] inline fs::path OurFolder() { return fs::current_path() / "Data" / "SKSE" / "Plugins" / std::string(kOurFolder); }

	using LightKit::Relaxed;  // a value one thread writes and another reads (LightKit.h)

	// the mod's own values in the advanced settings file (his rule, Fade::Tuning; registered in main.cpp)
	inline std::atomic<float> gNearby{ 2800.0f };          // [Lights] NearbyDistance: Hand lights for - Everyone nearby
	inline std::atomic<float> gGroundLightsOn{ 1.0f };     // [Vanilla and ENB] GroundLights
	inline std::atomic<float> gGroundLightVRHand{ 0.0f };  // [Vanilla and ENB] GroundLightAtVRHand - VR only (his answer 2026-10-16)
	inline std::atomic<float> gArtLightsOn{ 1.0f };        // [Vanilla and ENB] ArtLights

	// ------------------------------------------------------------------ Text.cpp: small text helpers
	using LightKit::Lower;  // LightMath.h: the one lower-casing helper
	[[nodiscard]] std::string NormalPath(std::string_view a_path);
	[[nodiscard]] bool        Contains(std::string_view a_text, std::string_view a_part);
	using ConfigText::ParseFloat;  // ConfigText.h (game-free): a number only when the whole field is one
	using ConfigText::ParseInt;
	using ConfigText::Split;
	using ConfigText::Trim;

	// fire, frost or shock, read off an editor ID (the spray and stream passes)
	enum class Family
	{
		kNone,
		kFire,
		kFrost,
		kShock
	};
	// frost / ice, then flame / fire, then shock / lightning (+ spark / storm with a_sparkStorm); "voice" is read past first, so a
	// fire shout (VoiceFireBreath...) is never frost by its "ice"
	[[nodiscard]] Family FamilyOf(std::string_view a_editorID, bool a_sparkStorm);
	// projectiles that spray but are no spell (a trap's spotlight, a cloak's): passes 4 and 6 leave them alone
	inline constexpr std::array<std::string_view, 2> kNotSpellProjectiles{ "trapspotlightprojectile",
		"sum_any_projectile_defaultcloakprojectile" };

	// ------------------------------------------------------------------ Lighting.cpp: which lighting the game draws with
	enum class Lighting : int
	{
		kShaders = 0,  // Community Shaders: inverse square lights, as the configs are made
		kEnb = 1,      // an ENB: the game's own lighting, every light drawn plain
		kVanilla = 2,  // the game's own lighting, every light drawn plain
	};

	void                      ReadLighting();   // data load, before pass 0
	[[nodiscard]] Lighting    LightingFound();  // never picked by the player - his word
	[[nodiscard]] const char* LightingName(Lighting a_lighting);
	[[nodiscard]] bool        InverseSquare();  // lights are drawn inverse square (Community Shaders); off it, LightKit::PlainOf
	[[nodiscard]] std::string LightingReport();

	// ------------------------------------------------------------------ EditorIDs.cpp: editor IDs, through the fading module's
	// ONE recorder (Fade.h; main.cpp asks it for the form types the passes read)
	void                      RememberEditorID(const RE::TESForm* a_form, const char* a_id);
	[[nodiscard]] std::string EditorID(const RE::TESForm* a_form);
	[[nodiscard]] std::string Label(const RE::TESForm* a_form);

	// ------------------------------------------------------------------ Settings.cpp: the menu's settings (Illuminated only)
	using ConfigText::Clause;  // a GetGlobalValue item of a light's conditions (ConfigText.h)

	struct Setting
	{
		std::string              id, ini, page, group, label;
		bool                     isChoice{ false }, isSlider{ false }, restart{ false }, autoAll{ false };
		std::vector<std::string> choices, tips, needs, autoPlugins;
		int                      defaultValue{ 0 };
		Relaxed<int>             value{ 0 };                                    // the menu (render thread) sets it, the main thread reads it
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
		// pass 0's copy of a game magic light (LightSettings.cpp): the game's own light, and the look the copy was made with -
		// with every Illuminated light off the copy wears the game's own look, and its made look again after
		const RE::TESObjectLIGH* original{ nullptr };
		RE::OBJ_LIGH             madeData{};
	};

	struct SprayMarker
	{
		std::string                      name;
		std::vector<std::vector<Clause>> when;
		std::string                      values;  // key=value;key=value
	};

	using ConfigText::ParseConditions;  // ConfigText.h (game-free)
	[[nodiscard]] bool                            ConditionsHold(const std::vector<std::vector<Clause>>& a_tests);
	void                                          LoadSettings();
	void                                          ApplyGlobals();
	[[nodiscard]] bool                            SetLightPlacerRoute(bool a_lightPlacerDraws);  // the route global every Light Placer row waits on (his answer Q7); false = it could not be made
	void                                          SaveSettings();
	void                                          SetSetting(std::size_t a_index, int a_value, bool a_save = true);
	[[nodiscard]] std::vector<Setting>&           Settings();
	[[nodiscard]] const std::vector<MenuNote>&    Notes();
	[[nodiscard]] const std::vector<SprayMarker>& SprayMarkers();
	[[nodiscard]] bool                            PluginLoaded(std::string_view a_plugin);
	[[nodiscard]] bool                            PluginLoadedStartingWith(std::initializer_list<std::string_view> a_lowerPrefixes);  // lower-case name starts: a mod shipped under several plugin names
	[[nodiscard]] bool                            SettingAvailable(const Setting& a_setting);
	[[nodiscard]] int                             AllowedValue(const Setting& a_setting, int a_value);  // clamped, and on a slider's step
	[[nodiscard]] int                             SettingValue(std::string_view a_id, int a_fallback);
	[[nodiscard]] bool                            LightsOff();        // Performance > all of Illuminated's lights off (his switch)
	[[nodiscard]] bool                            PerformanceMode();  // Performance > Quality is on Performance: the rows of lights go, hand lights for the player and followers
	[[nodiscard]] bool                            RegisterEditorID(RE::TESForm* a_form, const std::string& a_id);
	[[nodiscard]] std::vector<LightCopy>&         LightCopies();
	void                                          MakeLightCopies();   // LightCopies.cpp
	void                                          NoteSpellHazards();  // LightCopies.cpp: the hazard tint's tables, while editor IDs are kept
	[[nodiscard]] float                           BrightnessNow();     // LightCopies.cpp: Brightness x Dim in daylight, as the copies take it
	// StreamLights.cpp: pass 6 - each spray, stream and bolt lit by its own light record made bigger and brighter (his stream
	// rule), and a ward dome's light when Dynamic Wards is not here
	void               StreamLights();
	[[nodiscard]] bool StreamLightsOn();                                                                // StreamLights.cpp: the setting
	void               ApplyStreamLights(bool a_log);                                                   // StreamLights.cpp: the setting, on or off
	void               StreamLightsFrame();                                                             // StreamLights.cpp: once a frame, what a loader thread left to do
	void               ApplyLightStrength(bool a_log, bool a_quiet = false);                            // LightCopies.cpp: the sliders onto the copies (a flicker's Brightness as it is drawn); a_quiet: a daylight step, no log line
	void               RequestRefresh();                                                                // LightCopies.cpp: RefreshLights at the next frame, on the main thread
	void               NoteFadeWrite(const RE::NiPointLight* a_light, float a_before, float a_after);   // LightCopies.cpp: the fading module wrote it (never scaled twice)
	void               NoteReachWrite(const RE::NiPointLight* a_light, float a_before, float a_after);  // LightCopies.cpp: the same for the reach (nothing to do here)
	void               RegisterMenu();                                                                  // Menu.cpp
	void               RegisterMenuTail();                                                              // Menu.cpp: the last page, after the fading module's pages
	void               OfferToDevBench();                                                               // DevBench.cpp: the settings and the light copies, for a test bench
	void               RefreshLights();                                                                 // EffectLights.cpp: the sliders and passes 1, 2 and 6 again, for the settings as they are now

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
	void                                KeepConfigLights(bool a_keep);                                        // before ReadCoverage: keep every light row too, per model
	[[nodiscard]] const ConfigLightMap& ConfigLights();
	void                                DropConfigLights();  // once the record lights are made
	// a beam's row out of the Streams folder whose settings hold now (lagen move_beams), or nullptr - pass 6's color for it
	[[nodiscard]] const ConfigLight* StreamRowFor(const std::string& a_model);
	// the color an element's own hand light wears for its art pick as the settings hold now, or none
	[[nodiscard]] std::optional<RE::Color> ArtHandColor(Family a_family);
	// a model one of the game's own projectiles or explosions uses (a_hazard: one of its hazards): the art the spell art picks recolor
	[[nodiscard]] bool VanillaArtModel(const std::string& a_model, bool a_hazard = false);
	// which Dynamic Lighting twin a Light Placer light's "<config>|<light>" is (made by ConfigText.h TwinKey; TwinOf), for
	// LightCopies.cpp to swap each pair at once when the setting moves
	[[nodiscard]] ConfigText::Twin DynamicTwinOf(const std::string& a_configAndLight);

	// ------------------------------------------------------------------ LightPlacerQueue.cpp: Light Placer's light put back on a resume stays
	void KeepLightPlacerLights();  // at data load, after Light Placer hooked the resume when it loaded
	void KeepCastingLights();      // once a frame, main thread: the casting art's Light Placer lights after a cell change

	// ------------------------------------------------------------------ RecordLights.cpp: the game's own light records (no Light Placer)
	void                             DecideRecordRoute();  // before ReadCoverage, after ReadLighting: Vanilla / ENB, or no Light Placer
	[[nodiscard]] bool               RecordRoute();
	[[nodiscard]] const char*        RecordRouteWhy();                                                       // the reason, for the log and DevBench
	void                             MakeRecordLights();                                                     // after MakeLightCopies and ReadCoverage, before pass 1
	void                             RecordFlicker(RE::ActorMagicCaster* a_caster);                          // after a caster's update: its hand light's flicker
	void                             RecordTablesReady();                                                    // the data load is done: RecordFlicker may read the tables
	void                             AdvanceRecordFlicker(float a_delta);                                    // once a frame, on the main thread
	[[nodiscard]] RE::TESObjectLIGH* RecordLightFor(const std::string& a_model);                             // nullptr: no row lights that model now
	[[nodiscard]] RE::TESObjectLIGH* RecordLightOfData(const std::string& a_model);                          // its first row's light, whatever the settings
	[[nodiscard]] float              RecordFadeNow(const RE::TESObjectLIGH* a_record, const void* a_light);  // its fade now, a flicker's keys played
	// ArtLights.cpp (his report: "bound weapons not lighting up on vanilla"): a light of ours on every art effect and
	// drawn weapon whose model a config row lights now; once a frame on the main thread
	void                      NoteEnchantShaders();  // at load, while the passes' editor IDs are still known (record route)
	void                      TickArtLights();
	void                      DropArtLights();  // a game loads, or a new one starts
	[[nodiscard]] std::size_t ArtLightCount();
	[[nodiscard]] std::size_t ArtLightsMade();
	// FleshLights.cpp (his call on FleshFX: "the dim light in its own color"): with Light Placer, a flesh spell's light takes
	// FleshFX's hue right after Light Placer wrote it (the effect's update, any thread); without it ArtLights.cpp does
	void FleshLightPlacer(RE::ShaderReferenceEffect* a_effect);
	void NoteFleshRoots();  // main thread, once a frame: where each flesh effect's lights hang (FleshLightPlacer only reads it)
	// the third-person node of the same name as a_root, where Light Placer and ArtLights.cpp hang a body light (main thread)
	[[nodiscard]] RE::NiAVObject* FleshBodyNode(RE::TESObjectREFR* a_ref, RE::NiAVObject* a_root);
	// GroundLights.cpp (his report: no light on the ground on Vanilla): a twin of each hand's casting light, made the
	// way Dynamic Wards makes its hand light (land lighting on); once a frame on the main thread
	void                      TickGroundLights();
	void                      DropGroundLights();  // a game loads, or a new one starts: the twins and the hands they follow are the old game's
	[[nodiscard]] std::size_t GroundLightCount();
	[[nodiscard]] std::size_t GroundLightsMade();
	// SneakFade.cpp: a sneaking caster's lights fade to Sneaking brightness over half a second and back when they stand. Our
	// write is the last word on a dimmed light and never a base for another pass: SneakPause around a pass that writes fades
	// gives it the numbers the others left
	void SneakFrameStart();                                          // the player update, before the game's: every dimmed light back
	void SneakFrameEnd(float a_delta);                               // the player update, last but the ground twins: the levels, the dimming
	void SneakDim(RE::NiLight* a_light, const RE::Actor* a_caster);  // one light by its caster's level (any thread, outside a SneakPause)
	void DropSneakFade();                                            // a game loads, or a new one starts
	void FindShadowSigns();                                          // data load: the perks and abilities that need the dark
	class SneakPause
	{
	public:
		explicit SneakPause(const RE::TESObjectREFR* a_owner = nullptr);  // nullptr: every caster's lights
		~SneakPause();
		SneakPause(const SneakPause&) = delete;
		SneakPause& operator=(const SneakPause&) = delete;

	private:
		RE::FormID owner_{ 0 };
		bool       held_{ false };
	};
	[[nodiscard]] bool IsOurLightPlacerLight(const RE::NiPointLight& a_light);  // LightCopies.cpp: Light Placer's light of one of our copies
	// automatic lights (his go-to pick): a light of ours in a_color, at the middle strength and reach of the tuned
	// hand lights a_tuned (made once per color, at data load; follows the sliders)
	[[nodiscard]] RE::TESObjectLIGH* AutoLight(RE::Color a_color, const std::vector<const RE::TESObjectLIGH*>& a_tuned);
	[[nodiscard]] std::size_t        AutoLightCount();
	[[nodiscard]] std::size_t        AutoCastingCount();  // CastingLights.cpp: the spells given an automatic light
	// stepping aside for other light mods (RecordLights.cpp)
	[[nodiscard]] bool        YieldToENBLight();  // its switch on, ENB Light.esp loaded, the lighting ENB
	[[nodiscard]] bool        TouchedByENBLight(const RE::TESForm* a_form);
	[[nodiscard]] std::string RecordReport();  // for DevBench

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
		[[nodiscard]] bool                                                         ModelLit(const std::string& a_model) const;
	};

	[[nodiscard]] const Coverage& ReadCoverage();
	[[nodiscard]] const Coverage& LastCoverage();  // what the last ReadCoverage found (the testing DLL's Diagnostics page)
	// the key a config's formIDs entry is kept under beside the models (record route): "formid:" + its lower-case editor ID
	[[nodiscard]] inline std::string FormKey(std::string_view a_lowerEditorID) { return "formid:" + std::string(a_lowerEditorID); }
	// ... and one that names the record by its id ("0x4046D~Skyrim.esm"): "formid:#" + its runtime form ID, which no editor ID
	// can spell (gen.RENAMED_BASE_FORMS)
	[[nodiscard]] inline std::string FormIdKey(RE::FormID a_id) { return std::format("formid:#{:08x}", a_id); }

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

	[[nodiscard]] SprayChoice ReadSprayChoice();
	void                      ApplySprayColors();  // SprayLights.cpp: the raised sprays' colors for the settings as they are now
	// SprayLights.cpp: the light a spray projectile had before pass 4 stretched a copy of it onto it, or nullptr
	[[nodiscard]] RE::TESObjectLIGH* SprayOriginal(const RE::BGSProjectile* a_projectile);
	// SprayLights.cpp: pass 4's copy of a spray's light, recolored on every refresh - the color its row wears - or nullptr
	[[nodiscard]] const RE::TESObjectLIGH* SprayCopyOf(const RE::BGSProjectile* a_projectile);

	// ------------------------------------------------------------------ FormCopies.cpp: making copies in memory
	using LightKit::NewForm;  // a form made in memory by the game's own factory (LightKit.h)

	[[nodiscard]] RE::TESObjectLIGH* CopyLight(const RE::TESObjectLIGH* a_src);
	// the record's own flicker / pulse bits taken off while the Flicker switch is off and put back when it is on (main thread; the
	// bits as the game made the record are kept the first time it is asked about)
	void               HoldFlicker(RE::TESObjectLIGH* a_light, bool a_flickers);
	[[nodiscard]] bool FlickerOn();                                                              // the Flicker switch (Dynamic Lighting), LightCopies.cpp
	void               CopyLightInto(RE::TESObjectLIGH* a_out, const RE::TESObjectLIGH* a_src);  // the whole light: look, sound, flare, model

	// ------------------------------------------------------------------ the passes (main.cpp runs them in its own order:
	// 0, 7, the wards, 3, 1, 2, 4, 6 - pass 5 retired 2026-10-16)
	void LightSettings();                       // LightSettings.cpp
	void CastingLights(const Coverage& a_cov);  // CastingLights.cpp
	template <class T>
	void                                     EffectLights(const Coverage& a_cov, std::string_view a_kind);  // EffectLights.cpp
	void                                     PoisonRuneArt();                                               // PoisonRune.cpp
	void                                     SprayLights();                                                 // SprayLights.cpp
	void                                     ApplyCastingLights(bool a_log);                                // CastingLights.cpp: pass 1 again, for the settings as they are now
	void                                     ApplyEffectLights(bool a_log);                                 // EffectLights.cpp: pass 2 again
	void                                     VaerSwirls();                                                  // VaerSwirls.cpp: pass 7, VAER Reborn's brighter strands and Thaumaturgy's copies
	void                                     Wards();                                                       // Wards.cpp: one dome per ward, 360 Ward's sphere in the vanilla blue
	[[nodiscard]] bool                       DynamicWardsLoaded();                                          // Wards.cpp: every ward is Dynamic Wards' then (this mod and pass 6 step down)
	void                                     ApplyWardLook();                                               // Wards.cpp: the Ward look pick onto the dome art (main thread)
	[[nodiscard]] bool                       StrangeWardsInstalled();                                       // Wards.cpp: Strange Wards' dome is the vanilla mesh
	[[nodiscard]] std::optional<RE::NiColor> StrangeWardColor();                                            // Wards.cpp: its pick's color, none = the vanilla blue-white
	void                                     WatchBlocking();                                               // Wards.cpp: a shield's ward checked the moment a block starts (each game load)
	void                                     KeepBlockWatch(float a_delta);                                 // Wards.cpp, player update: the watch follows a rebuilt animation graph

	// ------------------------------------------------------------------ VfxPacks.cpp: the VFX switcher (his yes 2026-10-19)
	// OFF in this release (his order 2026-10-19: "keep the spell switcher turned off in illuminated for now so we can ship with
	// it in the code but not playable"): no pack list is read, no model is changed, no path is read as a pack's
	inline constexpr bool kVfxSwitcherOn = false;
	void                  LoadVfxPacks();   // data load, after every pass: the converted packs read, the spell forms wearing their originals noted
	void                  ApplyVfxPacks();  // main thread: each noted form wears the copy of the pack its element's art pick names, or its own
}
