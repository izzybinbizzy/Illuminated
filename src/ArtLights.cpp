// Illuminated - SKSE plugin
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE.txt and the notice at the top of main.cpp.
//
// Lights on ART and WEAPON models, without Light Placer (Vanilla and ENB). HIS REPORT 2026-10-10: "bound weapons not lighting
// up on vanilla." A config that lights a model the game shows as an ART OBJECT (a bound weapon's enchantment art, an
// enchanted weapon's art) or as a weapon's own mesh (the bound bow) has no light record slot to point at: the game gives light
// records to magic effects, projectiles, explosions and hazards only (RecordLights.cpp). Light Placer hangs a light on such a
// model by itself; without it nothing did. So here, once a frame on the main thread: every magic art effect in the world whose
// model a config row lights now (RecordLightFor - the settings' tests), and every drawn weapon whose own model one lights, gets
// a light of ours on that model - in the record light's color, at its strength and reach (so the sliders, daylight and the
// flicker reach it), made the way Dynamic Wards and GroundLights.cpp make theirs (land lighting on). The light hangs under the
// art's or the weapon's 3D, so the fading module finds and fades it like any other weapon light (a bound weapon over the last
// seconds of its spell). It goes when the art or the weapon goes, or a setting stops lighting its model.
//
// CREDIT: a light is made and registered the way ReLight by Truman does it (github.com/TrumanGIT/ReLight, GPL-3.0-or-later,
// used with his permission): a master NiPointLight cloned for every use and handed to the shadow scene node.

#include "Plugin.h"

namespace Plugin
{
	namespace
	{
		constexpr const char* kName = "IlluminatedArtLight";
		constexpr float       kAmbient = 0.1f;  // a plain light's ambient: a tenth of its color (RE::Light's rule)

		struct Kept
		{
			RE::NiPointer<RE::NiRefObject>  owner;  // the art effect or the weapon part it is for (held: its address stays its own)
			RE::NiPointer<RE::NiNode>       root;   // the model it hangs on
			RE::NiPointer<RE::NiPointLight> light;
			RE::NiPointer<RE::BSLight>      bs;
			const RE::TESObjectLIGH*        record{ nullptr };
			bool                            seen{ false };
		};

		std::unordered_map<const void*, Kept>        gKept;         // main thread only
		std::unordered_map<std::string, std::string> gWeaponModel;  // a weapon's model as the configs key it (cache)
		RE::NiPointer<RE::NiPointLight>              gMaster;
		std::size_t                                  gMade = 0;

		RE::NiPointLight* CloneMaster()
		{
			if (!gMaster) {
				const RE::NiPointer<RE::NiPointLight> fresh(RE::NiPointLight::Create());
				auto*                                 clone = fresh ? netimmerse_cast<RE::NiPointLight*>(fresh->Clone()) : nullptr;
				if (!clone) {
					return nullptr;
				}
				gMaster.reset(clone);
			}
			return netimmerse_cast<RE::NiPointLight*>(gMaster->Clone());
		}

		void Drop(Kept& a_kept, RE::ShadowSceneNode* a_scene)
		{
			if (a_scene && a_kept.bs) {
				a_scene->RemoveLight(a_kept.bs);
			}
			if (a_kept.light && a_kept.light->parent) {
				a_kept.light->parent->DetachChild(a_kept.light.get());
			}
			a_kept = {};
		}

		// this frame's numbers of the record onto our light
		void Follow(Kept& a_kept)
		{
			auto&       data = a_kept.light->GetLightRuntimeData();
			const auto& c = a_kept.record->data.color;
			data.diffuse = { c.red / 255.0f, c.green / 255.0f, c.blue / 255.0f };
			data.fade = RecordFadeNow(a_kept.record, a_kept.light.get());
			const float radius = (std::max)(static_cast<float>(a_kept.record->data.radius), 1.0f);
			if (data.radius.x != radius) {
				data.radius = { radius, radius, LightKit::kLightSize };
				a_kept.light->SetLightAttenuation(radius);
			}
			if (InverseSquare()) {
				LightKit::Isl::SetCutoff(a_kept.light.get(), a_kept.record->data.fallofExponent, 0.01f, 1.0f);
			} else {
				data.ambient = { data.diffuse.red * kAmbient, data.diffuse.green * kAmbient, data.diffuse.blue * kAmbient };
			}
		}

