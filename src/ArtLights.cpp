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
// a light of ours on that model - in the record light's color, at its strength and reach (so the sliders and the
// flicker reach it), made the way Dynamic Wards and GroundLights.cpp make theirs (land lighting on). The light hangs under the
// art's or the weapon's 3D - in first person on the body node, noted as that hand's (Fade::NoteHandLight) - so the fading
// module and Lights by Item reach it like any other weapon light (a bound weapon over the last seconds of its spell). It goes
// when the art or the weapon goes, or a setting stops lighting its model.
// The flesh spells the same way: their configs light an effect SHADER on the caster's body (Oakflesh ... Dragonhide, LightMath.h
// kFleshLooks), which has no light record slot either, and without Light Placer nothing lit them. Each flesh shader effect
// gets its row's light on the body (the effect's own attach root, the third-person one - a light under the first-person
// skeleton lights nothing on ENB), and drawn in FleshFX's
// hue where FleshFX's look wins (FleshLights.cpp has why). Only the flesh shaders are followed as effects; a drawn weapon's
// enchantment SHADER (the blade's glow) is reached through the weapon instead (CollectWeapons, EnchantShaderLight).
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
			const RE::ShaderReferenceEffect* shader{ nullptr };  // a body shader's effect, held by owner: FecBodyFade reads its age
			bool                             seen{ false };
			bool                             hers{ false };  // on the player: never put out by the cap
			std::uint32_t                    wait{ 0 };      // frames until a light the scene refused is made again
		};
		// the scene refused a light (its light limit): made again after this many frames, never a clone every frame (L10)
		constexpr std::uint32_t kRefusedWait = 60;

		std::unordered_map<const void*, Kept> gKept;  // main thread only
		// a weapon's / an art object's model as the configs key it, worked out once per form and model (main thread); the
		// model text rides along, so a form made at the address of a freed one is worked out again, never read as the old one
		struct ModelKeyOf
		{
			std::string model, key;
		};
		std::unordered_map<const RE::TESForm*, ModelKeyOf> gModelKey;
		// a shield's models as the configs key them, once per form: its world model, then its addons' worn models (Mysticism's
		// Bound Shield wears the Daedric shield in the world and its own model on the arm); the world model text rides along as above
		struct ShieldKeysOf
		{
			std::string              model;
			std::vector<std::string> keys;
		};
		std::unordered_map<const RE::TESForm*, ShieldKeysOf> gShieldKeys;
		std::atomic<std::size_t>                             gMade{ 0 };       // the testing DLL's Diagnostics page reads these two
		std::atomic<std::size_t>                             gKeptCount{ 0 };  // from the render thread
		std::atomic<std::size_t>                             gLitCount{ 0 };
		// the Performance cap on glows on people (his checklist 336): how many of gKept are lit now, the cap this frame
		std::size_t gLit{ 0 };
		std::size_t gCap{ (std::numeric_limits<std::size_t>::max)() };
		bool        gCapLogged{ false };

		// every effect in the world is walked only when an art starts or resumes (LightCopies.cpp ArtInit, ArtResume - only a flag:
		// an effect kept by reference from its Init can be destroyed all the same), after a settings change and at a load. Between
		// walks the effects a row lit at the last walk are visited, under the game's effect lock
		std::atomic<bool>                            gArtLook{ true };
		std::vector<RE::NiPointer<RE::BSTempEffect>> gArts;  // main thread only

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

		[[nodiscard]] const std::vector<std::string>& ShieldKeys(const RE::TESObjectARMO* a_shield)
		{
			const char*            world = a_shield->worldModels[RE::SEXES::kMale].GetModel();
			const std::string_view model = world ? world : "";
			auto [it, added] = gShieldKeys.try_emplace(a_shield);
			if (added || it->second.model != model) {
				it->second.model.assign(model);
				auto& keys = it->second.keys;
				keys.clear();
				const auto add = [&](const char* a_path) {
					if (!a_path || !*a_path) {
						return;
					}
					if (auto key = NormalPath(a_path); !key.empty() && std::ranges::find(keys, key) == keys.end()) {
						keys.push_back(std::move(key));
					}
				};
				add(world);
				for (const auto* addon : a_shield->armorAddons) {
					if (addon) {
						for (const auto& worn : addon->bipedModels) {
							add(worn.GetModel());
						}
					}
				}
			}
			return it->second.keys;
		}

		void Drop(Kept& a_kept, RE::ShadowSceneNode* a_scene)
		{
			// the cap counts glows on others only: hers never take one of their places
			if (a_kept.light && !a_kept.hers && gLit > 0) {
				--gLit;
			}
			LightKit::TakeOff(a_scene, a_kept.bs);
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
			data.fade = RecordFadeNow(a_kept.record, a_kept.light.get()) * FecBodyFade(a_kept.shader);
			const float radius = (std::max)(static_cast<float>(a_kept.record->data.radius), 1.0f);
			const bool  isl = InverseSquare() && (a_kept.record->data.flags.underlying() & LightKit::kRecordInverseSquare) != 0;
			// an inverse-square record's field of view is its row's size (RecordLights.cpp): the peak goes by 1 / size^2
			data.radius.z = isl ? LightKit::LightPlacerSize(a_kept.record->data.fov) : LightKit::kLightSize;
			if (data.radius.x != radius) {
				data.radius.x = radius;
				data.radius.y = radius;
				a_kept.light->SetLightAttenuation(radius);
			}
			if (InverseSquare()) {
				// after SetLightAttenuation, which writes the same two words: a light of our own, its words written whole (LightKit.h);
				// the record's cutoff when it is an inverse-square light, else the cutoff that reaches its radius at this fade (a
				// plain record's falloff exponent is no cutoff)
				LightKit::Isl::SetOwn(a_kept.light.get());
				LightKit::Isl::SetCutoff(a_kept.light.get(), isl ? a_kept.record->data.fallofExponent : LightKit::CutoffFor(data.fade, radius), 0.01f, 1.0f);
			} else if (LightLimitFix()) {
				LightKit::Isl::SetOwnLinear(a_kept.light.get());
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
			// a light on her goes through doorways with her: portal strict, it lit only the room's own meshes, never her or the
			// people beside her indoors
			auto* bs = LightKit::AddToScene(a_scene, light, 1.0f, false);
			if (!bs) {
				a_root->DetachChild(light);
				a_kept = {};
				return false;
			}
			a_kept.bs.reset(bs);
			++gMade;
			gLit += a_kept.hers ? 0 : 1;
			return true;
		}

		// the light this owner wants on this model now (a_record nullptr: none); a_hers: on the player, never held back by the cap
		void Keep(RE::NiRefObject* a_owner, RE::NiAVObject* a_model, const RE::TESObjectLIGH* a_record, RE::ShadowSceneNode* a_scene,
			bool a_hers, const RE::NiColor* a_hue = nullptr, const RE::ShaderReferenceEffect* a_shader = nullptr)
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
			kept.hers = a_hers;
			if (!kept.light) {
				if (kept.wait) {
					--kept.wait;
					kept.seen = true;  // kept while it waits, so the sweep does not start it over
					return;
				}
				if (ConfigText::GlowWaits(gLit, gCap, a_hers)) {
					// past the Performance cap: kept, waiting, until a lit one goes out
					if (!std::exchange(gCapLogged, true)) {
						SKSE::log::info("art lights: {} glows on others are lit, the most this lighting takes - the rest wait for one to go out", gCap);
					}
					kept.seen = true;
					return;
				}
				kept.owner.reset(a_owner);
				if (!Make(kept, root, a_record, a_scene)) {
					kept.wait = kRefusedWait;
					kept.seen = true;
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
		// full strength, Dynamic Lighting). True while a row lights it, so it is visited
		// again between walks - faded out or with its body not loaded too (the Stay pick or the 3D brings it back)
		bool KeepFlesh(RE::ShaderReferenceEffect* a_effect, RE::ShadowSceneNode* a_scene)
		{
			const auto& shaders = FleshShaders();
			const auto  it = a_effect->effectData ? shaders.find(a_effect->effectData) : shaders.end();
			const auto* record = it != shaders.end() ? RecordLightFor(it->second) : nullptr;
			if (!record) {
				return false;
			}
			if (FecBodyFade(a_effect) <= 0.0f) {
				return true;  // faded out on its pick: not seen, so the sweep drops it and its place under the glow cap
			}
			const auto ref = a_effect->target.get();
			auto*      root = ref ? FleshBodyNode(ref.get(), a_effect->GetAttachRoot()) : nullptr;
			if (root) {
				Keep(a_effect, root, record, a_scene, ref.get() == RE::PlayerCharacter::GetSingleton(), LightKit::FleshFx::HueOf(a_effect->effectData), a_effect);
			}
			return true;
		}

		// the weapon parts an art light already hangs under (measured on Vanilla: a bound sword's configs light both its
		// own model and its enchantment art - two lights on one blade; the art's is the one kept)
		std::vector<const RE::NiAVObject*> gArtUnder;  // sorted; made at a look, its storage kept

		[[nodiscard]] bool ArtUnder(const RE::NiAVObject* a_node) { return std::ranges::binary_search(gArtUnder, a_node); }

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

		// a drawn weapon's light as the last look worked it out: what it hangs on, its row. Between looks each is only kept and followed
		struct WeaponWant
		{
			RE::NiPointer<RE::NiAVObject> part;
			RE::NiPointer<RE::NiAVObject> node;
			const RE::TESObjectLIGH*      record{ nullptr };
			const RE::Actor*              actor{ nullptr };  // read only while the list of drawn actors stays the one it came from
			bool                          left{ false }, firstPerson{ false }, hers{ false };
		};
		std::vector<WeaponWant> gWeaponWants;  // main thread only
		bool                    gWeaponWaits{ false };
		// a look for a few frames after what can change a want: a weapon's part can come a moment after its equip
		constexpr int kWeaponLooks = 8;
		constexpr int kWeaponWaitLooks = 300;
		// what the last look saw; the pass rests while these stand and nothing is kept, visited or announced
		std::vector<RE::Actor*> gDrawnWas;
		std::size_t             gKeptWas = 0;
		int                     gWaitsLeft = 0;  // a part or a body not in yet is looked for about 5 s after what asked for it, not for ever
		bool                    gHerDrawnWas = false;
		std::atomic<int>        gWeaponLooks{ kWeaponLooks };
		bool                    gArtsOff = false;  // switched off and all let go: the pass rests until the switch moves

		[[nodiscard]] bool LeftHand(const RE::NiAVObject* a_hand)
		{
			return a_hand && std::string_view(a_hand->name.c_str()).find("L Hand") != std::string_view::npos;
		}

		// The node an art light hangs on: the art's own model, or - when the game put the art on an actor's first-person
		// skeleton (the player in first person: a cloak, a bound weapon's art) - the third-person node of the same name as the
		// one the art hangs under, as RELight's body lights do (the body node for one on an arm). A light under the first-person
		// skeleton lights nothing in the world (Dynamic Wards 3.1), so in first person those lights were dark while third person lit.
		// An art whose model carries attach techniques ("AttachT" strings, as MuffleHitFX does) has the game move its pieces between
		// the skeleton's nodes and track them; a light of ours put in that art is swept into the tracking and later read freed
		// (RELight's Featherfall / Muffle crash). Such art lights from the body node, which the game never moves
		[[nodiscard]] bool MovedByTheGame(const RE::NiAVObject* a_art, const RE::NiAVObject* a_root)
		{
			static const RE::BSFixedString kAttachTechniques{ "AttachT" };
			for (const RE::NiAVObject* up = a_art; up && up != a_root; up = up->parent) {
				if (up->HasExtraData(kAttachTechniques)) {
					return true;
				}
			}
			return false;
		}

		[[nodiscard]] RE::NiAVObject* ArtNode(RE::ModelReferenceEffect* a_art, const RE::NiAVObject*& a_hand)
		{
			a_hand = nullptr;
			auto*       art = a_art->artObject3D.get();
			const auto  ref = a_art->target.get();
			auto* const actor = ref ? ref->As<RE::Actor>() : nullptr;
			auto* const first = actor ? actor->Get3D(true) : nullptr;
			auto* const third = actor ? actor->Get3D(false) : nullptr;
			if (art && third && MovedByTheGame(art, third)) {
				return BodyNode(third);
			}
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
		RE::TESObjectLIGH* ShaderRowLight(const RE::Effect* a_effect, bool a_artifact)
		{
			const auto* shader = a_effect && a_effect->baseEffect ? a_effect->baseEffect->data.enchantShader : nullptr;
			if (!shader) {
				return nullptr;
			}
			const auto it = gEnchShaderKey.find(shader);
			return it == gEnchShaderKey.end() ? nullptr : RecordLightFor(it->second, a_artifact);
		}

		RE::TESObjectLIGH* EnchantShaderLight(const RE::EnchantmentItem* a_ench, bool a_artifact)
		{
			if (!a_ench) {
				return nullptr;
			}
			if (auto* record = ShaderRowLight(a_ench->GetCostliestEffectItem(), a_artifact)) {
				return record;
			}
			for (const auto* effect : a_ench->effects) {
				if (auto* record = ShaderRowLight(effect, a_artifact)) {
					return record;
				}
			}
			return nullptr;
		}

		// the artifacts stay lit with Enchanted weapons off (ConfigText.h kArtifactClause): the game's own DaedricArtifact keyword,
		// Skyrim.esm 0xA8668, on either hand - as Light Placer's WornHasKeyword reads the holder, not one hand
		[[nodiscard]] bool HoldsArtifact(const RE::Actor* a_actor)
		{
			static const auto* const keyword = RE::TESForm::LookupByID<RE::BGSKeyword>(0x000A8668);
			if (!a_actor || !keyword) {
				return false;
			}
			for (const bool left : { false, true }) {
				auto*       form = a_actor->GetEquippedObject(left);
				const auto* weapon = form ? form->As<RE::TESObjectWEAP>() : nullptr;
				if (weapon && weapon->HasKeyword(keyword)) {
					return true;
				}
			}
			return false;
		}

		void CollectWeapons(RE::Actor* a_actor)
		{
			const auto* state = a_actor ? a_actor->AsActorState() : nullptr;
			if (!a_actor || !state || !state->IsWeaponDrawn()) {
				return;
			}
			if (!a_actor->Is3DLoaded()) {
				gWeaponWaits = true;  // the game stores its body a moment after its load is noted, and nothing says so
				return;
			}
			const bool artifact = HoldsArtifact(a_actor);
			for (const bool left : { false, true }) {
				auto* form = a_actor->GetEquippedObject(left);
				auto* weapon = form ? form->As<RE::TESObjectWEAP>() : nullptr;
				// a shield a spell puts in her left hand (Holy Templar's Sunlight Targe) is lit by its model's row as Light Placer lights it
				auto* shield = !weapon && left && form ? form->As<RE::TESObjectARMO>() : nullptr;
				if (!weapon && left && !shield) {
					// the left hand's slot names no shield in every case: the worn shield's own (no inventory made for it)
					shield = a_actor->GetWornArmor(RE::BGSBipedObjectForm::BipedObjectSlot::kShield, true);
				}
				if (!weapon && !shield) {
					continue;
				}
				RE::TESObjectLIGH* record = nullptr;
				if (weapon) {
					const auto& model = ModelKey(weapon, weapon->GetModel());
					record = model.empty() ? nullptr : RecordLightFor(model, artifact);
				} else {
					for (const auto& key : ShieldKeys(shield)) {
						if ((record = RecordLightFor(key, artifact))) {
							break;
						}
					}
				}
				if (!record && weapon) {
					// the held instance's own (FadeCharge.cpp ReadHand: the WORN copy's - the entry can hold other copies of the
					// same weapon, CodeRabbit's point on #13 - else the weapon record's). ReadHand counts a bound weapon's time
					// and reads no enchantment, so a bound weapon on an ordinary model (Odin's) takes its record's own
					const auto* ench = Fade::ReadHand(a_actor, left).ench;
					record = EnchantShaderLight(ench ? ench : weapon->IsBound() ? weapon->formEnchanting :
																				  nullptr,
						artifact);
				}
				if (!record) {
					continue;
				}
				// the weapon's third-person 3D; in first person the body node, as for art (ArtNode: the arms are not posed there)
				auto*       part = Fade::WeaponPart(a_actor, false, left, weapon ? static_cast<const RE::TESForm*>(weapon) : shield);
				const auto* hand = part ? HandAbove(part) : nullptr;
				gWeaponWaits = gWeaponWaits || !part;
				if (part && !ArtUnder(part) && !ArtUnder(hand) && !gHandsOnBody.contains(hand)) {
					const auto* camera = RE::PlayerCamera::GetSingleton();
					const bool  firstPerson = a_actor->IsPlayerRef() && camera && camera->IsInFirstPerson();
					auto*       node = firstPerson ? BodyNode(a_actor->Get3D(false)) : part;
					gWeaponWants.push_back({ RE::NiPointer<RE::NiAVObject>(part), RE::NiPointer<RE::NiAVObject>(node), record, a_actor, left, firstPerson, a_actor->IsPlayerRef() });
				}
			}
		}

		// one effect: its light kept while a row lights it now; true while it is to be visited again before the next walk (an art
		// a row lights waits there for its 3D; a flesh shader - KeepFlesh)
		bool Visit(RE::BSTempEffect* a_effect, RE::PlayerCharacter* a_player, RE::ShadowSceneNode* a_scene)
		{
			if (auto* art = a_effect ? netimmerse_cast<RE::ModelReferenceEffect*>(a_effect) : nullptr) {
				if (art->finished || !art->artObject) {
					return false;
				}
				const auto& model = ModelKey(art->artObject, art->artObject->GetModel());
				const auto  holder = model.empty() ? RE::NiPointer<RE::TESObjectREFR>{} : art->target.get();
				const auto* record = model.empty() ? nullptr : RecordLightFor(model, HoldsArtifact(holder ? holder->As<RE::Actor>() : nullptr));
				if (!record) {
					return false;
				}
				if (art->artObject3D) {
					const RE::NiAVObject* hand = nullptr;
					Keep(art, ArtNode(art, hand), record, a_scene, art->target.get().get() == a_player);
					const auto  target = hand ? art->target.get() : RE::NiPointer<RE::TESObjectREFR>{};
					const auto* actor = target ? target->As<RE::Actor>() : nullptr;
					if (actor) {
						gOnBody.push_back({ art, actor, LeftHand(hand) });
					}
				}
				return true;
			}
			auto* shader = a_effect ? netimmerse_cast<RE::ShaderReferenceEffect*>(a_effect) : nullptr;
			return shader && !shader->finished && KeepFlesh(shader, a_scene);
		}

		void ForgetArts()
		{
			gArts.clear();
			gWeaponWants.clear();
			gArtLook.store(true, std::memory_order_relaxed);  // every effect walked at the next pass
		}
	}

	void NoteArtLook() { gArtLook.store(true, std::memory_order_relaxed); }

	void NoteWeaponLook() noexcept { gWeaponLooks.store(kWeaponLooks, std::memory_order_relaxed); }

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
		if (gArtsOff) {
			if (gArtLightsOn.load(std::memory_order_relaxed) < 0.5f || LightsOff()) {
				return;
			}
			gArtsOff = false;
		}
		// she is always on the drawn list, and not every way her weapon goes away sends an event: one value, read each frame
		auto*      her = RE::PlayerCharacter::GetSingleton();
		const bool herDrawn = her && her->AsActorState()->IsWeaponDrawn();
		if (gKept.empty() && gKeptWas == 0 && gArts.empty() && gWeaponWants.empty() && !gArtLook.load(std::memory_order_relaxed) &&
			gWeaponLooks.load(std::memory_order_relaxed) <= 0 && !(gWeaponWaits && gWaitsLeft > 0) && herDrawn == gHerDrawnWas && DrawnActors() == gDrawnWas) {
			return;
		}
		const LightKit::Cost::Pass pass{ 6, "art lights" };
		auto*                      scene = LightKit::Scene();
		if (!scene || gArtLightsOn.load(std::memory_order_relaxed) < 0.5f || LightsOff()) {  // switched off (advanced settings file, or all lights off)
			for (auto& [key, kept] : gKept) {
				Drop(kept, scene);  // with a scene, each light leaves its lists too (CodeRabbit, Illuminated #6)
			}
			gKept.clear();
			gKeptCount = 0;
			gLitCount = 0;
			gLit = 0;
			ForgetArts();
			gArtsOff = scene != nullptr;
			gKeptWas = 0;
			gHerDrawnWas = herDrawn;
			gDrawnWas = DrawnActors();
			return;
		}
		gCap = GlowLimit();
		for (auto& [key, kept] : gKept) {
			kept.seen = false;
		}
		gOnBody.clear();
		if (auto* lists = RE::ProcessLists::GetSingleton()) {
			gHandsOnBody.clear();
			auto*      player = RE::PlayerCharacter::GetSingleton();
			const bool walked = gArtLook.exchange(false, std::memory_order_relaxed);
			if (walked) {
				// every effect in the world: an art started or resumed, the settings changed, or a load
				gArts.clear();
				lists->ForEachMagicTempEffect([&](RE::BSTempEffect* a_effect) {
					if (Visit(a_effect, player, scene)) {
						gArts.emplace_back(a_effect);
					}
					return RE::BSContainer::ForEachResult::kContinue;
				});
			} else if (!gArts.empty()) {  // the game's lock only with something to visit: its effect threads wait on it
				static std::vector<RE::NiPointer<RE::BSTempEffect>> gone;
				{
					RE::BSSpinLockGuard locker(lists->magicEffectsLock);
					for (auto& effect : gArts) {
						if (!Visit(effect.get(), player, scene)) {
							gone.push_back(std::move(effect));
						}
					}
				}
				std::erase_if(gArts, [](const auto& a_effect) { return !a_effect; });
				gone.clear();  // past the lock: the last reference to a finished effect may be let go here
			}
			// what a drawn weapon wants changes only with what announces it (NoteWeaponLook), a walk, the list of drawn actors, or an art light coming or going
			const auto& drawn = DrawnActors();
			int         looks = gWeaponLooks.load(std::memory_order_relaxed);
			const bool  partGone = std::ranges::any_of(gWeaponWants, [](const WeaponWant& a_w) { return !a_w.part->parent; });
			const bool  asked = looks > 0 || walked || drawn != gDrawnWas || gKept.size() != gKeptWas || herDrawn != gHerDrawnWas;
			gHerDrawnWas = herDrawn;
			if (asked) {
				gWaitsLeft = kWeaponWaitLooks;
			}
			const bool waiting = gWeaponWaits && gWaitsLeft > 0;
			gWaitsLeft -= waiting ? 1 : 0;
			if (asked || waiting || partGone) {
				if (looks > 0) {
					gWeaponLooks.compare_exchange_strong(looks, looks - 1, std::memory_order_relaxed);  // a note in between keeps its count
				}
				gDrawnWas = drawn;
				gArtUnder.clear();
				for (const auto& [key, kept] : gKept) {
					if (kept.seen && kept.root) {
						for (const RE::NiAVObject* up = kept.root.get(); up && gArtUnder.size() < 4096; up = up->parent) {
							gArtUnder.push_back(up);
						}
					}
				}
				std::ranges::sort(gArtUnder);
				gWeaponWants.clear();
				gWeaponWaits = false;
				for (auto* actor : drawn) {
					CollectWeapons(actor);  // the player first, then every actor with drawn hands (Events.cpp)
				}
			}
			for (const auto& w : gWeaponWants) {
				Keep(w.part.get(), w.node.get(), w.record, scene, w.hers);
				if (w.firstPerson) {
					gOnBody.push_back({ w.part.get(), w.actor, w.left });
				}
			}
			gKeptWas = gKept.size();
		}
		std::erase_if(gKept, [&](auto& a_entry) {
			if (!a_entry.second.seen) {
				Drop(a_entry.second, scene);
				return true;
			}
			return false;
		});
		// never more than the cap on others: the ones past it go out at once and wait like any other
		for (auto it = gKept.begin(); gLit > gCap && it != gKept.end(); ++it) {
			if (it->second.light && !it->second.hers) {
				Drop(it->second, scene);
			}
		}
		gKeptCount = gKept.size();
		gLitCount = gLit;
		// the lights that went to the body for a hand, now that the walk is over: the fading module and Lights by Item reach
		// them as that hand's (both run after this in the player update)
		for (const auto& b : gOnBody) {
			if (const auto it = gKept.find(b.owner); it != gKept.end() && it->second.light) {
				Fade::NoteHandLight(b.actor, b.left, it->second.light.get());
			}
		}
	}

	std::size_t ArtGlowsLit()
	{
		return gLit;
	}

	void DropArtLights()
	{
		auto* scene = LightKit::Scene();
		for (auto& [key, kept] : gKept) {
			Drop(kept, scene);
		}
		gKept.clear();
		gKeptCount = 0;
		gLitCount = 0;
		gLit = 0;
		gModelKey.clear();  // keyed by form address: a form made in memory and freed could hand its address to another
		gShieldKeys.clear();
		ForgetArts();
	}

	std::size_t ArtLightCount() { return gKeptCount.load(); }
	std::size_t ArtLightsLitNow() { return gLitCount.load(); }
	std::size_t ArtLightsMade() { return gMade.load(); }
}
