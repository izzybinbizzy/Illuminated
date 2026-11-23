// Illuminated - SKSE plugin
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE.txt and the notice at the top of main.cpp.
//
// Pass 6: the light of a spray, a stream or a bolt - and a ward dome's light.
//
// HIS RULE (Illuminated's CLAUDE.md, "RELIGHT'S METHOD"): a spray, stream or bolt keeps the PROJECTILE'S OWN LIGHT RECORD;
// his checklist 140 fallback (below) adds a row of small lights along its length. Each cone, beam and flamethrower projectile
// keeps its own light, Streams on or off; an in-memory copy of that light carries the color its row wears, picked again on
// every settings refresh: a BEAM's row in the Streams folder whose settings hold (the data's color - the build moves every beam
// row out of Light Placer, lagen move_beams, because on Community Shaders the data's six points stopped half way along Sparks,
// measured 2026-10-13), else the spray markers (frost, shock), else the record's own; a beam with no light of its own wears
// its row's light. A spray model pass 4 copied (the Spray Lights setting) is left to that setting alone - his answer
// 2026-10-13: Streams off does not touch the sprays Spray Lights lights, two settings, each doing one thing.
// `PC Runner\stream rule check.py` fails a build that hangs lights on these projectiles anywhere but the shared StreamRows rows (BeamRow.h).
//
// A ward dome (Dynamic Wards absent) is the one projectile this pass still hangs a light on: its Load3D / Release3D hooks,
// the live list and the main-thread queue below are for that. CREDIT: how that light is made and registered follows ReLight
// by Truman (github.com/TrumanGIT/ReLight), GPL-3.0-or-later, with his permission and kept under the same license: one master
// NiPointLight made once and cloned for every use, the light's size carried in the radius' z, the create parameters a
// non-shadow light needs, and handing the light to the shadow scene node. The scene is only ever touched on the main thread
// (3D built or released on a loader thread is queued for the next frame - StreamLightsFrame).

#include "Fade.h"
#include "Plugin.h"
#include "StreamRows.h"

