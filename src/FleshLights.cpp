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
// Frozen Electrocuted Combustion's lit shaders (gen.FEC) go the same way. A frozen body stays frozen, so on the
// "Frozen bodies" fade-out pick the frozen shader's light goes out a few seconds in (FrozenFade).

#include "Plugin.h"

namespace Plugin
{
	namespace
	{
		constexpr std::string_view kFrozenPick = "IlluminatedFecFrozenGlow";  // 0 fades out (the default), 1 stays while frozen
		constexpr float            kFrozenHold = 4.0f;                        // seconds at full strength
		constexpr float            kFrozenOut = 2.0f;                         // then out over these

		std::unordered_map<const RE::TESEffectShader*, std::string> gFecKey;  // FEC's lit shaders -> their rows' key, at data load
		const RE::TESEffectShader*                                  gFrozen = nullptr;

		// a noted flesh effect's own Light Placer lights (held: a light Light Placer lets go of stays valid until the next note);
		// the effect's own ID rides along, so an effect freed and another made at the same address before the next note is
		// never read as the old one
		struct NotedLights
		{
			std::uint32_t                                effectID{ 0 };
			std::vector<RE::NiPointer<RE::NiPointLight>> lights;
		};
		std::mutex                                                        gRootLock;
		std::unordered_map<const RE::ShaderReferenceEffect*, NotedLights> gRoots;  // main thread writes, guarded

		// Light Placer names an effect's light "LP_Light[<config>|<light>](<effect ID>)#<n>"
		[[nodiscard]] bool LightOfEffect(const RE::NiPointLight* a_light, std::uint32_t a_effectID)
		{
			const char*            raw = a_light ? a_light->name.c_str() : nullptr;
			const std::string_view name = raw ? raw : "";
			if (!name.starts_with("LP_Light[")) {
				return false;
			}
			std::array<char, 24>   text{};
			const auto             end = std::format_to_n(text.data(), text.size() - 1, "]({})#", a_effectID).out;
			const std::string_view mark(text.data(), static_cast<std::size_t>(end - text.data()));
			return name.find(mark) != std::string_view::npos;
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
		gFrozen = nullptr;
		for (auto* shader : LightKit::FormsOf<RE::TESEffectShader>()) {
			const auto low = shader ? Lower(EditorID(shader)) : std::string{};
			if (!low.starts_with("fec_") || !a_cov.shaders.contains(low)) {
				continue;
			}
			gFecKey.emplace(shader, FormKey(low));
			if (low == "fec_frostfxs") {
				gFrozen = shader;
			}
		}
		if (!gFecKey.empty()) {
			SKSE::log::info("Frozen Electrocuted Combustion: {} of its shaders have lights{}", gFecKey.size(),
				gFrozen ? "" : " (its frozen shader is not one)");
		}
	}

	const std::unordered_map<const RE::TESEffectShader*, std::string>& FecShaders() { return gFecKey; }

	float FrozenFade(const RE::ShaderReferenceEffect* a_effect)
	{
		if (!a_effect || !gFrozen || a_effect->effectData != gFrozen || SettingValue(kFrozenPick, 0) != 0) {
			return 1.0f;
		}
		return std::clamp(1.0f - (a_effect->age - kFrozenHold) / kFrozenOut, 0.0f, 1.0f);
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

	void NoteFleshRoots()
	{
		if (LightKit::FleshFx::Hues().empty() && !gFrozen) {
			return;  // no flesh spell wears FleshFX's look and FEC is not here: nothing to note, ever
		}
		std::unordered_map<const RE::ShaderReferenceEffect*, NotedLights> roots;
		if (auto* lists = RE::ProcessLists::GetSingleton()) {
			lists->ForEachMagicTempEffect([&](RE::BSTempEffect* a_effect) {
				auto* shader = a_effect ? netimmerse_cast<RE::ShaderReferenceEffect*>(a_effect) : nullptr;
				if (shader && !shader->finished && (LightKit::FleshFx::HueOf(shader->effectData) || shader->effectData == gFrozen)) {
					const auto ref = shader->target.get();
					if (auto* node = FleshBodyNode(ref.get(), shader->GetAttachRoot())) {
						NotedLights noted{ shader->effectID, {} };
						RE::BSVisit::TraverseScenegraphLights(node, [&](RE::NiPointLight* a_light) {
							if (LightOfEffect(a_light, noted.effectID)) {
								noted.lights.emplace_back(a_light);
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
			gRoots.swap(roots);
		}
		std::scoped_lock lock{ gRootLock, gMissLock };
		std::erase_if(gMisses, [](const auto& a_miss) { return !gRoots.contains(a_miss.first); });
	}

	void FleshLightPlacer(RE::ShaderReferenceEffect* a_effect)
	{
		const auto* hue = a_effect && !a_effect->finished ? LightKit::FleshFx::HueOf(a_effect->effectData) : nullptr;
		const float frozen = a_effect && !a_effect->finished ? FrozenFade(a_effect) : 1.0f;
		if (!hue && frozen >= 1.0f) {
			return;
		}
		std::uint32_t matched = 0;
		{
			std::scoped_lock lock{ gRootLock };
			const auto       it = gRoots.find(a_effect);
			if (it == gRoots.end() || it->second.effectID != a_effect->effectID) {
				return;  // not noted by the main thread yet (a cast this frame): its lights take the hue from the next frame on
			}
			// only the lights the main thread found for this effect: their color, on the values Light Placer just wrote
			for (const auto& light : it->second.lights) {
				auto& data = light->GetLightRuntimeData();
				if (hue) {
					data.diffuse = LightKit::FleshFx::Shown(*hue, data.diffuse);
				}
				data.fade *= frozen;
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
