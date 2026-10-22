// Illuminated - SKSE plugin
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE.txt and the notice at the top of main.cpp.
//
// A Light Placer light taken away and put back in the same frame stays away. Drawing a weapon (or loading a save with one drawn)
// suspends and resumes its enchantment effect in one frame: Light Placer queues the light's removal on the suspend and its addition
// on the resume, and the scene works through its queued additions before its removals - so the light is gone until the weapon is
// drawn again. Our fix for it is in Light Placer itself (powerof3/LightPlacer#27); until a release carries it, this does the same
// from outside, right after each effect resumes: a light queued both to be added and to be removed is kept as the scene holds it.
// Installed at data load, after Light Placer hooked the same call when it loaded, so it runs after Light Placer's own resume work.

#include "Plugin.h"

namespace Plugin
{
	namespace
	{
		std::atomic<std::size_t> gKept{ 0 };

		void EraseAll(RE::BSTArray<RE::NiPointer<RE::BSLight>>& a_list, const RE::BSLight* a_light)
		{
			for (auto it = a_list.begin(); it != a_list.end();) {
				it = it->get() == a_light ? a_list.erase(it) : std::next(it);
			}
		}

		// the queues as they stood before the resume: how many additions waited, and which lights waited to be removed
		struct Queued
		{
			std::size_t                     added{ 0 };
			std::vector<const RE::BSLight*> removed;
		};

		[[nodiscard]] Queued QueuedNow(RE::ShadowSceneNode* a_scene)
		{
			auto&               data = a_scene->GetRuntimeData();
			RE::BSSpinLockGuard locker(data.lightQueueLock);
			Queued              out{ data.lightQueueAdd.size(), {} };
			for (const auto& removed : data.lightQueueRemove) {
				if (removed) {
					out.removed.push_back(removed.get());
				}
			}
			return out;
		}

		// under the scene's queue lock. Only an addition THIS resume queued (past a_before.added) of a light whose removal waited
		// before it: the removal is dropped, and so is that addition when the light is still in the scene, so it is not listed twice
		std::size_t KeepRequeued(RE::ShadowSceneNode* a_scene, const Queued& a_before)
		{
			auto&                           data = a_scene->GetRuntimeData();
			RE::BSSpinLockGuard             locker(data.lightQueueLock);
			std::vector<const RE::BSLight*> requeued;
			for (std::size_t i = a_before.added; i < data.lightQueueAdd.size(); ++i) {
				const auto* light = data.lightQueueAdd[static_cast<std::uint32_t>(i)].get();
				if (light && std::ranges::contains(a_before.removed, light) &&
					std::ranges::any_of(data.lightQueueRemove, [&](const auto& removed) { return removed.get() == light; })) {
					requeued.push_back(light);
				}
			}
			for (const auto* light : requeued) {
				EraseAll(data.lightQueueRemove, light);
				if (std::ranges::any_of(data.activeLights, [&](const auto& active) { return active.get() == light; })) {
					for (std::size_t i = data.lightQueueAdd.size(); i-- > a_before.added;) {
						if (data.lightQueueAdd[static_cast<std::uint32_t>(i)].get() == light) {
							data.lightQueueAdd.erase(data.lightQueueAdd.begin() + static_cast<std::ptrdiff_t>(i));
						}
					}
				}
			}
			return requeued.size();
		}

		struct ResumeAfterLightPlacer
		{
			static void thunk(RE::ShaderReferenceEffect* a_this)
			{
				auto*        scene = RE::BSShaderManager::State::GetSingleton().shadowSceneNode[0];
				const Queued before = scene ? QueuedNow(scene) : Queued{};
				func(a_this);
				if (!scene || before.removed.empty()) {
					return;
				}
				LightKit::Guarded("effect resume", [scene, &before]() {
					if (const auto n = KeepRequeued(scene, before); n > 0) {
						if (const auto total = gKept.fetch_add(n, std::memory_order_relaxed) + n; total <= 5) {
							SKSE::log::info("light placer: {} light(s) put back on an effect's resume kept in the scene ({} so far)", n, total);
						}
					}
				});
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
	}

	void KeepLightPlacerLights()
	{
		if (REL::Module::IsVR() || !REX::W32::GetModuleHandleA("po3_LightPlacer.dll")) {
			return;
		}
		REL::Relocation<std::uintptr_t> vtbl{ RE::ShaderReferenceEffect::VTABLE[0] };
		ResumeAfterLightPlacer::func = vtbl.write_vfunc(0x38, ResumeAfterLightPlacer::thunk);  // ShaderReferenceEffect::Resume
		SKSE::log::info("light placer: a light put back on an effect's resume is kept (its fix, until a Light Placer release carries it)");
	}
}
