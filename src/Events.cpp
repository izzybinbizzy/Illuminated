// Illuminated - SKSE plugin
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE.txt and the notice at the top of main.cpp.
//
// SKSE's weapon draw events (the way RE::Light by Truman sees a draw - its disableLights.cpp, GPL-3.0, written here from SKSE's
// event, not copied): the actors with drawn hands are known as they draw, so the record route's per-actor passes (the
// drawn-weapon lights, ArtLights.cpp; the ground twins, GroundLights.cpp) look only at those, never at every actor near her each
// frame. A sink only notes the form id; the main thread acts. An actor that loads with its hands out comes through its own 3D
// slots, and every actor near her is looked at once when a game loads.

#include "Fade.h"
#include "Plugin.h"

namespace Plugin
{
	namespace
	{
		std::mutex                               gDrawLock;
		std::vector<RE::FormID>                  gDrawQueue;              // gDrawLock
		std::vector<std::pair<RE::FormID, bool>> gLoadedQueue;            // gDrawLock
		std::atomic<bool>                        gDrawsWatched{ false };  // the record route's: only it reads the queues
		std::unordered_set<RE::FormID>           gDrawnIds;               // the main thread's
		std::vector<RE::Actor*>                  gDrawn;                  // this frame's, the main thread's
		bool                                     gLookAround{ true };
		std::atomic<bool>                        gDrawWork{ false };  // a sink queued something since the last pass

		// a loading screen takes every light out of the scene's lists and a light of ours is not put back: kept, it lights nothing, and what it
		// lit keeps drawing it. So each is let go on the main thread and the passes make it again (Dynamic Wards' way). A sink only queues this
		void AfterLoadingScreen()
		{
			DropArtLights();
			DropAtronachLights();
			RenewGroundLights();
		}

