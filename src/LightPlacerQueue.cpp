// Illuminated - SKSE plugin
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE.txt and the notice at the top of main.cpp.
//
// Light Placer 5.1.0 or newer is what the page asks for: it keeps a light put back on an effect's resume (powerof3/LightPlacer#27) and
// puts a casting art's lights back after a loading screen (powerof3/LightPlacer#29), so neither is done here.
//
// A light of ours can still stay lit in the scene with no node when the casting art is cleared away in one go (a dispel and a
// temporary-effects clear with the hands up); every few frames the ones that have stood so for two looks are put out.

#include "Plugin.h"
#include "StrayLights.h"

namespace Plugin
{
	namespace
	{
		// a stray light comes of a spell's effects being cleared with its art up, seen at the next spell: looks for two seconds after an
		// art starts (LightCopies.cpp), an effect ends or a hand moves, none at any other time. Any thread
		constexpr int    kStrayLooks = 8;
		std::atomic<int> gStrayLooks{ 0 };

		// only a casting art torn down with the hands up leaves a stray: an effect ending on someone at rest leaves none
		[[nodiscard]] bool HandsOut(const RE::TESObjectREFR* a_ref)
		{
			const auto* actor = a_ref ? a_ref->As<RE::Actor>() : nullptr;
			return actor && actor->AsActorState()->IsWeaponDrawn();
		}

