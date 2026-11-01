// Illuminated - SKSE plugin
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE.txt and the notice at the top of main.cpp.
//
// The Brightness and Reach sliders. A Light Placer light that states no fade, radius or cutoff of its own takes them
// from its light record, and reads them each time it makes a light (Light Placer's LightData.cpp: GetFade, GetRadius,
// GetCutoff). So every light the configs name is an in-memory copy made here - one per light record, fade, radius and
// cutoff the configs used - and the sliders set those three on the copy. A copy is found by its editor ID, which Light
// Placer looks up when it reads its configs, after this runs. The copies are listed in the settings files ([light]
// blocks), written by lagen.py. Pass 0's own copies of the game's magic lights (LightSettings.cpp) and pass 4's spray
// lights (SprayLights.cpp) join the list too, so every light this mod makes follows the sliders.
//
// How the two sliders reach a light:
//   Brightness  the fade.
//   Reach       the radius - and, for an inverse-square light, the cutoff. Community Shaders works such a light's reach
// out from its fade and its cutoff alone (measured: the stated radius changed nothing, and a
//               brighter light reached further). The cutoff is the made-with one moved by LightKit::ReachCutoffFactor, which moves the
//               reach by Reach and holds it still while Brightness changes.
//               On ENB and Vanilla (Lighting.cpp) an inverse-square copy is made plain when it is made: no cutoff, the
//               reach it had drawn as a radius, and Reach moves that radius.
//
// A light that is already lit follows at once: on the frame a slider changes, every lit light that names one of our
// copies takes its copy's cutoff, and a steady light's fade moves by the change (Frame, on the main thread).
//
// A flickering light (`flicker=1`) is the one kind the record cannot carry: Light Placer writes its fade from the
// config's keys every frame, never from the record. Those lights are kept in a list, and each is scaled by Brightness
// straight after Light Placer wrote it - Light Placer does that in five places, most of them on the game's worker
// threads (traced), and each is wrapped here, after Light Placer's own hook:
//   a caster's update        the hands of every actor (the player's among them)
//   an effect's update       enchantment and hit effects (and, on a flesh spell under FleshFX, its hue: FleshLights.cpp)
//   an explosion's update    } SE/AE only: these three are calls inside game functions, at the places Light Placer
//   a hazard's update        } (powerof3/LightPlacer, Hooks/Update.cpp) hooks them. In VR these lights keep
//   a cell's animations      } their made-with strength.
// Light Placer skips a culled or distant light, so a value it did not rewrite is never scaled twice. Nothing in this
// part runs while Brightness is 100%, nor without Light Placer (the record route has no Light Placer light to scale).
// Besides: on the record route (Vanilla / ENB, or no Light Placer - RecordLights.cpp DecideRecordRoute) the hazard's update
// places a hazard's own light (PlaceHazardLight - the game never places a light hung on a model's root, measured on the
// burning-ground fires) and the explosion's update lifts an explosion's light off its root (LiftExplosionLight); on every
// route a spell's explosion and hazard take the art pick's hand color (TintUnder, TintHazard). In VR those wraps are not
// made, so there those fires keep the game's unplaced light.
// Every hook's own work runs inside LightKit::Guarded: no exception may leave into the game.
//
// The fading module (Fade*.cpp) scales some of the same lights by charge and magicka, in the same three places (the
// player's update, an effect's update, the cell's animations). Each place has ONE wrapper, here: Brightness first,
// then the fading module, always in that order. The two never take the other's write for a new value from Light
// Placer: the fading module reports every fade it writes (NoteFadeWrite - taken only when it started from a value this
// pass had scaled, or a light never scaled yet would look done), and a slider change tells it how far a
// steady light moved (Fade::Rebase). Lock order is always the fading module's lock, then gFlickerLock - never the
// other way round (Fade::Rebase is called after gFlickerLock is let go).

#include "Plugin.h"
#include "Fade.h"

namespace Plugin
{
	namespace
	{
		constexpr std::string_view kBrightness = "IlluminatedBrightness";
		constexpr std::string_view kReach = "IlluminatedReach";
		constexpr float            kLowestCutoff = 0.01f, kHighestCutoff = 1.0f;  // what Light Placer keeps a cutoff within
		constexpr std::uint32_t    kInverseSquare = LightKit::kRecordInverseSquare;

		std::atomic<bool>  gRefresh{ false };        // a setting changed: the passes again (RefreshLights)
		std::atomic<bool>  gDaylightMoved{ false };  // only the daylight moved: the sliders' numbers again, nothing else
		std::atomic<float> gBrightness{ 1.0f };

		// HIS GO-TO PICKS ("do all of them"): smarter brightness and a light budget
		constexpr std::string_view kSmartBrightness = "IlluminatedSmartBrightness";  // 0 off, 1 a little, 2 more
		constexpr std::string_view kHandLights = "IlluminatedHandLights";            // 0 everyone, 1 nearby, 2 + followers, 3 player only
		// "about forty paces" in game units: the advanced settings file's [Lights] NearbyDistance (gNearby, Plugin.h)
		std::atomic<float> gDaylight{ 1.0f };  // what Brightness is multiplied by now: 1 at night and in the dark
		std::atomic<int>   gHandBudget{ 0 };   // the Hand lights for choice, read by every caster's update
		float              gDaylightClock{ 0.0f };

		// how much dimmer the lights are now (LightKit.h: by the hour outdoors, by the room's own light indoors)
		float DaylightFactor() { return LightKit::Daylight(SettingValue(kSmartBrightness, 0)); }

		// every two seconds on the main thread: a change worth seeing moves every light the way a Brightness change does (only
		// the strength - the passes are not run again for it)
		void WatchDaylight(float a_delta)
		{
			gDaylightClock += a_delta;
			if (gDaylightClock < 2.0f) {
				return;
			}
			gDaylightClock = 0.0f;
			if (const float now = DaylightFactor(); std::abs(now - gDaylight.load()) >= 0.02f) {
				gDaylight = now;
				gDaylightMoved = true;
			}
		}

		// the casting lights the budget put out, by the fade they had: the game sets a steady one's fade once, when the spell
		// is readied, so one left out keeps 0 until its caster is kept again (walks near) and it is given back
		std::mutex                                    gBudgetLock;
		std::unordered_map<const RE::NiLight*, float> gPutOut;
		constexpr std::size_t                         kMostPutOut = 512;  // casters gone with their light out: dropped at this many

