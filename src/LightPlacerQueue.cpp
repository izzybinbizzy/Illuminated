// Illuminated - SKSE plugin
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE.txt and the notice at the top of main.cpp.
//
// Light Placer 5.2.0 or newer is what the page asks for: it keeps a light put back on an effect's resume (powerof3/LightPlacer#27),
// puts a casting art's lights back after a loading screen (#29), places a weapon's light at its bound centre (#33, the CenterOnBounds
// our weapon rows carry) and takes a casting art's lights away when the art is cleared or its actor goes (#35), so none of that is
// done here.

#include "Plugin.h"

namespace Plugin
{
	std::vector<RE::NiPointer<RE::BSLight>> QueuedLights(RE::ShadowSceneNode* a_scene)
	{
		std::vector<RE::NiPointer<RE::BSLight>> out;
		if (!a_scene) {
			return out;
		}
		auto&               data = a_scene->GetRuntimeData();
		RE::BSSpinLockGuard locker(data.lightQueueLock);
		out.reserve(data.lightQueueAdd.size());
		for (const auto& bs : data.lightQueueAdd) {
			out.push_back(bs);
		}
		return out;
	}

	void CheckLightPlacer()
	{
		const auto* info = SKSE::GetPluginInfo("Light Placer");
		if (!info || REL::Version::unpack(info->version) >= REL::Version{ 5, 2, 0 }) {
			return;
		}
		const auto have = REL::Version::unpack(info->version).string();
		SKSE::log::error("light placer: Illuminated needs 5.2.0 or newer, this one reads as {}", have);
		// one box at data load, Po3Tweaks' way: an older Light Placer skips every file of ours holding CenterOnBounds, spells included.
		// Not on 1.5.97, where SKSE packs the number differently and it reads lower
		if (REL::Module::get().version() >= REL::Version{ 1, 6, 0, 0 }) {
			const auto text = std::format(
				"Illuminated needs Light Placer 5.2.0 or newer; the one installed is {}, and it skips part of "
				"Illuminated's light files. Update Light Placer from Nexus (mod 127557).",
				have);
			RE::DebugMessageBox(text.c_str());
		}
	}
}
