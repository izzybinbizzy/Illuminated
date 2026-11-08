// Illuminated - SKSE plugin
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE.txt and the notice at the top of main.cpp.
//
// The flesh spells under FleshFX. HIS CALL: "do the changes proposed with the dim light in its own color". FleshFX.esp takes
// the flesh spells' glowing rim away and dresses the skin in its own textures (bark, stone, metal, ebony, scales), so a light
// in the old rim's hue lit the ground in a color nothing on the caster shows. Where FleshFX's look wins on a flesh shader
// (LightKit.h FleshFx, found once at data load), that spell's light takes the texture's own hue (LightMath.h kFleshLooks) at
// the strength its config row gives it now: the subtle row's top channel of 184 gives the dim color itself, and the
// full-strength switch and a Dynamic Lighting color key keep their strength, in the new hue.
//   With Light Placer: Light Placer names a magic effect's light "LP_Light[<config>|<light>](<effect ID>)#<n>" and writes
//   it in its own wrap of the effect's update; ours runs right after it (LightCopies.cpp's one wrapper), so this effect's
//   lights - and no other effect's on the same actor - take the hue on the values Light Placer just wrote. That wrap runs on
//   the game's worker threads, so it never walks the scene: the effect's own lights are found on the main thread once a
//   frame (NoteFleshRoots, under the node they hang on) and the wrap only writes their color.
//   Without Light Placer: ArtLights.cpp hangs the flesh row's light itself and gives it the hue as it follows the record.
// Frozen Electrocuted Combustion's lit shaders (gen.FEC) go the same way. A frozen body stays frozen and a burned one keeps
// smouldering, so on the "Frozen bodies" / "Burning bodies" fade-out picks their light goes out a few seconds in (FecBodyFade).

#include "Plugin.h"

namespace Plugin
{
	namespace
	{
		// a FEC body shader whose light fades out on its pick (0 = fades, the default): seconds lit, then out over `out`
		struct BodyFade
		{
			std::string_view pick;
			float            hold{ 0.0f };
			float            out{ 0.0f };
			bool             endlessOnly{ false };  // FEC plays its fire 2-5 s and the effect ends with it: only a fire with no end fades
		};

		// FEC plays its charred / skeleton skin burn with no end: the skin is seen burning for its record's fill time (3.4 / 4.4 s)
		constexpr std::array<std::pair<std::string_view, BodyFade>, 5> kFadeShaders{ {
			{ "fec_frostfxs", { "IlluminatedFecFrozenGlow", 4.0f, 2.0f, false } },
			{ "fec_firefxs", { "IlluminatedFecBurningGlow", 8.0f, 1.0f, true } },
			{ "fec_fireheavyfxs", { "IlluminatedFecBurningGlow", 8.0f, 1.0f, true } },
			{ "fec_firedisintegrateskinfxs", { "IlluminatedFecBurningGlow", 3.0f, 0.5f, true } },
			{ "fec_firedisintegrateskinlongfxs", { "IlluminatedFecBurningGlow", 4.0f, 0.5f, true } },
		} };

		std::unordered_map<const RE::TESEffectShader*, std::string> gFecKey;  // FEC's lit shaders -> their rows' key, at data load
		std::unordered_map<const RE::TESEffectShader*, BodyFade>    gFades;   // frozen and burning, at data load

		// the fade this effect's light can follow, on either pick (turned back to stay, the light comes back); none when FEC ends
		// the effect itself before the fade would
		[[nodiscard]] const BodyFade* TrackedFade(const RE::ShaderReferenceEffect* a_effect)
		{
			const auto it = a_effect ? gFades.find(a_effect->effectData) : gFades.end();
			const bool endsFirst = it != gFades.end() && it->second.endlessOnly && a_effect->lifetime > 0.0f &&
			                       a_effect->lifetime <= it->second.hold + it->second.out;
			if (it == gFades.end() || endsFirst) {
				return nullptr;
			}
			return &it->second;
		}

		[[nodiscard]] float FadeNow(const BodyFade* a_fade, const RE::ShaderReferenceEffect* a_effect)
		{
			return a_fade && SettingValue(a_fade->pick, 0) == 0 ? ConfigText::BodyFadeAt(a_effect->age, a_fade->hold, a_fade->out) : 1.0f;
		}

		// one noted light: the fade Light Placer gave it and the fade we wrote, so the body fade is base x factor every update
		// (Light Placer writes a steady light once - a multiply on the value read would compound)
		struct NotedLight
		{
			RE::NiPointer<RE::NiPointLight> light;
			float                           base{ 0.0f };
			float                           wrote{ -1.0f };
		};