		// the light budget: a hand light the Hand lights for choice leaves out is put out after the game wrote it this frame.
		// HIS ANSWER 2026-10-13: only WITHOUT Light Placer (the record route), as its hover text says - with Light Placer the
		// game's casting light on a far caster's unpatched spell is left as the game makes it
		void ApplyHandBudget(RE::ActorMagicCaster* a_caster)
		{
			const int budget = gHandBudget.load(std::memory_order_relaxed);
			if (budget == 0 || !a_caster || !RecordRoute()) {
				return;
			}
			auto* actor = a_caster->GetCasterAsActor();
			auto* light = a_caster->light ? a_caster->light->light.get() : nullptr;
			if (!actor || !light || actor->IsPlayerRef()) {
				return;
			}
			bool keep = false;
			if (budget == 1) {
				const auto* player = RE::PlayerCharacter::GetSingleton();
				const float nearby = gNearby.load(std::memory_order_relaxed);
				keep = player && actor->GetPosition().GetDistance(player->GetPosition()) <= nearby;
			} else if (budget == 2) {
				keep = actor->IsPlayerTeammate();
			}
			const auto       turn = LightKit::Turn();  // before gBudgetLock (LightKit.h's order)
			auto&            fade = light->GetLightRuntimeData().fade;
			std::scoped_lock lock{ gBudgetLock };
			if (!keep) {
				if (fade > 0.0f) {
					if (gPutOut.size() >= kMostPutOut) {
						gPutOut.clear();
					}
					gPutOut[light] = fade;  // what the game or RecordFlicker wrote this frame
				}
				fade = 0.0f;
			} else if (const auto it = gPutOut.find(light); it != gPutOut.end()) {
				if (fade == 0.0f) {
					fade = it->second;  // nobody wrote it since we put it out
				}
				gPutOut.erase(it);
			}
		}

		// Without Light Placer the hand light is the game's own casting light, which no row's condition reaches, so a sneaking
		// caster's is faded here, after every other pass wrote it this frame (SneakFade.cpp). The ground twin copies it
		// (GroundLights.cpp - setting the game light's own land flag afterwards was measured not enough).
		void ApplyRecordHand(RE::ActorMagicCaster* a_caster)
		{
			if (!a_caster || !RecordRoute()) {
				return;
			}
			auto* bs = a_caster->light.get();
			SneakDim(bs ? bs->light.get() : nullptr, a_caster->GetCasterAsActor());
		}

		// what a lit light of a config's copy needs, by the copy's editor ID. Written at data load and on the main thread.
		struct Lit
		{
			bool  flicker{ false };
			float cutoff{ 0.0f };  // 0: not an inverse-square light
		};
		Fade::StringMap<Lit> gLit;

		// the flickering lights lit now. The light is held, so its address is never another light's while it is listed.
		struct Flicker
		{
			RE::NiPointer<RE::NiPointLight> light;
			RE::FormID                      owner{ 0 };        // the reference it hangs on
			const RE::TESObjectCELL*        cell{ nullptr };   // where that reference was when the light was found
			float                           written{ -1.0f };  // what the light held after our last write
		};
		std::unordered_map<const RE::NiPointLight*, Flicker> gFlickers;
		// the same lights by owner and by cell, so a caster's or a cell's update scales only its own (it runs for every caster
		// every frame); a light joins them when it is listed, and they are remade only when lights leave the list
		std::unordered_map<RE::FormID, std::vector<Flicker*>>               gByOwner;
		std::unordered_map<const RE::TESObjectCELL*, std::vector<Flicker*>> gByCell;
		std::mutex                                                          gFlickerLock;

		// gFlickerLock is held by every caller. A map node never moves, so the pointers stay good until the entry goes.
		void Index(Flicker& a_flicker)
		{
			gByOwner[a_flicker.owner].push_back(&a_flicker);
			gByCell[a_flicker.cell].push_back(&a_flicker);
		}

		void Reindex()
		{
			gByOwner.clear();
			gByCell.clear();
			for (auto& flicker : gFlickers | std::views::values) {
				Index(flicker);
			}
		}

		// Light Placer names a light "LP_Light[<config>|<light record's editor ID>]...". Read with no allocation: a prefix
		// test turns away every other light, and the editor ID is looked up as a view (a cache by the light's address
		// would still need a lookup per light, and a check that the address is the same light)
		const Lit* LitOf(const RE::NiPointLight& a_light)
		{
			const std::string_view name = a_light.name.c_str();
			if (!name.starts_with("LP_Light[")) {
				return nullptr;
			}
			const auto bar = name.find('|');
			const auto end = bar == std::string_view::npos ? bar : name.find(']', bar);
			if (end == std::string_view::npos) {
				return nullptr;
			}
			const auto it = gLit.find(name.substr(bar + 1, end - bar - 1));
			return it == gLit.end() ? nullptr : &it->second;
		}

		const RE::TESObjectREFR* OwnerOf(const RE::NiAVObject* a_object)
		{
			for (; a_object; a_object = a_object->parent) {
				if (const auto* ref = a_object->GetUserData()) {
					return ref;
				}
			}
			return nullptr;
		}

		// gFlickerLock is held by every caller
		void Scale(Flicker& a_flicker)
		{
			auto& fade = a_flicker.light->GetLightRuntimeData().fade;
			if (fade != a_flicker.written) {
				fade *= gBrightness.load(std::memory_order_relaxed);  // this frame's own value from Light Placer
			}
			a_flicker.written = fade;
		}

		// every use is about Light Placer's own lights: none without it
		bool Scaling()
		{
			return gBrightness.load(std::memory_order_relaxed) != 1.0f && !RecordRoute();
		}

		// straight after Light Placer wrote the lights of one reference, or of one cell
		template <class Map, class Key>
		void ScaleIn(const Map& a_map, const Key& a_key)
		{
			const auto      turn = LightKit::Turn();
			std::lock_guard l{ gFlickerLock };
			if (const auto it = a_map.find(a_key); it != a_map.end()) {
				for (auto* flicker : it->second) {
					Scale(*flicker);
				}
			}
		}

		void ScaleOwned(const RE::TESObjectREFR* a_owner)
		{
			if (a_owner && Scaling()) {
				ScaleIn(gByOwner, a_owner->GetFormID());
			}
		}

