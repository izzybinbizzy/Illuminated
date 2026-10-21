// Illuminated - SKSE plugin
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE.txt and the notice at the top of main.cpp.
//
// The game's own light records, lit by Illuminated: the route on Vanilla and ENB, and on Community Shaders when Light Placer
// is not loaded (DecideRecordRoute). HIS CALL:
// "this is a vanilla lighting mod that will add never before seen vanilla light to spells and then optionally have
// enb/lightplacer versions. vanilla is the main focus though."
//
// The game already gives a spell its lights through four record slots: a magic effect's casting light (the hand while
// the spell is readied and cast), a projectile's light, an explosion's light and a hazard's light. The engine makes,
// moves, culls and removes those lights itself, on every renderer and in VR, and nothing of ours runs while the game
// plays. So without Light Placer, each model the configs light gives the record that shows it a light of its own, made
// here in memory from the config row:
//   the light copy the row names (LightCopies.cpp: drawn plain off Community Shaders, the sliders on it),
//   in the row's color,
//   at the steady share of the row's fade controller, if it has one: a flicker's mean, a flash's mean over the time it
// runs (the game fades an explosion's light out by itself: ~1.0 -> 0 over ~3 s, measured).
// A flicker is played by us: the game does not move a casting light by its record's flicker flags (measured
//   - Pulse / Flicker + amplitude set on the record: the hands read one fade in 380 samples, while every placed lamp round
//   her moved). So a flicker row's keys stay with its record light, and right after each caster's update (LightCopies.cpp's
//   one wrapper) the caster's own hand light takes the fade those keys give now, x the record's fade (the sliders) / the
// mean - HIS ORDER: "build flicker in our dll you can use relight code if you want". The keyframe maths is
//   Truman's (RE::Light's controller.h, GPL-3.0; his permission for RELight's code, credited), each light starting at its
//   own place in the loop as RE::Light's random start does. Nothing walks the scene: each caster writes the one light it
//   holds, on the thread the game updates it on. Not in VR (the caster's layout there is not proven; VR is never played here).
// The color: the row's, as the game draws it - deeper than Community Shaders shows the same row, whose colors are
// linear light. On Community Shaders each one is drawn as Community Shaders shows it (the row's color turned from linear
// light to the screen's, Dynamic Wards' wardgen.srgb); on Vanilla / ENB it keeps the row's deeper color (his call: "use pale
// colors for cs and default for vanilla/enb" - the "Light colors" setting that once forced one look is gone).
// On Community Shaders a record light of an inverse-square row is flagged as one, with its cutoff in the falloff field and
// its size in the field of view - what Community Shaders reads off a record (LightKit.h), as pass 0's lights are.
// A model with several rows takes the strongest one whose settings hold now: a record has ONE light. Passes 1 and 2
// (CastingLights.cpp, EffectLights.cpp) point the records at these lights instead of taking the game's light off, and
// give one to the records that had none. A row that names a light other than one of our copies (CS Light's own configs)
// is not used here; those models keep the game's light.
//
// The lights are made once, at data load, and never reach a save: magic effects, projectiles, explosions and hazards
// are base records the game does not write to a save, and they point at these lights only in memory.

#include "Plugin.h"

namespace Plugin
{
	namespace
	{
		constexpr std::uint32_t kPortalStrict = 1u << 13;  // TES_LIGHT_FLAGS::kPortalStrict

		// a flicker row's keys, played on the record light's live lights (the hands)
		struct Flicker
		{
			std::vector<std::pair<float, float>> keys;  // time, fade - sorted, at least two, a loop longer than 0
			std::uint8_t                         interpolation{ 1 };
			float                                start{ 0.0f }, duration{ 0.0f }, mean{ 1.0f };

			// the fade the keys give at a_time into the loop (LightMath.h's KeyAt: Truman's KeyframeSequence maths)
			[[nodiscard]] float At(float a_time) const
			{
				return LightKit::KeyAt(keys, interpolation, start + std::fmod((std::max)(a_time, 0.0f), duration));
			}
		};

		struct Choice
		{
			std::vector<std::vector<Clause>> test;  // the settings it waits on
			RE::TESObjectLIGH*               light{ nullptr };
			float                            strength{ 0.0f };  // fade x radius², to put the strongest first
		};

