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
//               brighter light reached further). The cutoff is the made-with one x Brightness / Reach², which moves the
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
// The one exception: the hazard's update also places a hazard's own light on the record route (Vanilla / ENB, or no Light
// Placer - RecordLights.cpp DecideRecordRoute; PlaceHazardLight) - the
// game never places a light hung on a model's root (measured, the burning-ground fires); in VR that wrap is not made, so
// there those fires keep the game's unplaced light.
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
			if (!keep) {
				light->GetLightRuntimeData().fade = 0.0f;
			}
		}

		// Without Light Placer the hand light is the game's own casting light, and Light Placer's sneak condition has no twin on
		// it (his report: "lights still dont go off while sneaking in the new version"), so the Sneaking setting is
		// applied here: 0 (the default) puts a sneaking caster's hand light out after the game wrote it this frame, 1 keeps it.
		// (The ground: GroundLights.cpp - setting the game light's own land flag afterwards was measured not enough.)
		constexpr std::string_view kSneaking = "IlluminatedSneaking";

		void ApplyRecordHand(RE::ActorMagicCaster* a_caster)
		{
			if (!a_caster || !RecordRoute()) {
				return;
			}
			auto* bs = a_caster->light.get();
			auto* light = bs ? bs->light.get() : nullptr;
			auto* actor = a_caster->GetCasterAsActor();
			if (light && actor && actor->IsSneaking() && SettingValue(kSneaking, 0) == 0) {
				light->GetLightRuntimeData().fade = 0.0f;
			}
		}

		// what a lit light of a config's copy needs, by the copy's editor ID. Written at data load and on the main thread.
		struct Lit
		{
			bool  flicker{ false };
			float cutoff{ 0.0f };  // 0: not an inverse-square light
		};
		struct NameHash
		{
			using is_transparent = void;
			std::size_t operator()(std::string_view a_name) const noexcept { return std::hash<std::string_view>{}(a_name); }
		};
		std::unordered_map<std::string, Lit, NameHash, std::equal_to<>> gLit;

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
		// every frame); remade with the list each frame, a light added between frames joins at once
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
		void ScaleOwned(const RE::TESObjectREFR* a_owner)
		{
			if (!a_owner || !Scaling()) {
				return;
			}
			std::lock_guard l{ gFlickerLock };
			if (const auto it = gByOwner.find(a_owner->GetFormID()); it != gByOwner.end()) {
				for (auto* flicker : it->second) {
					Scale(*flicker);
				}
			}
		}

		void ScaleCell(const RE::TESObjectCELL* a_cell)
		{
			if (!a_cell || !Scaling()) {
				return;
			}
			std::lock_guard l{ gFlickerLock };
			if (const auto it = gByCell.find(a_cell); it != gByCell.end()) {
				for (auto* flicker : it->second) {
					Scale(*flicker);
				}
			}
		}

		// an explosion's or a hazard's lights are found here as well, so even their first frame is at the slider's strength
		void ScaleUnder(RE::TESObjectREFR* a_owner)
		{
			auto* root = a_owner && Scaling() ? a_owner->Get3D() : nullptr;
			if (!root) {
				return;
			}
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
					if (a_changed) {
						data.fade *= a_ratio;
						a_moved.emplace_back(light, data.fade);
					}
				} else if (auto [it, added] = gFlickers.try_emplace(light); added) {
					const auto* owner = OwnerOf(light);
					it->second = { RE::NiPointer<RE::NiPointLight>(light), owner ? owner->GetFormID() : 0, owner ? owner->GetParentCell() : nullptr };
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
			auto*     scene = RE::BSShaderManager::State::GetSingleton().shadowSceneNode[0];
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
			look(lists.lightQueueAdd);
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
			auto* scene = RE::BSShaderManager::State::GetSingleton().shadowSceneNode[0];
			if (!scene || (!changed && !Scaling())) {
				return;
			}
			const auto&                                      lights = scene->GetRuntimeData();
			const float                                      ratio = gBrightness / before;
			std::vector<std::pair<RE::NiPointLight*, float>> moved;
			{
				std::lock_guard l{ gFlickerLock };
				VisitList(lights.activeLights, changed, ratio, moved);
				VisitList(lights.lightQueueAdd, changed, ratio, moved);  // added before the next draw
				if (Scaling()) {
					std::erase_if(gFlickers, [](const auto& a_kv) { return a_kv.second.light->GetRefCount() <= 1; });  // only we hold it: gone
				} else {
					gFlickers.clear();  // Light Placer's own values stand from here on
				}
				Reindex();
			}
			for (const auto& [light, now] : moved) {
				Fade::Rebase(light, ratio, now);  // the scene's lists hold each light, so it is alive this frame
			}
		}

		// ------------------------------------------------------------------ the hooks
		// All installed at data load, long after Light Placer installed its own at plugin load, so each wraps Light
		// Placer's: ours runs after it, on the values it just wrote. The fading module's work in the same places is
		// called from here, after Brightness (the header has why).
