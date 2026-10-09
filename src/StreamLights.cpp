// Illuminated - SKSE plugin
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE.txt and the notice at the top of main.cpp.
//
// Pass 6: lights that travel with a spray or a bolt.
//
// A spray or a shock stream is one projectile whose light record sits at the caster's hand, so the game lights the
// hand and not the stream. Light Placer does not light these at all (it attaches to a model, and these have none it
// can hold). ReLight lights them by giving the projectile's own 3D a light of its own.
//
// CREDIT: how a light is made and registered here follows ReLight by Truman (github.com/TrumanGIT/ReLight),
// GPL-3.0-or-later, with his permission and kept under the same license. From it: one master NiPointLight made
// once and cloned for every use (a freshly made light attached straight away crashes), the light's size carried
// in the radius' z, the create parameters a non-shadow light needs (field of view 90, portal-strict, never
// fades), and handing the light to the shadow scene node, which is what renders it. Which projectiles get lights,
// where they sit along the stream, and how the settings drive them is ours.
//
// Four rules, each one invisible in the code that follows it:
//  1. `RE::FlameProjectile` is hooked here because nothing else hooks it (not Light Placer, not RE::Light).
//  2. A flame spray's object sits at the caster's hand and its art streams out of it, so it gets ONE light, a little way
//     out along the spray, that reaches from there to the spray's end (the numbers RE::Light's sprays were tuned to).
//  3. A beam's light is parented to its `BeamEnd` node, so it rides the tip (the root when the mesh has none).
//  4. Community Shaders' inverse square flag and cutoff are written after `SetLightAttenuation`, with
//     cutoff = K * fade / (radius² + size²), so the reach comes out at the radius asked for.
//     On ENB and Vanilla (Lighting.cpp) nothing is written there; the light is drawn plain, the reach asked for as
//     the game's own lighting draws it, with a tenth of its colour as ambient.
//  5. The scene is only ever touched on the main thread: 3D built or released on one of the game's loader threads is
//     queued, and lit or dropped at the next frame (StreamLightsFrame).
// Live lights are capped per projectile base and swept when their parent is gone, or a held spray piles them up.
//
// How it works. When the game builds a cone, beam, flame or barrier projectile's 3D, this hooks the call and
// hangs NiPointLights off that 3D - one for a flame spray or a beam, up to three at fixed steps along the
// forward axis for a cone - then hands each one to the shadow scene node, which is what actually renders it.
// The lights are children of the projectile's own node, so they travel and turn with the stream and disappear
// with it; when the game releases that 3D, the lights are taken off the scene node here as well. Nothing on any
// record is edited except the projectile's own hand light, which is taken off while this pass is on and given
// back when it is switched off - the same remember-and-restore the other passes use.
//
// The settings decide everything: the setting "Stream Lights" turns the pass on, and the Brightness and Reach
// sliders scale each light's fade and radius. Colors come from the spray markers (frost, shock, fire), or from the
// projectile's own light record when the markers say nothing about it.

#include "Plugin.h"