		// written once at data load, read on the main thread after it
		bool                                                  gOn = false;
		const char*                                           gWhy = "not decided yet";  // why the route is what it is (DecideRecordRoute)
		std::unordered_map<std::string, std::vector<Choice>>  gChoices;                  // model -> its record lights, strongest first
		std::size_t                                           gMade = 0, gRows = 0, gNotOurs = 0, gMissing = 0, gFailed = 0;
		double                                                gMs = 0.0;
		std::vector<std::pair<RE::TESObjectLIGH*, RE::Color>> gMadeColors;     // every record light and the color it was made in
		int                                                   gLook = 0;       // what ApplyRecordColors last drew: 0 vanilla, 1 paler, 2 deeper
		std::unordered_map<const RE::TESObjectLIGH*, Flicker> gFlickerOf;      // the record lights of flicker rows
		std::unordered_set<const RE::TESObjectLIGH*>          gOurRecords;     // every record light made here
		std::atomic<float>                                    gClock{ 0.0f };  // seconds, advanced once a frame on the main thread

		// the keys' signature, so two rows that flicker differently never share one record light
		[[nodiscard]] std::string KeysOf(const ConfigLight& a_row)
		{
			std::string out = std::to_string(a_row.interpolation);
			for (const auto& [t, v] : a_row.keys) {
				out += std::format(":{:.3f},{:.3f}", t, v);
			}
			return out;
		}

		// a light's own place in the loop, from its address (RE::Light starts each light at a random place)
		[[nodiscard]] float PhaseOf(const void* a_light, float a_duration)
		{
			const auto bits = std::hash<const void*>{}(a_light);
			return static_cast<float>(bits % 10007u) / 10007.0f * a_duration;
		}

		// every light record of ours a hand can wear: the record lights, the automatic ones, and every light copy
		// (pass 0's copies of the game's magic lights among them) - filled when the data load is done (RecordTablesReady)
		std::unordered_set<const RE::TESObjectLIGH*> gAllOurs;

		struct HandRecord
		{
			const RE::TESObjectLIGH* record{ nullptr };
			bool                     matched{ false };  // the live light still wears its color
		};

		// the light record a caster's hand light was made from: one of the hand's spell's effects' lights, matched by color.
		// 🔁 HIS REPORT on Vanilla: "configs for spell art don't change until unequip/re equip" and "brightness and
		// reach still don't track live" - a setting that points the effect at ANOTHER record leaves the lit hand in the old
		// color, so nothing matched and the hand kept its old light until the spell was readied again; and a pass-0 copy was
		// never ours to follow. Now: a match by color among every record of ours; else the record the spell's costliest effect
		// wears NOW (unmatched - the hand takes that record's look).
		[[nodiscard]] HandRecord RecordOf(RE::ActorMagicCaster& a_caster, const RE::NiLight& a_light)
		{
			const auto source = a_caster.GetCastingSource();
			auto*      actor = a_caster.GetCasterAsActor();
			if (!actor || (source != RE::MagicSystem::CastingSource::kLeftHand && source != RE::MagicSystem::CastingSource::kRightHand)) {
				return {};
			}
			auto* spell = a_caster.currentSpell;
			if (!spell) {
				auto* equipped = actor->GetEquippedObject(source == RE::MagicSystem::CastingSource::kLeftHand);
				spell = equipped ? equipped->As<RE::MagicItem>() : nullptr;
			}
			if (!spell) {
				return {};
			}
			const auto& diffuse = a_light.GetLightRuntimeData().diffuse;
			const auto  same = [](float a_have, std::uint8_t a_want) { return std::abs(a_have * 255.0f - a_want) < 1.5f; };
			for (const auto* effect : spell->effects) {
				const auto* base = effect ? effect->baseEffect : nullptr;
				const auto* record = base ? base->data.light : nullptr;
				if (record && gAllOurs.contains(record) && same(diffuse.red, record->data.color.red) &&
					same(diffuse.green, record->data.color.green) && same(diffuse.blue, record->data.color.blue)) {
					return { record, true };
				}
			}
			const auto* top = spell->GetCostliestEffectItem();
			const auto* topBase = top ? top->baseEffect : nullptr;
			const auto* now = topBase ? topBase->data.light : nullptr;
			return { now && gAllOurs.contains(now) ? now : nullptr, false };
		}

