// Illuminated - SKSE plugin
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE.txt and the notice at the top of main.cpp.
//
// Pass 6: the light of a spray, a stream or a bolt - and a ward dome's light.
//
// HIS RULE (Illuminated's CLAUDE.md, "RELIGHT'S METHOD"): a spray, stream or bolt is lit by the PROJECTILE'S OWN LIGHT RECORD,
// made bigger and brighter - one light per projectile, the way RE::Light lights them in RELight - Spell Addon (its spray
// configs: the record's own light, brightness 1.72 at size 2.5, reach the whole length plus 15%, bounded by one light's
// reach) - and never dimmer than the light it was raised from (his answer 2026-10-13, LightKit::NeverDimmer). So this pass
// gives each cone, beam and flamethrower projectile an in-memory copy of its own light raised that way, worn while the
// "Streams" setting is on, its own light when it is off. A spray model pass 4 already raised (the Spray Lights setting) keeps
// pass 4's copy and is left to that setting alone - his answer 2026-10-13: Streams off does not touch the sprays Spray Lights
// brightens, two settings, each doing one thing. The Brightness and Reach sliders reach the copies (LightCopies). Colors, picked again on every settings refresh: a
// BEAM's row in the Streams folder whose settings hold (the data's color - the build moves every beam row out of Light Placer,
// lagen move_beams, because on Community Shaders the data's six points stopped half way along Sparks, measured 2026-10-13),
// else the spray markers (frost, shock), else the record's own; a beam with no light of its own wears a raised copy of its
// row's light (the row that held when the game loaded). ⛔ No light of ours is hung on a spray, stream or
// bolt; `PC Runner\stream rule check.py` fails a build that does (it took several rounds of ladders before his word was read
// right).
//
// A ward dome (Dynamic Wards absent) is the one projectile this pass still hangs a light on: its Load3D / Release3D hooks,
// the live list and the main-thread queue below are for that. CREDIT: how that light is made and registered follows ReLight
// by Truman (github.com/TrumanGIT/ReLight), GPL-3.0-or-later, with his permission and kept under the same license: one master
// NiPointLight made once and cloned for every use, the light's size carried in the radius' z, the create parameters a
// non-shadow light needs, and handing the light to the shadow scene node. The scene is only ever touched on the main thread
// (3D built or released on a loader thread is queued for the next frame - StreamLightsFrame).

#include "BeamRow.h"
#include "Plugin.h"