namespace Plugin
{
	namespace
	{
		constexpr std::string_view kStreamSetting = "IlluminatedStreamLights";
		constexpr std::string_view kWardSetting = "IlluminatedWardLights";
		constexpr float            kWardRadius = 220.0f;  // a ward dome is about waist-high and an arm in front
		constexpr float            kWardFade = 1.0f;
		// a ward with no color of its own: the pale blue-white of the vanilla dome
		constexpr float       kWardRed = 0.62f, kWardGreen = 0.78f, kWardBlue = 1.0f;
		constexpr std::size_t kMaxLightsPerStream = 3;
		constexpr float       kStepUnits = 300.0f;   // one light per this much stream, up to the maximum
		constexpr float       kRadiusOfStep = 1.4f;  // each light reaches a little past the next step
		constexpr float       kShortestStream = 120.0f;
		constexpr const char* kLightName = "IlluminatedStream";
		constexpr float       kLightSize = LightKit::kLightSize;  // the light's size, which lives in the radius' z (from ReLight)
		// rule 2: a flame spray's one light sits this far out (half the range for a short spray) and reaches from there
		// to the end of the spray and a little past, never less than the shortest reach
		constexpr float kFlameForward = 128.0f;
		constexpr float kFlameEndReach = 1.15f;
		constexpr float kFlameShortestReach = 133.0f;
		// rule 3: a beam's light rides the tip, on this node when the mesh has it
		constexpr float       kBeamRadius = 420.0f;
		constexpr const char* kBeamNode = "BeamEnd";
		// rule 4: the house K, 0.8 * 69.99² - the same number gen.py writes every config's cutoff from (LightKit.h)
		using LightKit::kK;
		constexpr float kLowestCutoff = LightKit::Isl::kLowestCutoff;
		// no one projectile base keeps more than this many of its refs lit at a time
		constexpr std::size_t kMaxLivePerBase = 4;

		struct Recipe
		{
			bool        ward{ false };   // a ward dome: one light where the dome sits, and its own setting
			bool        flame{ false };  // a flamethrower-type spray: one light at the root (rule 2)
			bool        beam{ false };   // a beam: one light on its BeamEnd node (rule 3)
			std::size_t lights{ 1 };
			float       gap{ 0.0f };  // units between lights along the stream
			float       radius{ 300.0f };
			float       fade{ 1.0f };
			float       falloff{ 2.0f };
			RE::NiColor color{ 1.0f, 1.0f, 1.0f };
		};

		std::unordered_map<RE::FormID, Recipe>             gRecipes;    // projectile base form -> what to hang on it
		std::unordered_map<RE::FormID, RE::TESObjectLIGH*> gHandLight;  // its own light, to give back

		// One lit projectile, keyed by its 3D ROOT, not its reference ID: a projectile's reference is temporary and the game
		// hands its ID to a new one, so an ID-keyed entry could be taken for the new projectile or dropped by the old one's
		// late release. The root is held here, so its address is never another 3D's while it is listed (the re-score's
		// Illuminated issue 3).
		struct Live
		{
			RE::NiPointer<RE::NiAVObject>           root;
			RE::FormID                              base{ 0 };
			std::vector<RE::NiPointer<RE::BSLight>> lights;
		};
		std::unordered_map<const RE::NiAVObject*, Live> gLive;
		// the live count, by projectile BASE rather than by reference: a held spray key spawns a new reference several times
		// a second, and it is the base that has to be capped
		std::unordered_map<RE::FormID, std::size_t> gLiveCount;
		std::mutex                                  gLiveLock;
		// rule 5: what a loader thread built or released, waiting for the main thread (under gLiveLock). The reference is
		// held (it is refcounted) - never a handle made off the main thread (the fading module's rule; re-score issue 2)
		std::vector<RE::NiPointer<RE::TESObjectREFR>> gToHang;
		std::vector<const RE::NiAVObject*>            gToDrop;
		std::uint32_t                                 gSweepClock = 0;  // frames since the last sweep (main thread)
		bool                                          gTaken = false;   // the hand lights are off right now

		// fire, frost or shock, read from the editor ID (the spray pass's words, and spark / storm for the shock streams)
		Family StreamFamily(std::string_view a_editorID)
		{
			const auto id = Lower(a_editorID);
			if (Contains(id, "frost") || Contains(id, "ice")) {
				return Family::kFrost;
			}
			if (Contains(id, "flame") || Contains(id, "fire")) {
				return Family::kFire;
			}
			if (Contains(id, "shock") || Contains(id, "lightning") || Contains(id, "spark") || Contains(id, "storm")) {
				return Family::kShock;
			}
			return Family::kNone;
		}