		bool Make(Kept& a_kept, RE::NiNode* a_root, const RE::TESObjectLIGH* a_record, RE::ShadowSceneNode* a_scene)
		{
			auto* light = CloneMaster();
			if (!light) {
				return false;
			}
			light->name = kName;
			a_root->AttachChild(light, true);
			a_kept.light.reset(light);
			a_kept.root.reset(a_root);
			a_kept.record = a_record;
			if (InverseSquare()) {
				LightKit::Isl::SetOn(light);  // the flag Community Shaders reads (LightKit.h)
			}
			Follow(a_kept);
			RE::NiUpdateData update{};
			light->Update(update);
			RE::ShadowSceneNode::LIGHT_CREATE_PARAMS params{};  // Dynamic Wards' hand light (DomeLights.cpp Make)
			params.dynamic = true;
			params.shadowLight = false;
			params.portalStrict = (a_record->data.flags.underlying() & (1u << 13)) != 0;  // the row's PortalStrict
			params.affectLand = true;
			params.affectWater = true;
			params.neverFades = true;
			params.fov = 90.0f;
			params.falloff = 1.0f;
			params.nearDistance = 5.0f;
			params.depthBias = 1.0f;
			auto* bs = a_scene->AddLight(light, params);
			if (!bs) {
				a_root->DetachChild(light);
				a_kept = {};
				return false;
			}
			a_kept.bs.reset(bs);
			++gMade;
			return true;
		}

		// the light this owner wants on this model now (a_record nullptr: none)
		void Keep(RE::NiRefObject* a_owner, RE::NiAVObject* a_model, const RE::TESObjectLIGH* a_record, RE::ShadowSceneNode* a_scene)
		{
			auto* root = a_model ? a_model->AsNode() : nullptr;
			if (!a_owner || !root || !a_record) {
				return;
			}
			auto& kept = gKept[a_owner];
			if (kept.light && (kept.root.get() != root || kept.light->parent != root)) {
				Drop(kept, a_scene);  // the model was rebuilt (a draw, a view change): a new one
			}
			if (!kept.light) {
				kept.owner.reset(a_owner);
				if (!Make(kept, root, a_record, a_scene)) {
					return;
				}
			}
			kept.record = a_record;
			kept.seen = true;
			Follow(kept);
		}

		// a weapon's model path as the configs key it ("weapons\boundweapons\boundbow.nif")
		const std::string& WeaponModel(const RE::TESObjectWEAP* a_weapon)
		{
			const std::string key = std::format("{:08X}", a_weapon->GetFormID());
			auto [it, added] = gWeaponModel.try_emplace(key);
			if (added) {
				it->second = NormalPath(a_weapon->GetModel() ? a_weapon->GetModel() : "");
			}
			return it->second;
		}

		// the weapon's third-person 3D (the light lights the world from there in first person too)
		RE::NiAVObject* WeaponPart(RE::Actor* a_actor, const RE::TESObjectWEAP* a_weapon, bool a_left)
		{
			const auto& biped = a_actor->GetBiped(false);
			if (!biped) {
				return nullptr;
			}
			using B = RE::BIPED_OBJECT;
			if (a_left) {
				const auto& shield = biped->objects[B::kShield];
				return shield.item == a_weapon && shield.partClone ? shield.partClone.get() : nullptr;
			}
			for (auto slot = static_cast<std::uint32_t>(B::kHandToHandMelee); slot <= static_cast<std::uint32_t>(B::kCrossbow); ++slot) {
				const auto& obj = biped->objects[slot];
				if (obj.item == a_weapon && obj.partClone) {
					return obj.partClone.get();
				}
			}
			return nullptr;
		}