		using LightKit::LinearChannel;  // a screen-color channel as linear light (the Deeper look)
		using LightKit::ScreenChannel;  // a linear-light channel as the screen color that shows the same

		// one channel in the Light colors look: 0 its own, 1 paler, 2 deeper
		[[nodiscard]] std::uint8_t Shown(std::uint8_t a_c, int a_look)
		{
			return a_look == 1 ? ScreenChannel(a_c) : a_look == 2 ? LinearChannel(a_c) :
			                                                        a_c;
		}

		[[nodiscard]] float Strength(const LightCopy& a_copy)
		{
			const auto radius = static_cast<float>(a_copy.startRadius);
			return a_copy.startFade * radius * radius;
		}
	}

	// HIS ANSWER 2026-10-13 (the cloud audit's Q7, "yes to all your picks"): on Vanilla and ENB Illuminated always lights
	// its own way, even when Light Placer is installed for another mod - Light Placer is the Community Shaders route only.
	// One download serves all three lightings and Light Placer reads every file under Data\LightPlacer when it loads, so our
	// configs stay where they are and every light row in them waits first on one more global, IlluminatedLightPlacerRoute
	// (lagen.py route_switch): 1 only here, on Community Shaders with Light Placer loaded; 0 everywhere else, so on Vanilla
	// and ENB Light Placer draws none of our lights and this route does the work.
	void DecideRecordRoute()
	{
		const bool lightPlacer = REX::W32::GetModuleHandleA("po3_LightPlacer.dll") != nullptr;
		const bool shaders = LightingFound() == Lighting::kShaders;
		gOn = !lightPlacer || !shaders;
		gWhy = !lightPlacer ? "Light Placer is not loaded" :
		       !shaders     ? "the lighting is not Community Shaders" :
		                      "Community Shaders with Light Placer";
		// every Light Placer row waits on the route global: without it Light Placer draws none of ours, so this route lights them
		if (!SetLightPlacerRoute(!gOn) && !gOn) {
			gOn = true;
			gWhy = "the route global could not be made";
		}
		KeepConfigLights(gOn);
		if (gOn) {
			SKSE::log::info("record lights: {} ({}), so the configs light the game's own light records{}", gWhy,
				LightingName(LightingFound()), lightPlacer ? " - Light Placer, loaded for other mods, draws none of Illuminated's lights" : "");
		} else {
			SKSE::log::info("record lights: not used - {}: Light Placer lights the configs", gWhy);
		}
	}

	bool RecordRoute()
	{
		return gOn;
	}

	const char* RecordRouteWhy()
	{
		return gWhy;
	}