		// every ward belongs to Dynamic Wards: when that mod is here, this pass does not light a ward
		bool DynamicWardsHere()
		{
			for (const auto* name : { "Dynamic Wards.esp", "Dynamic Wards.esl", "DynamicWards.esp" }) {
				if (PluginLoaded(name)) {
					return true;
				}
			}
			// Dynamic Wards 2.0 has no plugin, only `DynamicWards.dll`, so it is found by its DLL
			return REX::W32::GetModuleHandleW(L"DynamicWards.dll") != nullptr;
		}

		bool WardLightsOn()
		{
			return SettingValue(kWardSetting, 0) != 0 && !DynamicWardsHere();
		}

		float Percent(std::string_view a_id)
		{
			return static_cast<float>(SettingValue(a_id, 100)) / 100.0f;
		}

		RE::ShadowSceneNode* SceneNode()
		{
			return RE::BSShaderManager::State::GetSingleton().shadowSceneNode[0];
		}

		bool OnMainThread()
		{
			const auto* main = RE::Main::GetSingleton();
			return main && REX::W32::GetCurrentThreadId() == main->threadID;
		}

		RE::NiColor ColorOf(const RE::TESObjectLIGH* a_light)
		{
			if (!a_light) {
				return { 1.0f, 1.0f, 1.0f };
			}
			return { a_light->data.color.red / 255.0f, a_light->data.color.green / 255.0f, a_light->data.color.blue / 255.0f };
		}

		// ------------------------------------------------------------------ forgetting one live projectile
		// gLiveLock is held by every caller; the main thread's (the scene node is touched)
		void Forget(const RE::NiAVObject* a_root, RE::ShadowSceneNode* a_scene)
		{
			const auto it = gLive.find(a_root);
			if (it == gLive.end()) {
				return;
			}
			if (a_scene) {
				for (auto& bs : it->second.lights) {
					if (bs) {
						a_scene->RemoveLight(bs);
					}
				}
			}
			if (const auto c = gLiveCount.find(it->second.base); c != gLiveCount.end() && c->second) {
				--c->second;
			}
			gLive.erase(it);  // the last reference to the projectile's 3D may go here, on the main thread
		}

		// the sweep: a projectile 3D the game has taken out of the world (no parent) is not live, whether or not its release
		// reached this pass. gLiveLock is held by the caller.
		std::size_t SweepGone(RE::ShadowSceneNode* a_scene)
		{
			std::vector<const RE::NiAVObject*> gone;
			for (const auto& [root, live] : gLive) {
				if (!live.root || !live.root->parent) {
					gone.push_back(root);
				}
			}
			for (const auto* root : gone) {
				Forget(root, a_scene);
			}
			return gone.size();
		}

		// ------------------------------------------------------------------ the lights on one live projectile
		// counted so the log can say which step a stream light stopped at, rather than saying nothing at all
		std::size_t gCalls = 0, gNo3D = 0, gNoRecipe = 0, gOff = 0, gNoScene = 0, gAlready = 0, gAtCap = 0;

		void Told(std::string_view a_what, const RE::TESObjectREFR* a_ref)
		{
			if (gCalls <= 10) {
				SKSE::log::info("[STREAM-HOOK] call {} | {} | base {}", gCalls, a_what,
					a_ref && a_ref->GetBaseObject() ? Label(a_ref->GetBaseObject()) : "(none)");
			}
		}

