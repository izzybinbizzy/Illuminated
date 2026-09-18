// Luminous Arcana - SKSE plugin
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
// How it works. When the game builds a cone or beam projectile's 3D, this hooks the call and hangs one to three
// NiPointLights off that 3D at fixed steps along its forward axis, then hands each one to the shadow scene node,
// which is what actually renders it. The lights are children of the projectile's own node, so they travel and turn
// with the stream and disappear with it; when the game releases that 3D, the lights are taken off the scene node
// here as well. Nothing on any record is edited except the projectile's own hand light, which is taken off while
// this pass is on and given back when it is switched off - the same remember-and-restore the other passes use.
//
// The settings decide everything: the setting "Stream Lights" turns the pass on, and the Brightness and Reach
// sliders scale each light's fade and radius. Colors come from the spray markers (frost, shock, fire), or from the
// projectile's own light record when the markers say nothing about it.

#include "Plugin.h"

namespace Plugin
{
	namespace
	{
		constexpr std::string_view kStreamSetting = "LuminousArcanaStreamLights";
		constexpr std::string_view kWardSetting = "LuminousArcanaWardLights";
		constexpr float            kWardRadius = 220.0f;  // a ward dome is about waist-high and an arm in front
		constexpr float            kWardFade = 1.0f;
		// a ward with no color of its own: the pale blue-white of the vanilla dome
		constexpr float            kWardRed = 0.62f, kWardGreen = 0.78f, kWardBlue = 1.0f;
		constexpr std::size_t      kMaxLightsPerStream = 3;
		constexpr float            kStepUnits = 300.0f;   // one light per this much stream, up to the maximum
		constexpr float            kRadiusOfStep = 1.4f;  // each light reaches a little past the next step
		constexpr float            kShortestStream = 120.0f;
		constexpr const char*      kLightName = "LuminousArcanaStream";
		constexpr float            kLightSize = 1.414f;   // the light's size, which lives in the radius' z (from ReLight)
		constexpr float            kFieldOfView = 90.0f;  // what a light that casts no shadow is given (from ReLight)

		// one master light, made once and cloned for every use: ReLight found that a freshly made light, attached
		// straight away, crashes
		RE::NiPointer<RE::NiPointLight> gMaster;

		RE::NiPointLight* CloneMaster()
		{
			if (!gMaster) {
				auto* fresh = RE::NiPointLight::Create();
				if (!fresh) {
					return nullptr;
				}
				auto* clone = netimmerse_cast<RE::NiPointLight*>(fresh->Clone());
				if (!clone) {
					return nullptr;
				}
				gMaster.reset(clone);
			}
			return netimmerse_cast<RE::NiPointLight*>(gMaster->Clone());
		}

		struct Recipe
		{
			bool          ward{ false };  // a ward dome: one light where the dome sits, and its own setting
			std::size_t   lights{ 1 };
			float         gap{ 0.0f };  // units between lights along the stream
			float         radius{ 300.0f };
			float         fade{ 1.0f };
			float         falloff{ 2.0f };
			RE::NiColor   color{ 1.0f, 1.0f, 1.0f };
		};

		std::unordered_map<RE::FormID, Recipe>                             gRecipes;   // projectile base form -> what to hang on it
		std::unordered_map<RE::FormID, RE::TESObjectLIGH*>                 gHandLight;  // its own light, to give back
		std::unordered_map<std::uint32_t, std::vector<RE::NiPointer<RE::BSLight>>> gLive;  // live 3D -> the lights this pass made
		std::mutex                                                          gLiveLock;
		bool                                                                gTaken = false;  // the hand lights are off right now

		// fire, frost or shock, read from the editor ID the way the spray pass reads it
		std::string StreamFamily(const std::string& a_editorID)
		{
			const auto id = Lower(a_editorID);
			if (Contains(id, "frost") || Contains(id, "ice")) {
				return "frost";
			}
			if (Contains(id, "flame") || Contains(id, "fire")) {
				return "fire";
			}
			if (Contains(id, "shock") || Contains(id, "lightning") || Contains(id, "spark") || Contains(id, "storm")) {
				return "shock";
			}
			return {};
		}