		void ScaleCell(const RE::TESObjectCELL* a_cell)
		{
			if (a_cell && Scaling()) {
				ScaleIn(gByCell, a_cell);
			}
		}

		// an explosion's or a hazard's lights are found here as well, so even their first frame is at the slider's strength
		void ScaleUnder(RE::TESObjectREFR* a_owner)
		{
			auto* root = a_owner && Scaling() ? a_owner->Get3D() : nullptr;
			if (!root) {
				return;
			}
			const auto      turn = LightKit::Turn();
			std::lock_guard l{ gFlickerLock };
			RE::BSVisit::TraverseScenegraphLights(root, [&](RE::NiPointLight* a_light) {
				if (const auto* lit = a_light ? LitOf(*a_light) : nullptr; lit && lit->flicker) {
					auto [it, added] = gFlickers.try_emplace(a_light);
					if (added) {
						it->second = { RE::NiPointer<RE::NiPointLight>(a_light), a_owner->GetFormID(), a_owner->GetParentCell() };
						Index(it->second);
					}
					Scale(it->second);
				}
				return RE::BSVisit::BSVisitControl::kContinue;
			});
		}

		// the main thread's look at the scene's lights: new flickering lights are listed; on the frame a slider changed
		// (Brightness by a_ratio) every lit light takes its copy's cutoff and a steady light's fade moves with it - those
		// go into a_moved, for the fading module to hear about once gFlickerLock is let go
		template <class List>
		void VisitList(const List& a_list, bool a_changed, float a_ratio, std::vector<std::pair<RE::NiPointLight*, float>>& a_moved)
		{
			for (const auto& bsLight : a_list) {
				auto*      light = bsLight ? netimmerse_cast<RE::NiPointLight*>(bsLight->light.get()) : nullptr;
				const Lit* lit = light ? LitOf(*light) : nullptr;
				if (!lit) {
					continue;
				}
				auto& data = light->GetLightRuntimeData();
				if (a_changed && lit->cutoff > 0.0f) {
					LightKit::Isl::SetCutoff(light, lit->cutoff, kLowestCutoff, kHighestCutoff);  // where Light Placer put it
				}
				if (!lit->flicker) {
					// a light both lit and queued is scaled once (only on the frame a slider moved - a short list)
					if (a_changed && std::ranges::find(a_moved, light, &std::pair<RE::NiPointLight*, float>::first) == a_moved.end()) {
						data.fade *= a_ratio;
						a_moved.emplace_back(light, data.fade);
					}
				} else if (auto [it, added] = gFlickers.try_emplace(light); added) {
					const auto* owner = OwnerOf(light);
					it->second = { RE::NiPointer<RE::NiPointLight>(light), owner ? owner->GetFormID() : 0, owner ? owner->GetParentCell() : nullptr };
					Index(it->second);
				}
			}
		}

		// THE DYNAMIC LIGHTING SWITCH AT ONCE - his report 2026-10-17: "it also takes an entire second to switch from dynamic to
		// static". Light Placer tests a light's conditions once a second (its PlacedLights.cpp, UpdateTimer(a_delta, 1.0f)), so the
		// switch showed up to a second late. On the frame the setting moves, each Dynamic Lighting light in the scene and its
		// steady twin - the light of the same config hanging at the same place, the k-th of each kind there by Light Placer's own
		// numbering (ConfigText.h) - are shown / hidden here the way Light Placer's next test will (SwapTwins); only what is drawn:
		// Light Placer's own verdict (the ambient red's top byte) is never written. Light Placer rewrites a light only when its
		// verdict changes, so a light shown here whose other conditions turned false first (she sneaked) would stay shown; after
		// kHandBack seconds of game time (and once more a second later) every light touched here is given Light Placer's own
		// verdict back (HandBack) - a no-op where its test agreed. A place whose two kinds do not come in equal numbers, or a pair hidden for another reason, is left
		// to Light Placer's test.
		constexpr std::string_view                   kDynamicLighting = "IlluminatedDynamicLighting";
		constexpr float                              kHandBack = 1.5f;  // Light Placer tests each light once a second of its own update time
		constexpr float                              kHandBackAgain = 1.0f;
		int                                          gDynamicSeen = -1;  // the value the scene's lights were last swapped for; main thread only
		std::vector<RE::NiPointer<RE::NiPointLight>> gSwapped;           // shown or hidden ahead of Light Placer, held until handed back
		float                                        gSwapClock = 0.0f;

		[[nodiscard]] std::uint32_t HideBits(RE::NiPointLight& a_light)
		{
			return std::bit_cast<std::uint32_t>(a_light.GetLightRuntimeData().ambient.red);
		}

		// every light touched by the last swap drawn as Light Placer holds it (hidden while any of its hide bits is set). Light
		// Placer culls a light on its worker threads (bits, then the drawn state) while this runs on the main thread, so one
		// read here can meet the bits from just before its write: the lights are handed back a second time a second later
		// (kHandBackAgain), and only then let go
		void HandBack(bool a_letGo)
		{
			for (const auto& light : gSwapped) {
				if (const bool hidden = (HideBits(*light) & ConfigText::kHidden) != 0; light->GetAppCulled() != hidden) {
					light->SetAppCulled(hidden);
				}
			}
			if (a_letGo) {
				gSwapped.clear();
			}
		}

		// the player's update, on the main thread: the game's own time, so a paused game (a menu) waits too
		void TickSwapped(float a_delta)
		{
			if (gSwapped.empty()) {
				return;
			}
			const float before = gSwapClock;
			gSwapClock += a_delta;
			if (before < kHandBack && gSwapClock >= kHandBack) {
				HandBack(false);
			} else if (gSwapClock >= kHandBack + kHandBackAgain) {
				HandBack(true);
			}
		}

		// where a light hangs, the same numbers for both twins: a light Light Placer counts as static (no position / rotation
		// controller, no offset - a Dynamic Lighting row has only color / fade controllers) at a zero point hangs straight on the
		// model's node; any other gets a node of its own at the row's point (point + offset - the same floats for both twins) under
		// the model's node (its LightData.cpp GetOrCreateNode)
		[[nodiscard]] std::string PlaceOf(const RE::NiPointLight& a_light)
		{
			const auto* parent = a_light.parent;
			const bool  made = std::string_view(parent->name.c_str()).starts_with("LP_Node[");
			const auto  at = made ? parent->local.translate : RE::NiPoint3{};
			return std::format("{}|{}|{}|{}", static_cast<const void*>(made ? parent->parent : parent), std::bit_cast<std::uint32_t>(at.x),
				std::bit_cast<std::uint32_t>(at.y), std::bit_cast<std::uint32_t>(at.z));
		}