		// a noted flesh effect's own Light Placer lights (held: a light Light Placer lets go of stays valid until the next note);
		// the effect's own ID rides along, so an effect freed and another made at the same address before the next note is
		// never read as the old one
		struct NotedLights
		{
			std::uint32_t           effectID{ 0 };
			std::vector<NotedLight> lights;
		};
		std::mutex                                                        gRootLock;
		std::unordered_map<const RE::ShaderReferenceEffect*, NotedLights> gRoots;  // main thread writes, guarded

		// Light Placer names an effect's light "LP_Light[<config>|<light>](<effect ID>)#<n>"; a_mark is "](<effect ID>)#"
		[[nodiscard]] bool LightOfEffect(const RE::NiPointLight* a_light, std::string_view a_mark)
		{
			const char*            raw = a_light ? a_light->name.c_str() : nullptr;
			const std::string_view name = raw ? raw : "";
			return name.starts_with("LP_Light[") && name.find(a_mark) != std::string_view::npos;
		}

		// a flesh effect whose lights matched no Light Placer light for a while: logged once per shader (a silent miss would
		// leave the old color with nothing to say why). Counted per EFFECT - one caster found and another not never reset each
		// other - and an effect the main thread no longer notes is forgotten
		std::mutex                                                          gMissLock;
		std::unordered_map<const RE::ShaderReferenceEffect*, std::uint32_t> gMisses;
		std::unordered_set<const RE::TESEffectShader*>                      gMissLogged;
	}

	void NoteFecShaders(const Coverage& a_cov)
	{
		gFecKey.clear();
		gFades.clear();
		std::array<bool, kFadeShaders.size()> present{};
		std::array<bool, kFadeShaders.size()> lit{};
		for (auto* shader : LightKit::FormsOf<RE::TESEffectShader>()) {
			const auto low = shader ? Lower(EditorID(shader)) : std::string{};
			if (!low.starts_with("fec_")) {
				continue;
			}
			const bool covered = a_cov.shaders.contains(low);
			if (covered) {
				gFecKey.emplace(shader, FormKey(low));
			}
			for (std::size_t i = 0; i < kFadeShaders.size(); ++i) {
				if (low == kFadeShaders[i].first) {
					present[i] = true;
					if (covered) {
						gFades.emplace(shader, kFadeShaders[i].second);
						lit[i] = true;
					}
				}
			}
		}
		if (!gFecKey.empty()) {
			std::string unlit;
			for (std::size_t i = 0; i < kFadeShaders.size(); ++i) {
				if (present[i] && !lit[i]) {
					unlit += (unlit.empty() ? " (no light, so no fade pick, on " : ", ") + std::string(kFadeShaders[i].first);
				}
			}
			SKSE::log::info("Frozen Electrocuted Combustion: {} of its shaders have lights{}{}", gFecKey.size(), unlit, unlit.empty() ? "" : ")");
		}
	}

	const std::unordered_map<const RE::TESEffectShader*, std::string>& FecShaders() { return gFecKey; }

	float FecBodyFade(const RE::ShaderReferenceEffect* a_effect)
	{
		return FadeNow(TrackedFade(a_effect), a_effect);
	}

	RE::NiAVObject* FleshBodyNode(RE::TESObjectREFR* a_ref, RE::NiAVObject* a_root)
	{
		// Light Placer (and ArtLights.cpp) hang a first-person player's effect light on the third-person node of the same name;
		// an unnamed root is used as it is - a search for "" would answer with any unnamed node
		auto*       third = a_ref && a_root ? a_ref->Get3D(false) : nullptr;
		const char* name = a_root ? a_root->name.c_str() : nullptr;
		if (!third || third == a_root || !name || !*name) {
			return a_root;
		}
		auto* same = third->GetObjectByName(a_root->name);
		return same ? same : a_root;
	}

	namespace
	{
		// frames NoteFleshRoots still walks every frame: Light Placer may hang a starting effect's lights a few frames after it starts
		constexpr int    kFleshLookFrames = 10;
		std::atomic<int> gFleshLook{ kFleshLookFrames };
	}

	void NoteFleshStarted(const RE::ShaderReferenceEffect* a_effect)
	{
		if (a_effect && (LightKit::FleshFx::HueOf(a_effect->effectData) || TrackedFade(a_effect))) {
			gFleshLook.store(kFleshLookFrames, std::memory_order_relaxed);
		}
	}

