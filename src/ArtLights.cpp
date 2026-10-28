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
// art's or the weapon's 3D - in first person on the body node, noted as that hand's (Fade::NoteHandLight) - so the fading
// module and Lights by Item reach it like any other weapon light (a bound weapon over the last seconds of its spell). It goes
// when the art or the weapon goes, or a setting stops lighting its model.
// The flesh spells the same way: their configs light an effect SHADER on the caster's body (Oakflesh ... Dragonhide, LightMath.h
// kFleshLooks), which has no light record slot either, and without Light Placer nothing lit them. Each flesh shader effect
// gets its row's light on the body (the effect's own attach root, the third-person one - a light under the first-person
// skeleton lights nothing on ENB), faded while its caster sneaks (SneakFade.cpp), and drawn in FleshFX's
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
			RE::NiPointer<RE::NiRefObject>   owner;  // the art effect or the weapon part it is for (held: its address stays its own)
			RE::NiPointer<RE::NiNode>        root;   // the model it hangs on
			RE::NiPointer<RE::NiPointLight>  light;
			RE::NiPointer<RE::BSLight>       bs;
			const RE::TESObjectLIGH*         record{ nullptr };
			const RE::NiColor*               hue{ nullptr };     // FleshFX's hue for a flesh spell's light (LightKit.h), or none
			const RE::ShaderReferenceEffect* shader{ nullptr };  // a body shader's effect, held by owner: FrozenFade reads its age
			bool                             seen{ false };
		};

		std::unordered_map<const void*, Kept> gKept;  // main thread only
		// a weapon's / an art object's model as the configs key it, worked out once per form and model (main thread); the
		// model text rides along, so a form made at the address of a freed one is worked out again, never read as the old one
		struct ModelKeyOf
		{
			std::string model, key;
		};
		std::unordered_map<const RE::TESForm*, ModelKeyOf> gModelKey;
		std::atomic<std::size_t>                           gMade{ 0 };       // the testing DLL's Diagnostics page reads these two
		std::atomic<std::size_t>                           gKeptCount{ 0 };  // from the render thread

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
			const auto        turn = LightKit::Turn();
			auto&             data = a_kept.light->GetLightRuntimeData();
			const auto&       c = a_kept.record->data.color;
			const RE::NiColor own{ c.red / 255.0f, c.green / 255.0f, c.blue / 255.0f };
			data.diffuse = a_kept.hue ? LightKit::FleshFx::Shown(*a_kept.hue, own) : own;
			data.fade = RecordFadeNow(a_kept.record, a_kept.light.get()) * FrozenFade(a_kept.shader);
			const float radius = (std::max)(static_cast<float>(a_kept.record->data.radius), 1.0f);
			if (data.radius.x != radius) {
				data.radius = { radius, radius, LightKit::kLightSize };
				a_kept.light->SetLightAttenuation(radius);
			}
			if (InverseSquare()) {
				// after SetLightAttenuation, which writes the same two words: a light of our own, its words written whole (LightKit.h);
				// the record's cutoff when it is an inverse-square light, else the cutoff that reaches its radius at this fade (a
				// plain record's falloff exponent is no cutoff)
				const bool isl = (a_kept.record->data.flags.underlying() & LightKit::kRecordInverseSquare) != 0;
				LightKit::Isl::SetOwn(a_kept.light.get());
				LightKit::Isl::SetCutoff(a_kept.light.get(), isl ? a_kept.record->data.fallofExponent : LightKit::CutoffFor(data.fade, radius), 0.01f, 1.0f);
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
			light->GetLightRuntimeData().radius.x = -1.0f;  // the clone's own may equal the record's: Follow attenuates it at once
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
			const RE::NiColor* a_hue = nullptr, const RE::ShaderReferenceEffect* a_shader = nullptr)
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
			kept.shader = a_shader;
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

		// the body shaders followed here and the key their config rows are kept under (Configs.cpp: "formid:" + the editor ID
		// the configs claim): the five flesh shaders in this load order (LightKit::FleshFx::Resolved) and FEC's lit ones
		// (FleshLights.cpp NoteFecShaders), both found at data load - built on the first frame
		const std::unordered_map<const RE::TESEffectShader*, std::string>& FleshShaders()
		{
			static const auto shaders = [] {
				std::unordered_map<const RE::TESEffectShader*, std::string> out(FecShaders());
				for (const auto& [shader, look] : LightKit::FleshFx::Resolved()) {
					out.emplace(shader, FormKey(Lower(look->editorID)));
				}
				return out;
			}();
			return shaders;
		}

		// a flesh spell's light on its caster's body, while a setting lights its row (RecordLightFor - the Flesh Spells switch,
		// full strength, Dynamic Lighting); a sneaking caster's fades (SneakFade.cpp)
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
			Keep(a_effect, root, record, a_scene, LightKit::FleshFx::HueOf(a_effect->effectData), a_effect);
		}

		// the weapon parts an art light already hangs under (measured on Vanilla: a bound sword's configs light both its
		// own model and its enchantment art - two lights on one blade; the art's is the one kept)
		std::unordered_set<const RE::NiAVObject*> gArtUnder;

		// the hand a node sits under (itself when it is one), or nullptr
		[[nodiscard]] RE::NiAVObject* HandAbove(RE::NiAVObject* a_node)
		{
			for (auto* up = a_node; up; up = up->parent) {
				if (std::string_view(up->name.c_str()).find("Hand [") != std::string_view::npos) {
					return up;
				}
			}
			return nullptr;
		}

		// In first person the third-person arms are not posed: the right hand and its WEAPON node sat on one spot 94 above her
		// feet, and a light there lit nothing on the floor (bound battleaxe, Brightness 200 %). The third-person MagicEffectsNode
		// on the spine does light it (a first-person cloak +41), so a light for what she holds hangs there in first person.
		[[nodiscard]] RE::NiAVObject* BodyNode(RE::NiAVObject* a_third)
		{
			static const RE::BSFixedString kEffectsNode{ "MagicEffectsNode" };
			auto*                          node = a_third ? a_third->GetObjectByName(kEffectsNode) : nullptr;
			return node ? node : a_third;
		}

		// the third-person hands whose art light went to the body this frame: their weapon is lit already
		std::unordered_set<const RE::NiAVObject*> gHandsOnBody;

		// a light that went to the body node for a hand: for the fading module, once the effects' walk is done (never under
		// the game's effect lock)
		struct OnBody
		{
			const void*      owner{ nullptr };
			const RE::Actor* actor{ nullptr };
			bool             left{ false };
		};
		std::vector<OnBody> gOnBody;

		[[nodiscard]] bool LeftHand(const RE::NiAVObject* a_hand)
		{
			return a_hand && std::string_view(a_hand->name.c_str()).find("L Hand") != std::string_view::npos;
		}

		// The node an art light hangs on: the art's own model, or - when the game put the art on an actor's first-person
		// skeleton (the player in first person: a cloak, a bound weapon's art) - the third-person node of the same name as the
		// one the art hangs under, as RELight's body lights do (the body node for one on an arm). A light under the first-person
		// skeleton lights nothing in the world (Dynamic Wards 3.1), so in first person those lights were dark while third person lit.
		[[nodiscard]] RE::NiAVObject* ArtNode(RE::ModelReferenceEffect* a_art, const RE::NiAVObject*& a_hand)
		{
			a_hand = nullptr;
			auto*       art = a_art->artObject3D.get();
			const auto  ref = a_art->target.get();
			auto* const actor = ref ? ref->As<RE::Actor>() : nullptr;
			auto* const first = actor ? actor->Get3D(true) : nullptr;
			auto* const third = actor ? actor->Get3D(false) : nullptr;
			if (!art || !first || !third || first == third) {
				return art;
			}
			bool underFirst = false;
			for (const RE::NiAVObject* up = art->parent; up && !underFirst; up = up->parent) {
				underFirst = up == first;
			}
			if (!underFirst) {
				// in first person a third-person hand is not posed either: art the game left on one goes to the body as well
				// (CodeRabbit, Illuminated #37: its light sat on the unposed hand and held the weapon's light off)
				const auto* camera = RE::PlayerCamera::GetSingleton();
				auto*       hand = actor->IsPlayerRef() && camera && camera->IsInFirstPerson() ? HandAbove(art) : nullptr;
				if (!hand) {
					return art;
				}
				gHandsOnBody.insert(hand);
				a_hand = hand;
				return BodyNode(third);
			}
			auto*       attach = art->parent;
			const char* name = attach ? attach->name.c_str() : nullptr;
			auto*       same = name && *name ? third->GetObjectByName(attach->name) : nullptr;
			// the third-person hand that holds what the art is on: above its namesake, or (no namesake) the namesake of the
			// first-person hand above it - so the weapon in that hand is not lit a second time (CodeRabbit, Illuminated #37)
			RE::NiAVObject* hand = same ? HandAbove(same) : nullptr;
			if (!same) {
				const auto* firstHand = HandAbove(attach);
				hand = firstHand ? third->GetObjectByName(firstHand->name) : nullptr;
			}
			if (hand) {
				gHandsOnBody.insert(hand);
				a_hand = hand;
				return BodyNode(third);
			}
			return same ? same : third;
		}

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
					// same weapon, CodeRabbit's point on #13 - else the weapon record's). ReadHand counts a bound weapon's time
					// and reads no enchantment, so a bound weapon on an ordinary model (Odin's) takes its record's own
					const auto* ench = Fade::ReadHand(a_actor, left).ench;
					record = EnchantShaderLight(ench ? ench : weapon->IsBound() ? weapon->formEnchanting :
																				  nullptr);
				}
				if (!record) {
					continue;
				}
				// the weapon's third-person 3D; in first person the body node, as for art (ArtNode: the arms are not posed there)
				auto*       part = Fade::WeaponPart(a_actor, false, left, weapon);
				const auto* hand = part ? HandAbove(part) : nullptr;
				if (part && !gArtUnder.contains(part) && !gArtUnder.contains(hand) && !gHandsOnBody.contains(hand)) {
					const auto* camera = RE::PlayerCamera::GetSingleton();
					const bool  firstPerson = a_actor->IsPlayerRef() && camera && camera->IsInFirstPerson();
					Keep(part, firstPerson ? BodyNode(a_actor->Get3D(false)) : part, record, a_scene);
					if (firstPerson) {
						gOnBody.push_back({ part, a_actor, left });
					}
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
		auto* scene = LightKit::Scene();
		if (!scene || gArtLightsOn.load(std::memory_order_relaxed) < 0.5f || LightsOff()) {  // switched off (advanced settings file, or all lights off)
			for (auto& [key, kept] : gKept) {
				Drop(kept, scene);  // with a scene, each light leaves its lists too (CodeRabbit, Illuminated #6)
			}
			gKept.clear();
			gKeptCount = 0;
			return;
		}
		for (auto& [key, kept] : gKept) {
			kept.seen = false;
		}
		gOnBody.clear();
		if (auto* lists = RE::ProcessLists::GetSingleton()) {
			gHandsOnBody.clear();
			lists->ForEachMagicTempEffect([&](RE::BSTempEffect* a_effect) {
				auto* art = a_effect ? netimmerse_cast<RE::ModelReferenceEffect*>(a_effect) : nullptr;
				if (art && !art->finished && art->artObject && art->artObject3D) {
					const auto& model = ModelKey(art->artObject, art->artObject->GetModel());
					if (const auto* record = model.empty() ? nullptr : RecordLightFor(model)) {
						const RE::NiAVObject* hand = nullptr;
						Keep(art, ArtNode(art, hand), record, scene);
						const auto  target = hand ? art->target.get() : RE::NiPointer<RE::TESObjectREFR>{};
						const auto* actor = target ? target->As<RE::Actor>() : nullptr;
						if (actor) {
							gOnBody.push_back({ art, actor, LeftHand(hand) });
						}
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
		gKeptCount = gKept.size();
		// the lights that went to the body for a hand, now that the walk is over: the fading module and Lights by Item reach
		// them as that hand's (both run after this in the player update)
		for (const auto& b : gOnBody) {
			if (const auto it = gKept.find(b.owner); it != gKept.end() && it->second.light) {
				Fade::NoteHandLight(b.actor, b.left, it->second.light.get());
			}
		}
	}

	void DropArtLights()
	{
		auto* scene = LightKit::Scene();
		for (auto& [key, kept] : gKept) {
			Drop(kept, scene);
		}
		gKept.clear();
		gKeptCount = 0;
		gModelKey.clear();  // keyed by form address: a form made in memory and freed could hand its address to another
	}

	std::size_t ArtLightCount() { return gKeptCount.load(); }
	std::size_t ArtLightsMade() { return gMade.load(); }
}