		bool StreamLightsOn()
		{
			return SettingValue(kStreamSetting, 0) != 0;
		}

		// ⛔ HIS RULE: every ward belongs to Dynamic Wards. When that mod is here, this pass does not light a ward.
		bool DynamicWardsHere()
		{
			for (const auto* name : { "Dynamic Wards.esp", "Dynamic Wards.esl", "DynamicWards.esp" }) {
				if (PluginLoaded(name)) {
					return true;
				}
			}
			return false;
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

		RE::NiColor ColorOf(const RE::TESObjectLIGH* a_light)
		{
			if (!a_light) {
				return { 1.0f, 1.0f, 1.0f };
			}
			return { a_light->data.color.red / 255.0f, a_light->data.color.green / 255.0f, a_light->data.color.blue / 255.0f };
		}

		// ------------------------------------------------------------------ the lights on one live projectile
		// counted so the log can say which step a stream light stopped at, rather than saying nothing at all
		std::size_t gCalls = 0, gNo3D = 0, gNoRecipe = 0, gOff = 0, gNoScene = 0;

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
			Told("making its lights", a_ref);
			const float radius = r.radius * Percent("LuminousArcanaReach");
			const float fade = r.fade * Percent("LuminousArcanaBrightness");
			std::vector<RE::NiPointer<RE::BSLight>> made;
			for (std::size_t i = 0; i < r.lights; ++i) {
				auto* light = CloneMaster();
				if (!light) {
					break;
				}
				light->name = kLightName;
				auto& data = light->GetLightRuntimeData();
				data.diffuse = r.color;
				data.fade = fade;
				// x and y are the reach; z carries the light's SIZE, not a third radius (ReLight and Light Placer both do this)
				data.radius = { radius, radius, kLightSize };
				// no ambient on purpose: Community Shaders' inverse square lighting reuses those fields
				// without this a light has no attenuation of its own and never brightens anything (Light Placer does it too)
				light->SetLightAttenuation(radius);
				light->local.translate = { 0.0f, r.gap * static_cast<float>(i + 1), 0.0f };
				light->local.scale = 1.0f;
				root->AttachChild(light, true);
				// give it a world position now, rather than waiting for whatever updates the projectile next
				RE::NiUpdateData update{};
				light->Update(update);
				RE::ShadowSceneNode::LIGHT_CREATE_PARAMS params{};
				params.dynamic = true;
				params.shadowLight = false;
				params.portalStrict = true;
				params.affectLand = true;
				params.affectWater = true;
				params.neverFades = true;
				params.fov = kFieldOfView;
				params.falloff = r.falloff;
				params.nearDistance = 5.0f;
				params.depthBias = 1.0f;
				params.sceneGraphIndex = 0;
				params.restrictedNode = nullptr;
				params.lensFlareData = nullptr;
				if (auto* bs = scene->AddLight(light, params)) {
					made.emplace_back(bs);
				}
			}
			// the first few of each kind are written down, so his log says whether this pass is doing anything at all
			static std::size_t told = 0;
			if (told < 12) {
				++told;
				SKSE::log::info("[STREAM-LIT] {} | {} light(s) of {} asked for | radius {:.0f} | fade {:.2f} | node {}",
					Label(a_ref->GetBaseObject()), made.size(), r.lights, radius, fade, root->name.empty() ? "(unnamed)" : root->name.c_str());
			}
			if (made.empty()) {
				SKSE::log::warn("[STREAM-FAILED] {} | no light could be made or registered", Label(a_ref->GetBaseObject()));
				return;
			}
			std::lock_guard l{ gLiveLock };
			auto& kept = gLive[a_ref->GetFormID()];
			kept.insert(kept.end(), made.begin(), made.end());
		}