		// on the main thread, on the frame a setting changed
		void SwapDynamicTwins()
		{
			const int now = SettingValue(kDynamicLighting, 1);
			const int before = std::exchange(gDynamicSeen, now);
			auto*     scene = LightKit::Scene();
			if (before < 0 || before == now || RecordRoute() || !scene) {
				return;
			}
			HandBack(true);                                                             // a switch back within the window: the last one's lights as Light Placer holds them first
			using Numbered = std::vector<std::pair<std::uint32_t, RE::NiPointLight*>>;  // Light Placer's number, the light
			struct Place
			{
				Numbered dynamic;
				Numbered steady;
			};
			std::unordered_map<std::string, Place> places;  // by config and place
			std::unordered_set<RE::NiPointLight*>  seen;    // a light both lit and queued is counted once
			std::size_t                            left = 0;
			const auto                             look = [&](const auto& a_list) {
				for (const auto& bsLight : a_list) {
					auto*      light = bsLight ? netimmerse_cast<RE::NiPointLight*>(bsLight->light.get()) : nullptr;
					const auto name = light && light->parent ? ConfigText::LightPlacerKey(light->name.c_str()) : std::string_view{};
					if (name.empty() || !seen.insert(light).second) {
						continue;
					}
					const auto key = ConfigText::TwinKey(name);
					const auto twin = DynamicTwinOf(key);
					const auto number = ConfigText::LightPlacerIndex(light->name.c_str());
					if (twin == ConfigText::Twin::kNeither) {
						continue;
					}
					if (!number) {
						left += twin == ConfigText::Twin::kDynamic ? 1 : 0;
						continue;
					}
					auto& p = places[std::string(std::string_view(key).substr(0, key.find('|'))) + '|' + PlaceOf(*light)];
					(twin == ConfigText::Twin::kDynamic ? p.dynamic : p.steady).emplace_back(*number, light);
				}
			};
			const auto& lists = scene->GetRuntimeData();
			look(lists.activeLights);
			look(QueuedLights(scene));
			const bool  on = now != 0;
			std::size_t swapped = 0;
			const auto  draw = [](RE::NiPointLight& a_light, std::uint32_t a_bits) {
				const bool hidden = (a_bits & ConfigText::kHidden) != 0;
				if (a_light.GetAppCulled() != hidden) {
					a_light.SetAppCulled(hidden);
					gSwapped.emplace_back(&a_light);
				}
			};
			for (auto& p : places | std::views::values) {
				if (p.dynamic.size() != p.steady.size()) {
					left += p.dynamic.size();
					continue;
				}
				std::ranges::sort(p.dynamic);
				std::ranges::sort(p.steady);
				for (std::size_t k = 0; k < p.dynamic.size(); ++k) {
					auto&      dynamic = *p.dynamic[k].second;
					auto&      steady = *p.steady[k].second;
					const auto next = ConfigText::SwapTwins({ HideBits(dynamic), HideBits(steady) }, on);
					if (!next) {
						++left;
						continue;
					}
					draw(dynamic, next->dynamic);
					draw(steady, next->steady);
					++swapped;
				}
			}
			gSwapClock = 0.0f;
			SKSE::log::info(
				"[DYNAMIC-SWITCH] Dynamic Lighting {}: {} light pair(s) at once ({} light(s) drawn ahead of Light Placer), {} "
				"dynamic light(s) left to its next test",
				on ? "on" : "off", swapped, gSwapped.size(), left);
		}

		// once a frame, on the main thread
		void Frame()
		{
			const bool  settings = gRefresh.exchange(false);
			const bool  changed = gDaylightMoved.exchange(false) || settings;
			const float before = gBrightness;
			if (settings) {
				RefreshLights();
				ApplyWardLook();     // the Ward look pick: the next ward raised wears it
				SwapDynamicTwins();  // the Dynamic Lighting switch shows now, not at Light Placer's next test
			} else if (changed) {
				ApplyLightStrength(false, true);  // the daylight moved: the copies at the new Brightness x daylight, no log line
			}
			StreamLightsFrame();
			auto* scene = LightKit::Scene();
			if (!scene || (!changed && !Scaling())) {
				return;
			}
			const auto&                                      lights = scene->GetRuntimeData();
			const float                                      ratio = before > 0.0f ? gBrightness / before : 1.0f;
			std::vector<std::pair<RE::NiPointLight*, float>> moved;
			// copied under the game's queue lock BEFORE the turn: inside a turn nothing waits on a game lock (LightKit.h)
			const auto queued = QueuedLights(scene);
			{
				const auto      turn = LightKit::Turn();  // let go at the brace, before Fade::Rebase (the fading lock comes before the turn)
				std::lock_guard l{ gFlickerLock };
				VisitList(lights.activeLights, changed, ratio, moved);
				VisitList(queued, changed, ratio, moved);  // added before the next draw
				if (!Scaling()) {
					gFlickers.clear();  // Light Placer's own values stand from here on
					Reindex();
				} else if (std::erase_if(gFlickers, [](const auto& a_kv) { return a_kv.second.light->GetRefCount() <= 1; }) > 0) {  // only we hold it: gone
					Reindex();                                                                                                      // the gone entries' pointers leave the two maps
				}
			}
			for (const auto& [light, now] : moved) {
				Fade::Rebase(light, ratio, now);  // the scene's lists hold each light, so it is alive this frame
			}
		}