		// the weapon parts an art light already hangs under (measured 2026-10-10 on Vanilla: a bound sword's configs light both its
		// own model and its enchantment art - two lights on one blade; the art's is the one kept)
		std::unordered_set<const RE::NiAVObject*> gArtUnder;

		void VisitWeapons(RE::Actor* a_actor, RE::ShadowSceneNode* a_scene)
		{
			const auto* state = a_actor ? a_actor->AsActorState() : nullptr;
			if (!a_actor || !a_actor->Is3DLoaded() || !state || !state->IsWeaponDrawn()) {
				return;
			}
			for (const bool left : { false, true }) {
				auto* form = a_actor->GetEquippedObject(left);
				auto* weapon = form ? form->As<RE::TESObjectWEAP>() : nullptr;
				if (!weapon) {
					continue;
				}
				const auto& model = WeaponModel(weapon);
				auto*       record = model.empty() ? nullptr : RecordLightFor(model);
				if (!record) {
					continue;
				}
				if (auto* part = WeaponPart(a_actor, weapon, left); part && !gArtUnder.contains(part)) {
					Keep(part, part, record, a_scene);
				}
			}
		}
	}

	void TickArtLights()
	{
		if (!RecordRoute()) {
			return;
		}
		auto* scene = RE::BSShaderManager::State::GetSingleton().shadowSceneNode[0];
		if (!scene || gArtLightsOn.load(std::memory_order_relaxed) < 0.5f) {  // switched off in the advanced settings file
			for (auto& [key, kept] : gKept) {
				Drop(kept, scene);  // with a scene, each light leaves its lists too (CodeRabbit, Illuminated #6)
			}
			gKept.clear();
			return;
		}
		for (auto& [key, kept] : gKept) {
			kept.seen = false;
		}
		if (auto* lists = RE::ProcessLists::GetSingleton()) {
			lists->ForEachMagicTempEffect([&](RE::BSTempEffect* a_effect) {
				auto* art = a_effect ? netimmerse_cast<RE::ModelReferenceEffect*>(a_effect) : nullptr;
				if (art && !art->finished && art->artObject && art->artObject3D) {
					const auto model = NormalPath(art->artObject->GetModel() ? art->artObject->GetModel() : "");
					if (const auto* record = model.empty() ? nullptr : RecordLightFor(model)) {
						Keep(art, art->artObject3D.get(), record, scene);
					}
				}
				return RE::BSContainer::ForEachResult::kContinue;
			});
			gArtUnder.clear();
			for (const auto& [key, kept] : gKept) {
				if (kept.seen && kept.root) {
					for (const RE::NiAVObject* up = kept.root.get(); up && gArtUnder.size() < 4096; up = up->parent) {
						gArtUnder.insert(up);
					}
				}
			}
			auto* player = RE::PlayerCharacter::GetSingleton();
			VisitWeapons(player, scene);
			for (auto& handle : lists->highActorHandles) {
				if (auto actor = handle.get(); actor && actor.get() != player) {
					VisitWeapons(actor.get(), scene);
				}
			}
		}
		std::erase_if(gKept, [&](auto& a_entry) {
			if (!a_entry.second.seen) {
				Drop(a_entry.second, scene);
				return true;
			}
			return false;
		});
	}

	void DropArtLights()
	{
		auto* scene = RE::BSShaderManager::State::GetSingleton().shadowSceneNode[0];
		for (auto& [key, kept] : gKept) {
			Drop(kept, scene);
		}
		gKept.clear();
	}

	std::size_t ArtLightCount() { return gKept.size(); }
	std::size_t ArtLightsMade() { return gMade; }
}