		void HangLights(RE::TESObjectREFR* a_ref, RE::NiAVObject* a_object)
		{
			++gCalls;
			if (!a_ref || !a_object) {
				++gNo3D;
				Told("no reference or no 3D", a_ref);
				return;
			}
			auto* root = a_object->AsNode();
			auto* base = a_ref->GetBaseObject();
			if (!root || !base) {
				++gNo3D;
				Told("the 3D is not a node, or there is no base form", a_ref);
				return;
			}
			const auto it = gRecipes.find(base->GetFormID());
			if (it == gRecipes.end()) {
				++gNoRecipe;
				Told("no recipe for this projectile", a_ref);
				return;
			}
			const auto& r = it->second;
			if (!(r.ward ? WardLightsOn() : StreamLightsOn())) {
				++gOff;
				Told("its setting is off", a_ref);
				return;
			}
			auto* scene = SceneNode();
			if (!scene) {
				++gNoScene;
				Told("the game has no shadow scene node right now", a_ref);
				return;
			}
			// the cap, before any light is made
			{
				std::lock_guard l{ gLiveLock };
				if (gLive.contains(root)) {  // keyed by the 3D itself: this very 3D already carries our lights
					++gAlready;
					Told("this projectile is already lit", a_ref);
					return;
				}
				if (gLiveCount[base->GetFormID()] >= kMaxLivePerBase) {
					SweepGone(scene);
					if (gLiveCount[base->GetFormID()] >= kMaxLivePerBase) {
						++gAtCap;
						Told("at the live cap for this projectile", a_ref);
						return;
					}
				}
			}
			// rule 3: a beam's light rides the tip of the bolt, on the node the mesh names
			RE::NiNode* parent = root;
			if (r.beam) {
				if (auto* node = root->GetObjectByName(RE::BSFixedString(kBeamNode)); node && node->AsNode()) {
					parent = node->AsNode();
				}
			}
			Told("making its lights", a_ref);
			const bool isl = InverseSquare();
			// ENB and Vanilla (Lighting.cpp): the reach asked for, drawn by the game's own lighting
			const auto                              plain = Plain(r.fade, 0.0f, r.radius);
			const float                             radius = (isl ? r.radius : plain.radius) * Percent("IlluminatedReach");
			const float                             fade = (isl ? r.fade : plain.fade) * Percent("IlluminatedBrightness");
			std::vector<RE::NiPointer<RE::BSLight>> made;
			for (std::size_t i = 0; i < r.lights; ++i) {
				auto* light = LightKit::CloneLight();  // ReLight's master-and-clone (LightKit.h)
				if (!light) {
					break;
				}
				light->name = kLightName;
				auto& data = light->GetLightRuntimeData();
				data.diffuse = r.color;
				data.fade = fade;
				// x and y are the reach; z carries the light's SIZE, not a third radius (ReLight and Light Placer both do this)
				data.radius = { radius, radius, kLightSize };
				// no ambient color with inverse square lighting: Community Shaders reuses those fields. Without it a new light's
				// ambient is white, so it takes RE::Light's rule (Truman): a tenth of the diffuse (Dynamic Wards does the same)
				// without this a light has no attenuation of its own and never brightens anything (Light Placer does it too)
				light->SetLightAttenuation(radius);
				if (!isl) {  // after SetLightAttenuation, which writes the same two words
					data.ambient = { r.color.red * 0.1f, r.color.green * 0.1f, r.color.blue * 0.1f };
				}
				// rule 4: inverse square flag and cutoff, after SetLightAttenuation - Community Shaders only
				if (isl) {
					LightKit::Isl::SetOn(light);
					LightKit::Isl::SetCutoff(light, LightKit::CutoffFor(fade, radius, kLightSize));
				}
				light->local.translate = { 0.0f, r.gap * static_cast<float>(i + 1), 0.0f };
				light->local.scale = 1.0f;
				parent->AttachChild(light, true);
				// give it a world position now, rather than waiting for whatever updates the projectile next
				RE::NiUpdateData update{};
				light->Update(update);
				if (auto* bs = LightKit::AddToScene(scene, light, r.falloff)) {
					made.emplace_back(bs);
				} else {
					parent->DetachChild(light);
				}
			}
			// the first few of each kind are logged, so the log says whether this pass is doing anything
			static std::size_t told = 0;
			if (told < 12) {
				++told;
				SKSE::log::info("[STREAM-LIT] {} | {} | {} light(s) of {} asked for | radius {:.0f} | fade {:.2f} | node {}",
					Label(a_ref->GetBaseObject()), r.flame ? "flame spray" : r.beam ? "bolt" :
																		 r.ward     ? "ward" :
																					  "spray",
					made.size(), r.lights, radius, fade, parent->name.empty() ? "(unnamed)" : parent->name.c_str());
			}
			if (made.empty()) {
				SKSE::log::warn("[STREAM-FAILED] {} | no light could be made or registered", Label(a_ref->GetBaseObject()));
				return;
			}
			std::lock_guard l{ gLiveLock };
			gLive.emplace(root, Live{ RE::NiPointer<RE::NiAVObject>(root), base->GetFormID(), std::move(made) });
			++gLiveCount[base->GetFormID()];
		}