		// ------------------------------------------------------------------ the hooks
		// All installed at data load, long after Light Placer installed its own at plugin load, so each wraps Light
		// Placer's: ours runs after it, on the values it just wrote. The fading module's work in the same places is
		// called from here, after Brightness (the header has why).
		struct PlayerUpdate
		{
			static void thunk(RE::PlayerCharacter* a_this, float a_delta)
			{
				LightKit::Guarded("sneak fade", []() { SneakFrameStart(); });  // before the game and every pass write this frame's fades
				func(a_this, a_delta);
				LightKit::Guarded("player update", [a_delta]() {
					AdvanceRecordFlicker(a_delta);
					WatchDaylight(a_delta);
					Frame();
					TickSwapped(a_delta);     // the Dynamic Lighting swap's lights back to Light Placer's own verdict
					TickArtLights();          // before the fading module, which finds and fades them on the weapons (ArtLights.cpp)
					TickAtronachLights();     // the HexRim add-on's atronachs (AtronachLights.cpp)
					KeepCastingLights();      // a held spell's Light Placer lights after a cell change (LightPlacerQueue.cpp)
					KeepBlockWatch(a_delta);  // the shield ward's block watch follows a rebuilt animation graph (Wards.cpp)
					if (!RecordRoute()) {
						NoteFleshRoots();  // for the flesh spells' Light Placer lights, recolored on the worker threads (FleshLights.cpp)
					}
					{
						const SneakPause pause;  // the fading module reads the fades the others left, never our dimmed one
						Fade::UpdateHands(a_delta);
					}
					SneakFrameEnd(a_delta);  // over every fade written above (SneakFade.cpp)
					TickGroundLights();      // last: the hand lights' numbers of this frame are written (GroundLights.cpp)
				});
			}
			static inline REL::Relocation<decltype(thunk)> func;
			static void                                    Install()
			{
				REL::Relocation<std::uintptr_t> vtbl{ RE::PlayerCharacter::VTABLE[0] };
				func = vtbl.write_vfunc(REL::Relocate(0xAD, 0xAD, 0xAF), thunk);  // Actor::Update
			}
		};

		struct CasterUpdate
		{
			static void thunk(RE::ActorMagicCaster* a_this, float a_delta)
			{
				func(a_this, a_delta);
				LightKit::Guarded("caster update", [a_this]() {
					{
						auto*                     actor = a_this->GetCasterAsActor();
						std::optional<SneakPause> pause;
						if (actor) {
							pause.emplace(actor);  // only this caster's lights
						}
						ScaleOwned(actor);
						RecordFlicker(a_this);  // without Light Placer: the hand light's flicker (RecordLights.cpp), before the fading module scales it
						ApplyHandBudget(a_this);
						Fade::AfterCaster(a_this);  // Lights by Item over Light Placer's animation of the casting lights
					}
					ApplyRecordHand(a_this);  // last: the sneak fade over everything written above
				});
			}
			static inline REL::Relocation<decltype(thunk)> func;
			static void                                    Install()
			{
				REL::Relocation<std::uintptr_t> vtbl{ RE::ActorMagicCaster::VTABLE[0] };
				func = vtbl.write_vfunc(REL::Relocate(0x1D, 0x1D, 0x1F), thunk);  // MagicCaster::Update
			}
		};

		template <class T>
		struct EffectUpdate
		{
			static void thunk(T* a_this)
			{
				func(a_this);
				LightKit::Guarded("effect update", [a_this]() {
					// the owner by the effect's own 3D, not its target handle: this hook runs off the main thread, where making or
					// reading a handle is not ours to do (the fading module's rule; the re-score's Illuminated issue 2)
					const auto*               owner = OwnerOf(a_this->Get3D());
					std::optional<SneakPause> pause;
					if (owner) {
						pause.emplace(owner);  // only the lights of the actor it is on; an effect on nothing pauses none
					}
					if (Scaling()) {
						ScaleOwned(owner);
					}
					if constexpr (std::is_same_v<T, RE::ShaderReferenceEffect>) {
						if (!RecordRoute()) {
							const auto      turn = LightKit::Turn();  // before gFlickerLock (LightKit.h's order)
							std::lock_guard l{ gFlickerLock };        // the slider pass writes the same fade
							FleshLightPlacer(a_this);                 // a flesh spell under FleshFX: Light Placer's light just written takes its hue
						}
					}
					Fade::AfterReferenceEffect(a_this, std::is_same_v<T, RE::ModelReferenceEffect>);  // an art model, not a shader's actor
				});
			}
			static inline REL::Relocation<decltype(thunk)> func;
			static void                                    Install()
			{
				REL::Relocation<std::uintptr_t> vtbl{ T::VTABLE[0] };
				func = vtbl.write_vfunc(0x3B, thunk);  // ReferenceEffect::UpdatePosition
			}
		};

		// a 5-byte call inside a game function, wrapped where it stands; left alone if the call is not there
		template <class Thunk>
		void WrapCall(REL::Relocation<std::uintptr_t> a_site, std::string_view a_what, std::string_view a_lost)
		{
			if (!LightKit::IsCallAt(a_site.address())) {
				SKSE::log::warn("{}: the expected call is not there (another plugin rewrote it?); {}", a_what, a_lost);
				return;
			}
			Thunk::func = SKSE::GetTrampoline().write_call<5>(a_site.address(), Thunk::thunk);
		}

		// the explosions a magic effect sets off, itself or through its projectile: a creature's death or attack explosion is not one
		[[nodiscard]] bool SpellExplosion(const RE::BGSExplosion* a_expl)
		{
			static const auto spells = [] {
				std::unordered_set<const RE::BGSExplosion*> out;
				for (const auto* mgef : LightKit::FormsOf<RE::EffectSetting>()) {
					if (!mgef) {
						continue;
					}
					out.insert(mgef->data.explosion);
					if (const auto* proj = mgef->data.projectileBase) {
						out.insert(proj->data.explosionType);
					}
				}
				out.erase(nullptr);
				return out;
			}();
			return spells.contains(a_expl);
		}

		// the element of a form wearing the game's own art, Family::kNone for any other; atronach and wraith models are left
		// alone (their lights belong to the elementals' own mods)
		[[nodiscard]] Family ArtFamily(const RE::TESForm* a_form, const char* a_model, bool a_hazard = false)
		{
			if (!a_model || !*a_model) {
				return Family::kNone;
			}
			const auto model = NormalPath(a_model);
			const auto file = std::string_view(model).substr(model.find_last_of('\\') + 1);
			if (!VanillaArtModel(model, a_hazard) || file.contains("atronach") || file.contains("wraith")) {
				return Family::kNone;
			}
			// the game keeps no editor ID for an explosion or a hazard, so the model's file name tells the element when the ID is gone
			const auto family = FamilyOf(EditorID(a_form), true);
			return family != Family::kNone ? family : FamilyOf(file, true);
		}

