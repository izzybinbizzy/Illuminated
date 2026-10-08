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
				const float t = start + std::fmod(std::max(a_time, 0.0f), duration);
				const auto  next = std::upper_bound(keys.begin(), keys.end(), t, [](float a_t, const auto& a_key) { return a_t < a_key.first; });
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

		// the record light this caster's hand light was made from: one of the hand's spell's effects' lights, of a flicker
		// row, in the light's own color (a spell whose light comes from another effect is left alone)
		[[nodiscard]] const std::pair<const RE::TESObjectLIGH* const, Flicker>* FlickerFor(RE::ActorMagicCaster& a_caster,
			const RE::NiLight&                                                                                   a_light)
		{
			const auto source = a_caster.GetCastingSource();
			auto*      actor = a_caster.GetCasterAsActor();
			if (!actor || (source != RE::MagicSystem::CastingSource::kLeftHand && source != RE::MagicSystem::CastingSource::kRightHand)) {
				return nullptr;
			}
			auto* spell = a_caster.currentSpell;
			if (!spell) {
				auto* equipped = actor->GetEquippedObject(source == RE::MagicSystem::CastingSource::kLeftHand);
				spell = equipped ? equipped->As<RE::MagicItem>() : nullptr;
			}
			if (!spell) {
				return nullptr;
			}
			const auto& diffuse = a_light.GetLightRuntimeData().diffuse;
			const auto  same = [](float a_have, std::uint8_t a_want) { return std::abs(a_have * 255.0f - a_want) < 1.5f; };
			for (const auto* effect : spell->effects) {
				const auto* base = effect ? effect->baseEffect : nullptr;
				const auto* record = base ? base->data.light : nullptr;
				if (!record) {
					continue;
				}
				if (const auto it = gFlickerOf.find(record); it != gFlickerOf.end() && same(diffuse.red, record->data.color.red) &&
															 same(diffuse.green, record->data.color.green) && same(diffuse.blue, record->data.color.blue)) {
					return &*it;
				}
			}
			return nullptr;
		}

		// one channel of a linear-light color as the screen color that shows the same (wardgen.srgb)
		[[nodiscard]] std::uint8_t ScreenChannel(std::uint8_t a_linear)
		{
			const float x = static_cast<float>(a_linear) / 255.0f;
			const float v = x <= 0.0031308f ? 12.92f * x : 1.055f * std::pow(x, 1.0f / 2.4f) - 0.055f;
			return static_cast<std::uint8_t>(std::clamp(std::lround(v * 255.0f), 0L, 255L));
		}

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
				std::stable_sort(choices.begin(), choices.end(), [](const Choice& a, const Choice& b) { return a.strength > b.strength; });
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

	void AdvanceRecordFlicker(float a_delta)
	{
		if (gOn && a_delta > 0.0f && a_delta < 1.0f) {
			// wrapped well before a float loses the hundredths a 2 s loop needs (every loop is far shorter than 3600 s)
			gClock.store(std::fmod(gClock.load(std::memory_order_relaxed) + a_delta, 3600.0f), std::memory_order_relaxed);
		}
	}

	void RecordFlicker(RE::ActorMagicCaster* a_caster)
	{
		if (!gOn || gFlickerOf.empty() || !a_caster || REL::Module::IsVR()) {
			return;
		}
		RE::NiLight* light = a_caster->light ? a_caster->light->light.get() : nullptr;
		if (!light) {
			return;
		}
		if (const auto* found = FlickerFor(*a_caster, *light)) {
			const auto& [record, flicker] = *found;
			const float now = gClock.load(std::memory_order_relaxed) + PhaseOf(light, flicker.duration);
			light->GetLightRuntimeData().fade = record->fade * flicker.At(now) / flicker.mean;
		}
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