	void MakeRecordLights()
	{
		if (!gOn) {
			return;
		}
		const auto started = std::chrono::steady_clock::now();
		auto&      copies = LightCopies();
		// the copies the rows name, by editor ID (LightCopies() is not grown until the loop below is done)
		std::unordered_map<std::string, std::size_t> byId;
		for (std::size_t i = 0; i < copies.size(); ++i) {
			byId.emplace(Lower(copies[i].id), i);
		}
		std::unordered_map<std::string, std::size_t> made;  // copy + color + fade share -> index into `added`
		std::vector<LightCopy>                       added;
		for (const auto& [model, entries] : ConfigLights()) {
			std::vector<Choice> choices;
			for (const auto& entry : entries) {
				for (const auto& row : *entry) {
					++gRows;
					const auto it = byId.find(Lower(row.light));
					if (it == byId.end()) {
						++gNotOurs;
						continue;
					}
					const LightCopy& copy = copies[it->second];
					if (!copy.form) {
						++gMissing;  // its light is not in this load order (LightCopies.cpp logged it)
						continue;
					}
					// a fade controller's steady share of the fade the copy was made with
					const float share = row.controller && row.meanFade > 0.0f && copy.fade > 0.0f ? row.meanFade / copy.fade : 1.0f;
					const bool  flickers = row.controller && !row.flash && row.keys.size() > 1 && row.meanFade > 0.0f;
					const auto  key = std::format("{}|{}|{},{},{}|{}|{:.4f}|{}", copy.id, row.hasColor, row.r, row.g, row.b, row.portalStrict, share,
						flickers ? KeysOf(row) : std::string());
					auto [slot, isNew] = made.try_emplace(key, added.size());
					if (isNew) {
						auto* light = CopyLight(copy.form);
						if (!light) {
							++gFailed;
							made.erase(slot);
							continue;
						}
						if (row.hasColor) {
							light->data.color.red = row.r;
							light->data.color.green = row.g;
							light->data.color.blue = row.b;
						}
						if (row.portalStrict) {
							light->data.flags = static_cast<RE::TES_LIGHT_FLAGS>(light->data.flags.underlying() | kPortalStrict);
						}
						if (InverseSquare() && copy.startCutoff > 0.0f) {
							// Community Shaders reads an inverse-square record's falloff as its cutoff (the sliders write it there,
							// LightCopies.cpp) and its field of view as its size (Light Placer's GetSize)
							light->data.flags = static_cast<RE::TES_LIGHT_FLAGS>(light->data.flags.underlying() | LightKit::kRecordInverseSquare);
							light->data.fov = LightKit::LightPlacerSize(light->data.fov);
						}
						gMadeColors.emplace_back(light, light->data.color);
						gOurRecords.insert(light);
						if (flickers) {
							gFlickerOf.emplace(light, Flicker{ .keys = row.keys,
														  .interpolation = row.interpolation,
														  .start = row.keys.front().first,
														  .duration = row.keys.back().first - row.keys.front().first,
														  .mean = row.meanFade });
						}
						// the sliders reach it like every other copy (LightCopies.cpp), from the copy's made-with values
						added.push_back({ .id = std::format("record {} {},{},{}", copy.id, light->data.color.red, light->data.color.green,
											  light->data.color.blue),
							.base = copy.id,
							.form = light,
							.startFade = copy.startFade * share,
							.startRadius = copy.startRadius,
							.startCutoff = copy.startCutoff });
					}
					const auto& chosen = added[slot->second];
					choices.push_back({ row.test, chosen.form, Strength(chosen) });
				}
			}
			if (!choices.empty()) {
				std::ranges::stable_sort(choices, std::greater{}, &Choice::strength);
				gChoices.emplace(model, std::move(choices));
			}
		}
		gMade = added.size();
		for (auto& a : added) {
			copies.push_back(std::move(a));
		}
		DropConfigLights();
		gLook = 0;  // the lights were just made in their own colors
		ApplyRecordColors();
		ApplyLightStrength(false);  // the new lights at the sliders' values
		gMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
		SKSE::log::info(
			"record lights: {} made ({} flicker) for {} lit models from {} config rows ({} name a light that is not one of ours, {} "
			"a copy whose light is not in this load order, {} failed) in {:.1f} ms",
			gMade, gFlickerOf.size(), gChoices.size(), gRows, gNotOurs, gMissing, gFailed, gMs);
	}

	RE::TESObjectLIGH* AutoLight(RE::Color a_color, const std::vector<const RE::TESObjectLIGH*>& a_tuned)
	{
		static std::unordered_map<std::uint32_t, RE::TESObjectLIGH*> made;              // one per color
		static const RE::TESObjectLIGH*                              middle = nullptr;  // by form: the copies' list moves as it grows
		if (!gOn || a_tuned.empty()) {
			return nullptr;
		}
		auto& copies = LightCopies();
		if (!middle) {
			// the tuned hand light in the middle by strength (fade x radius²): an automatic light is never the brightest
			std::vector<const LightCopy*> tuned;
			for (const auto& c : copies) {
				if (c.form && std::ranges::find(a_tuned, c.form) != a_tuned.end()) {
					tuned.push_back(&c);
				}
			}
			if (tuned.empty()) {
				return nullptr;
			}
			std::ranges::sort(tuned, [](const LightCopy* a, const LightCopy* b) { return Strength(*a) < Strength(*b); });
			middle = tuned[tuned.size() / 2]->form;
		}
		const std::uint32_t key = (static_cast<std::uint32_t>(a_color.red) << 16) | (static_cast<std::uint32_t>(a_color.green) << 8) | a_color.blue;
		if (const auto it = made.find(key); it != made.end()) {
			return it->second;
		}
		const auto source = std::ranges::find(copies, middle, &LightCopy::form);
		if (source == copies.end()) {
			return nullptr;
		}
		auto* light = CopyLight(middle);
		if (!light) {
			return nullptr;
		}
		// the baseline is the color a record was MADE in, never one Paler / Deeper already turned: a color read off one of our
		// record lights while a look is on is taken back to that light's own (CodeRabbit on #28), then the look drawn on it
		RE::Color base = a_color;
		if (gLook != 0) {
			const auto drawn = std::ranges::find_if(gMadeColors, [&](const auto& a_m) {
				const auto& c = a_m.first->data.color;
				return c.red == a_color.red && c.green == a_color.green && c.blue == a_color.blue;
			});
			if (drawn != gMadeColors.end()) {
				base = drawn->second;
			}
		}
		light->data.color.red = Shown(base.red, gLook);
		light->data.color.green = Shown(base.green, gLook);
		light->data.color.blue = Shown(base.blue, gLook);
		gMadeColors.emplace_back(light, base);
		gOurRecords.insert(light);
		LightCopy copy{ .id = std::format("auto {},{},{}", a_color.red, a_color.green, a_color.blue),
			.base = source->id,
			.form = light,
			.startFade = source->startFade,
			.startRadius = source->startRadius,
			.startCutoff = source->startCutoff };
		copies.push_back(std::move(copy));
		made.emplace(key, light);
		return light;
	}