// 🔁 HIS ANSWER 2026-10-20 (checklist 140): one light stretched along the beam - and "if you plain out cannot make part 1 of
// 140 to happen then revert to this as a backup plan", a row of lights. One stretched light cannot be drawn inside our mods
// (his research page, part 8), so a BOLT (beam projectile) now wears a ROW of small lights spread over its real length every
// frame (BeamRow.h: no two farther apart than 125, spots under 5 %, the light staying near the beam), and its own light at its
// own size - no more one light raised to the whole length (the flood he reported). It lifts the 2026-10-13 rule above for
// beams only (`stream rule check.py` allows a beam hook only beside BeamRow::Lay). Flame sprays and cones keep the raised own
// light until their row is built. The row's moving parts are the old ladder's (local branch beam-ladder): lights hung off the
// projectile's root along its path (PathOf: a bolt straight to what it hit), the length its newest impact or else its range.

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
			const auto* row = a_c.beam ? StreamRowFor(a_c.model) : nullptr;
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
			SetColor(a_copy, ColorFor(a_c, a_sc));
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

		// a bolt's row (his checklist 140): the color, the fade and the reach its raised copy was given - the target the row
		// matches under the beam's middle (BeamRow::Lay scales every light to it)
		struct RowRecipe
		{
			RE::NiColor              color{ 1.0f, 1.0f, 1.0f };
			float                    fade{ 1.0f };
			float                    reach{ 300.0f };
			bool                     beam{ true };          // a bolt (along its model) - else a flame spray or cone (along its aim)
			bool                     spray{ false };        // a spray pass 4 raised: its row answers to the Spray Lights setting
			const RE::TESObjectLIGH* colorFrom{ nullptr };  // pass 4's copy, recolored on every refresh: the row wears its color
		};
		std::unordered_map<RE::FormID, RowRecipe> gRows;                // beam base form -> its row
		constexpr std::size_t                     kMostRowLights = 64;  // every row light in the scene together
		constexpr std::size_t                     kMostPerRow = 20;
		constexpr float                           kRowHeight = 100.0f;  // a beam's usual height over the ground it lights
		constexpr const char*                     kRowName = "IlluminatedStreamRow";
		// HIS LOOK 2026-10-20 ~19:00: "make vanilla brighter enb softer as suggested to align with cs" - the same row lifted the
		// ground 3-33 per band on Community Shaders, 9-24 on Vanilla and 33-72 on ENB (run cs / vanilla / enb, 18:29-18:43)
		[[nodiscard]] float RowStrengthFor(Lighting a_lighting)
		{
			switch (a_lighting) {
			case Lighting::kVanilla:
				return 1.1f;
			case Lighting::kEnb:
				return 0.18f;  // kept to Community Shaders' lift once its row came off its cap
			default:
				return 1.0f;
			}
		}
		// Community Shaders draws a row light at 4x its fade, so twice the reach laid; drawn plain a row reaches only that
		[[nodiscard]] float RowReachFor(Lighting a_lighting)
		{
			return a_lighting == Lighting::kShaders ? 1.0f : 2.0f;
		}
		std::size_t gRowLights = 0;      // row lights in the scene now (under gLiveLock)
		bool        gSprayRows = false;  // pass 4's sprays wear a row: the Spray Lights setting holds and all lights are not off

		bool RowOn(const RowRecipe& a_rec)
		{
			return !PerformanceMode() && (a_rec.spray ? gSprayRows : StreamLightsOn());  // Performance: no rows (his order 2026-10-20)
		}

		// One lit projectile, keyed by its 3D ROOT, not its reference ID: a projectile's reference is temporary and the game
		// hands its ID to a new one, so an ID-keyed entry could be taken for the new projectile or dropped by the old one's
		// late release. The root is held here, so its address is never another 3D's while it is listed (the re-score's
		// Illuminated issue 3).
		struct Live
		{
			RE::NiPointer<RE::NiAVObject>           root;
			RE::FormID                              base{ 0 };
			std::vector<RE::NiPointer<RE::BSLight>> lights;
			RE::NiPointer<RE::TESObjectREFR>        beam;  // a bolt's row: its projectile, read every frame for its length
		};
		std::unordered_map<const RE::NiAVObject*, Live> gLive;
		// the live count, by projectile BASE rather than by reference: a ward raised again and again makes a new reference
		// each time, and it is the base that has to be capped
		std::unordered_map<RE::FormID, std::size_t> gLiveCount;
		std::mutex                                  gLiveLock;
		// what a loader thread built or released, waiting for the main thread (under gLiveLock). The reference and the 3D are
		// held (refcounted) - never a handle made off the main thread (the fading module's rule; re-score issue 2), and a
		// queued 3D's address is never another's when its turn comes (the cap's sweep may have let its entry go first)
		std::vector<RE::NiPointer<RE::TESObjectREFR>> gToHang;
		std::vector<RE::NiPointer<RE::NiAVObject>>    gToDrop;
		std::uint32_t                                 gSweepClock = 0;  // frames since the last sweep (main thread)
		// a projectile that flies while its row is switched off (Performance, Streams, Spray Lights, All lights off): its row is hung
		// when the switch comes back, or a held stream stays bare until the next cast (main thread)
		std::vector<RE::NiPointer<RE::TESObjectREFR>> gParkedRows;
		constexpr std::size_t                         kMostParked = 64;

		void ParkRow(RE::TESObjectREFR* a_ref)
		{
			std::erase_if(gParkedRows, [](const auto& a_parked) {
				const auto* root = a_parked ? a_parked->Get3D() : nullptr;
				return !root || !root->parent;
			});
			if (a_ref && gParkedRows.size() < kMostParked) {
				gParkedRows.emplace_back(a_ref);
			}
		}

		// every ward belongs to Dynamic Wards: when that mod is here, this pass does not light a ward
		bool WardLightsOn()
		{
			return SettingValue(kWardSetting, 0) != 0 && !DynamicWardsLoaded() && !LightsOff();
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
			if (it->second.beam) {
				gRowLights -= (std::min)(gRowLights, it->second.lights.size());
				// a row switched off while its projectile lives (a setting, Performance) leaves no light node behind on it - a row
				// hung again later starts clean (the code review)
				for (auto& bs : it->second.lights) {
					if (auto* light = bs ? bs->light.get() : nullptr; light && light->parent) {
						light->parent->DetachChild(light);
					}
				}
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

		// Community Shaders draws an inverse-square point light at four times its fade and writes back the reach that gives (run cs
		// 2026-10-20 18:15: every row light read twice the reach laid, 389 for 197) - so the cutoff is set for that
		constexpr float kDrawnTimes = 4.0f;

		float IslRoom(float a_radius)
		{
			return a_radius * a_radius + kK * kLightSize * kLightSize * 0.5f;
		}

		// the strongest fade a row light of this reach can take on Community Shaders before its cutoff passes its top
		float IslTop(float a_radius)
		{
			return LightKit::Isl::kHighestCutoff * IslRoom(a_radius) / (kDrawnTimes * kK);
		}

		// a light's strength, reach and color written whole - when it is made and again every frame a row moves (the ward's
		// way below: the size in the radius' z, the attenuation, then Community Shaders' own words or a tenth for ambient)
		void Dress(RE::NiPointLight* a_light, const RE::NiColor& a_color, float a_fade, float a_radius, bool a_isl)
		{
			auto& data = a_light->GetLightRuntimeData();
			data.diffuse = a_color;
			data.fade = a_fade;
			data.radius = { a_radius, a_radius, kLightSize };
			a_light->SetLightAttenuation(a_radius);
			if (a_isl) {
				data.fade = (std::min)(a_fade, IslTop(a_radius));  // past its top the reach would grow instead
				LightKit::Isl::SetOwn(a_light);
				LightKit::Isl::SetCutoff(a_light, kDrawnTimes * kK * data.fade / IslRoom(a_radius));
			} else {
				data.ambient = { a_color.red * 0.1f, a_color.green * 0.1f, a_color.blue * 0.1f };
			}
		}

		// where a row starts and which way it runs: a bolt along its model's +Y from its root; a flame spray or cone along the
		// projectile's own aim from where it stands (a spray's model points another way - RELight's measurement 2026-10-11:
		// its forward is -X, sideways in third person)
		struct Axis
		{
			RE::NiPoint3 start;
			RE::NiPoint3 dir{ 0.0f, 1.0f, 0.0f };
		};

		Axis AxisOf(const RE::TESObjectREFR* a_ref, const RE::NiAVObject* a_root, bool a_beam)
		{
			if (a_beam) {
				return { a_root->world.translate, a_root->world.rotate * RE::NiPoint3{ 0.0f, 1.0f, 0.0f } };
			}
			const float pitch = a_ref->GetAngleX(), yaw = a_ref->GetAngleZ();
			return { a_ref->GetPosition(), { std::sin(yaw) * std::cos(pitch), std::cos(yaw) * std::cos(pitch), -std::sin(pitch) } };
		}

		// how far along the row's axis the drawn art reaches (its geometry's bounds, our row lights not counted); 0 when nothing
		// is drawn. Main thread.
		float DrawnLength(RE::NiAVObject* a_root, const Axis& a_axis)
		{
			float reach = 0.0f;
			RE::BSVisit::TraverseScenegraphGeometries(a_root, [&](RE::BSGeometry* a_geometry) {
				const auto& bound = a_geometry->worldBound;
				if (bound.radius > 0.0f) {
					reach = (std::max)(reach, (bound.center - a_axis.start).Dot(a_axis.dir) + bound.radius);
				}
				return RE::BSVisit::BSVisitControl::kContinue;
			});
			return std::isfinite(reach) ? reach : 0.0f;
		}

		// where a row runs right now and how long it is: a bolt that hit something runs straight from its root to that impact
		// (the lightning is drawn to what it hit), one in the open along its model as far as its art is drawn; a flame spray or
		// cone along its aim, never past its newest impact. Never past the range. Main thread.
		struct Path
		{
			Axis  axis;
			float length{ 0.0f };
		};

		Path PathOf(RE::TESObjectREFR* a_ref, RE::NiAVObject* a_root, bool a_beam)
		{
			Path  path{ AxisOf(a_ref, a_root, a_beam) };
			auto* proj = a_ref ? a_ref->As<RE::Projectile>() : nullptr;
			if (!proj) {
				return path;
			}
			auto& rt = proj->GetProjectileRuntimeData();
			path.length = rt.range;
			if (const float drawn = DrawnLength(a_root, path.axis); drawn > 1.0f && drawn < path.length) {
				path.length = drawn;
			}
			for (const auto* impact : rt.impacts) {  // the newest impact comes first
				if (impact) {
					const auto  hit = impact->desiredTargetLoc - path.axis.start;
					const float d = hit.Length();
					if (d > 1.0f && d < rt.range && (a_beam || d < path.length)) {
						path.length = d;
						if (a_beam) {
							path.axis.dir = hit / d;
						}
					}
					break;
				}
			}
			if (!std::isfinite(path.length)) {
				path.length = 0.0f;
			}
			return path;
		}

		// every bolt's row spread over its length, strength and reach laid again for that length (BeamRow::Lay), once a frame.
		// Main thread, gLiveLock held by the caller.
		void MoveRows()
		{
			const bool  isl = InverseSquare();
			const float bright = BrightnessNow() * RowStrengthFor(LightingFound()), reachPct = Percent("IlluminatedReach");
			// his report: "the streams/sprays don't flicker at all so it's just a beam" - a spray's own light is steady and Light
			// Placer's hand lights are separate objects, so the row flickers by itself while the Flicker switch is on
			static const auto start = std::chrono::steady_clock::now();
			const double      seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
			const bool        flickers = FlickerOn();
			RE::NiUpdateData  update{};
			for (auto& [root, live] : gLive) {
				if (!live.beam || !live.root || live.lights.empty()) {
					continue;
				}
				const auto rec = gRows.find(live.base);
				if (rec == gRows.end()) {
					continue;
				}
				const bool  beam = rec->second.beam;
				const auto  path = PathOf(live.beam.get(), live.root.get(), beam);
				const auto& axis = path.axis;
				const float length = path.length;
				if (length <= 1.0f) {
					continue;
				}
				if (static std::uint32_t frames = 0; ++frames % 30 == 0 && frames <= 30 * 60) {
					SKSE::log::info("[STREAM-ROW-LENGTH] {} | drawn art reaches {:.0f} | length used {:.0f} | start {:.0f} from its root | path . root +Y {:.2f} | start {:.0f},{:.0f},{:.0f} end {:.0f},{:.0f},{:.0f}",
						beam ? "bolt" : "spray", DrawnLength(live.root.get(), axis), length, axis.start.GetDistance(live.root->world.translate),
						axis.dir.Dot(live.root->world.rotate * RE::NiPoint3{ 0.0f, 1.0f, 0.0f }), axis.start.x, axis.start.y, axis.start.z,
						axis.start.x + axis.dir.x * length, axis.start.y + axis.dir.y * length, axis.start.z + axis.dir.z * length);
				}
				const RE::NiColor color = rec->second.colorFrom ? ColorOf(rec->second.colorFrom) : rec->second.color;
				const auto        toLocal = live.root->world.Invert();
				const auto        row = BeamRow::Lay(length, static_cast<int>(live.lights.size()), rec->second.reach, kRowHeight);
				const float       scale = live.root->world.scale > 0.0f ? live.root->world.scale : 1.0f;
				for (std::size_t i = 0; i < live.lights.size(); ++i) {
					auto* bs = live.lights[i].get();
					auto* light = bs ? netimmerse_cast<RE::NiPointLight*>(bs->light.get()) : nullptr;
					if (!light) {
						continue;
					}
					if (i >= row.size()) {  // a beam cut so short it needs fewer: the spare lights rest dark at the hand
						Dress(light, color, 0.0f, 1.0f, isl);
						light->local.translate = {};
						continue;
					}
					const auto& s = row[i];
					// the radius is the scene's own units - the root's scale moves a child light's place, never its radius: divided
					// by the Sparks root's scale (~0.4-0.5) it read 2-2.4x too wide (run cs 2026-10-20 17:11: laid ~262 / ~180,
					// read 540 / 437, while the places read right)
					const float reach = s.reach * reachPct * RowReachFor(LightingFound());
					const float want = rec->second.fade * s.strength * bright;
					// a dimmer Brightness or daylight lowers the cap too, so the sliders still reach a capped row
					const float steady = BeamRow::Steady(want, isl ? IslTop(reach) * (std::min)(BrightnessNow(), 1.0f) : 0.0f);
					Dress(light, color, steady * (flickers ? BeamRow::Flicker(seconds, i) : 1.0f), reach, isl);
					light->local.translate = toLocal * (axis.start + axis.dir * (s.along * length));  // on the path, in the root's units
					if (static std::size_t told = 0; told < 6) {
						++told;
						SKSE::log::info("[STREAM-ROW-LAID] {} light(s) over {:.0f} | reach {:.0f} | strength {:.2f} steady {:.2f} (set {:.2f}, top {:.2f}) | brightness {:.2f} | root scale {:.2f}",
							row.size(), length, reach, want, steady, light->GetLightRuntimeData().fade, isl ? IslTop(reach) : 0.0f, bright, scale);
					}
					light->Update(update);
				}
			}
		}

		// a bolt's row: as many small lights as its range needs (BeamRow::Lay), hung off its root, within the scene's budget.
		// Main thread.
		void HangRow(RE::TESObjectREFR* a_ref, RE::NiNode* a_root, RE::FormID a_base, const RowRecipe& a_rec)
		{
			auto* scene = SceneNode();
			auto* proj = a_ref->As<RE::Projectile>();
			if (!scene || !proj) {
				return;
			}
			if (!RowOn(a_rec)) {
				ParkRow(a_ref);
				return;
			}
			std::size_t n = 0;
			{
				std::lock_guard l{ gLiveLock };
				if (gLive.contains(a_root)) {
					return;
				}
				if (gRowLights >= kMostRowLights) {
					SweepGone(scene);
				}
				const auto want = BeamRow::Lay(proj->GetProjectileRuntimeData().range, static_cast<int>(kMostPerRow), a_rec.reach, kRowHeight);
				n = (std::min)(want.size(), kMostRowLights - (std::min)(gRowLights, kMostRowLights));
			}
			if (!n) {
				return;
			}
			const bool                              isl = InverseSquare();
			std::vector<RE::NiPointer<RE::BSLight>> made;
			for (std::size_t i = 0; i < n; ++i) {
				auto* light = LightKit::CloneLight();  // ReLight's master-and-clone (LightKit.h)
				if (!light) {
					break;
				}
				light->name = kRowName;
				Dress(light, a_rec.color, 0.0f, 1.0f, isl);  // dark until MoveRows lays it out this frame
				light->local.translate = {};
				light->local.scale = 1.0f;
				a_root->AttachChild(light, true);
				RE::NiUpdateData update{};
				light->Update(update);
				if (auto* bs = LightKit::AddToScene(scene, light, 2.0f)) {
					made.emplace_back(bs);
				} else {
					a_root->DetachChild(light);
				}
			}
			static std::size_t told = 0;
			if (told < 8 && !made.empty()) {
				++told;
				SKSE::log::info("[STREAM-ROW] {} | {} light(s) along {:.0f} | reach target {:.0f} | fade {:.2f}", Label(a_ref->GetBaseObject()),
					made.size(), proj->GetProjectileRuntimeData().range, a_rec.reach, a_rec.fade);
			}
			if (made.empty()) {
				return;
			}
			std::lock_guard l{ gLiveLock };
			gRowLights += made.size();
			gLive.emplace(a_root, Live{ RE::NiPointer<RE::NiAVObject>(a_root), a_base, std::move(made), RE::NiPointer<RE::TESObjectREFR>(a_ref) });
			++gLiveCount[a_base];
			MoveRows();  // laid out at once, not a frame late
		}

		void HangLights(RE::TESObjectREFR* a_ref, RE::NiAVObject* a_object)
		{
			if (auto* base = a_ref ? a_ref->GetBaseObject() : nullptr; base && a_object && a_object->AsNode()) {
				if (const auto row = gRows.find(base->GetFormID()); row != gRows.end()) {
					HangRow(a_ref, a_object->AsNode(), base->GetFormID(), row->second);
					return;
				}
			}
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
			auto* scene = SceneNode();
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
			// ENB and Vanilla (Lighting.cpp): the reach asked for, drawn by the game's own lighting. Brightness x Dim in daylight,
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
					data.ambient = { r.color.red * 0.1f, r.color.green * 0.1f, r.color.blue * 0.1f };
				}
				// inverse square flag and cutoff, after SetLightAttenuation - Community Shaders only
				if (isl) {
					LightKit::Isl::SetOwn(light);  // a light of our own: Community Shaders' words written whole (LightKit.h)
					LightKit::Isl::SetCutoff(light, LightKit::CutoffFor(fade, radius, kLightSize));
				}
				light->local.translate = {};
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
				Forget(root, SceneNode());
			} else {
				gToDrop.emplace_back(root);
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
				LightKit::Guarded("ward light (3D load)", [&]() {
					if (OnMainThread()) {
						HangLights(a_this, object);
					} else if (object) {
						std::lock_guard l{ gLiveLock };
						gToHang.emplace_back(a_this);  // held by its refcount, no handle made on this thread
					}
				});
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
		std::size_t cones = 0, beams = 0, flames = 0, kept = 0, skipped = 0;
		for (auto* proj : LightKit::FormsOf<RE::BGSProjectile>()) {
			if (!proj) {
				continue;
			}
			// a flamethrower, cone or beam projectile: one light, its own, raised
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
			const float range = proj->data.range;
			// a flame spray's range is the reach of the cone, so the shortest-stream floor does not apply
			if (!flame && range < kShortestStream) {
				++skipped;
				continue;
			}
			// HIS RULE (CLAUDE.md, "RELIGHT'S METHOD"): the projectile's OWN light record, a bigger and brighter copy - one light
			// per spray / stream / bolt, as RE::Light's isPluginLight configs do (RELight - Spell Addon's spray configs: the record's
			// copy, brightness 1.72 at size 2.5, reach from the hand to the end plus 15%, bounded by one light's reach). No light
			// of ours is hung on it. A projectile with no light of its own is left as it is.
			// a beam whose rows the build moved out of Light Placer (lagen move_beams): the data's color for it, and its light when
			// the projectile has none of its own (taken again on every settings refresh - ApplyStreamLights)
			const auto* row = beam ? StreamRowFor(model) : nullptr;
			auto*       own = proj->data.light;
			const auto* base = own ? own : RowLight(row);
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
				gRows[proj->GetFormID()] = { ColorOf(tint ? tint : original), original->fade, static_cast<float>(original->data.radius), false, true, tint };
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
			const auto&  color = gStreamColor[proj->GetFormID()] = { model, family, beam, base->data.color, own ? nullptr : base, range,
				VanillaArtModel(model) };
			PickColor(color, copy, sc);  // and again on every refresh (ApplyStreamLights)
			// his checklist 140: a bolt wears a row of lights (BeamRow.h) whose middle glows as its OWN light would right under
			// it - never the raised copy's numbers: matched to those (run cs 2026-10-20 16:59) the row lit the whole courtyard,
			// steps and walls, his "reach the entire universe"
			// (and since his 2026-10-20 "Do the flame sprays and cones in Illuminated first", a flame spray and a cone the same way,
			// along their aim)
			gRows[proj->GetFormID()] = { ColorOf(copy), base->fade, static_cast<float>(base->data.radius), beam };
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
			"4's stretched copy (the Spray Lights setting's, not this one's); {} left alone; the setting is {}",
			flames, cones, beams, kept, skipped, StreamLightsOn() ? "on" : "off");
		ApplyLightStrength(false);  // every copy at the sliders' values from the start, pass 4's and these
		WardLights();
		Load3D<RE::BarrierProjectile>::Install();  // a ward dome's light (Dynamic Wards absent)
		Release3D<RE::BarrierProjectile>::Install();
		Load3D<RE::BeamProjectile>::Install();  // a bolt's row of lights (his checklist 140, BeamRow.h)
		Release3D<RE::BeamProjectile>::Install();
		Load3D<RE::FlameProjectile>::Install();  // a flame spray's and a cone's row, along their aim (2026-10-20)
		Release3D<RE::FlameProjectile>::Install();
		Load3D<RE::ConeProjectile>::Install();
		Release3D<RE::ConeProjectile>::Install();
		const auto beamRows = std::ranges::count_if(gRows, [](const auto& a_r) { return a_r.second.beam; });
		SKSE::log::info(
			"stream lights: {} bolt(s) and {} spray(s) / cone(s) wear a row of lights along their real length (at most {} "
			"per row, {} in the scene)",
			beamRows, gRows.size() - static_cast<std::size_t>(beamRows), kMostPerRow, kMostRowLights);
		ApplyStreamLights(true);
		ApplySprayColors();  // pass 4's sprays put back on their own light from the start - their rows light the length
	}

	// once a frame, on the main thread - what the loader threads left to do
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
			if (gRowLights) {
				MoveRows();  // his checklist 140: every bolt's row follows its real length this frame
			}
			if (gToHang.empty() && gToDrop.empty()) {
				return;
			}
			auto* scene = SceneNode();
			for (const auto& root : gToDrop) {
				Forget(root.get(), scene);
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
				c.own = now->data.color;
				++reborrowed;
			}
			PickColor(c, copy->second, sc);
			if (const auto row = gRows.find(formID); row != gRows.end()) {
				row->second.color = ColorOf(copy->second);  // the row takes the color the settings pick now
				if (c.borrowed) {                           // and a borrowed light's own strength and reach (the code review)
					row->second.fade = c.borrowed->fade;
					row->second.reach = static_cast<float>(c.borrowed->data.radius);
				}
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
			// a bolt with a row (his checklist 140) wears its own light at its own size: the row lights its length. On Performance
			// (no rows) it keeps its own light too - the one raised light flooded the whole courtyard (ENB test 2026-10-20 21:08,
			// radius 1,342; his "the spray lighting in the background is ruining all of the feature images")
			// one with no light of its own wears the light it borrows, unraised, while no row lights it (Performance) - never
			// nothing (CodeRabbit on #28)
			// with Flicker off its own light, and the raised copy made from it, carry no flicker bits (his report: "performance was
			// flickering when casting sparks even though flicker was turned off")
			HoldFlicker(own, FlickerOn());
			HoldFlicker(copy->second, FlickerOn());
			// the record's own color is vanilla's: a spell on the game's art wears a copy in its pick's color while Streams is on
			auto* worn = own;
			if (const auto tint = gOwnTint.find(formID); want && tint != gOwnTint.end()) {
				if (const auto c = gStreamColor.find(formID); c != gStreamColor.end()) {
					SetColor(tint->second, ColorFor(c->second, sc));
				}
				HoldFlicker(tint->second, FlickerOn());
				worn = tint->second;
			}
			auto* wanted = want && !PerformanceMode() && !gRows.contains(formID) ? copy->second : worn;
			if (!wanted && want && PerformanceMode()) {
				wanted = BorrowedLight(formID);
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
		{
			// a row goes with its switch (Streams, or Spray Lights for pass 4's sprays) and All lights off, at once (his
			// 2026-10-20 report: All lights off left sprays lit)
			auto*                              scene = SceneNode();
			std::lock_guard                    l{ gLiveLock };
			std::vector<const RE::NiAVObject*> rows;
			for (const auto& [root, live] : gLive) {
				if (const auto rec = gRows.find(live.base); live.beam && (rec == gRows.end() || !RowOn(rec->second))) {
					rows.push_back(root);
					if (rec != gRows.end()) {
						ParkRow(live.beam.get());
					}
				}
			}
			for (const auto* root : rows) {
				Forget(root, scene);
			}
		}
		{
			// a row switched back on: the projectiles still flying get theirs now (HangRow parks one whose row is still off)
			auto parked = std::move(gParkedRows);
			gParkedRows.clear();
			for (const auto& ref : parked) {
				auto* root = ref ? ref->Get3D() : nullptr;
				auto* base = ref ? ref->GetBaseObject() : nullptr;
				if (!root || !root->parent || !root->AsNode() || !base) {
					continue;
				}
				if (const auto rec = gRows.find(base->GetFormID()); rec != gRows.end()) {
					HangRow(ref.get(), root->AsNode(), base->GetFormID(), rec->second);
				}
			}
		}
		if (!WardLightsOn()) {
			// a ward's light switched off (or Dynamic Wards there) comes off the scene node now, with every map that counts it:
			// a light only forgotten stays registered and keeps shining where its ward was, and a count left behind keeps the
			// cap full so the pass never lights anything again
			auto*                              scene = SceneNode();
			std::lock_guard                    l{ gLiveLock };
			std::vector<const RE::NiAVObject*> lit;
			for (const auto& [root, live] : gLive) {
				if (!live.beam) {  // a ward's - the rows answer to their own switches above
					lit.push_back(root);
				}
			}
			for (const auto* root : lit) {
				Forget(root, scene);
			}
		}
	}
}
