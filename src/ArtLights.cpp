// Illuminated - SKSE plugin
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE.txt and the notice at the top of main.cpp.
//
// Lights on ART and WEAPON models, without Light Placer (Vanilla and ENB). HIS REPORT: "bound weapons not lighting
// up on vanilla." A config that lights a model the game shows as an ART OBJECT (a bound weapon's enchantment art, an
// enchanted weapon's art) or as a weapon's own mesh (the bound bow) has no light record slot to point at: the game gives light
// records to magic effects, projectiles, explosions and hazards only (RecordLights.cpp). Light Placer hangs a light on such a
// model by itself; without it nothing did. So here, once a frame on the main thread: every magic art effect in the world whose
// model a config row lights now (RecordLightFor - the settings' tests), and every drawn weapon whose own model one lights, gets
// a light of ours on that model - in the record light's color, at its strength and reach (so the sliders, daylight and the
// flicker reach it), made the way Dynamic Wards and GroundLights.cpp make theirs (land lighting on). The light hangs under the
// art's or the weapon's 3D, so the fading module finds and fades it like any other weapon light (a bound weapon over the last
// seconds of its spell). It goes when the art or the weapon goes, or a setting stops lighting its model.
// The flesh spells the same way: their configs light an effect SHADER on the caster's body (Oakflesh ... Dragonhide, LightMath.h
// kFleshLooks), which has no light record slot either, and without Light Placer nothing lit them. Each flesh shader effect
// gets its row's light on the body (the effect's own attach root, the third-person one - a light under the first-person
// skeleton lights nothing on ENB), switched off while its caster sneaks as the row's sneak test says, and drawn in FleshFX's
// hue where FleshFX's look wins (FleshLights.cpp has why). Only the flesh shaders are followed as effects; a drawn weapon's
// enchantment SHADER (the blade's glow) is reached through the weapon instead (VisitWeapons, EnchantShaderLight).
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
			const RE::NiColor*              hue{ nullptr };  // FleshFX's hue for a flesh spell's light (LightKit.h), or none
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
			auto&             data = a_kept.light->GetLightRuntimeData();
			const auto&       c = a_kept.record->data.color;
			const RE::NiColor own{ c.red / 255.0f, c.green / 255.0f, c.blue / 255.0f };
			data.diffuse = a_kept.hue ? LightKit::FleshFx::Shown(*a_kept.hue, own) : own;
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
		void Keep(RE::NiRefObject* a_owner, RE::NiAVObject* a_model, const RE::TESObjectLIGH* a_record, RE::ShadowSceneNode* a_scene,
			const RE::NiColor* a_hue = nullptr)
		{
			auto* root = a_model ? a_model->AsNode() : nullptr;
			if (!a_owner || !root || !a_record) {
				return;
			}
			auto& kept = gKept[a_owner];
			if (kept.light && (kept.root.get() != root || kept.light->parent != root)) {
				Drop(kept, a_scene);  // the model was rebuilt (a draw, a view change): a new one
			}
			kept.hue = a_hue;  // before Make, which draws the light's first color
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

		// the five flesh shaders in this load order (LightKit::FleshFx::Resolved, found at data load) and the key their config
		// rows are kept under (Configs.cpp: "formid:" + the editor ID the configs claim) - built on the first frame
		const std::unordered_map<const RE::TESEffectShader*, std::string>& FleshShaders()
		{
			static const auto shaders = [] {
				std::unordered_map<const RE::TESEffectShader*, std::string> out;
				for (const auto& [shader, look] : LightKit::FleshFx::Resolved()) {
					out.emplace(shader, FormKey(Lower(look->editorID)));
				}
				return out;
			}();
			return shaders;
		}

		// a flesh spell's light on its caster's body, while a setting lights its row (RecordLightFor - the Flesh Spells switch,
		// full strength, Dynamic Lighting) and the caster is not sneaking with the Sneaking setting off (the row's own test)
		void KeepFlesh(RE::ShaderReferenceEffect* a_effect, RE::ShadowSceneNode* a_scene)
		{
			const auto& shaders = FleshShaders();
			const auto  it = a_effect->effectData ? shaders.find(a_effect->effectData) : shaders.end();
			const auto* record = it != shaders.end() ? RecordLightFor(it->second) : nullptr;
			const auto  ref = record ? a_effect->target.get() : RE::NiPointer<RE::TESObjectREFR>{};
			auto*       root = ref ? FleshBodyNode(ref.get(), a_effect->GetAttachRoot()) : nullptr;
			if (!root) {
				return;
			}
			if (const auto* actor = ref->As<RE::Actor>(); actor && actor->IsSneaking() && SettingValue("IlluminatedSneaking", 0) == 0) {
				return;
			}
			Keep(a_effect, root, record, a_scene, LightKit::FleshFx::HueOf(a_effect->effectData));
		}

		// the weapon parts an art light already hangs under (measured on Vanilla: a bound sword's configs light both its
		// own model and its enchantment art - two lights on one blade; the art's is the one kept)
		std::unordered_set<const RE::NiAVObject*> gArtUnder;

		// the enchantment on the weapon in this hand: the one made at the table (ExtraEnchantment on the WORN copy - the entry can
		// hold other copies of the same weapon, CodeRabbit's point on #13), else the weapon record's own
		RE::EnchantmentItem* HeldEnchantment(RE::Actor* a_actor, const RE::TESObjectWEAP* a_weapon, bool a_left)
		{
			if (auto* entry = a_actor->GetEquippedEntryData(a_left); entry && entry->extraLists) {
				for (auto* list : *entry->extraLists) {
					if (!list || !(a_left ? list->HasType<RE::ExtraWornLeft>() : list->HasType<RE::ExtraWorn>())) {
						continue;
					}
					if (const auto* xEnch = list->GetByType<RE::ExtraEnchantment>(); xEnch && xEnch->enchantment) {
						return xEnch->enchantment;
					}
					break;  // the worn copy carries no enchantment of its own
				}
			}
			return a_weapon->formEnchanting;
		}

		// every magic effect's enchantment shader -> the key its config rows are kept under (Configs.cpp: "formid:" + its editor
		// ID), noted at load (NoteEnchantShaders) - the game keeps no effect shader's editor ID, and ours are forgotten once
		// the passes are done (Fade::ForgetPassEditorIDs). Measured 2026-10-13 on Vanilla: asked of the game at run time the
		// name was empty and the Ebony Sword of Storms stayed dark.
		std::unordered_map<const RE::TESEffectShader*, std::string> gEnchShaderKey;

		// the light row of a weapon enchantment's SHADER (the blade's glow - Shock / Fire / Frost Damage and every other
		// enchantment without art of its own). His word 2026-10-13: "yes make the weapon light fix for vanilla and enb" - with
		// Light Placer these rows hang on the blade by the shader's form; without it nothing lit them (only art and a weapon's
		// own model were followed here), so an ordinary enchanted weapon stayed dark on Vanilla and ENB. The effect the game SHOWS
		// first (the costliest - its shader is the one on the blade, as Enchantments.cpp keeps it), then the first effect whose
		// shader has a row that holds now
		RE::TESObjectLIGH* ShaderRowLight(const RE::Effect* a_effect)
		{
			const auto* shader = a_effect && a_effect->baseEffect ? a_effect->baseEffect->data.enchantShader : nullptr;
			if (!shader) {
				return nullptr;
			}
			const auto it = gEnchShaderKey.find(shader);
			return it == gEnchShaderKey.end() ? nullptr : RecordLightFor(it->second);
		}

		RE::TESObjectLIGH* EnchantShaderLight(RE::EnchantmentItem* a_ench)
		{
			if (!a_ench) {
				return nullptr;
			}
			if (auto* record = ShaderRowLight(a_ench->GetCostliestEffectItem())) {
				return record;
			}
			for (const auto* effect : a_ench->effects) {
				if (auto* record = ShaderRowLight(effect)) {
					return record;
				}
			}
			return nullptr;
		}

		void VisitWeapons(RE::Actor* a_actor, RE::ShadowSceneNode* a_scene)
		{
			const auto* state = a_actor ? a_actor->AsActorState() : nullptr;
			if (!a_actor || !a_actor->Is3DLoaded() || !state || !state->IsWeaponDrawn()) {
				return;
			}
			// the weapon rows' own sneak test (as KeepFlesh does for the flesh rows)
			if (a_actor->IsSneaking() && SettingValue("IlluminatedSneaking", 0) == 0) {
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
					record = EnchantShaderLight(HeldEnchantment(a_actor, weapon, left));
				}
				if (!record) {
					continue;
				}
				if (auto* part = WeaponPart(a_actor, weapon, left); part && !gArtUnder.contains(part)) {
					Keep(part, part, record, a_scene);
				}
			}
		}
	}

	void NoteEnchantShaders()
	{
		gEnchShaderKey.clear();
		for (const auto* effect : LightKit::FormsOf<RE::EffectSetting>()) {
			const auto* shader = effect ? effect->data.enchantShader : nullptr;
			if (!shader || gEnchShaderKey.contains(shader)) {
				continue;
			}
			if (const auto id = EditorID(shader); !id.empty()) {
				gEnchShaderKey.emplace(shader, FormKey(Lower(id)));
			}
		}
		SKSE::log::info("art lights: {} enchantment shader(s) noted for the drawn-weapon lights", gEnchShaderKey.size());
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
				} else if (auto* shader = art ? nullptr : netimmerse_cast<RE::ShaderReferenceEffect*>(a_effect); shader && !shader->finished) {
					KeepFlesh(shader, scene);
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
