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

#include "Fade.h"
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

		std::unordered_map<const void*, Kept> gKept;  // main thread only
		// a weapon's / an art object's model as the configs key it, worked out once per form and model (main thread); the
		// model text rides along, so a form made at the address of a freed one is worked out again, never read as the old one
		struct ModelKeyOf
		{
			std::string model, key;
		};
		std::unordered_map<const RE::TESForm*, ModelKeyOf> gModelKey;
		std::size_t                                        gMade = 0;

		[[nodiscard]] const std::string& ModelKey(const RE::TESForm* a_form, const char* a_model)
		{
			const std::string_view model = a_model ? a_model : "";
			auto [it, added] = gModelKey.try_emplace(a_form);
			if (added || it->second.model != model) {
				it->second.model.assign(model);
				it->second.key = NormalPath(it->second.model.c_str());
			}
			return it->second.key;
		}

		// the rows' own sneak test (gen.py's While Sneaking clause): a caster who sneaks with the Sneaking setting off keeps
		// none of these lights
		[[nodiscard]] bool SneakHides(const RE::TESObjectREFR* a_ref)
		{
			const auto* actor = a_ref ? a_ref->As<RE::Actor>() : nullptr;
			return actor && actor->IsSneaking() && SettingValue("IlluminatedSneaking", 0) == 0;
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
				// after SetLightAttenuation, which writes the same two words: a light of our own, its words written whole (LightKit.h)
				LightKit::Isl::SetOwn(a_kept.light.get());
				LightKit::Isl::SetCutoff(a_kept.light.get(), a_kept.record->data.fallofExponent, 0.01f, 1.0f);
			} else {
				data.ambient = { data.diffuse.red * kAmbient, data.diffuse.green * kAmbient, data.diffuse.blue * kAmbient };
			}
		}

		bool Make(Kept& a_kept, RE::NiNode* a_root, const RE::TESObjectLIGH* a_record, RE::ShadowSceneNode* a_scene)
		{
			auto* light = LightKit::CloneLight();
			if (!light) {
				return false;
			}
			light->name = kName;
			a_root->AttachChild(light, true);
			a_kept.light.reset(light);
			a_kept.root.reset(a_root);
			a_kept.record = a_record;
			Follow(a_kept);
			RE::NiUpdateData update{};
			light->Update(update);
			const bool portalStrict = (a_record->data.flags.underlying() & (1u << 13)) != 0;  // the row's PortalStrict
			auto*      bs = LightKit::AddToScene(a_scene, light, 1.0f, portalStrict);
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
			if (SneakHides(ref.get())) {
				return;
			}
			Keep(a_effect, root, record, a_scene, LightKit::FleshFx::HueOf(a_effect->effectData));
		}

		// the weapon parts an art light already hangs under (measured on Vanilla: a bound sword's configs light both its
		// own model and its enchantment art - two lights on one blade; the art's is the one kept)
		std::unordered_set<const RE::NiAVObject*> gArtUnder;

		// every magic effect's enchantment shader -> the key its config rows are kept under (Configs.cpp: "formid:" + its editor
		// ID), noted at load (NoteEnchantShaders) - the game keeps no effect shader's editor ID, and ours are forgotten once
		// the passes are done (Fade::ForgetPassEditorIDs). Measured 2026-10-13 on Vanilla: asked of the game at run time the
		// name was empty and the Ebony Sword of Storms stayed dark.
		std::unordered_map<const RE::TESEffectShader*, std::string> gEnchShaderKey;

		// the light row of a weapon enchantment's SHADER (the blade's glow - Shock / Fire / Frost Damage and every other
		// enchantment without art of its own). His word 2026-10-13: "yes make the weapon light fix for vanilla and enb" - with
		// Light Placer these rows hang on the blade by the shader's form; without it nothing lit them (only art and a weapon's
		// own model were followed here), so an ordinary enchanted weapon stayed dark on Vanilla and ENB. The shader the game
		// plays on the blade is the costliest effect's (measured on Chillrend, 2026-10-13 and the E2 test 2026-10-16), so that effect's row comes
		// first, then the first effect whose shader has a row that holds now
		RE::TESObjectLIGH* ShaderRowLight(const RE::Effect* a_effect)
		{
			const auto* shader = a_effect && a_effect->baseEffect ? a_effect->baseEffect->data.enchantShader : nullptr;
			if (!shader) {
				return nullptr;
			}
			const auto it = gEnchShaderKey.find(shader);
			return it == gEnchShaderKey.end() ? nullptr : RecordLightFor(it->second);
		}

		RE::TESObjectLIGH* EnchantShaderLight(const RE::EnchantmentItem* a_ench)
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
			if (SneakHides(a_actor)) {
				return;
			}
			for (const bool left : { false, true }) {
				auto* form = a_actor->GetEquippedObject(left);
				auto* weapon = form ? form->As<RE::TESObjectWEAP>() : nullptr;
				if (!weapon) {
					continue;
				}
				const auto& model = ModelKey(weapon, weapon->GetModel());
				auto*       record = model.empty() ? nullptr : RecordLightFor(model);
				if (!record) {
					// the held instance's own (FadeCharge.cpp ReadHand: the WORN copy's - the entry can hold other copies of the
					// same weapon, CodeRabbit's point on #13 - else the weapon record's)
					record = EnchantShaderLight(Fade::ReadHand(a_actor, left).ench);
				}
				if (!record) {
					continue;
				}
				// the weapon's third-person 3D: the light lights the world from there in first person too
				if (auto* part = Fade::WeaponPart(a_actor, false, left, weapon); part && !gArtUnder.contains(part)) {
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
		if (!scene || gArtLightsOn.load(std::memory_order_relaxed) < 0.5f || LightsOff()) {  // switched off (advanced settings file, or all lights off)
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
				if (art && !art->finished && art->artObject && art->artObject3D && !SneakHides(art->target.get().get())) {
					const auto& model = ModelKey(art->artObject, art->artObject->GetModel());
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
		gModelKey.clear();  // keyed by form address: a form made in memory and freed could hand its address to another
	}

	std::size_t ArtLightCount() { return gKept.size(); }
	std::size_t ArtLightsMade() { return gMade; }
}