		void DropLights(RE::TESObjectREFR* a_ref)
		{
			if (!a_ref) {
				return;
			}
			std::vector<RE::NiPointer<RE::BSLight>> mine;
			{
				std::lock_guard l{ gLiveLock };
				const auto it = gLive.find(a_ref->GetFormID());
				if (it == gLive.end()) {
					return;
				}
				mine.swap(it->second);
				gLive.erase(it);
			}
			if (auto* scene = SceneNode()) {
				for (auto& bs : mine) {
					if (bs) {
						scene->RemoveLight(bs);
					}
				}
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
				HangLights(a_this, object);
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

	// ------------------------------------------------------------------ wards: one light where the dome sits
	// ⛔ HIS RULE, and his call again 2026-09-17: every ward belongs to Dynamic Wards. This lights a ward ONLY when
	// Dynamic Wards is not installed, and it stands down the moment it is. The color is the ward's own, when the ward
	// carries a light of its own to read it from; otherwise the pale blue-white of the vanilla dome.
	void WardLights()
	{
		std::size_t wards = 0, colored = 0;
		for (auto* proj : RE::TESDataHandler::GetSingleton()->GetFormArray<RE::BGSProjectile>()) {
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
		std::size_t cones = 0, beams = 0, skipped = 0;
		for (auto* proj : RE::TESDataHandler::GetSingleton()->GetFormArray<RE::BGSProjectile>()) {
			if (!proj) {
				continue;
			}
			const bool cone = proj->data.types.any(RE::BGSProjectileData::Type::kCone, RE::BGSProjectileData::Type::kFlamethrower);
			const bool beam = proj->data.types.any(RE::BGSProjectileData::Type::kBeam);
			if (!cone && !beam) {
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
			if (range < kShortestStream) {
				++skipped;
				continue;
			}
			Recipe r;
			r.lights = static_cast<std::size_t>(std::clamp<int>(static_cast<int>(range / kStepUnits), 1, static_cast<int>(kMaxLightsPerStream)));
			r.gap = range / static_cast<float>(r.lights + 1);
			r.radius = r.gap * kRadiusOfStep;
			r.falloff = sc.falloff > 0.0f ? sc.falloff : 2.0f;
			const auto family = StreamFamily(id);
			r.fade = family == "frost" ? sc.frostFade : sc.fade;
			if (r.fade <= 0.0f) {
				r.fade = 1.0f;
			}
			if (family == "frost" && sc.frostSet) {
				r.color = { sc.frost.r / 255.0f, sc.frost.g / 255.0f, sc.frost.b / 255.0f };
			} else if (family == "shock" && sc.shockSet) {
				r.color = { sc.shock.r / 255.0f, sc.shock.g / 255.0f, sc.shock.b / 255.0f };
			} else {
				r.color = ColorOf(proj->data.light);
			}
			gRecipes[proj->GetFormID()] = r;
			gHandLight[proj->GetFormID()] = proj->data.light;
			(cone ? cones : beams)++;
			SKSE::log::info("[STREAM] {} | {} | range {:.0f} | {} light(s) every {:.0f} | radius {:.0f} | fade {:.2f} | model {}",
				Label(proj), cone ? "spray" : "bolt", range, r.lights, r.gap, r.radius, r.fade, model);
		}
		SKSE::log::info("stream lights: {} sprays and {} bolts can carry a light of their own, {} left alone; the setting is {}",
			cones, beams, skipped, StreamLightsOn() ? "on" : "off");
		WardLights();
		Load3D<RE::ConeProjectile>::Install();
		Load3D<RE::BeamProjectile>::Install();
		Load3D<RE::BarrierProjectile>::Install();
		Release3D<RE::ConeProjectile>::Install();
		Release3D<RE::BeamProjectile>::Install();
		Release3D<RE::BarrierProjectile>::Install();
		ApplyStreamLights(true);
	}

	// the projectile's own hand light is taken off while this pass is on, and given back when it is switched off
	void ApplyStreamLights(bool a_log)
	{
		const bool want = StreamLightsOn();
		if (want == gTaken && !a_log) {
			return;
		}
		std::size_t changed = 0;
		for (const auto& [formID, own] : gHandLight) {
			auto* proj = RE::TESForm::LookupByID<RE::BGSProjectile>(formID);
			if (!proj) {
				continue;
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
			std::lock_guard l{ gLiveLock };
			gLive.clear();
		}
	}
}