		// called before the game releases the reference's 3D, so its root is still there to find the entry by
		void DropLights(RE::TESObjectREFR* a_ref)
		{
			const auto* root = a_ref ? a_ref->Get3D() : nullptr;
			if (!root) {
				return;
			}
			std::lock_guard l{ gLiveLock };
			if (!gLive.contains(root)) {
				return;
			}
			if (OnMainThread()) {
				Forget(root, SceneNode());
			} else {
				gToDrop.push_back(root);  // the entry holds the root, so the address stays this 3D's until the main thread drops it
			}
		}

		// ------------------------------------------------------------------ the two hooks
		// Load3D builds a reference's 3D (vtable slot 0x6A); Release3DRelatedData takes it apart (0x6B)
		template <class T>
		struct Load3D
		{
			static RE::NiAVObject* thunk(T* a_this, bool a_backgroundLoading)
			{
				auto* object = original(a_this, a_backgroundLoading);
				if (OnMainThread()) {
					HangLights(a_this, object);
				} else if (object) {
					std::lock_guard l{ gLiveLock };
					gToHang.emplace_back(a_this);  // held by its refcount, no handle made on this thread
				}
				return object;
			}

			static inline REL::Relocation<decltype(thunk)> original;

			static void Install()
			{
				original = REL::Relocation<std::uintptr_t>(T::VTABLE[0]).write_vfunc(0x6A, thunk);
			}
		};

		template <class T>
		struct Release3D
		{
			static void thunk(T* a_this)
			{
				DropLights(a_this);
				original(a_this);
			}

			static inline REL::Relocation<decltype(thunk)> original;

			static void Install()
			{
				original = REL::Relocation<std::uintptr_t>(T::VTABLE[0]).write_vfunc(0x6B, thunk);
			}
		};
	}

	bool StreamLightsOn()
	{
		return SettingValue(kStreamSetting, 0) != 0;
	}

	// ------------------------------------------------------------------ wards: one light where the dome sits
	// every ward belongs to Dynamic Wards: this lights a ward only when Dynamic Wards is not installed. The
	// color is the ward's own light when it has one, else the pale blue-white of the vanilla dome.
	void WardLights()
	{
		std::size_t wards = 0, colored = 0;
		for (auto* proj : LightKit::FormsOf<RE::BGSProjectile>()) {
			if (!proj || !proj->data.types.any(RE::BGSProjectileData::Type::kBarrier)) {
				continue;
			}
			Recipe r;
			r.ward = true;
			r.lights = 1;
			r.gap = 0.0f;  // where the dome itself sits, not out along a stream
			r.radius = kWardRadius;
			r.fade = kWardFade;
			r.falloff = 2.0f;
			if (proj->data.light) {
				r.color = ColorOf(proj->data.light);
				++colored;
			} else {
				r.color = { kWardRed, kWardGreen, kWardBlue };
			}
			gRecipes[proj->GetFormID()] = r;
			++wards;
			SKSE::log::info("[WARD] {} | color {:.2f},{:.2f},{:.2f} | radius {:.0f}", Label(proj), r.color.red, r.color.green, r.color.blue, r.radius);
		}
		SKSE::log::info("ward lights: {} ward dome(s), {} with a color of their own; Dynamic Wards is {}; the setting is {}",
			wards, colored, DynamicWardsHere() ? "installed, so this pass stands down" : "not installed",
			SettingValue(kWardSetting, 0) ? "on" : "off");
	}

