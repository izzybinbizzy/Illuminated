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
//   lights - and no other effect's on the same actor - take the hue on the values Light Placer just wrote.
//   Without Light Placer: ArtLights.cpp hangs the flesh row's light itself and gives it the hue as it follows the record.

#include "Plugin.h"

namespace Plugin
{
	void FleshLightPlacer(RE::ShaderReferenceEffect* a_effect)
	{
		const auto*     hue = a_effect && !a_effect->finished ? LightKit::FleshFx::HueOf(a_effect->effectData) : nullptr;
		RE::NiAVObject* root = hue ? a_effect->GetAttachRoot() : nullptr;
		if (!root) {
			return;
		}
		// Light Placer hangs a first-person player's effect light on the third-person node of the same name: the reference is
		// found up the root's own parents (no handle is made here - this runs on the game's worker threads too)
		for (const RE::NiAVObject* up = root; up; up = up->parent) {
			if (auto* ref = up->GetUserData()) {
				if (auto* third = ref->Get3D(false); third && third != root) {
					if (auto* same = third->GetObjectByName(root->name)) {
						root = same;
					}
				}
				break;
			}
		}
		std::array<char, 24>   text{};
		const auto             end = std::format_to_n(text.data(), text.size() - 1, "]({})#", a_effect->effectID).out;
		const std::string_view mark(text.data(), static_cast<std::size_t>(end - text.data()));
		RE::BSVisit::TraverseScenegraphLights(root, [&](RE::NiPointLight* a_light) {
			const char*            raw = a_light ? a_light->name.c_str() : nullptr;
			const std::string_view name = raw ? raw : "";
			if (name.starts_with("LP_Light[") && name.find(mark) != std::string_view::npos) {
				auto& data = a_light->GetLightRuntimeData();
				data.diffuse = LightKit::FleshFx::Shown(*hue, data.diffuse);
			}
			return RE::BSVisit::BSVisitControl::kContinue;
		});
	}
}
