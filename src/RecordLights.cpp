// Illuminated - SKSE plugin
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE.txt and the notice at the top of main.cpp.
//
// The game's own light records, lit by Illuminated: the route when Light Placer is not loaded. HIS CALL 2026-10-07:
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
//   runs (the game fades an explosion's light out by itself: ~1.0 -> 0 over ~3 s, measured 2026-10-08).
//   A flicker is played by us: the game does not move a casting light by its record's flicker flags (measured 2026-10-08
//   - Pulse / Flicker + amplitude set on the record: the hands read one fade in 380 samples, while every placed lamp round
//   her moved). So a flicker row's keys stay with its record light, and right after each caster's update (LightCopies.cpp's
//   one wrapper) the caster's own hand light takes the fade those keys give now, x the record's fade (the sliders) / the
//   mean - HIS ORDER 2026-10-08: "build flicker in our dll you can use relight code if you want". The keyframe maths is
//   Truman's (RE::Light's controller.h, GPL-3.0; his permission for RELight's code, credited), each light starting at its
//   own place in the loop as RE::Light's random start does. Nothing walks the scene: each caster writes the one light it
//   holds, on the thread the game updates it on. Not in VR (the caster's layout there is not proven).
// The color: the row's, as the game draws it - deeper than Community Shaders shows the same row, whose colors are
// linear light. The paler look draws each one as Community Shaders shows it (the row's color turned from linear light to
// the screen's, Dynamic Wards' wardgen.srgb). The setting "Light colors": Automatic (his call 2026-10-08) = paler on the
// Community Shaders pick, deeper on Vanilla / ENB; Paler / Deeper force one. It reaches the next light the game makes.
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

			// the fade the keys give at a_time into the loop - Truman's KeyframeSequence::Interpolate (RE::Light,
			// controller.h): step, linear, or cubic Hermite (the configs give no tangents, so they are 0)
			[[nodiscard]] float At(float a_time) const
			{
				const float t = start + std::fmod((std::max)(a_time, 0.0f), duration);
				const auto  next = std::ranges::upper_bound(keys, t, {}, &std::pair<float, float>::first);
				if (next == keys.begin()) {
					return keys.front().second;
				}
				if (next == keys.end()) {
					return keys.back().second;
				}
				const auto& [t0, v0] = *std::prev(next);
				const auto& [t1, v1] = *next;
				const float dt = t1 - t0;
				if (dt <= 0.0f || interpolation == 0) {
					return v0;
				}
				const float u = (t - t0) / dt;
				if (interpolation == 1) {
					return (1.0f - u) * v0 + u * v1;
				}
				const float u2 = u * u, u3 = u2 * u;
				return (2.0f * u3 - 3.0f * u2 + 1.0f) * v0 + (-2.0f * u3 + 3.0f * u2) * v1;
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
		std::unordered_map<std::string, std::vector<Choice>>  gChoices;  // model -> its record lights, strongest first
		std::size_t                                           gMade = 0, gRows = 0, gNotOurs = 0, gMissing = 0, gFailed = 0;
		double                                                gMs = 0.0;
		std::vector<std::pair<RE::TESObjectLIGH*, RE::Color>> gMadeColors;     // every record light and the color it was made in
		bool                                                  gPale = false;   // what ApplyRecordColors last drew
		std::unordered_map<const RE::TESObjectLIGH*, Flicker> gFlickerOf;      // the record lights of flicker rows
		std::unordered_set<const RE::TESObjectLIGH*>          gOurRecords;     // every record light made here
		std::atomic<float>                                    gClock{ 0.0f };  // seconds, advanced once a frame on the main thread

		constexpr std::string_view kPaleColors = "IlluminatedPaleColors";

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

		// every light record of ours a hand can wear: the record lights, the automatic and element ones, and every light copy
		// (pass 0's copies of the game's magic lights among them) - filled when the data load is done (RecordTablesReady)
		std::unordered_set<const RE::TESObjectLIGH*> gAllOurs;

		struct HandRecord
		{
			const RE::TESObjectLIGH* record{ nullptr };
			bool                     matched{ false };  // the live light still wears its color
		};

		// the light record a caster's hand light was made from: one of the hand's spell's effects' lights, matched by color.
		// 🔁 HIS REPORT 2026-10-10 on Vanilla: "configs for spell art don't change until unequip/re equip" and "brightness and
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

		using LightKit::ScreenChannel;  // a linear-light channel as the screen color that shows the same

		[[nodiscard]] float Strength(const LightCopy& a_copy)
		{
			const auto radius = static_cast<float>(a_copy.startRadius);
			return a_copy.startFade * radius * radius;
		}
	}

	void DecideRecordRoute()
	{
		gOn = REX::W32::GetModuleHandleA("po3_LightPlacer.dll") == nullptr;
		KeepConfigLights(gOn);
		if (gOn) {
			SKSE::log::info("record lights: Light Placer is not loaded, so the configs light the game's own light records");
		} else {
			SKSE::log::info("record lights: not used - Light Placer is loaded and lights the configs");
		}
	}

	bool RecordRoute()
	{
		return gOn;
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
		gPale = false;
		ApplyRecordColors();
		ApplyLightStrength(false);  // the new lights at the sliders' values
		gMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
		SKSE::log::info(
			"record lights: {} made ({} flicker{}) for {} lit models from {} config rows ({} name a light that is not one of ours, {} "
			"a copy whose light is not in this load order, {} failed) in {:.1f} ms",
			gMade, gFlickerOf.size(), REL::Module::IsVR() ? ", held steady in VR" : "", gChoices.size(), gRows, gNotOurs, gMissing, gFailed,
			gMs);
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
		light->data.color.red = a_color.red;
		light->data.color.green = a_color.green;
		light->data.color.blue = a_color.blue;
		gMadeColors.emplace_back(light, light->data.color);
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
	// HIS GO-TO PICK (2026-10-07 ~20:48, "4 make it optional"): where another light mod lights the same spell, a switch says
	// whether Illuminated steps aside, so nothing is lit twice. Without Light Placer:
	//   ENB Light (ENB Light.esp, ENB only): a spell whose effect or casting art ENB Light changed keeps the light ENB Light
	//   gave it (its mesh's ENB light).
	// (RELight is never used with Illuminated - his word 2026-10-08: "two separate mods and will never be used together" - so
	// nothing here looks for it.)
	bool YieldToENBLight()
	{
		static const bool loaded = [] {
			auto* dh = RE::TESDataHandler::GetSingleton();
			return dh && (dh->LookupLoadedModByName("ENB Light.esp") || dh->LookupLoadedLightModByName("ENB Light.esp"));
		}();
		return loaded && LightingPick() == Lighting::kEnb && SettingValue("IlluminatedYieldENBLight", 0) != 0;
	}

	bool TouchedByENBLight(const RE::TESForm* a_form)
	{
		const auto* files = a_form ? a_form->sourceFiles.array : nullptr;
		return files && std::ranges::any_of(*files, [](const RE::TESFile* f) { return f && f->GetFilename() == "ENB Light.esp"; });
	}

	// ------------------------------------------------------------------ per-element colors
	// HIS GO-TO PICK (2026-10-07 ~20:45, "a per-element colour row with Auto - match the spell art"; "do all of them"
	// 2026-10-08), without Light Placer: Fire, Frost and Shock each have a color setting - Auto (the spell's own, as tuned)
	// or one named color. A spell's element is read off its effect (what it is resisted by); its bolt and explosion take
	// the element of the effect that fires them. Each record light such a spell can wear gets one copy per element at data
	// load, so a change of color in the menu only recolors those copies and points the records at them - no light is made
	// while the game plays. A changed color reaches the next spell readied.
	namespace
	{
		constexpr std::string_view kElementSetting[] = { "", "IlluminatedFireColor", "IlluminatedFrostColor", "IlluminatedShockColor" };
		// the named colors are LightKit.h's (lagen.py ELEMENT_COLORS, in the same order): Dynamic Wards' preset hues
		std::map<std::pair<const RE::TESObjectLIGH*, int>, RE::TESObjectLIGH*> gVariants;
		std::unordered_map<const RE::TESForm*, int>                            gElementOfForm;  // projectiles and explosions
		bool                                                                   gElementsBuilt = false;
	}

	int ElementOf(const RE::EffectSetting* a_effect)
	{
		if (!a_effect) {
			return 0;
		}
		switch (a_effect->data.resistVariable) {
		case RE::ActorValue::kResistFire:
			return 1;
		case RE::ActorValue::kResistFrost:
			return 2;
		case RE::ActorValue::kResistShock:
			return 3;
		default:
			return 0;
		}
	}

	int ElementOfForm(const RE::TESForm* a_form)
	{
		if (!gElementsBuilt) {
			gElementsBuilt = true;
			// once, at data load: a projectile or an explosion fired by effects of one element is that element; by more than
			// one, none
			for (const auto* effect : LightKit::FormsOf<RE::EffectSetting>()) {
				const int e = ElementOf(effect);
				for (const RE::TESForm* f : { static_cast<const RE::TESForm*>(effect ? effect->data.projectileBase : nullptr),
						 static_cast<const RE::TESForm*>(effect ? effect->data.explosion : nullptr) }) {
					if (f && e) {
						auto [it, added] = gElementOfForm.try_emplace(f, e);
						if (!added && it->second != e) {
							it->second = -1;
						}
					}
				}
			}
		}
		const auto it = gElementOfForm.find(a_form);
		return it == gElementOfForm.end() || it->second < 0 ? 0 : it->second;
	}

	void PrepareElementLights(const RE::TESObjectLIGH* a_record, int a_element)
	{
		if (!gOn || !a_record || a_element <= 0 || gVariants.contains({ a_record, a_element })) {
			return;
		}
		auto&      copies = LightCopies();
		const auto source = std::ranges::find(copies, a_record, &LightCopy::form);
		auto*      light = source == copies.end() ? nullptr : CopyLight(a_record);
		if (!light) {
			return;
		}
		gOurRecords.insert(light);
		if (const auto f = gFlickerOf.find(a_record); f != gFlickerOf.end()) {
			gFlickerOf.emplace(light, f->second);
		}
		LightCopy copy{ .id = std::format("{} element {}", source->id, a_element),
			.base = source->id,
			.form = light,
			.startFade = source->startFade,
			.startRadius = source->startRadius,
			.startCutoff = source->startCutoff };
		copies.push_back(std::move(copy));
		gVariants.emplace(std::pair{ a_record, a_element }, light);
	}

	void PrepareElementLightsFor(const std::string& a_model, int a_element)
	{
		if (const auto it = gChoices.find(a_model); it != gChoices.end()) {
			for (const auto& c : it->second) {
				PrepareElementLights(c.light, a_element);
			}
		}
	}

	RE::TESObjectLIGH* ElementLight(RE::TESObjectLIGH* a_record, int a_element)
	{
		if (!gOn || !a_record || a_element <= 0 || a_element >= static_cast<int>(std::size(kElementSetting))) {
			return a_record;
		}
		const int  pick = SettingValue(kElementSetting[a_element], 0);
		const auto it = gVariants.find({ a_record, a_element });
		if (pick <= 0 || pick > LightKit::kNamedColorCount || it == gVariants.end()) {
			return a_record;
		}
		// the named color, drawn as ApplyRecordColors last drew every record light (it runs first on each refresh), so
		// "Light colors" reaches the element colors too (CodeRabbit, Illuminated #5: the variant skipped Paler)
		const auto c = LightKit::NamedColorRgb(pick);
		const auto drawn = [](std::uint32_t a_c) { return gPale ? ScreenChannel(static_cast<std::uint8_t>(a_c)) : static_cast<std::uint8_t>(a_c); };
		auto&      color = it->second->data.color;
		color.red = drawn(c >> 16);
		color.green = drawn(c >> 8);
		color.blue = drawn(c);
		return it->second;
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
			// 2026-10-10, "configs for spell art don't change until unequip/re equip, needs to be live")
			const auto& c = record->data.color;
			data.diffuse = { c.red / 255.0f, c.green / 255.0f, c.blue / 255.0f };
		}
		// HIS REPORT 2026-10-09 ~11:05 on Vanilla, "reach still doesn't work": the game set the radius once, when the spell
		// was readied, and Reach only changed the record - so a lit hand takes its record's radius too (GroundLights.cpp's
		// twin copies it and re-attenuates when it moves)
		const float radius = static_cast<float>(record->data.radius);
		if (radius > 0.0f && data.radius.x != radius) {
			data.radius = RE::NiPoint3{ radius, radius, radius };
		}
		// a steady one takes its record's fade too, so Brightness and Dim in daylight reach a hand already lit (run 9,
		// 2026-10-08: the game set it once, when the spell was readied, and it never moved after); a flicker plays its keys
		data.fade = RecordFadeNow(record, light);
	}

	void ApplyRecordColors()
	{
		// HIS CALL 2026-10-08: "use pale colors for cs and default for vanilla/enb" - Automatic (0) follows the lighting;
		// Paler (1) and Deeper (2) keep one look on every lighting (he kept the switch)
		const int  pick = SettingValue(kPaleColors, 0);
		const bool pale = pick == 1 || (pick == 0 && LightingPick() == Lighting::kShaders);
		if (!gOn || pale == gPale) {
			return;
		}
		gPale = pale;
		for (auto& [light, made] : gMadeColors) {
			light->data.color.red = pale ? ScreenChannel(made.red) : made.red;
			light->data.color.green = pale ? ScreenChannel(made.green) : made.green;
			light->data.color.blue = pale ? ScreenChannel(made.blue) : made.blue;
		}
		SKSE::log::info("record lights: {} drawn in {} colors", gMadeColors.size(), pale ? "the paler Community Shaders" : "their deeper");
	}

	RE::TESObjectLIGH* RecordLightFor(const std::string& a_model)
	{
		const auto it = gChoices.find(a_model);
		if (it == gChoices.end()) {
			return nullptr;
		}
		for (const auto& c : it->second) {
			if (ConditionsHold(c.test)) {
				return c.light;
			}
		}
		return nullptr;
	}

	std::string RecordReport()
	{
		return std::format(
			R"({{"on":{},"lights":{},"flicker":{},"pale":{},"models":{},"rows":{},"notOurs":{},"missing":{},"failed":{},"ms":{:.1f}}})", gOn,
			gMade, gFlickerOf.size(), gPale, gChoices.size(), gRows, gNotOurs, gMissing, gFailed, gMs);
	}
}
