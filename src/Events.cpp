// Illuminated - SKSE plugin
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE.txt and the notice at the top of main.cpp.
//
// SKSE's weapon draw events (the way RE::Light by Truman sees a draw - its disableLights.cpp, GPL-3.0, written here from SKSE's
// event, not copied): the actors with drawn hands are known as they draw, so the record route's per-actor passes (the
// drawn-weapon lights, ArtLights.cpp; the ground twins, GroundLights.cpp) look only at those, never at every actor near her each
// frame. A sink only notes the form id; the main thread acts. Twice a second every actor near her is looked at again, so one
// drawn before its 3D loaded, at a load, or by a mod that skips the event is found.

#include "Plugin.h"

namespace Plugin
{
	namespace
	{
		constexpr int kFullLookEvery = 30;

		std::mutex                     gDrawLock;
		std::vector<RE::FormID>        gDrawQueue;  // gDrawLock
		std::unordered_set<RE::FormID> gDrawnIds;   // the main thread's
		std::vector<RE::Actor*>        gDrawn;      // this frame's, the main thread's
		int                            gFullLookIn{ 0 };

		// may come from an animation thread: only the form id is handed on
		class Draws final : public RE::BSTEventSink<SKSE::ActionEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const SKSE::ActionEvent* a_event, RE::BSTEventSource<SKSE::ActionEvent>*) override
			{
				LightKit::Guarded("draw event", [a_event]() {
					using T = SKSE::ActionEvent::Type;
					const auto type = a_event && a_event->actor ? a_event->type.get() : T::kWeaponSwing;
					if (type == T::kBeginDraw || type == T::kEndDraw) {
						std::lock_guard lock(gDrawLock);
						gDrawQueue.push_back(a_event->actor->GetFormID());
					}
				});
				return RE::BSEventNotifyControl::kContinue;
			}
		};

		[[nodiscard]] bool HandsDrawn(const RE::Actor* a_actor)
		{
			const auto* state = a_actor ? a_actor->AsActorState() : nullptr;
			return state && state->IsWeaponDrawn();
		}
	}

	void WatchDraws()
	{
		static Draws draws;
		auto*        actions = SKSE::GetActionEventSource();
		if (!actions) {
			SKSE::log::warn("events: weapon draws NOT watched - the actors with drawn hands are found twice a second only");
			return;
		}
		actions->AddEventSink(&draws);
		SKSE::log::info("events: weapon draws watched");
	}

	void RefreshDrawn()
	{
		if (--gFullLookIn <= 0) {
			// every actor near her again: the high actors are the ones the passes looked at every frame before
			gFullLookIn = kFullLookEvery;
			gDrawnIds.clear();
			if (auto* lists = RE::ProcessLists::GetSingleton()) {
				for (auto& handle : lists->highActorHandles) {
					if (const auto actor = handle.get(); HandsDrawn(actor.get())) {
						gDrawnIds.insert(actor->GetFormID());
					}
				}
			}
		}
		{
			std::lock_guard lock(gDrawLock);
			gDrawnIds.insert(gDrawQueue.begin(), gDrawQueue.end());
			gDrawQueue.clear();
		}
		gDrawn.clear();
		auto* player = RE::PlayerCharacter::GetSingleton();
		if (player) {
			gDrawn.push_back(player);
		}
		std::erase_if(gDrawnIds, [&](RE::FormID a_id) {
			auto* actor = RE::TESForm::LookupByID<RE::Actor>(a_id);
			if (!HandsDrawn(actor)) {
				return true;  // put away, gone or not an actor: looked at again when it draws (its end-of-draw event)
			}
			if (actor != player) {
				gDrawn.push_back(actor);
			}
			return false;
		});
	}

	const std::vector<RE::Actor*>& DrawnActors() { return gDrawn; }

	void ForgetDrawn()
	{
		{
			std::lock_guard lock(gDrawLock);
			gDrawQueue.clear();
		}
		gDrawnIds.clear();
		gDrawn.clear();
		gFullLookIn = 0;
	}
}