	// ------------------------------------------------------------------ the table, read once from the load order
	void StreamLights()
	{
		gRecipes.clear();
		gHandLight.clear();
		const auto  sc = ReadSprayChoice();
		std::size_t cones = 0, beams = 0, flames = 0, skipped = 0;
		for (auto* proj : LightKit::FormsOf<RE::BGSProjectile>()) {
			if (!proj) {
				continue;
			}
			// rules 1 and 2: a flamethrower-type projectile takes one light at the root
			const bool flame = proj->data.types.any(RE::BGSProjectileData::Type::kFlamethrower);
			const bool cone = !flame && proj->data.types.any(RE::BGSProjectileData::Type::kCone);
			const bool beam = !flame && !cone && proj->data.types.any(RE::BGSProjectileData::Type::kBeam);
			if (!cone && !beam && !flame) {
				continue;
			}
			const auto id = EditorID(proj);
			const auto low = Lower(id);
			const auto model = NormalPath(proj->GetModel() ? proj->GetModel() : "");
			// poison sprays stay dark, the way every other pass leaves them
			if (Contains(low, "poison") || Contains(low, "poision")) {
				++skipped;
				continue;
			}
			const float range = proj->data.range;
			// a flame spray's range is the reach of the cone, so the shortest-stream floor does not apply
			if (!flame && range < kShortestStream) {
				++skipped;
				continue;
			}
			Recipe r;
			r.flame = flame;
			r.beam = beam;
			if (flame) {
				// rule 2; its reach is set below, once its fade is known
				r.lights = 1;
				r.gap = (std::min)(kFlameForward, range / 2.0f);
			} else if (beam) {
				// rule 3: one light, on BeamEnd, so it rides the tip
				r.lights = 1;
				r.gap = 0.0f;
				r.radius = kBeamRadius;
			} else {
				r.lights = static_cast<std::size_t>(std::clamp<int>(static_cast<int>(range / kStepUnits), 1, static_cast<int>(kMaxLightsPerStream)));
				r.gap = range / static_cast<float>(r.lights + 1);
				r.radius = r.gap * kRadiusOfStep;
			}
			r.falloff = sc.falloff > 0.0f ? sc.falloff : 2.0f;
			const auto family = StreamFamily(id);
			r.fade = family == Family::kFrost ? sc.frostFade : sc.fade;
			if (r.fade <= 0.0f) {
				r.fade = 1.0f;
			}
			if (flame) {
				// no further than one light of this fade carries (rule 4 at the lowest cutoff): a dragon's breath ranges 3000
				r.radius = std::clamp((range - r.gap) * kFlameEndReach, kFlameShortestReach, std::sqrt(kK * r.fade / kLowestCutoff));
			}
			if (family == Family::kFrost && sc.frostSet) {
				r.color = { sc.frost.r / 255.0f, sc.frost.g / 255.0f, sc.frost.b / 255.0f };
			} else if (family == Family::kShock && sc.shockSet) {
				r.color = { sc.shock.r / 255.0f, sc.shock.g / 255.0f, sc.shock.b / 255.0f };
			} else {
				r.color = ColorOf(proj->data.light);
			}
			gRecipes[proj->GetFormID()] = r;
			gHandLight[proj->GetFormID()] = proj->data.light;
			if (flame) {
				++flames;
			} else if (cone) {
				++cones;
			} else {
				++beams;
			}
			SKSE::log::info("[STREAM] {} | {} | range {:.0f} | {} light(s) every {:.0f} | radius {:.0f} | fade {:.2f} | model {}",
				Label(proj), flame ? "flame spray" : cone ? "spray" :
															"bolt",
				range, r.lights, r.gap, r.radius, r.fade, model);
		}
		SKSE::log::info(
			"stream lights: {} flame spray(s), {} spray(s) and {} bolt(s) can carry a light of their own, "
			"{} left alone; no one projectile keeps more than {} lit at a time; the setting is {}",
			flames, cones, beams, skipped, kMaxLivePerBase, StreamLightsOn() ? "on" : "off");
		WardLights();
		Load3D<RE::ConeProjectile>::Install();
		Load3D<RE::BeamProjectile>::Install();
		Load3D<RE::BarrierProjectile>::Install();
		// rule 1: nothing else hooks this one, and it is what the vanilla sprays are
		Load3D<RE::FlameProjectile>::Install();
		Release3D<RE::ConeProjectile>::Install();
		Release3D<RE::BeamProjectile>::Install();
		Release3D<RE::BarrierProjectile>::Install();
		Release3D<RE::FlameProjectile>::Install();
		ApplyStreamLights(true);
	}