		[[nodiscard]] Family ExplosionFamily(const RE::BGSExplosion* a_expl)
		{
			return SpellExplosion(a_expl) ? ArtFamily(a_expl, a_expl->GetModel()) : Family::kNone;
		}

		// every light under a_root in a_color; how many
		std::size_t PaintLightsUnder(RE::NiAVObject* a_root, const RE::Color& a_color)
		{
			const RE::NiColor diffuse{ a_color.red / 255.0f, a_color.green / 255.0f, a_color.blue / 255.0f };
			std::size_t       painted = 0;
			const auto        turn = LightKit::Turn();
			if (a_root) {
				RE::BSVisit::TraverseScenegraphLights(a_root, [&](RE::NiPointLight* a_light) {
					if (a_light) {
						a_light->GetLightRuntimeData().diffuse = diffuse;
						++painted;
					}
					return RE::BSVisit::BSVisitControl::kContinue;
				});
			}
			return painted;
		}

		// an explosion of the game's own art wears its element's hand color for the art pick, as the rest of its spell does
		void TintUnder(RE::Explosion* a_explosion)
		{
			auto*       root = a_explosion->Get3D();
			const auto* base = a_explosion->GetBaseObject();
			const auto* expl = base ? base->As<RE::BGSExplosion>() : nullptr;
			if (!root || !expl || LightsOff()) {  // all lights off: the explosion keeps the game's own color
				return;
			}
			static std::mutex                             lock;
			static std::unordered_map<RE::FormID, Family> families;
			Family                                        family = Family::kNone;
			{
				std::lock_guard l{ lock };
				auto [it, added] = families.try_emplace(expl->GetFormID(), Family::kNone);
				if (added) {
					it->second = ExplosionFamily(expl);
				}
				family = it->second;
			}
			const auto                  color = family == Family::kNone ? std::nullopt : ArtHandColor(family);
			[[maybe_unused]] const auto tinted = color ? PaintLightsUnder(root, *color) : 0;
#ifdef FADE_DEBUG_PAGE
			if (static std::atomic<int> told = 0; family != Family::kNone && told < 12 && ++told) {
				SKSE::log::info("[EXPLOSION-TINT] {} | model {} | family {} | color {} | {} light(s) tinted", Label(expl),
					expl->GetModel() ? expl->GetModel() : "", static_cast<int>(family),
					color ? std::format("{},{},{}", color->red, color->green, color->blue) : std::string("none"), tinted);
			}
#endif
		}

		// the element of a hazard a spell leaves, Family::kNone for a trap's or any other: the burning ground a spell explosion
		// places, the hazard a Spawn Hazard effect makes, and the wall pieces an effect's impact data places (Wall of Flames
		// and the other walls - MAGFlamesBarrierImpact -> FireBarrierHazard; no explosion names them)
		[[nodiscard]] Family HazardFamily(const RE::BGSHazard* a_hazard)
		{
			static const auto families = [] {
				std::unordered_map<const RE::BGSHazard*, Family> out;
				const auto                                       add = [&out](const RE::TESForm* a_form) {
					if (const auto* hazard = a_form ? a_form->As<RE::BGSHazard>() : nullptr) {
						out.try_emplace(hazard, ArtFamily(hazard, hazard->GetModel(), true));
					}
				};
				for (const auto* expl : LightKit::FormsOf<RE::BGSExplosion>()) {
					if (expl && SpellExplosion(expl)) {
						add(expl->data.impactPlacedObject);
					}
				}
				for (const auto* mgef : LightKit::FormsOf<RE::EffectSetting>()) {
					if (!mgef) {
						continue;
					}
					if (mgef->HasArchetype(RE::EffectSetting::Archetype::kSpawnHazard)) {
						add(mgef->data.associatedForm);
					}
					if (const auto* set = mgef->data.impactDataSet) {
						for (const auto& entry : set->impactMap) {
							add(entry.second ? entry.second->hazard : nullptr);
						}
					}
				}
				return out;
			}();
			const auto it = families.find(a_hazard);
			return it == families.end() ? Family::kNone : it->second;
		}

		// a spell's hazard of the game's own art wears its element's hand color for the art pick, as its explosion does; every
		// update, so a pick changed while it burns reaches it
		void TintHazard(RE::Hazard* a_hazard)
		{
			const auto* base = a_hazard->GetBaseObject();
			const auto* form = base ? base->As<RE::BGSHazard>() : nullptr;
			const auto  family = form ? HazardFamily(form) : Family::kNone;
			if (family == Family::kNone) {
				return;
			}
			auto*      own = netimmerse_cast<RE::NiPointLight*>(a_hazard->GetHazardRuntimeData().light.get());
			const auto color = LightsOff() ? std::nullopt : ArtHandColor(family);
			if (!color) {
				// all lights off, or a pick with no color: its own light back in its record's color (a hazard lives long enough to keep a stale tint)
				if (form->data.light) {
					const auto& record = form->data.light->data.color;
					if (own) {
						const auto turn = LightKit::Turn();
						own->GetLightRuntimeData().diffuse = { record.red / 255.0f, record.green / 255.0f, record.blue / 255.0f };
					}
					PaintLightsUnder(a_hazard->Get3D(), record);
				}
				return;
			}
			if (own) {
				const auto turn = LightKit::Turn();
				own->GetLightRuntimeData().diffuse = { color->red / 255.0f, color->green / 255.0f, color->blue / 255.0f };
			}
			PaintLightsUnder(a_hazard->Get3D(), *color);
#ifdef FADE_DEBUG_PAGE
			if (static std::atomic<int> told = 0; told < 12 && ++told) {
				const char* model = form->GetModel();
				SKSE::log::info("[HAZARD-TINT] {} | model {} | family {} | color {},{},{}", Label(form), model ? model : "(none)", static_cast<int>(family),
					color->red, color->green, color->blue);
			}
#endif
		}

		// Without Light Placer an explosion's light is the game's own. On a model with no AttachLight node (the fire storms'
		// FireStormAreaEffect) the game hangs it on the model's root, level with the ground the explosion sits on, and it lit the
		// floor edge-on: listed with fade 0.8 and radius 732, the room unchanged (four storms, Vanilla). So it is raised above the root.
		constexpr float kExplosionLift = 128.0f;  // world units above the root

