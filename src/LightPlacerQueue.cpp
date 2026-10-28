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
//
// A spell held in the hand loses its Light Placer light when the player enters another cell, until it is sheathed or equipped
// again: on a cell change Light Placer puts its worn lights back (Actor::ReAddCasterLights) and its effect lights (the cell
// event), but not the casting art's. So a few frames after the player enters a cell, each Light Placer light under the player's
// casting art is registered with the scene again, as a re-equip would; that registration is ours to take back when the light
// leaves the art or Light Placer registers it again itself.

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
				auto*  scene = RE::BSShaderManager::State::GetSingleton().shadowSceneNode[0];
				Queued before;
				if (scene) {
					LightKit::Guarded("effect resume queues", [scene, &before]() { before = QueuedNow(scene); });
				}
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

		// ---- the casting art's lights after a cell change (main thread only, but the event)
		struct Readded
		{
			RE::NiPointer<RE::NiLight> light;
			RE::NiPointer<RE::BSLight> bs;
		};
		std::vector<Readded> gReadded;
		std::atomic<bool>    gCellEntered{ false };
		// seconds after the first player update in the new cell: the art and Light Placer's own reattaching may land late
		constexpr std::array                                 kLookAt{ 0.5f, 2.0f, 5.0f };
		std::optional<std::chrono::steady_clock::time_point> gCellSince;
		std::size_t                                          gNextLook = 0;
		bool                                                 gCellWatchOn = false;
		std::size_t                                          gCellLogs = 0;
		const void*                                          gLastSpace = nullptr;  // the interior cell, or the worldspace outside
		bool                                                 gLastInside = false;

		struct CellWatch final : RE::BSTEventSink<RE::BGSActorCellEvent>
		{
			RE::BSEventNotifyControl ProcessEvent(const RE::BGSActorCellEvent* a_event, RE::BSTEventSource<RE::BGSActorCellEvent>*) override
			{
				if (a_event && a_event->flags == RE::BGSActorCellEvent::CellFlag::kEnter) {
					gCellEntered.store(true, std::memory_order_relaxed);
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};
		CellWatch gCellWatch;

		// the Light Placer lights under the player's casting art - the art itself, and in first person the third-person node of
		// the magic node's name, where Light Placer hangs them then; only Light Placer's own (named LP_Light[...]) - the game's
		// casting light, ours and another mod's are never touched here
		[[nodiscard]] std::vector<RE::NiPointLight*> CastingArtLights(RE::PlayerCharacter* a_player)
		{
			std::vector<RE::NiPointLight*> out;
			auto*                          third = a_player->Get3D(false);
			const auto                     collect = [&out](RE::NiAVObject* a_root) {
				if (!a_root) {
					return;
				}
				RE::BSVisit::TraverseScenegraphObjects(a_root, [&out](RE::NiAVObject* a_object) {
					auto*                  light = netimmerse_cast<RE::NiPointLight*>(a_object);
					const std::string_view name = light ? light->name.c_str() : "";
					if (light && name.starts_with("LP_Light[") && !std::ranges::contains(out, light)) {
						out.push_back(light);
					}
					return RE::BSVisit::BSVisitControl::kContinue;
				});
			};
			for (auto* caster : a_player->GetActorRuntimeData().magicCasters) {
				if (!caster || caster->flags.none(RE::ActorMagicCaster::Flags::kCastingArtAttached)) {
					continue;
				}
				collect(caster->castingArtData.attachedArt.get());
				if (const auto* node = caster->magicNode; node && third) {
					collect(third->GetObjectByName(node->name));
				}
			}
			return out;
		}

		// how the scene holds a light now: queued to be added, or registered (its registrations listed), or not at all
		struct Held
		{
			bool                                    queued{ false };
			std::vector<RE::NiPointer<RE::BSLight>> active;
		};

		[[nodiscard]] Held HeldNow(RE::ShadowSceneNode* a_scene, const RE::NiLight* a_light)
		{
			auto& data = a_scene->GetRuntimeData();
			Held  out;
			for (const auto& bs : data.activeLights) {
				if (bs && bs->light.get() == a_light) {
					out.active.push_back(bs);
				}
			}
			for (const auto& bs : data.activeShadowLights) {
				if (bs && bs->light.get() == a_light) {
					out.active.emplace_back(bs.get());
				}
			}
			RE::BSSpinLockGuard locker(data.lightQueueLock);
			out.queued = std::ranges::any_of(data.lightQueueAdd, [a_light](const auto& bs) { return bs && bs->light.get() == a_light; });
			return out;
		}

		// a light the scene still holds is never touched: taking a registration out and adding one in the same frame left the
		// light dark (measured in game); only a light registered nowhere gets one
		void LookAfterCellChange(RE::ShadowSceneNode* a_scene, RE::PlayerCharacter* a_player, std::size_t a_look)
		{
			std::size_t again = 0;
			std::size_t held = 0;
			const bool  logging = gCellLogs < 12;
			for (auto* light : CastingArtLights(a_player)) {
				if (std::ranges::any_of(gReadded, [light](const Readded& r) { return r.light.get() == light; })) {
					continue;
				}
				const auto now = HeldNow(a_scene, light);
#ifdef FADE_DEBUG_PAGE
				if (logging) {
					SKSE::log::info("[CELL-LIGHT] look {} | {} | registered {} | queued {} | portal strict {} | culled {}", a_look + 1,
						light->name.c_str(), now.active.size(), now.queued, !now.active.empty() && now.active.front()->portalStrict,
						light->GetAppCulled());
				}
#endif
				if (now.queued || !now.active.empty()) {
					++held;
					continue;
				}
				if (auto* bs = LightKit::AddToScene(a_scene, light)) {
					gReadded.push_back({ RE::NiPointer<RE::NiLight>(light), RE::NiPointer<RE::BSLight>(bs) });
					++again;
				}
			}
			if (logging) {
				++gCellLogs;
				SKSE::log::info("light placer: cell change look {} - {} light(s) on the casting art held by the scene, {} registered again",
					a_look + 1, held, again);
			}
		}

		// ours goes when its light left the art (a sheathe or an unequip: Light Placer detached it) or the scene holds the light
		// another way again (Light Placer's own registration back)
		void TendReadded(RE::ShadowSceneNode* a_scene)
		{
			std::erase_if(gReadded, [a_scene](const Readded& r) {
				const auto held = HeldNow(a_scene, r.light.get());
				const bool other = held.queued || std::ranges::any_of(held.active, [&r](const auto& bs) { return bs != r.bs; });
				if (r.light->parent && !other) {
					return false;
				}
				a_scene->RemoveLight(r.bs);
				return true;
			});
		}
	}

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

	void KeepLightPlacerLights()
	{
		if (REL::Module::IsVR() || !REX::W32::GetModuleHandleA("po3_LightPlacer.dll")) {
			return;
		}
		REL::Relocation<std::uintptr_t> vtbl{ RE::ShaderReferenceEffect::VTABLE[0] };
		ResumeAfterLightPlacer::func = vtbl.write_vfunc(0x38, ResumeAfterLightPlacer::thunk);  // ShaderReferenceEffect::Resume
		SKSE::log::info("light placer: a light put back on an effect's resume is kept (its fix, until a Light Placer release carries it)");
		if (auto* player = RE::PlayerCharacter::GetSingleton()) {
			player->AsBGSActorCellEventSource()->AddEventSink(&gCellWatch);
			gCellWatchOn = true;
			SKSE::log::info("light placer: the casting art's lights are registered again after a cell change");
		}
	}

	void DropCastingLights()
	{
		auto* scene = RE::BSShaderManager::State::GetSingleton().shadowSceneNode[0];
		for (const auto& r : gReadded) {
			if (scene && r.bs) {
				scene->RemoveLight(r.bs);
			}
		}
		gReadded.clear();
		gCellSince.reset();
		gNextLook = kLookAt.size();
		gLastSpace = nullptr;  // the first cell of the loaded game came through a loading screen
		gLastInside = false;
	}

	void KeepCastingLights()
	{
		if (!gCellWatchOn) {
			return;
		}
		auto* scene = RE::BSShaderManager::State::GetSingleton().shadowSceneNode[0];
		auto* player = RE::PlayerCharacter::GetSingleton();
		if (!scene || !player) {
			return;
		}
		if (!gReadded.empty()) {
			TendReadded(scene);
		}
		const auto now = std::chrono::steady_clock::now();
		if (gCellEntered.exchange(false, std::memory_order_relaxed)) {
			gCellSince = now;
			gNextLook = 0;
		}
		if (!gCellSince || gNextLook >= kLookAt.size()) {
			return;
		}
		if (std::chrono::duration<float>(now - *gCellSince).count() >= kLookAt[gNextLook]) {
			// the lights go only through a loading screen (an interior on either side, or another worldspace); a step across an
			// exterior cell border keeps them, so it is not looked at
			if (gNextLook == 0) {
				const auto* cell = player->GetParentCell();
				const bool  inside = cell && cell->IsInteriorCell();
				const void* space = inside ? static_cast<const void*>(cell) : static_cast<const void*>(player->GetWorldspace());
				const bool  through = inside || gLastInside || space != gLastSpace;
				gLastSpace = space;
				gLastInside = inside;
				if (!through) {
					gNextLook = kLookAt.size();
					return;
				}
			}
			LookAfterCellChange(scene, player, gNextLook);
			++gNextLook;
		}
	}
}