#ifdef FADE_DEBUG_PAGE
		void CheckHazardLights();  // the testing DLL's hazard probe, below
#endif
		struct PlayerUpdate
		{
			static void thunk(RE::PlayerCharacter* a_this, float a_delta)
			{
				func(a_this, a_delta);
				LightKit::Guarded("player update", [a_delta]() {
					AdvanceRecordFlicker(a_delta);
					WatchDaylight(a_delta);
					Frame();
					TickSwapped(a_delta);     // the Dynamic Lighting swap's lights back to Light Placer's own verdict
					TickArtLights();          // before the fading module, which finds and fades them on the weapons (ArtLights.cpp)
					KeepBlockWatch(a_delta);  // the shield ward's block watch follows a rebuilt animation graph (Wards.cpp)
					if (!RecordRoute()) {
						NoteFleshRoots();  // for the flesh spells' Light Placer lights, recolored on the worker threads (FleshLights.cpp)
					}
					Fade::UpdateHands(a_delta);
					TickGroundLights();  // last: the hand lights' numbers of this frame are written (GroundLights.cpp)
#ifdef FADE_DEBUG_PAGE
					CheckHazardLights();
#endif
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
					ScaleOwned(a_this->GetCasterAsActor());
					RecordFlicker(a_this);  // without Light Placer: the hand light's flicker (RecordLights.cpp), before the fading module scales it
					ApplyHandBudget(a_this);
					ApplyRecordHand(a_this);
					Fade::AfterCaster(a_this);  // last: Lights by Item over Light Placer's animation of the casting lights
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
					if (Scaling()) {
						// the owner by the effect's own 3D, not its target handle: this hook runs off the main thread, where making or
						// reading a handle is not ours to do (the fading module's rule; the re-score's Illuminated issue 2)
						ScaleOwned(OwnerOf(a_this->Get3D()));
					}
					if constexpr (std::is_same_v<T, RE::ShaderReferenceEffect>) {
						if (!RecordRoute()) {
							FleshLightPlacer(a_this);  // a flesh spell under FleshFX: Light Placer's light just written takes its hue
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

		// the element of a spell explosion of the game's own art, Family::kNone for any other; atronach and wraith models are
		// left alone (their lights belong to the elementals' own mods)
		[[nodiscard]] Family ExplosionFamily(const RE::BGSExplosion* a_expl)
		{
			if (!a_expl->GetModel() || !SpellExplosion(a_expl)) {
				return Family::kNone;
			}
			const auto model = NormalPath(a_expl->GetModel());
			const auto file = std::string_view(model).substr(model.find_last_of('\\') + 1);
			if (!VanillaArtModel(model) || file.contains("atronach") || file.contains("wraith")) {
				return Family::kNone;
			}
			// the game keeps no editor ID for an explosion, so the model's file name tells the element when the ID is gone
			const auto family = FamilyOf(EditorID(a_expl), true);
			return family != Family::kNone ? family : FamilyOf(file, true);
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
			const auto  color = family == Family::kNone ? std::nullopt : ArtHandColor(family);
			std::size_t tinted = 0;
			if (color) {
				const RE::NiColor diffuse{ color->red / 255.0f, color->green / 255.0f, color->blue / 255.0f };
				RE::BSVisit::TraverseScenegraphLights(root, [&](RE::NiPointLight* a_light) {
					if (a_light) {
						a_light->GetLightRuntimeData().diffuse = diffuse;
						++tinted;
					}
					return RE::BSVisit::BSVisitControl::kContinue;
				});
			}
			if (static std::atomic<int> told = 0; family != Family::kNone && told < 12 && ++told) {
				SKSE::log::info("[EXPLOSION-TINT] {} | model {} | family {} | color {} | {} light(s) tinted", Label(expl),
					expl->GetModel() ? expl->GetModel() : "", static_cast<int>(family),
					color ? std::format("{},{},{}", color->red, color->green, color->blue) : std::string("none"), tinted);
			}
		}

		// the hazards a spell explosion leaves (Flames' burning ground ...): an explosion's placed object
		[[nodiscard]] bool SpellHazard(const RE::BGSHazard* a_hazard)
		{
			static const auto hazards = [] {
				std::unordered_set<const RE::BGSHazard*> out;
				for (const auto* expl : LightKit::FormsOf<RE::BGSExplosion>()) {
					const auto* placed = expl ? reinterpret_cast<const RE::TESForm*>(expl->data.impactPlacedObject) : nullptr;
					if (placed && SpellExplosion(expl)) {
						out.insert(placed->As<RE::BGSHazard>());
					}
				}
				out.erase(nullptr);
				return out;
			}();
			return hazards.contains(a_hazard);
		}

		// a spell's hazard of the game's own art wears its element's hand color for the art pick, as its explosion does
		void TintHazard(RE::Hazard* a_hazard)
		{
			const auto* base = a_hazard->GetBaseObject();
			const auto* form = base ? base->As<RE::BGSHazard>() : nullptr;
			auto*       root = a_hazard->Get3D();
			if (!form || LightsOff()) {
				return;
			}
			static std::mutex                             lock;
			static std::unordered_map<RE::FormID, Family> families;
			Family                                        family = Family::kNone;
			{
				std::lock_guard l{ lock };
				auto [it, added] = families.try_emplace(form->GetFormID(), Family::kNone);
				if (added && form->GetModel() && SpellHazard(form)) {
					const auto model = NormalPath(form->GetModel());
					const auto file = std::string_view(model).substr(model.find_last_of('\\') + 1);
					if (VanillaArtModel(model)) {
						const auto byId = FamilyOf(EditorID(form), true);
						it->second = byId != Family::kNone ? byId : FamilyOf(file, true);
					}
				}
				family = it->second;
			}
			const auto color = family == Family::kNone ? std::nullopt : ArtHandColor(family);
			if (!color) {
				return;
			}
			const RE::NiColor diffuse{ color->red / 255.0f, color->green / 255.0f, color->blue / 255.0f };
			if (auto* own = netimmerse_cast<RE::NiPointLight*>(a_hazard->GetHazardRuntimeData().light.get())) {
				own->GetLightRuntimeData().diffuse = diffuse;
			}
			if (root) {
				RE::BSVisit::TraverseScenegraphLights(root, [&](RE::NiPointLight* a_light) {
					if (a_light) {
						a_light->GetLightRuntimeData().diffuse = diffuse;
					}
					return RE::BSVisit::BSVisitControl::kContinue;
				});
			}
			if (static std::atomic<int> told = 0; told < 12 && ++told) {
				SKSE::log::info("[HAZARD-TINT] {} | model {} | family {} | color {},{},{}", Label(form), form->GetModel(), static_cast<int>(family),
					color->red, color->green, color->blue);
			}
		}

		struct ExplosionUpdate
		{
			static bool thunk(RE::Explosion* a_this)
			{
				const bool result = func(a_this);
				LightKit::Guarded("explosion update", [a_this]() {
					ScaleUnder(a_this);
					TintUnder(a_this);
				});
				return result;
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

#ifdef FADE_DEBUG_PAGE
		// TESTING DLL ONLY (2026-10-17, his "fire hazards are not working" sweep): once per hazard reference, the light record
		// its hazard form carries and the light the game made from it (or that it made none) - so a bug report's log names
		// which hazard went dark and why
		// and, once the light is made, a second look 90 frames later on the main thread (CheckHazardLights): is the light in the
		// scene's light lists (what the renderer draws), does its parent chain reach the hazard's own 3D, where it sits - so the
		// "made but lights nothing" case (the burning-ground fires) shows which of those fails
		struct HazardWatch
		{
			RE::NiPointer<RE::NiLight> light;
			std::uint32_t              ref{ 0 };
			std::string                model;
			int                        frames{ 0 };
		};
		std::mutex               gHazardWatchLock;
		std::vector<HazardWatch> gHazardWatch;

		void LogHazardLight(RE::Hazard* a_hazard)
		{
			static std::mutex                        lock;
			static std::unordered_set<std::uint32_t> seen, seenNone;
			const auto*                              base = a_hazard->GetBaseObject();
			const auto*                              form = base ? base->As<RE::BGSHazard>() : nullptr;
			auto*                                    made = a_hazard->GetHazardRuntimeData().light.get();
			{
				std::lock_guard l{ lock };
				if (!form || seen.size() + seenNone.size() > 4000 || seen.contains(a_hazard->GetFormID())) {
					return;
				}
				if (made) {
					seen.insert(a_hazard->GetFormID());
				} else if (!seenNone.insert(a_hazard->GetFormID()).second) {
					return;  // "none made" is logged once; the made light, when it comes, once more
				}
			}
			const auto* record = form->data.light;
			auto*       point = made ? netimmerse_cast<RE::NiPointLight*>(made) : nullptr;
			if (made) {
				std::lock_guard l{ gHazardWatchLock };
				gHazardWatch.push_back({ RE::NiPointer<RE::NiLight>(made), a_hazard->GetFormID(), form->GetModel() ? form->GetModel() : "" });
			}
			std::string got = "NO LIGHT MADE";
			if (point) {
				const auto& d = point->GetLightRuntimeData();
				got = std::format("made: radius {:.0f} fade {:.2f} rgb {:.2f},{:.2f},{:.2f} parent '{}' culled {}", d.radius.x, d.fade,
					d.diffuse.red, d.diffuse.green, d.diffuse.blue, point->parent ? point->parent->name.c_str() : "(none)",
					point->GetAppCulled());
			}
			SKSE::log::info("[HAZARD-LIGHT] {:08X} (form {:08X}) model '{}' | record {:08X} rgb {},{},{} radius {} fade {:.2f} flags {:#x} | {}",
				a_hazard->GetFormID(), form->GetFormID(), form->GetModel() ? form->GetModel() : "", record ? record->GetFormID() : 0u,
				record ? record->data.color.red : 0, record ? record->data.color.green : 0, record ? record->data.color.blue : 0,
				record ? record->data.radius : 0u, record ? record->fade : 0.0f, record ? record->data.flags.underlying() : 0u, got);
		}

		// on the main thread, once a frame
		void CheckHazardLights()
		{
			std::vector<HazardWatch> due;
			{
				std::lock_guard l{ gHazardWatchLock };
				for (auto it = gHazardWatch.begin(); it != gHazardWatch.end();) {
					if (++it->frames >= 90) {
						due.push_back(std::move(*it));
						it = gHazardWatch.erase(it);
					} else {
						++it;
					}
				}
			}
			if (due.empty()) {
				return;
			}
			auto* scene = RE::BSShaderManager::State::GetSingleton().shadowSceneNode[0];
			for (const auto& w : due) {
				auto* light = w.light.get();
				bool  active = false, queued = false;
				if (scene) {
					const auto& rt = scene->GetRuntimeData();
					for (const auto& bs : rt.activeLights) {
						active = active || (bs && bs->light.get() == light);
					}
					for (const auto& bs : rt.lightQueueAdd) {
						queued = queued || (bs && bs->light.get() == light);
					}
				}
				std::string chain;
				bool        reaches = false, hidden = false;
				auto*       ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(w.ref);
				auto*       root = ref ? ref->Get3D() : nullptr;
				for (RE::NiAVObject* o = light; o; o = o->parent) {
					chain += std::format("{}{}{} > ", o->name.c_str(), o->GetAppCulled() ? "(culled)" : "", o->GetUserData() ? "(ref)" : "");
					hidden = hidden || o->GetAppCulled();
					reaches = reaches || o == root;
				}
				const auto& d = light->GetLightRuntimeData();
				const auto& p = light->world.translate;
				const auto  rp = ref ? ref->GetPosition() : RE::NiPoint3{};
				SKSE::log::info(
					"[HAZARD-LIGHT-90] {:08X} '{}' | refcount {} | in scene: active {} queued {} | reaches the ref's 3D {} | culled on the "
					"way {} | fade {:.2f} radius {:.0f} | light at {:.0f},{:.0f},{:.0f} ref at {:.0f},{:.0f},{:.0f} | {}",
					w.ref, w.model, light->GetRefCount(), active, queued, reaches, hidden, d.fade, d.radius.x, p.x, p.y, p.z, rp.x, rp.y, rp.z,
					chain);
			}
		}
#endif

		// Without Light Placer a hazard's light is the game's own, made from the record light pass 2 gave it (EffectLights.cpp).
		// MEASURED 2026-10-17 (his report "a lot of people saying fire hazards are not working", the testing DLL's [HAZARD-LIGHT-90]
		// lines on a Vanilla copy): on a model with no AttachLight node (the burning-ground fires, Effects\FXFire01New.nif /
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
#ifdef FADE_DEBUG_PAGE
					LogHazardLight(a_this);
#endif
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

	void MakeLightCopies()
	{
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
		const float scale = static_cast<float>(brightness) / 100.0f * daylight;
		const float stretch = static_cast<float>(reach) / 100.0f;
		gBrightness = scale;
		const bool  off = LightsOff();
		std::size_t set = 0;
		for (auto& c : LightCopies()) {
			if (!c.form) {
				continue;
			}
			if (c.original) {
				// pass 0's copy of a game magic light: with every Illuminated light off it wears the game's own look
				c.form->data = off ? c.original->data : c.madeData;
				if (off) {
					c.form->fade = c.original->fade;
					continue;
				}
			}
			c.form->fade = c.flicker ? c.startFade : c.startFade * scale;  // a flicker's Brightness is applied as it is written
			c.form->data.radius = static_cast<std::uint32_t>(std::lround(static_cast<double>(c.startRadius) * reach / 100.0));
			if (c.startCutoff > 0.0f) {
				c.form->data.fallofExponent = c.startCutoff * scale / (stretch * stretch);
				if (const auto it = gLit.find(c.id); it != gLit.end()) {
					it->second.cutoff = std::clamp(c.form->data.fallofExponent, kLowestCutoff, kHighestCutoff);
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
