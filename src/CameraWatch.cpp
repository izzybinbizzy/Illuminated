// Luminous Arcana - SKSE plugin
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE.txt and the notice at the top of main.cpp.
//
// A watcher for one bug and nothing else: HIS REPORT, 2026-09-17 - *"whenever i first load in it forces me into first
// person for a few seconds then forces me back to whatever i was doing"*.
//
// It changes nothing in the game. For twenty seconds after a save loads it looks, once a frame, at which camera the
// game is using and at whether the player's body has been rebuilt, and writes a line ONLY when either changes. So the
// log says exactly when the camera flipped, how long it stayed, and whether the body was reloaded at the same moment -
// which is what tells a script forcing the camera apart from something rebuilding the player.

#include "Plugin.h"

namespace Plugin
{
	namespace
	{
		// his report, 2026-09-17 night: the blip starts BEFORE the save is in, so the watch starts at the main menu
		// and runs long enough to cover the loading screen and the first minute of play
		constexpr double kWatchSeconds = 90.0;
		constexpr double kMarkEvery = 10.0;

		struct Watch
		{
			std::chrono::steady_clock::time_point started;
			std::chrono::steady_clock::time_point last;
			bool                                  firstPerson{ false };
			const void*                           body{ nullptr };
			bool                                  first{ true };
			std::size_t                           flips{ 0 };
			double                                marked{ 0.0 };
			std::string                           why;
		};

		std::shared_ptr<Watch> gWatch;

		double Seconds(const Watch& a_w)
		{
			return std::chrono::duration<double>(std::chrono::steady_clock::now() - a_w.started).count();
		}

		void Look(std::shared_ptr<Watch> a_watch)
		{
			auto* camera = RE::PlayerCamera::GetSingleton();
			auto* player = RE::PlayerCharacter::GetSingleton();
			if (!camera || !player || !a_watch) {
				return;
			}
			const bool  firstPerson = camera->IsInFirstPerson();
			const void* body = player->GetCurrent3D();
			if (a_watch->first || firstPerson != a_watch->firstPerson || body != a_watch->body) {
				if (!a_watch->first) {
					++a_watch->flips;
				}
				SKSE::log::info("[CAMERA] {:5.2f}s | {} | the player's body {}{}", Seconds(*a_watch),
					firstPerson ? "first person" : "third person",
					body == a_watch->body ? "is the one it was" : (a_watch->first ? "as it loaded" : "HAS BEEN REBUILT"),
					a_watch->first ? " | watching for 20 seconds; only changes are written" : "");
				a_watch->first = false;
				a_watch->firstPerson = firstPerson;
				a_watch->body = body;
			}
			// a mark every ten seconds, so a quiet stretch is proof the watch was running and saw nothing
			if (Seconds(*a_watch) - a_watch->marked >= kMarkEvery) {
				a_watch->marked = Seconds(*a_watch);
				SKSE::log::info("[CAMERA] {:5.1f}s | still {} | watching since {}", a_watch->marked,
					firstPerson ? "first person" : "third person", a_watch->why);
			}
			if (Seconds(*a_watch) >= kWatchSeconds) {
				SKSE::log::info("[CAMERA] done: {} change(s) in the first {:.0f} seconds after the load", a_watch->flips, kWatchSeconds);
				return;
			}
			SKSE::GetTaskInterface()->AddTask([a_watch]() { Look(a_watch); });
		}
	}

	void WatchCamera(std::string_view a_why)
	{
		if (gWatch && Seconds(*gWatch) < kWatchSeconds) {
			return;  // one watch at a time: the one that started at the main menu keeps running through the load
		}
		auto watch = std::make_shared<Watch>();
		watch->started = std::chrono::steady_clock::now();
		watch->why = std::string(a_why);
		gWatch = watch;
		SKSE::GetTaskInterface()->AddTask([watch]() { Look(watch); });
	}
}