		struct StrayWatch final :
			RE::BSTEventSink<RE::TESActiveEffectApplyRemoveEvent>,
			RE::BSTEventSink<SKSE::ActionEvent>
		{
			RE::BSEventNotifyControl ProcessEvent(const RE::TESActiveEffectApplyRemoveEvent* a_event, RE::BSTEventSource<RE::TESActiveEffectApplyRemoveEvent>*) override
			{
				if (a_event && !a_event->isApplied && (HandsOut(a_event->target.get()) || HandsOut(a_event->caster.get()))) {
					NoteStrayLook(StrayWhy::kEffectEnd);
				}
				return RE::BSEventNotifyControl::kContinue;
			}

			RE::BSEventNotifyControl ProcessEvent(const SKSE::ActionEvent* a_event, RE::BSTEventSource<SKSE::ActionEvent>*) override
			{
				// a swing or a bow can leave no casting art behind, and a town's fights would keep this awake
				using T = SKSE::ActionEvent::Type;
				const auto type = a_event ? a_event->type.get() : T::kWeaponSwing;
				if (type != T::kWeaponSwing && type != T::kBowDraw && type != T::kBowRelease) {
					NoteStrayLook(StrayWhy::kAction);
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};
		StrayWatch gStrayWatch;

		// A light of ours can stay lit in the scene with no node when a casting art is torn down without its removal (a dispel and a
		// temporary-effects clear with the hands up, then another spell). One that stands without a parent for two looks,
		// a quarter second apart, goes out the way the scene takes any light out; a light with a parent, one being removed already
		// and another mod's are never touched
		constexpr std::chrono::milliseconds   kStrayEvery{ 250 };
		StrayLights::Watch                    gStray;
		std::chrono::steady_clock::time_point gStrayAt;
		std::size_t                           gStrayPutOut = 0;
		std::size_t                           gStrayLogged = 0;
		bool                                  gWatching = false;

		// no node; or hidden on a tree that is no live body: its body was removed, and Light Placer, which keeps a light whose condition fails,
		// hidden, never hears of it. A light on a live body - a reference up its tree, the tree in the world (the world's root on top) or on one
		// of her own skeletons - stays: it comes back when its condition holds. A let-go body keeps its reference while its detached tree lives
		bool LeftBehind(const RE::NiLight* a_light)
		{
			if (!a_light->parent) {
				return true;
			}
			if (!a_light->GetAppCulled()) {
				return false;
			}
			bool                  referenced = false;
			const RE::NiAVObject* top = a_light;
			for (const RE::NiAVObject* node = a_light; node; node = node->parent) {
				referenced = referenced || node->GetUserData() != nullptr;
				top = node;
			}
			if (!referenced) {
				return true;
			}
			auto*      player = RE::PlayerCharacter::GetSingleton();
			const bool hers = player && (top == player->Get3D(false) || top == player->Get3D(true));
			return top != RE::Main::WorldRootNode() && !hers;
		}

		void PutOutStrayLights(RE::ShadowSceneNode* a_scene)
		{
			auto&                                   data = a_scene->GetRuntimeData();
			std::vector<RE::NiPointer<RE::NiLight>> lights;
			std::vector<StrayLights::Seen>          seen;
			const auto                              look = [&](const RE::BSLight* a_bs) {
				auto*       light = a_bs ? a_bs->light.get() : nullptr;
				const char* name = light ? light->name.c_str() : nullptr;
				if (name && StrayLights::IsOurs(name) && LeftBehind(light)) {
					lights.emplace_back(light);
					seen.push_back({ light, name, false, false });
				}
			};
			for (const auto& bs : data.activeLights) {
				look(bs.get());
			}
			for (const auto& bs : data.activeShadowLights) {
				look(bs.get());
			}
			if (!seen.empty()) {
				RE::BSSpinLockGuard locker(data.lightQueueLock);
				for (auto& s : seen) {
					s.removalQueued = std::ranges::any_of(data.lightQueueRemove, [&s](const auto& a_queued) { return a_queued && a_queued->light.get() == s.key; });
				}
			}
			const auto stray = gStray.Look(seen);
			for (const auto i : stray) {
				a_scene->RemoveLight(lights[i].get());
			}
			if (!stray.empty()) {
				gStrayPutOut += stray.size();
				if (gStrayLogged++ < 5) {
					SKSE::log::info("light placer: {} light(s) of ours left in the scene with no node put out ({} so far)", stray.size(), gStrayPutOut);
				}
			}
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

	void WatchStrayLights()
	{
		if (!REX::W32::GetModuleHandleA("po3_LightPlacer.dll")) {
			return;
		}
		gWatching = true;
		if (auto* events = RE::ScriptEventSourceHolder::GetSingleton()) {
			events->AddEventSink<RE::TESActiveEffectApplyRemoveEvent>(&gStrayWatch);
		}
		if (auto* actions = SKSE::GetActionEventSource()) {
			actions->AddEventSink(&gStrayWatch);
		}
		// only a line in the log: SKSE packs the number differently on 1.5.97, where it reads lower
		if (const auto* info = SKSE::GetPluginInfo("Light Placer"); info && REL::Version::unpack(info->version) < REL::Version{ 5, 1, 0 }) {
			SKSE::log::warn("light placer: Illuminated needs 5.1.0 or newer, this one reads as {}", REL::Version::unpack(info->version).string());
		}
	}

	void ForgetStrayLights()
	{
		gStray.Clear();
	}

	namespace
	{
		std::array<std::atomic<std::uint32_t>, static_cast<std::size_t>(StrayWhy::kCount)> gStrayNotes{};
	}

	void NoteStrayLook(StrayWhy a_why) noexcept
	{
		gStrayNotes[static_cast<std::size_t>(a_why)].fetch_add(1, std::memory_order_relaxed);
		gStrayLooks.store(kStrayLooks, std::memory_order_relaxed);
	}

	std::uint32_t StrayNotes(std::size_t a_why) noexcept { return a_why < gStrayNotes.size() ? gStrayNotes[a_why].load(std::memory_order_relaxed) : 0; }

	void StrayLightLooks()
	{
		// Light Placer draws no row of ours on the record route
		if (!gWatching || RecordRoute() || gStrayLooks.load(std::memory_order_relaxed) <= 0) {
			return;
		}
		auto*      scene = LightKit::Scene();
		const auto now = std::chrono::steady_clock::now();
		if (scene && now - gStrayAt >= kStrayEvery) {
			// the meter counts a look, not the frames between looks
			const LightKit::Cost::Pass pass{ 8, "stray lights" };
			gStrayAt = now;
			gStrayLooks.fetch_sub(1, std::memory_order_relaxed);
			PutOutStrayLights(scene);
		}
	}
}