	void NoteFleshRoots()
	{
		if (LightKit::FleshFx::Hues().empty() && gFades.empty()) {
			return;  // no flesh spell wears FleshFX's look and FEC is not here: nothing to note, ever
		}
		// every magic effect is walked for a few frames after a flesh or FEC shader starts, resumes or ends (LightCopies.cpp); between,
		// the lights noted stand
		int left = gFleshLook.load(std::memory_order_relaxed);
		if (left <= 0) {
			return;
		}
		gFleshLook.compare_exchange_strong(left, left - 1, std::memory_order_relaxed);  // a start in between keeps its count
		std::unordered_map<const RE::ShaderReferenceEffect*, NotedLights> roots;
		if (auto* lists = RE::ProcessLists::GetSingleton()) {
			lists->ForEachMagicTempEffect([&](RE::BSTempEffect* a_effect) {
				auto*      shader = a_effect ? netimmerse_cast<RE::ShaderReferenceEffect*>(a_effect) : nullptr;
				const bool live = shader && !shader->finished;
				if (live && (LightKit::FleshFx::HueOf(shader->effectData) || TrackedFade(shader))) {
					const auto ref = shader->target.get();
					if (auto* node = FleshBodyNode(ref.get(), shader->GetAttachRoot())) {
						NotedLights            noted{ shader->effectID, {} };
						std::array<char, 24>   text{};
						const auto             end = std::format_to_n(text.data(), text.size() - 1, "]({})#", noted.effectID).out;
						const std::string_view mark(text.data(), static_cast<std::size_t>(end - text.data()));
						RE::BSVisit::TraverseScenegraphLights(node, [&](RE::NiPointLight* a_light) {
							if (LightOfEffect(a_light, mark)) {
								noted.lights.push_back({ RE::NiPointer<RE::NiPointLight>(a_light) });
							}
							return RE::BSVisit::BSVisitControl::kContinue;
						});
						roots.emplace(shader, std::move(noted));
					}
				}
				return RE::BSContainer::ForEachResult::kContinue;
			});
		}
		{
			std::scoped_lock lock{ gRootLock };
			if (roots.empty() && gRoots.empty()) {
				return;  // no flesh spell up, before or now
			}
			// a light noted again keeps the fade it had and the one we wrote
			for (auto& [effect, noted] : roots) {
				const auto old = gRoots.find(effect);
				if (old == gRoots.end() || old->second.effectID != noted.effectID) {
					continue;
				}
				for (auto& now : noted.lights) {
					const auto was = std::ranges::find(old->second.lights, now.light, &NotedLight::light);
					if (was != old->second.lights.end()) {
						now.base = was->base;
						now.wrote = was->wrote;
					}
				}
			}
			gRoots.swap(roots);
		}
		std::scoped_lock lock{ gRootLock, gMissLock };
		std::erase_if(gMisses, [](const auto& a_miss) { return !gRoots.contains(a_miss.first); });
	}

	void FleshLightPlacer(RE::ShaderReferenceEffect* a_effect)
	{
		const bool  live = a_effect && !a_effect->finished;
		const auto* hue = live ? LightKit::FleshFx::HueOf(a_effect->effectData) : nullptr;
		const auto* fade = live ? TrackedFade(a_effect) : nullptr;
		if (!hue && !fade) {
			return;
		}
		const float   bodyFade = FadeNow(fade, a_effect);
		std::uint32_t matched = 0;
		{
			const auto       turn = LightKit::Turn();  // before gRootLock (LightKit.h's order)
			std::scoped_lock lock{ gRootLock };
			const auto       it = gRoots.find(a_effect);
			if (it == gRoots.end() || it->second.effectID != a_effect->effectID) {
				return;  // not noted by the main thread yet (a cast this frame): its lights take the hue from the next frame on
			}
			// only the lights the main thread found for this effect: their color, on the values Light Placer just wrote
			for (auto& noted : it->second.lights) {
				auto& data = noted.light->GetLightRuntimeData();
				if (hue) {
					data.diffuse = LightKit::FleshFx::Shown(*hue, data.diffuse);
				}
				if (data.fade != noted.wrote) {
					noted.base = data.fade;  // Light Placer wrote it since: a flickering row every update, a steady one once
				}
				data.fade = noted.base * bodyFade;
				noted.wrote = data.fade;
			}
			matched = static_cast<std::uint32_t>(it->second.lights.size());
		}
		if (!hue) {
			return;  // the miss log is the FleshFX hue's
		}
		std::scoped_lock lock{ gMissLock };
		auto&            misses = gMisses[a_effect];
		misses = matched ? 0 : misses + 1;
		if (misses == 300 && gMissLogged.insert(a_effect->effectData).second) {
			SKSE::log::warn(
				"FleshFX: no Light Placer light named for effect {} found under its body for 300 updates - that flesh "
				"spell keeps its old light color",
				a_effect->effectID);
		}
	}
}