	std::size_t AutoLightCount()
	{
		return static_cast<std::size_t>(std::ranges::count_if(LightCopies(), [](const LightCopy& c) { return c.id.starts_with("auto "); }));
	}

	// ------------------------------------------------------------------ stepping aside for other light mods
	// HIS GO-TO PICK ("4 make it optional"): where another light mod lights the same spell, a switch says
	// whether Illuminated steps aside, so nothing is lit twice. Without Light Placer:
	//   ENB Light (ENB Light.esp, ENB only): a spell whose effect or casting art ENB Light changed keeps the light ENB Light
	//   gave it (its mesh's ENB light).
	// (RELight is never used with Illuminated - his word: "two separate mods and will never be used together" - so
	// nothing here looks for it.)
	bool YieldToENBLight()
	{
		static const bool loaded = [] {
			auto* dh = RE::TESDataHandler::GetSingleton();
			return dh && (dh->LookupLoadedModByName("ENB Light.esp") || dh->LookupLoadedLightModByName("ENB Light.esp"));
		}();
		return loaded && LightingFound() == Lighting::kEnb && SettingValue("IlluminatedYieldENBLight", 0) != 0;
	}

	bool TouchedByENBLight(const RE::TESForm* a_form)
	{
		const auto* files = a_form ? a_form->sourceFiles.array : nullptr;
		// case-blind, as the load order's own lookup above (LookupLoadedModByName)
		return files && std::ranges::any_of(*files, [](const RE::TESFile* f) { return f && LightKit::SameText(f->GetFilename(), "ENB Light.esp"); });
	}

	void AdvanceRecordFlicker(float a_delta)
	{
		if (gOn && a_delta > 0.0f && a_delta < 1.0f) {
			// wrapped well before a float loses the hundredths a 2 s loop needs (every loop is far shorter than 3600 s)
			gClock.store(std::fmod(gClock.load(std::memory_order_relaxed) + a_delta, 3600.0f), std::memory_order_relaxed);
		}
	}

	namespace
	{
		// the caster hook is installed with the other hooks part way through the data load (MakeLightCopies), while the
		// tables below still grow; nothing reads them until the load is done (the re-score's Illuminated issue 4)
		std::atomic<bool> gTablesReady{ false };
	}

	void RecordTablesReady()
	{
		gAllOurs.clear();
		gAllOurs.insert(gOurRecords.begin(), gOurRecords.end());
		for (const auto& c : LightCopies()) {
			if (c.form) {
				gAllOurs.insert(c.form);
			}
		}
		gTablesReady.store(true, std::memory_order_release);
	}

	float RecordFadeNow(const RE::TESObjectLIGH* a_record, const void* a_light)
	{
		if (!a_record) {
			return 0.0f;
		}
		if (const auto it = gFlickerOf.find(a_record); it != gFlickerOf.end()) {
			const auto& flicker = it->second;
			const float now = gClock.load(std::memory_order_relaxed) + PhaseOf(a_light, flicker.duration);
			return a_record->fade * flicker.At(now) / flicker.mean;
		}
		return a_record->fade;
	}