		void LiftExplosionLight(RE::Explosion* a_explosion)
		{
			if (!RecordRoute()) {
				return;
			}
			auto*       light = a_explosion->GetExplosionRuntimeData().light.get();
			const auto* root = a_explosion->Get3D();
			if (!light || !light->parent || !root) {
				return;
			}
			for (const auto* up = light->parent; up != root; up = up->parent) {
				if (!up || std::string_view(up->name.c_str()).starts_with("AttachLight")) {
					return;
				}
			}
			// the storms' root is scaled (about 3.4): the lift is in world units
			const float scale = light->parent->world.scale > 0.01f ? light->parent->world.scale : 1.0f;
			const float lift = kExplosionLift / scale;
			if (light->local.translate.z >= lift) {
				return;
			}
			light->local.translate.z = lift;
			light->world = light->parent->world * light->local;
		}

		struct ExplosionUpdate
		{
			static bool thunk(RE::Explosion* a_this)
			{
				const bool result = func(a_this);
				LightKit::Guarded("explosion update", [a_this]() {
					LiftExplosionLight(a_this);
					ScaleUnder(a_this);
					TintUnder(a_this);
				});
				return result;
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		// Without Light Placer a hazard's light is the game's own, made from the record light pass 2 gave it (EffectLights.cpp).
		// MEASURED 2026-10-17 (his report "a lot of people saying fire hazards are not working", a testing-DLL probe of the
		// hazard lights on a Vanilla copy): on a model with no AttachLight node (the burning-ground fires, Effects\FXFire01New.nif /
		// 02New, and the copies mods make of them) the game hangs the light on the model's root and never works out where it is -
		// 90 frames on it was in the scene's light list, under the fire, at the world's origin (0,0,0), lighting nothing; on the
		// fire walls (an AttachLight node) it sat on the wall. So each update the light is put where its parent is.
		void PlaceHazardLight(RE::Hazard* a_hazard)
		{
			if (!RecordRoute()) {
				return;
			}
			auto* light = a_hazard->GetHazardRuntimeData().light.get();
			auto* parent = light ? light->parent : nullptr;
			if (parent) {
				light->world = parent->world * light->local;
			}
		}

		struct HazardUpdate
		{
			static bool thunk(RE::Hazard* a_this)
			{
				const bool result = func(a_this);
				LightKit::Guarded("hazard update", [a_this]() {
					PlaceHazardLight(a_this);
					ScaleUnder(a_this);
					TintHazard(a_this);
				});
				return result;
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct CellAnimations
		{
			static void thunk(RE::TESObjectCELL* a_cell)
			{
				func(a_cell);
				LightKit::Guarded("cell animations", [a_cell]() {
					const SneakPause pause;
					ScaleCell(a_cell);
					Fade::ReapplyAll();  // this frame's fading numbers again for every hand's lights, without advancing any timer
				});
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		void InstallHooks()
		{
			PlayerUpdate::Install();
			CasterUpdate::Install();
			EffectUpdate<RE::ShaderReferenceEffect>::Install();
			EffectUpdate<RE::ModelReferenceEffect>::Install();
			if (REL::Module::IsVR()) {
				return;
			}
			// the trampoline is made once, in main.cpp's SKSE::Init (room for these three and the fading module's two)
			constexpr std::string_view kFlickerLost = "its flickering lights keep their made-with strength";
			WrapCall<ExplosionUpdate>(REL::Relocation<std::uintptr_t>{ RELOCATION_ID(42664, 43836), 0x11 }, "explosion update", kFlickerLost);
			WrapCall<HazardUpdate>(REL::Relocation<std::uintptr_t>{ RELOCATION_ID(42791, 43959), REL::VariantOffset(0x11, 0x1F, 0x11) }, "hazard update",
				"its flickering lights keep their made-with strength, and on the record route (Vanilla / ENB, or no Light Placer) a hazard "
				"light hung on its model's root (the burning-ground fires) stays where the game left it - lighting nothing");
			WrapCall<CellAnimations>(REL::Relocation<std::uintptr_t>{ RELOCATION_ID(18458, 18889), 0x52 }, "cell animations", kFlickerLost);
		}
	}

	void NoteSpellHazards()
	{
		(void)HazardFamily(nullptr);  // builds the table now, not in the first hazard's update
	}

	// the configs find their lights by editor ID in the game's table, which only powerofthree's Tweaks fills for lights: without
	// it (or with its Load EditorIDs off) the names our recorder kept go into that table once, as Tweaks would put them
	namespace
	{
		void ShareLightNames()
		{
			if (Fade::TweaksKeepsEditorIDs()) {
				return;
			}
			std::size_t shared = 0;
			if (auto* data = RE::TESDataHandler::GetSingleton()) {
				for (auto* form : data->GetFormArray(RE::FormType::Light)) {
					if (auto* light = form ? form->As<RE::TESObjectLIGH>() : nullptr) {
						if (const auto id = EditorID(light); !id.empty() && RegisterEditorID(light, id)) {
							++shared;
						}
					}
				}
			}
			SKSE::log::info("light names: {} given to the game's editor ID table (powerofthree's Tweaks keeps none)", shared);
		}
	}

	void DropHandBudget()
	{
		std::scoped_lock lock{ gBudgetLock };
		gPutOut.clear();
	}

	void MakeLightCopies()
	{
		ShareLightNames();
		std::size_t made = 0, flickers = 0, missing = 0, failed = 0, plained = 0;
		for (auto& c : LightCopies()) {
			if (c.form) {
				continue;  // one of pass 0's own magic lights, made already (LightSettings.cpp)
			}
			auto* base = RE::TESForm::LookupByEditorID<RE::TESObjectLIGH>(c.base);
			if (!base) {
				++missing;
				SKSE::log::warn("[LIGHTCOPY-MISSING] {} | its light {} is not in this load order; the lights that name it stay dark", c.id, c.base);
				continue;
			}
			auto* copy = CopyLight(base);
			if (!copy || !RegisterEditorID(copy, c.id) || RE::TESForm::LookupByEditorID<RE::TESObjectLIGH>(c.id) != copy) {
				++failed;
				SKSE::log::warn("[LIGHTCOPY-FAILED] {} | could not make a findable copy of {}", c.id, c.base);
				continue;
			}
			c.form = copy;
			c.startFade = c.fade > 0.0f ? c.fade : base->fade;
			c.startRadius = c.radius > 0 ? static_cast<std::uint32_t>(c.radius) : base->data.radius;
			// the config's own cutoff, or the record's when the record itself is an inverse-square light
			c.startCutoff = c.cutoff > 0.0f ? c.cutoff : (base->data.flags.underlying() & kInverseSquare) ? base->data.fallofExponent :
			                                                                                                0.0f;
			if (!InverseSquare() && c.startCutoff > 0.0f) {
				// ENB and Vanilla (Lighting.cpp): the reach it has under Community Shaders, drawn plain; Reach moves the radius
				const auto plain = LightKit::PlainOf(c.startFade, static_cast<float>(c.startRadius), c.startCutoff,
					LightKit::LightPlacerSize(base->data.fov));
				c.startFade = plain.fade;
				c.startRadius = static_cast<std::uint32_t>(std::lround(plain.radius));
				c.startCutoff = 0.0f;
				copy->data.flags = static_cast<RE::TES_LIGHT_FLAGS>(copy->data.flags.underlying() & ~kInverseSquare);
				++plained;
			}
			gLit[c.id] = { c.flicker, 0.0f };
			flickers += c.flicker;
			++made;
		}
		SKSE::log::info("light copies: {} made ({} flicker, {} drawn plain for {}), {} whose light is not in this load order, {} failed", made,
			flickers, plained, LightingName(LightingFound()), missing, failed);
		gDynamicSeen = SettingValue(kDynamicLighting, 1);  // the scene's lights start as the settings file has it
		InstallHooks();
		ApplyLightStrength(true);
	}

	void RequestRefresh()
	{
		gRefresh = true;
	}

	float BrightnessNow()
	{
		return gBrightness.load(std::memory_order_relaxed);
	}

	bool IsOurLightPlacerLight(const RE::NiPointLight& a_light)
	{
		return LitOf(a_light) != nullptr;
	}

	void NoteFadeWrite(const RE::NiPointLight* a_light, float a_before, float a_after)
	{
		std::lock_guard l{ gFlickerLock };
		// Only when the fading module started from OUR scaled value: then its write carries Brightness already and must
		// not be scaled again. Measured: noting every write froze two steady lights of a flickering copy at full
		// strength - on the frame Brightness changed the fading module wrote before this pass had ever scaled them, and
		// the note made them look done.
		if (const auto it = gFlickers.find(a_light); it != gFlickers.end() && it->second.written == a_before) {
			it->second.written = a_after;
		}
	}

	// The fading module reports each reach it writes, as it reports each fade (the shared FadeLights.cpp, from RELight - Spell
	// Addon's 2026-10-14 audit). RELight - Spell Addon's Reach slider rescales a light's reach every frame and needs the report
	// so the two never compound; here Reach moves the light records' radius once (ApplyLightStrength), never a light in the
	// scene, so there is nothing to note
	void NoteReachWrite(const RE::NiPointLight*, float, float) {}
	void NoteCutoffWrite(const RE::NiPointLight*, float, float) {}  // no pass here takes a cutoff it did not write for its base

	bool FlickerOn()
	{
		return SettingValue(kDynamicLighting, 1) != 0;
	}

	void ApplyLightStrength(bool a_log, bool a_quiet)
	{
		const int brightness = SettingValue(kBrightness, 100);
		const int reach = SettingValue(kReach, 100);
		// Performance (his order 2026-10-20): hand lights stay with the player and followers at most
		gHandBudget = PerformanceMode() ? (std::max)(SettingValue(kHandLights, 0), 2) : SettingValue(kHandLights, 0);
		// read now too, so a change of the setting shows at once (run 8: it waited for WatchDaylight's next look); at data
		// load the player is in no cell yet, so 1
		gDaylight = DaylightFactor();
		const float daylight = gDaylight.load();
		// his checklist 384 ("go with the proposed one"): Vanilla's lights a third brighter, under the slider
		const float vanilla = LightingFound() == Lighting::kVanilla ? gVanillaStrength.load(std::memory_order_relaxed) : 1.0f;
		const float scale = static_cast<float>(brightness) / 100.0f * daylight * vanilla;
		const float stretch = (std::max)(static_cast<float>(reach) / 100.0f, 0.01f);
		gBrightness = scale;
		const bool off = LightsOff();
		// the look is put back only when "all lights off" turns: written on every daylight step it undid HoldFlicker's steady
		// flags (the audit's M3)
		static bool wasOff = false;
		const bool  turned = off != wasOff;
		wasOff = off;
		std::size_t set = 0;
		for (auto& c : LightCopies()) {
			if (!c.form) {
				continue;
			}
			if (c.original) {
				// pass 0's copy of a game magic light: with every Illuminated light off it wears the game's own look
				if (turned) {
					c.form->data = off ? c.original->data : c.madeData;
				}
				if (off) {
					c.form->fade = c.original->fade;
					continue;
				}
			}
			c.form->fade = c.flicker ? c.startFade : c.startFade * scale;  // a flicker's Brightness is applied as it is written
			c.form->data.radius = static_cast<std::uint32_t>(std::lround(static_cast<double>(c.startRadius) * reach / 100.0));
			if (c.startCutoff > 0.0f) {
				const float cutoff = std::clamp(c.startCutoff * LightKit::ReachCutoffFactor(scale, stretch, c.startFade, c.startCutoff, LightKit::LightPlacerSize(c.form->data.fov)), kLowestCutoff, kHighestCutoff);
				c.form->data.fallofExponent = cutoff;
				if (const auto it = gLit.find(c.id); it != gLit.end()) {
					it->second.cutoff = cutoff;
				}
			}
			++set;
		}
		if (!a_quiet) {
			SKSE::log::info("light strength: brightness {}% x daylight {:.2f}, reach {}%, hand lights for {}, on {} light copies", brightness, daylight,
				reach, gHandBudget.load(), set);
		}
		if (a_log) {
			for (const auto& c : LightCopies()) {
				if (c.form) {
					SKSE::log::info("[LIGHTCOPY] {} | {}{} | fade {} radius {} cutoff {}", c.id, c.base, c.flicker ? " | flicker" : "", c.form->fade,
						c.form->data.radius, c.startCutoff > 0.0f ? c.form->data.fallofExponent : 0.0f);
				}
			}
		}
	}
}