	// rule 5: once a frame, on the main thread - what the loader threads left to do
	void StreamLightsFrame()
	{
		std::vector<RE::NiPointer<RE::TESObjectREFR>> hang;
		{
			std::lock_guard l{ gLiveLock };
			// about every five seconds, the lights whose 3D is gone are let go even when no cap was reached
			if (++gSweepClock >= 300 && !gLive.empty()) {
				gSweepClock = 0;
				SweepGone(SceneNode());
			}
			if (gToHang.empty() && gToDrop.empty()) {
				return;
			}
			auto* scene = SceneNode();
			for (const auto* root : gToDrop) {
				Forget(root, scene);
			}
			gToDrop.clear();
			hang.swap(gToHang);
		}
		for (const auto& ref : hang) {
			// a 3D released before this frame is gone (or another one now): HangLights lights whatever the reference holds
			// now, and only when that 3D is in the world
			if (auto* root = ref ? ref->Get3D() : nullptr; root && root->parent) {
				HangLights(ref.get(), root);
			}
		}
	}

	// the projectile's own hand light is taken off while this pass is on, and given back when it is switched off
	void ApplyStreamLights(bool a_log)
	{
		const bool want = StreamLightsOn();
		if (!want && !gTaken && !a_log) {
			return;  // off and nothing taken; while it is on, every call takes off whatever light has come back
		}
		std::size_t changed = 0;
		for (auto& [formID, own] : gHandLight) {
			auto* proj = RE::TESForm::LookupByID<RE::BGSProjectile>(formID);
			if (!proj) {
				continue;
			}
			// taken off: what it carries now is what to give back - the spray pass may have given it a copy of its
			// own since this table was read, and that copy must come back, not the record's first light
			if (want && proj->data.light) {
				own = proj->data.light;
			}
			auto* wanted = want ? nullptr : own;
			if (proj->data.light != wanted) {
				proj->data.light = wanted;
				++changed;
			}
		}
		gTaken = want;
		if (a_log || changed) {
			SKSE::log::info("stream lights: {}; {} hand light(s) {}", want ? "on" : "off", changed, want ? "taken off" : "given back");
		}
		if (!want) {
			// every stream light still lit comes off the scene node now, with every map that counts it: a light
			// only forgotten stays registered and keeps shining where its stream was, and a count left behind keeps
			// the cap full so the pass never lights anything again. A ward's light follows its own setting.
			auto*                              scene = SceneNode();
			std::lock_guard                    l{ gLiveLock };
			std::vector<const RE::NiAVObject*> streams;
			for (const auto& [root, live] : gLive) {
				if (const auto r = gRecipes.find(live.base); r == gRecipes.end() || !r->second.ward) {
					streams.push_back(root);
				}
			}
			for (const auto* root : streams) {
				Forget(root, scene);
			}
		}
	}
}
