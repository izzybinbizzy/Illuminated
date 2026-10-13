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
//   the game's worker threads, so it never searches the actor's 3D: the node the lights hang on is found on the main thread
//   once a frame (NoteFleshRoots) and only read there.
//   Without Light Placer: ArtLights.cpp hangs the flesh row's light itself and gives it the hue as it follows the record.

#include "Plugin.h"

namespace Plugin
{
	namespace
	{
		std::mutex                                                                       gRootLock;
		std::unordered_map<const RE::ShaderReferenceEffect*, RE::NiPointer<RE::NiAVObject>> gRoots;  // main thread writes, guarded

		// a flesh shader the hue applies to that matched no Light Placer light for a while: logged once each (a silent miss
		// would leave the old color with nothing to say why)
		std::mutex                                                gMissLock;
		std::unordered_map<const RE::TESEffectShader*, std::uint32_t> gMisses;
		std::unordered_set<const RE::TESEffectShader*>                gMissLogged;
	}

	RE::NiAVObject* FleshBodyNode(RE::TESObjectREFR* a_ref, RE::NiAVObject* a_root)
	{
		// Light Placer (and ArtLights.cpp) hang a first-person player's effect light on the third-person node of the same name;
		// an unnamed root is used as it is - a search for "" would answer with any unnamed node
		auto* third = a_ref && a_root ? a_ref->Get3D(false) : nullptr;
		const char* name = a_root ? a_root->name.c_str() : nullptr;
		if (!third || third == a_root || !name || !*name) {
			return a_root;
		}
		auto* same = third->GetObjectByName(a_root->name);
		return same ? same : a_root;
	}

	void NoteFleshRoots()
	{
		std::unordered_map<const RE::ShaderReferenceEffect*, RE::NiPointer<RE::NiAVObject>> roots;
		if (auto* lists = RE::ProcessLists::GetSingleton(); lists && !LightKit::FleshFx::Hues().empty()) {
			lists->ForEachMagicTempEffect([&](RE::BSTempEffect* a_effect) {
				auto* shader = a_effect ? netimmerse_cast<RE::ShaderReferenceEffect*>(a_effect) : nullptr;
				if (shader && !shader->finished && LightKit::FleshFx::HueOf(shader->effectData)) {
					const auto ref = shader->target.get();
					if (auto* node = FleshBodyNode(ref.get(), shader->GetAttachRoot())) {
						roots.emplace(shader, RE::NiPointer<RE::NiAVObject>(node));
					}
				}
				return RE::BSContainer::ForEachResult::kContinue;
			});
		}
		std::scoped_lock lock{ gRootLock };
		gRoots.swap(roots);
	}

	void FleshLightPlacer(RE::ShaderReferenceEffect* a_effect)
	{
		const auto* hue = a_effect && !a_effect->finished ? LightKit::FleshFx::HueOf(a_effect->effectData) : nullptr;
		if (!hue) {
			return;
		}
		RE::NiPointer<RE::NiAVObject> root;
		{
			std::scoped_lock lock{ gRootLock };
			if (const auto it = gRoots.find(a_effect); it != gRoots.end()) {
				root = it->second;
			}
		}
		if (!root) {
			root.reset(a_effect->GetAttachRoot());  // not seen by the main thread yet: the effect's own root this once
		}
		if (!root) {
			return;
		}
		std::array<char, 24>   text{};
		const auto             end = std::format_to_n(text.data(), text.size() - 1, "]({})#", a_effect->effectID).out;
		const std::string_view mark(text.data(), static_cast<std::size_t>(end - text.data()));
		std::uint32_t          matched = 0;
		RE::BSVisit::TraverseScenegraphLights(root.get(), [&](RE::NiPointLight* a_light) {
			const char*            raw = a_light ? a_light->name.c_str() : nullptr;
			const std::string_view name = raw ? raw : "";
			if (name.starts_with("LP_Light[") && name.find(mark) != std::string_view::npos) {
				auto& data = a_light->GetLightRuntimeData();
				data.diffuse = LightKit::FleshFx::Shown(*hue, data.diffuse);
				++matched;
			}
			return RE::BSVisit::BSVisitControl::kContinue;
		});
		std::scoped_lock lock{ gMissLock };
		auto& misses = gMisses[a_effect->effectData];
		misses = matched ? 0 : misses + 1;
		if (misses == 300 && gMissLogged.insert(a_effect->effectData).second) {
			SKSE::log::warn("FleshFX: no Light Placer light named for effect {} found under its body for 300 updates - that flesh "
							"spell keeps its old light color", a_effect->effectID);
		}
	}
}