// 🔁 HIS ANSWER 2026-10-20 (checklist 140): one light stretched along the beam - and "if you plain out cannot make part 1 of
// 140 to happen then revert to this as a backup plan", a row of lights. One stretched light cannot be drawn inside our mods
// (his research page, part 8), so a bolt, a flame spray and a cone wear a ROW of small lights spread over their real length
// every frame (BeamRow.h: no two farther apart than BeamRow::kSpacing, spots under 5 %, the light staying near the beam), beside
// their own light at its own size; a cone the data lights as a cloud gets one light where its cloud is (HangCloud). The row's
// moving parts are the old ladder's (local branch beam-ladder): lights hung off the projectile's root along its path (PathOf: a
// bolt straight to what it hit), as long as its drawn art, never past its newest impact or its range.

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
		constexpr float       kShortestStream = 120.0f;
		constexpr const char* kLightName = "IlluminatedStream";
		constexpr float       kLightSize = LightKit::kLightSize;  // the light's size, which lives in the radius' z (from ReLight)
		// the projectile's own light, raised the way RE::Light's isPluginLight configs raise it in RELight - Spell Addon
		// (relightgen spray_record_config): brightness 1.72 at size 2.5 (the hand light's peak), reach the whole length plus
		// 15%, bounded by what one light carries at a cutoff of 0.01, never under the hand reach
		using LightKit::kK;
		constexpr float       kOwnSize = 2.5f;
		constexpr float       kOwnFade = 1077.69f * kOwnSize * kOwnSize / 3918.88f;
		constexpr float       kOwnEndReach = 1.15f;
		constexpr float       kOwnShortestReach = 133.0f;
		constexpr float       kOwnLowestCutoff = 0.01f;
		constexpr std::size_t kMaxLivePerBase = 4;
		// the creature bolts, cones and breaths Creatures 1.1 lights, by EditorID (lower case)
		constexpr std::array<std::string_view, 8> kCreatureProjectiles{ "shockboltaimdwarvenspider", "dragonicespikevolleyprojectile",
			"dragonicespikevolleyprojectile_aos", "dragonvoicealduinendtimesprojectile", "dragonfireprojectile02", "duncgalduinfireprojectile",
			"dragonfrostprojectile01", "dragonfrostprojectile01_aos" };

		// a ward dome's light: one, where the dome sits, under its own setting
		struct Recipe
		{
			float       radius{ 300.0f };
			float       fade{ 1.0f };
			float       falloff{ 2.0f };
			RE::NiColor color{ 1.0f, 1.0f, 1.0f };
		};

		std::unordered_map<RE::FormID, Recipe>             gRecipes;     // ward base form -> its light
		std::unordered_map<RE::FormID, RE::TESObjectLIGH*> gHandLight;   // a stream projectile's own light, to give back
		std::unordered_map<RE::FormID, RE::TESObjectLIGH*> gStreamCopy;  // its own light's raised copy, worn while the setting is on

		// what a raised copy's color is picked from, again on every settings refresh (the rows' and markers' settings change live)
		struct StreamColor
		{
			std::string              model;  // a beam's model: its Streams row
			Family                   family{ Family::kNone };
			bool                     beam{ false };
			RE::Color                own{};                // the color of the light it was raised from
			const RE::TESObjectLIGH* borrowed{ nullptr };  // a beam with no light of its own: the row's light it was raised from
			float                    range{ 0.0f };        // the projectile's range (the raised reach follows it)
			bool                     vanillaArt{ false };  // the game's own art, which the art picks recolor
			bool                     cloud{ false };       // a cone the data lights as a cloud (CloudModel): its Streams row, on every route
		};
		std::unordered_map<RE::FormID, StreamColor> gStreamColor;  // only copies this pass made (pass 4's keep their own)
		// a copy of a projectile's own light in the color its art pick gives - worn in place of the record, whose color is vanilla
		std::unordered_map<RE::FormID, RE::TESObjectLIGH*> gOwnTint;

		void SetColor(RE::TESObjectLIGH* a_light, const RE::Color& a_color)
		{
			a_light->data.color.red = a_color.red;
			a_light->data.color.green = a_color.green;
			a_light->data.color.blue = a_color.blue;
		}

		// a raised copy's color for the settings as they are now: on the game's own art, the color its element's hand wears for
		// the art pick (every light of a spell one color); else a beam's row whose settings hold (the data's
		// color), else the frost / shock spray marker, else the color of the light it was raised from
		RE::Color ColorFor(const StreamColor& a_c, const SprayChoice& a_sc)
		{
			if (a_c.vanillaArt) {
				if (const auto hand = ArtHandColor(a_c.family)) {
					return *hand;
				}
			}
			const auto* row = a_c.beam || a_c.cloud ? StreamRowFor(a_c.model) : nullptr;
			const auto  rgb = [](const Rgb& a_m) {
				return RE::Color(static_cast<std::uint8_t>(std::clamp(a_m.r, 0, 255)), static_cast<std::uint8_t>(std::clamp(a_m.g, 0, 255)),
					static_cast<std::uint8_t>(std::clamp(a_m.b, 0, 255)), 0);
			};
			if (row && row->hasColor) {
				return RE::Color(row->r, row->g, row->b, 0);
			}
			if (a_c.family == Family::kFrost && a_sc.frostSet) {
				return rgb(a_sc.frost);
			}
			if (a_c.family == Family::kShock && a_sc.shockSet) {
				return rgb(a_sc.shock);
			}
			return a_c.own;
		}

		void PickColor(const StreamColor& a_c, RE::TESObjectLIGH* a_copy, const SprayChoice& a_sc)
		{
			SetColor(a_copy, VanillaLook(ColorFor(a_c, a_sc)));
		}

		// the light a beam with none of its own borrows: its Streams row's light - one of our copies (pass 0's look), else the record
		const RE::TESObjectLIGH* RowLight(const ConfigLight* a_row)
		{
			if (!a_row || a_row->light.empty()) {
				return nullptr;
			}
			const auto c = std::ranges::find_if(LightCopies(), [&](const LightCopy& a_c) { return a_c.id == a_row->light; });
			return c != LightCopies().end() ? c->form : RE::TESForm::LookupByEditorID<RE::TESObjectLIGH>(a_row->light);
		}

		// the light record a beam with no light of its own borrows from its row, as the game can wear it
		RE::TESObjectLIGH* BorrowedLight(RE::FormID a_projectile)
		{
			const auto  c = gStreamColor.find(a_projectile);
			const auto* borrowed = c != gStreamColor.end() ? c->second.borrowed : nullptr;
			if (!borrowed) {
				return nullptr;
			}
			const auto copy = std::ranges::find(LightCopies(), borrowed, &LightCopy::form);
			return copy != LightCopies().end() ? copy->form : RE::TESForm::LookupByID<RE::TESObjectLIGH>(borrowed->GetFormID());
		}

		// a light's own fade and reach before the sliders: one of our copies' start (ApplyLightStrength scales the copy itself),
		// else the record's
		std::pair<float, float> StartOf(const RE::TESObjectLIGH* a_light)
		{
			const auto c = std::ranges::find(LightCopies(), a_light, &LightCopy::form);
			return c != LightCopies().end() ? std::pair{ c->startFade, static_cast<float>(c->startRadius) } :
			                                  std::pair{ a_light->fade, static_cast<float>(a_light->data.radius) };
		}

		struct Raised
		{
			float reach{ 0.0f };
			float fade{ 0.0f };
			float cutoff{ 0.0f };  // 0 off Community Shaders (drawn plain)
		};

		// a_copy (a copy of a_base) raised the RELight way - the numbers at the top; its color is PickColor's
		Raised Raise(RE::TESObjectLIGH* a_copy, const RE::TESObjectLIGH* a_base, float a_range)
		{
			// never dimmer than the light it was raised from (his answer 2026-10-13): the numbers it was made with, before the
			// sliders (a copy's start fade; a record of the game's own is never scaled), its size where it is drawn inverse square
			const auto  made = std::ranges::find_if(LightCopies(), [&](const LightCopy& a_c) { return a_c.form == a_base; });
			const float ownFade = made != LightCopies().end() ? made->startFade : a_base->fade;
			const bool  ownIsl = (a_base->data.flags.underlying() & LightKit::kRecordInverseSquare) != 0;
			Raised      r;
			r.fade = LightKit::NeverDimmer(kOwnFade, kOwnSize, ownFade, ownIsl ? LightKit::LightPlacerSize(a_base->data.fov) : 0.0f);
			const float limit = std::sqrt((std::max)(kK * r.fade / kOwnLowestCutoff - kOwnSize * kOwnSize, 1.0f));
			r.reach = std::clamp(a_range * kOwnEndReach, kOwnShortestReach, limit);
			float radius = r.reach;
			if (InverseSquare()) {
				// Community Shaders draws a record carrying the inverse-square flag by its fade and cutoff - the record's falloff
				// field IS the cutoff then (LightCopies.cpp, pass 0), and its size is the field of view
				r.cutoff = kK * r.fade / (r.reach * r.reach + kOwnSize * kOwnSize);
				a_copy->data.flags = static_cast<RE::TES_LIGHT_FLAGS>(a_copy->data.flags.underlying() | LightKit::kRecordInverseSquare);
				a_copy->data.fallofExponent = r.cutoff;
				a_copy->data.fov = kOwnSize;
			} else {
				// ENB and Vanilla (Lighting.cpp): the same reach drawn plain by the game's own lighting, its falloff left as the record
				// has it. (Until 2026-10-13 the cutoff went into the falloff field here too, where the game reads it as the falloff
				// exponent - a ~0.01 exponent is a flat disc of light that ends hard at the radius.)
				const auto plain = LightKit::PlainAt(r.fade, 0.0f, r.reach);
				r.fade = plain.fade;
				radius = plain.radius;
				a_copy->data.flags = static_cast<RE::TES_LIGHT_FLAGS>(a_copy->data.flags.underlying() & ~LightKit::kRecordInverseSquare);
			}
			a_copy->data.radius = static_cast<std::uint32_t>(std::lround(radius));
			a_copy->fade = r.fade;
			return r;
		}

		// the rows along bolts, flame sprays and cones live in the shared StreamRows module (Tools\Shared Plugin Code\); this pass
		// keeps their table - each projectile's row recipe, its raised copy and color - and hands it over (StreamRows::Claim)
		std::unordered_map<RE::FormID, StreamRows::Recipe> gRows;               // beam base form -> its row
		bool                                               gSprayRows = false;  // pass 4's sprays wear a row: the Spray Lights setting holds and all lights are not off

		bool RowOn(const StreamRows::Recipe& a_rec)
		{
			return !PerformanceMode() && (a_rec.spray ? gSprayRows : StreamLightsOn());  // Performance: no rows (his order 2026-10-20)
		}

		// One lit ward dome, keyed by its 3D ROOT, not its reference ID: a projectile's reference is temporary and the game hands its ID
		// to a new one. The root is held here, so its address is never another 3D's while it is listed.
		struct Live
		{
			RE::NiPointer<RE::NiAVObject>           root;
			RE::FormID                              base{ 0 };
			std::vector<RE::NiPointer<RE::BSLight>> lights;
		};
		std::unordered_map<const RE::NiAVObject*, Live> gLive;
		// the live count, by projectile BASE rather than by reference: a ward raised again and again makes a new reference each time,
		// and it is the base that has to be capped
		std::unordered_map<RE::FormID, std::size_t> gLiveCount;
		std::mutex                                  gLiveLock;
		// what a loader thread built or released, waiting for the main thread (under gLiveLock), held by refcount
		std::vector<RE::NiPointer<RE::TESObjectREFR>> gToHang;
		std::vector<RE::NiPointer<RE::NiAVObject>>    gToDrop;
		std::uint32_t                                 gSweepClock = 0;  // frames since the last sweep (main thread)

		// every ward belongs to Dynamic Wards: when that mod is here, this pass does not light a ward
		bool WardLightsOn()
		{
			return SettingValue(kWardSetting, 0) != 0 && !DynamicWardsLoaded() && !LightsOff();
		}

		float Percent(std::string_view a_id)
		{
			return static_cast<float>(SettingValue(a_id, 100)) / 100.0f;
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
			for (auto& bs : it->second.lights) {
				LightKit::TakeOff(a_scene, bs);
			}
			if (const auto c = gLiveCount.find(it->second.base); c != gLiveCount.end() && c->second) {
				--c->second;
			}
			gLive.erase(it);
		}

		// every live 3D a test picks, let go: all collected first, as Forget erases from gLive. gLiveLock is held by the caller.
		template <class Pick>
		std::size_t ForgetIf(RE::ShadowSceneNode* a_scene, Pick a_pick)
		{
			std::vector<const RE::NiAVObject*> picked;
			for (const auto& [root, live] : gLive) {
				if (a_pick(live)) {
					picked.push_back(root);
				}
			}
			for (const auto* root : picked) {
				Forget(root, a_scene);
			}
			return picked.size();
		}

		// the sweep: a projectile 3D the game has taken out of the world (no parent) is not live, whether or not its release
		// reached this pass. gLiveLock is held by the caller.
		std::size_t SweepGone(RE::ShadowSceneNode* a_scene)
		{
			return ForgetIf(a_scene, [](const Live& a_live) { return !a_live.root || !a_live.root->parent; });
		}

		// ------------------------------------------------------------------ the light on one live ward
		// the first calls are logged with the step a ward light stopped at, rather than saying nothing at all
		std::size_t gCalls = 0;

		void Told(std::string_view a_what, const RE::TESObjectREFR* a_ref)
		{
			if (gCalls <= 10) {
				SKSE::log::info("[STREAM-HOOK] call {} | {} | base {}", gCalls, a_what,
					a_ref && a_ref->GetBaseObject() ? Label(a_ref->GetBaseObject()) : "(none)");
			}
		}

		// a cloud's light follows its Streams row's settings (its patch's switch) and All lights off - not Performance nor Streams:
		// it is the spell's one light, the one Light Placer hung on the cone before
		bool CloudOn(RE::FormID a_base)
		{
			const auto c = gStreamColor.find(a_base);
			return !LightsOff() && c != gStreamColor.end() && StreamRowFor(c->second.model);
		}

		void HangLights(RE::TESObjectREFR* a_ref, RE::NiAVObject* a_object)
		{
			++gCalls;
			if (!a_ref || !a_object) {
				Told("no reference or no 3D", a_ref);
				return;
			}
			auto* root = a_object->AsNode();
			auto* base = a_ref->GetBaseObject();
			if (!root || !base) {
				Told("the 3D is not a node, or there is no base form", a_ref);
				return;
			}
			const auto it = gRecipes.find(base->GetFormID());
			if (it == gRecipes.end()) {
				Told("no recipe for this projectile", a_ref);
				return;
			}
			const auto& r = it->second;
			if (!WardLightsOn()) {
				Told("its setting is off", a_ref);
				return;
			}
			auto* scene = LightKit::Scene();
			if (!scene) {
				Told("the game has no shadow scene node right now", a_ref);
				return;
			}
			// the cap, before any light is made
			{
				std::lock_guard l{ gLiveLock };
				if (gLive.contains(root)) {  // keyed by the 3D itself: this very 3D already carries our lights
					Told("this projectile is already lit", a_ref);
					return;
				}
				if (gLiveCount[base->GetFormID()] >= kMaxLivePerBase) {
					SweepGone(scene);
					if (gLiveCount[base->GetFormID()] >= kMaxLivePerBase) {
						Told("at the live cap for this projectile", a_ref);
						return;
					}
				}
			}
			// a ward's light hangs off the barrier's root, where the dome sits
			RE::NiNode* parent = root;
			Told("making its light", a_ref);
			const bool isl = InverseSquare();
			// ENB and Vanilla (Lighting.cpp): the reach asked for, drawn by the game's own lighting. Brightness,
			// as every light copy takes it (LightCopies.cpp)
			const auto                              plain = LightKit::PlainAt(r.fade, 0.0f, r.radius);
			const float                             radius = (isl ? r.radius : plain.radius) * Percent("IlluminatedReach");
			const float                             fade = (isl ? r.fade : plain.fade) * BrightnessNow();
			std::vector<RE::NiPointer<RE::BSLight>> made;
			if (auto* light = LightKit::CloneLight()) {  // ReLight's master-and-clone (LightKit.h)
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
					data.ambient = LightKit::Tenth(r.color);
				}
				// inverse square flag and cutoff, after SetLightAttenuation - Community Shaders only
				if (isl) {
					LightKit::Isl::SetOwn(light);  // a light of our own: Community Shaders' words written whole (LightKit.h)
					LightKit::Isl::SetCutoff(light, LightKit::CutoffFor(fade, radius, kLightSize));
				}
				light->local.translate = {};
				light->local.scale = 1.0f;
				if (auto* bs = LightKit::Hang(parent, light, scene, r.falloff)) {
					made.emplace_back(bs);
				}
			}
			// the first few are logged, so the log says whether this pass is doing anything
			static std::size_t told = 0;
			if (told < 12 && !made.empty()) {
				++told;
				SKSE::log::info("[WARD-LIT] {} | radius {:.0f} | fade {:.2f} | node {}", Label(a_ref->GetBaseObject()), radius, fade,
					parent->name.empty() ? "(unnamed)" : parent->name.c_str());
			}
			if (made.empty()) {
				SKSE::log::warn("[WARD-FAILED] {} | no light could be made or registered", Label(a_ref->GetBaseObject()));
				return;
			}
			std::lock_guard l{ gLiveLock };
			Live            live{};
			live.root.reset(root);
			live.base = base->GetFormID();
			live.lights = std::move(made);
			gLive.emplace(root, std::move(live));
			++gLiveCount[base->GetFormID()];
		}

		// called before the game releases the reference's 3D, so its root is still there to find the entry by
		void DropLights(RE::TESObjectREFR* a_ref)
		{
			auto* root = a_ref ? a_ref->Get3D() : nullptr;
			if (!root) {
				return;
			}
			std::lock_guard l{ gLiveLock };
			if (!gLive.contains(root)) {
				return;
			}
			if (OnMainThread()) {
				Forget(root, LightKit::Scene());
			} else {
				gToDrop.emplace_back(root);
			}
		}

		// the per-frame pass runs from a projectile's 3D loading or going until nothing of it is left to move or let go
		std::atomic<bool> gStreamsAwake{ true };

		// ------------------------------------------------------------------ the two hooks
		// Load3D builds a reference's 3D (vtable slot 0x6A); Release3DRelatedData takes it apart (0x6B)
		template <class T>
		struct Load3D
		{
			static RE::NiAVObject* thunk(T* a_this, bool a_backgroundLoading)
			{
				auto* object = original(a_this, a_backgroundLoading);
				LightKit::Guarded("ward light (3D load)", [&]() {
					if (OnMainThread()) {
						HangLights(a_this, object);
					} else if (object) {
						std::lock_guard l{ gLiveLock };
						gToHang.emplace_back(a_this);  // held by its refcount, no handle made on this thread
					}
				});
				gStreamsAwake.store(true, std::memory_order_relaxed);  // after the work: the pass goes to sleep only under gLiveLock
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
				LightKit::Guarded("ward light (3D release)", [a_this]() { DropLights(a_this); });
				gStreamsAwake.store(true, std::memory_order_relaxed);
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
		return ConfigText::WearsRaised(SettingValue(kStreamSetting, 1) != 0, LightsOff());  // the setting ships on (lagen.py)
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
			wards, colored, DynamicWardsLoaded() ? "installed, so this pass stands down" : "not installed",
			SettingValue(kWardSetting, 0) ? "on" : "off");
	}

	// ------------------------------------------------------------------ the table, read once from the load order
	void StreamLights()
	{
		gRecipes.clear();
		gHandLight.clear();
		gStreamCopy.clear();
		gStreamColor.clear();
		gOwnTint.clear();
		const auto  sc = ReadSprayChoice();
		std::size_t cones = 0, beams = 0, flames = 0, kept = 0, skipped = 0, creatures = 0;
		for (auto* proj : LightKit::FormsOf<RE::BGSProjectile>()) {
			if (!proj) {
				continue;
			}
			// a flamethrower, cone or beam projectile: its own light, and a row of ours along it
			const bool flame = proj->data.types.any(RE::BGSProjectileData::Type::kFlamethrower);
			const bool cone = !flame && proj->data.types.any(RE::BGSProjectileData::Type::kCone);
			const bool beam = !flame && !cone && proj->data.types.any(RE::BGSProjectileData::Type::kBeam);
			if (!cone && !beam && !flame) {
				continue;
			}
			const auto id = EditorID(proj);
			const auto low = Lower(id);
			const auto model = NormalPath(proj->GetModel() ? proj->GetModel() : "");
			// poison sprays stay dark, the way every other pass leaves them; a trap's or a cloak's is not a spell (pass 4's list)
			if (Contains(low, "poison") || Contains(low, "poision") || std::ranges::find(kNotSpellProjectiles, low) != kNotSpellProjectiles.end()) {
				++skipped;
				continue;
			}
			// every creature light is Illuminated - Creature Lighting NG's: these creature projectiles' raised light and row are
			// Creatures'; the player spells on the same models stay here
			if (std::ranges::find(kCreatureProjectiles, low) != kCreatureProjectiles.end()) {
				++creatures;
				continue;
			}
			const float range = proj->data.range;
			// a flame spray's range is the reach of the cone, so the shortest-stream floor does not apply
			if (!flame && range < kShortestStream) {
				++skipped;
				continue;
			}
			// HIS RULE (CLAUDE.md, "RELIGHT'S METHOD"): the projectile keeps its OWN light record; its length is lit by a row of
			// small lights (StreamRows.cpp, his checklist 140 fallback). One with neither a light of its own nor a row light is left as it is.
			// a beam whose rows the build moved out of Light Placer (lagen move_beams): the data's color for it, and its light when
			// the projectile has none of its own (taken again on every settings refresh - ApplyStreamLights)
			// a cone the data lights as a cloud (a Streams row marked "cloud", lagen): its row's light on every route, held where
			// the cloud is (HangCloud) - not the cone's own faint light raised
			const bool  cloud = cone && CloudModel(model);
			const auto* row = beam || cloud ? StreamRowFor(model) : nullptr;
			auto*       own = proj->data.light;
			const auto* data = cloud ? RowLight(row) : nullptr;  // a cloud wears its row's light, though it has a faint one of its own
			const auto* base = data ? data : (own ? own : RowLight(row));
			if (!base) {
				++skipped;
				continue;
			}
			// pass 4 stretched a spray model's light already (the old patcher's spray lights, his settled numbers, under the Spray
			// Lights setting): that copy is this way's copy, and it is LEFT to that setting - his answer 2026-10-13 (CodeRabbit's
			// point on #13 answered no): Streams off does not touch the sprays Spray Lights brightens, two settings, each doing
			// one thing. Any other copy (pass 0's look, a record light) is the light the projectile wears - raised here.
			if (own && SprayOriginal(proj)) {
				// his 2026-10-20 "Do the flame sprays and cones in Illuminated first": it wears a row of small lights along its aim
				// (BeamRow.h) under that setting, matched to its ORIGINAL light, and the original light itself (SprayLights.cpp)
				const auto* original = SprayOriginal(proj);
				const auto* tint = SprayCopyOf(proj);
				const auto [fade, reach] = StartOf(original);  // before the sliders: the row module puts them on
				gRows[proj->GetFormID()] = { ColorOf(tint ? tint : original), fade, reach, false, true, tint };
				++kept;
				continue;
			}
			auto* copy = CopyLight(base);
			if (!copy) {
				++skipped;
				continue;
			}
			const auto   family = FamilyOf(id, true);
			const Raised raised = Raise(copy, base, range);
			const float  reach = raised.reach, fade = raised.fade, cutoff = raised.cutoff;
			const auto&  color = gStreamColor[proj->GetFormID()] = { model, family, beam, ColorAsMade(base), own ? nullptr : base, range,
				VanillaArtModel(model), cloud };
			PickColor(color, copy, sc);  // and again on every refresh (ApplyStreamLights)
			// his checklist 140: a bolt wears a row of lights (BeamRow.h) whose middle glows as its OWN light would right under
			// it - never the raised copy's numbers: matched to those (run cs 2026-10-20 16:59) the row lit the whole courtyard,
			// steps and walls, his "reach the entire universe"
			// (and since his 2026-10-20 "Do the flame sprays and cones in Illuminated first", a flame spray and a cone the same way,
			// along their aim)
			const auto [baseFade, baseReach] = StartOf(base);  // before the sliders: the row module puts them on
			gRows[proj->GetFormID()] = { ColorOf(copy), baseFade, baseReach, beam, false, nullptr, cloud };
			// the sliders reach it like every copy (LightCopies.cpp ApplyLightStrength): the cutoff too, so Reach moves an inverse-square
			// stream's reach (it is Community Shaders' only lever)
			LightCopies().push_back({ .id = "stream " + id,
				.base = EditorID(base),
				.form = copy,
				.startFade = copy->fade,
				.startRadius = copy->data.radius,
				.startCutoff = cutoff });
			gHandLight[proj->GetFormID()] = own;
			gStreamCopy[proj->GetFormID()] = copy;
			if (own && color.vanillaArt && family != Family::kNone) {
				if (auto* tint = CopyLight(own)) {
					// the sliders reach it as they reach the light it copies (a copy of ours is scaled, a record of the game's is not)
					if (const auto lc = std::ranges::find(LightCopies(), own, &LightCopy::form); lc != LightCopies().end()) {
						auto entry = *lc;
						entry.id = "tint " + id;
						entry.form = tint;
						LightCopies().push_back(std::move(entry));
					}
					gOwnTint[proj->GetFormID()] = tint;
				}
			}
			if (flame) {
				++flames;
			} else if (cone) {
				++cones;
			} else {
				++beams;
			}
			SKSE::log::info("[STREAM] {} | {} | range {:.0f} | {} {} raised: reach {:.0f} | fade {:.2f} | color {},{},{}{} | model {}",
				Label(proj), flame ? "flame spray" : cone ? "spray" :
															"bolt",
				range, own ? "its own light" : "(none of its own) the data's light",
				EditorID(base), reach, fade, copy->data.color.red, copy->data.color.green, copy->data.color.blue,
				row ? " (the data's beam row)" : "", model);
		}
		SKSE::log::info(
			"stream lights: {} flame spray(s), {} spray(s) and {} bolt(s) carry their own light, raised; {} spray(s) keep pass "
			"4's stretched copy (the Spray Lights setting's, not this one's); {} left alone; {} creature projectile(s) left to Creatures; "
			"the setting is {}",
			flames, cones, beams, kept, skipped, creatures, StreamLightsOn() ? "on" : "off");
		ApplyLightStrength(false);  // every copy at the sliders' values from the start, pass 4's and these
		WardLights();
		Load3D<RE::BarrierProjectile>::Install();  // a ward dome's light (Dynamic Wards absent)
		Release3D<RE::BarrierProjectile>::Install();
		// the rows along bolts, flame sprays and cones: the shared module, with Illuminated's switches, sliders and colors
		StreamRows::Host host{};
		host.rowName = "IlluminatedStreamRow";
		host.cloudName = "IlluminatedStreamCloud";
		host.rowOn = [](const StreamRows::Recipe& a_rec) { return RowOn(a_rec); };
		host.cloudOn = [](RE::FormID a_base) { return CloudOn(a_base); };
		host.brightness = []() { return BrightnessNow() * StreamStrength(); };
		host.reach = []() { return Percent("IlluminatedReach"); };
		host.flicker = []() { return FlickerOn(); };
		host.shaped = []() { return PulseRealistic(); };
		host.spellColor = [](RE::FormID a_spell) -> std::optional<RE::NiColor> { return Fade::Items::ChoiceColor(a_spell); };
		host.lighting = []() { return static_cast<int>(LightingFound()); };
		host.inverseSquare = []() { return InverseSquare(); };
		host.lightLimitFix = []() { return LightLimitFix(); };
		StreamRows::Install(host);
		StreamRows::Claim(gRows);
		ApplyStreamLights(true);
		ApplySprayColors();  // pass 4's sprays put back on their own light from the start - their rows light the length
	}

	// once a frame, on the main thread - what the loader threads left to do
	void DropStreamLights()
	{
		StreamRows::DropAll();
		auto* scene = LightKit::Scene();
		// every projectile this drops is let go after the lock: declared before it, so it goes last (L12)
		std::vector<RE::NiPointer<RE::TESObjectREFR>> letGo;
		std::lock_guard                               l{ gLiveLock };
		ForgetIf(scene, [](const Live&) { return true; });
		letGo.insert(letGo.end(), std::make_move_iterator(gToHang.begin()), std::make_move_iterator(gToHang.end()));
		gToHang.clear();
		gToDrop.clear();
	}

	bool StreamLightsAwake()
	{
		return gStreamsAwake.load(std::memory_order_relaxed) || StreamRows::Awake();
	}

	void StreamLightsFrame()
	{
		StreamRows::Frame();  // the rows along bolts, flame sprays and cones (asleep with nothing of theirs alive)
		if (!gStreamsAwake.load(std::memory_order_relaxed)) {
			return;
		}
		std::vector<RE::NiPointer<RE::TESObjectREFR>> hang;
		{
			std::lock_guard l{ gLiveLock };
			// about every five seconds, the lights whose 3D is gone are let go even when no cap was reached
			if (++gSweepClock >= 300 && !gLive.empty()) {
				gSweepClock = 0;
				SweepGone(LightKit::Scene());
			}
			if (gToHang.empty() && gToDrop.empty()) {
				if (gLive.empty()) {
					gStreamsAwake.store(false, std::memory_order_relaxed);
				}
				return;
			}
			auto* scene = LightKit::Scene();
			for (const auto& root : gToDrop) {
				Forget(root.get(), scene);
			}
			gToDrop.clear();
			hang.swap(gToHang);
		}
		for (const auto& ref : hang) {
			// a 3D released before this frame is gone (or another one now): lit only when the 3D it holds now is in the world
			if (auto* root = ref ? ref->Get3D() : nullptr; root && root->parent) {
				HangLights(ref.get(), root);
			}
		}
	}

	// the projectile wears its own light's raised copy while this pass is on, and its own light when it is switched off; the
	// copies' colors are picked again for the settings as they are now
	void ApplyStreamLights(bool a_log)
	{
		const bool  want = StreamLightsOn();
		const auto  sc = ReadSprayChoice();
		std::size_t changed = 0, reborrowed = 0;
		for (auto& [formID, c] : gStreamColor) {
			const auto copy = gStreamCopy.find(formID);
			if (copy == gStreamCopy.end() || !copy->second) {
				continue;
			}
			// HIS ANSWER 2026-10-14 ("bolt should switch at once"): a beam with no light of its own takes the light its row's
			// settings choose NOW, at once, as its color does - raised again from that light. (No row holding = it keeps the one
			// it wears; it was never left dark by a setting before either.)
			if (const auto* now = c.borrowed ? RowLight(StreamRowFor(c.model)) : nullptr; now && now != c.borrowed) {
				auto* lit = copy->second;
				CopyLightInto(lit, now);  // the whole new light - its sound, lens flare and model too, not only its look
				const Raised r = Raise(lit, now, c.range);
				if (const auto lc = std::ranges::find_if(LightCopies(), [&](const LightCopy& a_c) { return a_c.form == lit; });
					lc != LightCopies().end()) {
					lc->base = EditorID(now);
					lc->startFade = lit->fade;
					lc->startRadius = lit->data.radius;
					lc->startCutoff = r.cutoff;
				}
				c.borrowed = now;
				c.own = ColorAsMade(now);
				++reborrowed;
			}
			PickColor(c, copy->second, sc);
			if (const auto row = gRows.find(formID); row != gRows.end()) {
				row->second.color = ColorOf(copy->second);  // the row takes the color the settings pick now
				if (c.borrowed) {                           // and a borrowed light's own strength and reach (the code review)
					const auto [fade, reach] = StartOf(c.borrowed);
					row->second.fade = fade;
					row->second.reach = reach;
				}
				// a cloud takes its Streams row's light as the settings pick it now, at that light's own numbers
				if (const auto* now = c.cloud ? RowLight(StreamRowFor(c.model)) : nullptr) {
					const auto [fade, reach] = StartOf(now);
					row->second.fade = fade;
					row->second.reach = reach;
				}
				StreamRows::Recolor(formID, row->second);
			}
		}
		if (reborrowed) {
			ApplyLightStrength(false, true);  // the sliders onto the new numbers (RefreshLights ran it before this pass)
			SKSE::log::info("stream lights: {} bolt(s) with no light of their own took the light their row's settings choose now", reborrowed);
		}
		for (const auto& [formID, own] : gHandLight) {
			auto*      proj = RE::TESForm::LookupByID<RE::BGSProjectile>(formID);
			const auto copy = gStreamCopy.find(formID);
			if (!proj || copy == gStreamCopy.end()) {
				continue;
			}
			// a bolt wears its own light at its own size: its row, or on Performance nothing, lights its length (one raised light
			// flooded the whole scene). One with no light of its own wears the light it borrows, unraised, while no row lights it.
			// With Flicker off its own light and the copy made from it carry no flicker bits
			HoldFlicker(own, FlickerOn());
			HoldFlicker(copy->second, FlickerOn());
			// the record's own color is vanilla's: a spell on the game's art wears a copy in its pick's color while Streams is on
			// a switched-off nature kind (NatureKind.h): its spray wears no raised or tinted copy of ours
			const bool natureOff = !LightsOff() && NatureOff(proj->GetModel() ? proj->GetModel() : "");
			auto*      worn = own;
			if (const auto tint = gOwnTint.find(formID); want && !natureOff && tint != gOwnTint.end()) {
				if (const auto c = gStreamColor.find(formID); c != gStreamColor.end()) {
					SetColor(tint->second, VanillaLook(ColorFor(c->second, sc)));
				}
				HoldFlicker(tint->second, FlickerOn());
				worn = tint->second;
			}
			auto* wanted = want && !natureOff && !PerformanceMode() && !gRows.contains(formID) ? copy->second : worn;
			if (!wanted && want && !natureOff && PerformanceMode()) {
				wanted = BorrowedLight(formID);
			}
			if (natureOff) {
				wanted = nullptr;  // unlit, as the switch says
			}
			if (proj->data.light != wanted) {
				proj->data.light = wanted;
				++changed;
			}
		}
		if (a_log || changed) {
			SKSE::log::info("stream lights: {}; {} projectile(s) wear {}", want ? "on" : "off", changed,
				want && !PerformanceMode() ? "their own light and a row" : "their own light");
		}
		gSprayRows = sc.on && !LightsOff();
		// a row goes with its switch (Streams, or Spray Lights for pass 4's sprays) and All lights off, at once (his 2026-10-20
		// report: All lights off left sprays lit)
		StreamRows::Refresh();
		if (!WardLightsOn()) {
			// a ward's light switched off (or Dynamic Wards there) comes off the scene node now, with every map that counts it:
			// a light only forgotten stays registered and keeps shining where its ward was, and a count left behind keeps the
			// cap full so the pass never lights anything again
			auto*           scene = LightKit::Scene();
			std::lock_guard l{ gLiveLock };
			ForgetIf(scene, [](const Live&) { return true; });  // every live entry here is a ward's
		}
	}
}