	void RecordFlicker(RE::ActorMagicCaster* a_caster)
	{
		if (!gTablesReady.load(std::memory_order_acquire) || !gOn || gAllOurs.empty() || !a_caster || REL::Module::IsVR()) {
			return;
		}
		RE::NiLight* light = a_caster->light ? a_caster->light->light.get() : nullptr;
		if (!light) {
			return;
		}
		const auto  hand = RecordOf(*a_caster, *light);
		const auto* record = hand.record;
		if (!record) {
			return;
		}
		auto& data = light->GetLightRuntimeData();
		if (!hand.matched) {
			// a setting pointed the effect at another light since this one was lit: the hand takes its look now (his report
			// "configs for spell art don't change until unequip/re equip, needs to be live")
			const auto& c = record->data.color;
			data.diffuse = { c.red / 255.0f, c.green / 255.0f, c.blue / 255.0f };
		}
		// HIS REPORT on Vanilla, "reach still doesn't work": the game set the radius once, when the spell
		// was readied, and Reach only changed the record - so a lit hand takes its record's radius too (GroundLights.cpp's
		// twin copies it and re-attenuates when it moves)
		const float radius = static_cast<float>(record->data.radius);
		if (radius > 0.0f && data.radius.x != radius) {
			data.radius = RE::NiPoint3{ radius, radius, data.radius.z };  // z is the light's size: the game's, kept
		}
		// a steady one takes its record's fade too, so Brightness and Dim in daylight reach a hand already lit (run 9,
		// the game set it once, when the spell was readied, and it never moved after); a flicker plays its keys
		data.fade = RecordFadeNow(record, light);
		// on Community Shaders the cutoff the sliders gave the record reaches the lit hand too (LightKit.h's two words)
		if (InverseSquare() && (record->data.flags.underlying() & LightKit::kRecordInverseSquare)) {
			LightKit::Isl::SetCutoff(light, record->data.fallofExponent, 0.01f, 1.0f);
		}
	}

	void ApplyRecordColors()
	{
		// HIS ORDER 2026-10-20: "i want the paler/deeper settings back added into lights right below presets above light strength",
		// then "no automatic, just paler or deeper or vanilla" - Vanilla (0, the default) draws each light in its own color, Paler (1)
		// turns it from linear light to the screen's (the Community Shaders look), Deeper (2) the other way. (Was, his 2026-10-07
		// call: paler on Community Shaders by itself.) Again on every settings refresh (RefreshLights); an automatic light copies
		// the color drawn when it is made
		const int look = std::clamp(SettingValue("IlluminatedPaleColors", 0), 0, 2);
		if (!gOn || look == gLook) {
			return;
		}
		gLook = look;
		for (auto& [light, made] : gMadeColors) {
			light->data.color.red = Shown(made.red, look);
			light->data.color.green = Shown(made.green, look);
			light->data.color.blue = Shown(made.blue, look);
		}
		SKSE::log::info("record lights: {} drawn in {} colors", gMadeColors.size(), look == 1 ? "paler" : look == 2 ? "deeper" :
																													  "their own");
	}

	RE::TESObjectLIGH* RecordLightFor(const std::string& a_model)
	{
		const auto it = gChoices.find(a_model);
		if (it == gChoices.end() || LightsOff()) {  // all lights off: no record light of ours, the form keeps its own (CodeRabbit, #24)
			return nullptr;
		}
		for (const auto& c : it->second) {
			if (ConditionsHold(c.test)) {
				return c.light;
			}
		}
		return nullptr;
	}

	RE::TESObjectLIGH* RecordLightOfData(const std::string& a_model)
	{
		const auto it = gChoices.find(a_model);
		return it == gChoices.end() || it->second.empty() ? nullptr : it->second.front().light;
	}

	std::string RecordReport()
	{
		return std::format(
			R"({{"on":{},"why":"{}","lights":{},"flicker":{},"look":{},"models":{},"rows":{},"notOurs":{},"missing":{},"failed":{},"ms":{:.1f}}})", gOn,
			gWhy, gMade, gFlickerOf.size(), gLook, gChoices.size(), gRows, gNotOurs, gMissing, gFailed, gMs);
	}
}