		class LoadingWatch final : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
			{
				if (a_event && !a_event->opening && std::string_view(a_event->menuName.c_str()) == RE::LoadingMenu::MENU_NAME) {
					if (auto* tasks = SKSE::GetTaskInterface()) {
						tasks->AddTask([]() { LightKit::Guarded("loading screen ended", []() { AfterLoadingScreen(); }); });
					}
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};

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
						NoteWeaponLook();
						std::lock_guard lock(gDrawLock);
						gDrawQueue.push_back(a_event->actor->GetFormID());
						gDrawWork.store(true, std::memory_order_relaxed);
					}
				});
				return RE::BSEventNotifyControl::kContinue;
			}
		};

		// a_hadBody: the body this call let go was there - a load clears thousands of empty slots, which leave nothing behind
		void Note3D(RE::Actor* a_actor, bool a_in, bool a_hadBody = false)
		{
			NoteAtronach3D(a_actor, a_in);
			Fade::NoteActor3D(a_actor);
			if (!a_in && a_hadBody) {
				NoteStrayLook(StrayWhy::kBodyGone);  // a light of its body may be left behind with no node
			}
			if (gDrawsWatched.load(std::memory_order_relaxed)) {
				NoteWeaponLook();
				std::lock_guard lock(gDrawLock);
				gLoadedQueue.emplace_back(a_actor->GetFormID(), a_in);
				gDrawWork.store(true, std::memory_order_relaxed);
			}
		}

		// An actor's 3D in and out, at its own two slots: the game's load event names only a reference with a script or in an alias (read in
		// the 1.6.1170 exe). A queued load returns its body at the first on a loader thread and the game stores it later; a null at the
		// second is the body let go. Any thread: only noted
		template <class T>
		struct Actor3D
		{
			static RE::NiAVObject* LoadThunk(T* a_this, bool a_backgroundLoading)
			{
				auto* root = load(a_this, a_backgroundLoading);
				if (root) {
					LightKit::Guarded("actor 3D in", [a_this]() { Note3D(a_this, true); });
				}
				return root;
			}
			static void SetThunk(T* a_this, RE::NiAVObject* a_object, bool a_queue3DTasks)
			{
				const bool hadBody = a_this->Get3D() != nullptr;
				set(a_this, a_object, a_queue3DTasks);
				LightKit::Guarded("actor 3D set", [a_this, a_object, hadBody]() { Note3D(a_this, a_object != nullptr, hadBody); });
			}
			static inline REL::Relocation<decltype(LoadThunk)> load;
			static inline REL::Relocation<decltype(SetThunk)>  set;
			static void                                        Install()
			{
				REL::Relocation<std::uintptr_t> vtbl{ T::VTABLE[0] };
				load = vtbl.write_vfunc(0x6A, LoadThunk);
				set = vtbl.write_vfunc(0x6C, SetThunk);
			}
		};

		// what else changes which light a weapon in a hand wants. Only noted, any thread
		class WeaponChanges final :
			public RE::BSTEventSink<RE::TESEquipEvent>,
			public RE::BSTEventSink<SKSE::CameraEvent>,
			public RE::BSTEventSink<SKSE::NiNodeUpdateEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const RE::TESEquipEvent*, RE::BSTEventSource<RE::TESEquipEvent>*) override
			{
				NoteWeaponLook();
				return RE::BSEventNotifyControl::kContinue;
			}
			RE::BSEventNotifyControl ProcessEvent(const SKSE::CameraEvent*, RE::BSTEventSource<SKSE::CameraEvent>*) override
			{
				NoteWeaponLook();
				return RE::BSEventNotifyControl::kContinue;
			}
			RE::BSEventNotifyControl ProcessEvent(const SKSE::NiNodeUpdateEvent*, RE::BSTEventSource<SKSE::NiNodeUpdateEvent>*) override
			{
				NoteWeaponLook();
				return RE::BSEventNotifyControl::kContinue;
			}
		};

		[[nodiscard]] bool HandsDrawn(const RE::Actor* a_actor)
		{
			const auto* state = a_actor ? a_actor->AsActorState() : nullptr;
			return state && state->IsWeaponDrawn();
		}
	}

	void WatchActors3D()
	{
		Actor3D<RE::Character>::Install();
		Actor3D<RE::PlayerCharacter>::Install();
		static LoadingWatch loading;
		if (auto* ui = RE::UI::GetSingleton()) {
			ui->AddEventSink<RE::MenuOpenCloseEvent>(&loading);
		}
	}

	void WatchDraws()
	{
		static Draws draws;
		auto*        actions = SKSE::GetActionEventSource();
		if (actions) {
			actions->AddEventSink(&draws);
		}
		static WeaponChanges changes;
		if (auto* events = RE::ScriptEventSourceHolder::GetSingleton()) {
			events->AddEventSink<RE::TESEquipEvent>(&changes);
		}
		if (auto* cameras = SKSE::GetCameraEventSource()) {
			cameras->AddEventSink(&changes);
		}
		if (auto* updates = SKSE::GetNiNodeUpdateEventSource()) {
			updates->AddEventSink(&changes);
		}
		gDrawsWatched.store(true, std::memory_order_relaxed);
		SKSE::log::info("events: weapon draws {}", actions ? "watched" : "NOT watched");
	}

	void RefreshDrawn()
	{
		// nothing queued and only she on the list, her hands read by whoever needs them
		if (!gLookAround && !gDrawWork.load(std::memory_order_relaxed) && gDrawnIds.empty() && gDrawn.size() == 1) {
			return;
		}
		const LightKit::Cost::Pass pass{ 17, "drawn hands (the record route's actors)" };
		if (std::exchange(gLookAround, false)) {
			if (auto* lists = RE::ProcessLists::GetSingleton()) {
				for (auto& handle : lists->highActorHandles) {
					if (const auto actor = handle.get(); HandsDrawn(actor.get())) {
						gDrawnIds.insert(actor->GetFormID());
					}
				}
			}
		}
		{
			static std::vector<std::pair<RE::FormID, bool>> loads;  // kept so the pass allocates nothing once it has grown
			loads.clear();
			{
				std::lock_guard lock(gDrawLock);
				gDrawWork.store(false, std::memory_order_relaxed);
				gDrawnIds.insert(gDrawQueue.begin(), gDrawQueue.end());
				gDrawQueue.clear();
				loads.swap(gLoadedQueue);
			}
			// outside the lock: a cell's load is thousands of these, and the sinks wait on it
			for (const auto& [id, loaded] : loads) {
				if (!loaded) {
					gDrawnIds.erase(id);
				} else if (HandsDrawn(RE::TESForm::LookupByID<RE::Actor>(id))) {
					gDrawnIds.insert(id);
				}
			}
		}
		gDrawn.clear();
		auto* player = RE::PlayerCharacter::GetSingleton();
		if (player) {
			gDrawn.push_back(player);
		}
		std::erase_if(gDrawnIds, [&](RE::FormID a_id) {
			auto* actor = RE::TESForm::LookupByID<RE::Actor>(a_id);
			// put away, gone, its body unloaded or not an actor: looked at again when it draws or its body loads with the hands out
			// (Note3D). She needs no place here: she is always on the list
			if (!HandsDrawn(actor) || actor == player || !actor->Is3DLoaded()) {
				return true;
			}
			// a far actor stays listed and is looked at from the moment it comes near, as the passes always did
			const auto* process = actor->GetActorRuntimeData().currentProcess;
			if (actor != player && process && process->InHighProcess()) {
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
			gLoadedQueue.clear();
		}
		gDrawnIds.clear();
		gDrawn.clear();
		gLookAround = true;
	}
}
