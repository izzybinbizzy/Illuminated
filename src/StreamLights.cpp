// Luminous Arcana - SKSE plugin
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE.txt and the notice at the top of main.cpp.
//
// Pass 6: lights that travel with a spray or a bolt.
//
// A spray or a shock stream is one projectile whose light record sits at the caster's hand, so the game lights the
// hand and not the stream. Light Placer does not light these at all (it attaches to a model, and these have none it
// can hold). ReLight showed that a stream CAN be lit safely, by giving the projectile's own 3D a light of its own.
// This is our own reading of the same idea, written here from the game's interfaces; none of its code is used.
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
		constexpr std::size_t      kMaxLightsPerStream = 3;
		constexpr float            kStepUnits = 300.0f;   // one light per this much stream, up to the maximum
		constexpr float            kRadiusOfStep = 1.4f;  // each light reaches a little past the next step
		constexpr float            kShortestStream = 120.0f;
		constexpr const char*      kLightName = "LuminousArcanaStream";

		struct Recipe
		{
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
		void HangLights(RE::TESObjectREFR* a_ref, RE::NiAVObject* a_object)
		{
			if (!a_ref || !a_object || !StreamLightsOn()) {
				return;
			}
			auto* root = a_object->AsNode();
			auto* base = a_ref->GetBaseObject();
			if (!root || !base) {
				return;
			}
			const auto it = gRecipes.find(base->GetFormID());
			if (it == gRecipes.end()) {
				return;
			}
			auto* scene = SceneNode();
			if (!scene) {
				return;
			}
			const auto& r = it->second;
			const float radius = r.radius * Percent("LuminousArcanaReach");
			const float fade = r.fade * Percent("LuminousArcanaBrightness");
			std::vector<RE::NiPointer<RE::BSLight>> made;
			for (std::size_t i = 0; i < r.lights; ++i) {
				auto* light = RE::NiPointLight::Create();
				if (!light) {
					break;
				}
				light->name = kLightName;
				light->diffuse = r.color;
				light->fade = fade;
				light->radius = { radius, radius, radius };
				light->SetLightAttenuation(radius);
				light->local.translate = { 0.0f, r.gap * static_cast<float>(i + 1), 0.0f };
				light->local.scale = 1.0f;
				root->AttachChild(light, true);
				RE::ShadowSceneNode::LIGHT_CREATE_PARAMS params{};
				params.dynamic = true;
				params.shadowLight = false;
				params.portalStrict = true;
				params.affectLand = true;
				params.affectWater = true;
				params.neverFades = false;
				params.fov = 0.0f;
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
			if (made.empty()) {
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
		Load3D<RE::ConeProjectile>::Install();
		Load3D<RE::BeamProjectile>::Install();
		Release3D<RE::ConeProjectile>::Install();
		Release3D<RE::BeamProjectile>::Install();
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
