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

		// Off until a reading calls for it: flip it in both mods
		inline constexpr bool kEndCastingLights = false;

		// A casting light when its actor's 3D is let go. The game ends one only on a spell change and, for the player alone, on a node reset; held by the
		// caster, its BSLight outlives its node and stays on what it lit. This does to the light what the game's detach does, in its order. Main thread
		[[maybe_unused]] void EndCastingLights(RE::FormID a_id)
		{
			auto* actor = RE::TESForm::LookupByID<RE::Actor>(a_id);
			if (!actor || actor->IsPlayerRef() || actor->Is3DLoaded()) {
				return;  // its 3D is back; or hers, which the game's own detach looks after
			}
			auto* scene = LightKit::Scene();
			for (auto* caster : actor->GetActorRuntimeData().magicCasters) {
				if (!caster || !caster->light) {
					continue;
				}
				const auto bs = caster->light;
				if (scene) {
					scene->RemoveLight(bs);
				}
				if (auto* light = bs->light.get(); light && light->parent) {
					light->parent->DetachChild(light);
				}
				caster->light.reset();
			}
		}

		// any thread: only read, and only a caster with a light queues the main thread's end
		[[maybe_unused]] void QueueEndCastingLights(const RE::Actor* a_actor)
		{
			const auto& casters = a_actor->GetActorRuntimeData().magicCasters;
			if (a_actor->IsPlayerRef() || std::ranges::none_of(casters, [](const auto* a_caster) { return a_caster && a_caster->light; })) {
				return;
			}
			if (auto* tasks = SKSE::GetTaskInterface()) {
				tasks->AddTask([id = a_actor->GetFormID()]() { LightKit::Guarded("end casting lights", [id]() { EndCastingLights(id); }); });
			}
		}

		void Note3D(RE::Actor* a_actor, bool a_in)
		{
			if constexpr (kEndCastingLights) {
				if (!a_in) {
					QueueEndCastingLights(a_actor);
				}
			}
			NoteAtronach3D(a_actor, a_in);
			Fade::NoteActor3D(a_actor);
			if (gDrawsWatched.load(std::memory_order_relaxed)) {
				std::lock_guard lock(gDrawLock);
				gLoadedQueue.emplace_back(a_actor->GetFormID(), a_in);
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
				set(a_this, a_object, a_queue3DTasks);
				LightKit::Guarded("actor 3D set", [a_this, a_object]() { Note3D(a_this, a_object != nullptr); });
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
	}

	void WatchDraws()
	{
		static Draws draws;
		auto*        actions = SKSE::GetActionEventSource();
		if (actions) {
			actions->AddEventSink(&draws);
		}
		gDrawsWatched.store(true, std::memory_order_relaxed);
		SKSE::log::info("events: weapon draws {}", actions ? "watched" : "NOT watched");
	}

	void RefreshDrawn()
	{
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
			if (!HandsDrawn(actor)) {
				return true;  // put away, gone or not an actor: looked at again when it draws
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
