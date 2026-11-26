// HerShadow.cpp - the optional Bottled Shaders shadow download makes her own hand lights shadow casters, and two lit hands
// throw two crossed shadows. While both hands are lit their lights sit at one point between her hands, so they throw one
// shadow and light as much as before; with one hand lit that hand's lights go back where Light Placer put them. With "A shadow
// from each hand" each hand's lights stay at the hand, pushed out of her torso when an animation holds the hand against it.
// A glowing weapon's lights sit at the middle of the weapon, not in the fist at its grip, on every lighting (PlaceWeaponGlows).

#include "Plugin.h"
#include "StrayLights.h"

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace Plugin
{
	namespace
	{
		struct Moved
		{
			RE::NiPointer<RE::NiPointLight> light;
			const RE::NiNode*               parent{ nullptr };  // the place below is in this node's frame only
			RE::NiPoint3                    placed{};           // Light Placer's place for it
		};

		std::vector<Moved> gMoved;  // main thread only

		// the setting in whichever section holds it; parsed, so a repeated key keeps its last value as Open Shaders reads it
		[[nodiscard]] bool PromoteIn(const nlohmann::json& a_node)
		{
			if (a_node.is_object()) {
				if (const auto it = a_node.find("PromoteNormalToShadow"); it != a_node.end() && it->is_boolean()) {
					return it->get<bool>();
				}
			}
			if (a_node.is_structured()) {
				for (const auto& child : a_node) {
					if (PromoteIn(child)) {
						return true;
					}
				}
			}
			return false;
		}

		// Open Shaders' "Promote Normal Lights to Shadow Casters" makes every light a caster, hers too; it reads the setting at start
		[[nodiscard]] bool PromoteOn()
		{
			std::ifstream in("Data/SKSE/Plugins/CommunityShaders/SettingsUser.json", std::ios::binary);
			const auto    json = nlohmann::json::parse(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>(), nullptr, false);
			return !json.is_discarded() && PromoteIn(json);
		}

		// the shadow download's marker file, or Open Shaders promoting every light: without one nothing here moves a light
		[[nodiscard]] bool ShadowDownload()
		{
			static const bool on = [] {
				std::error_code ec;
				return std::filesystem::exists("Data/SKSE/Plugins/Illuminated/Bottled Shaders Shadows.ini", ec) || (InverseSquare() && PromoteOn());
			}();
			return on;
		}

		// "A shadow from each hand" (his 646, on by default): two lit hands keep their lights where Light Placer put them, one
		// shadow each, as two lights would; off = every hand light at one point between them (his 430)
		[[nodiscard]] bool HandsApart() { return ShadowFromEachHand(); }

		void OurLightsUnder(RE::NiAVObject* a_magic, std::vector<RE::NiPointLight*>& a_out)
		{
			if (!a_magic) {
				return;
			}
			// a light faded to nothing (the off choice) leaves its hand unlit, so the other hand casts from its own place
			RE::BSVisit::TraverseScenegraphLights(a_magic, [&a_out](RE::NiPointLight* a_light) {
				const char* name = a_light->name.c_str();
				if (a_light->parent && name && StrayLights::IsOurs(name) && a_light->GetLightRuntimeData().fade > 0.0f) {
					a_out.push_back(a_light);
				}
				return RE::BSVisit::BSVisitControl::kContinue;
			});
		}

		[[nodiscard]] bool WasMoved(const RE::NiPointLight* a_light)
		{
			return std::ranges::any_of(gMoved, [a_light](const Moved& a_m) { return a_m.light.get() == a_light; });
		}

		// where Light Placer put a light, in the world: a light already moved by us answers with its kept place
		[[nodiscard]] RE::NiPoint3 OwnPlace(const RE::NiPointLight* a_light)
		{
			for (const auto& m : gMoved) {
				if (m.light.get() == a_light && a_light->parent == m.parent) {
					return a_light->parent->world * m.placed;
				}
			}
			return a_light->parent->world * a_light->local.translate;
		}

		// Light Placer may attach a weapon glow's light some frames after the draw or equip that started its shader
		constexpr int kGlowLooks = 60;

		std::mutex              gGlowLock;
		std::vector<RE::FormID> gGlowQueue;  // actors whose weapon changed, noted by the sinks
		std::atomic<bool>       gGlowQueued{ false };
		// main thread: each noted actor, the looks it has left, and which of its four places (two views x WEAPON / SHIELD) were placed
		struct GlowWatch
		{
			RE::FormID   id{ 0 };
			int          looks{ kGlowLooks };
			std::uint8_t placed{ 0 };
		};
		constexpr std::uint8_t kAllPlaces = 0b1111;
		std::vector<GlowWatch> gGlowWatch;

		// a weapon drawn or equipped: only its actor's id is noted here (any thread); the player update places the light
		class GlowSinks final :
			public RE::BSTEventSink<SKSE::ActionEvent>,
			public RE::BSTEventSink<RE::TESEquipEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const SKSE::ActionEvent* a_event, RE::BSTEventSource<SKSE::ActionEvent>*) override
			{
				using T = SKSE::ActionEvent::Type;
				if (a_event && a_event->actor && (a_event->type.get() == T::kBeginDraw || a_event->type.get() == T::kEndDraw)) {
					Note(a_event->actor->GetFormID());
				}
				return RE::BSEventNotifyControl::kContinue;
			}
			RE::BSEventNotifyControl ProcessEvent(const RE::TESEquipEvent* a_event, RE::BSTEventSource<RE::TESEquipEvent>*) override
			{
				if (a_event && a_event->actor) {
					Note(a_event->actor->GetFormID());
				}
				return RE::BSEventNotifyControl::kContinue;
			}

		private:
			static void Note(RE::FormID a_id) { NoteWeaponGlow(a_id); }
		};

		// the weapon's own mesh under its hand node: the biggest visible triangle shape at its drawn size, never the swirls'
		// particles or a hidden shape (a blood overlay, a swirl's emitter source scaled four times)
		[[nodiscard]] const RE::BSGeometry* WeaponModel(RE::NiAVObject* a_hand)
		{
			const RE::BSGeometry* model = nullptr;
			float                 biggest = 0.0f;
			RE::BSVisit::TraverseScenegraphGeometries(a_hand, [a_hand, &model, &biggest](RE::BSGeometry* a_geometry) {
				const auto type = a_geometry->GetType().get();
				if (type != RE::BSGeometry::Type::kTriShape && type != RE::BSGeometry::Type::kDynamicTriShape) {
					return RE::BSVisit::BSVisitControl::kContinue;
				}
				float radius = a_geometry->modelBound.radius;
				for (const RE::NiAVObject* object = a_geometry; object && object != a_hand; object = object->parent) {
					if (object->GetAppCulled()) {
						return RE::BSVisit::BSVisitControl::kContinue;
					}
					radius *= object->local.scale;
				}
				if (radius > biggest) {
					model = a_geometry;
					biggest = radius;
				}
				return RE::BSVisit::BSVisitControl::kContinue;
			});
			return model;
		}

		// the mesh's own bound centre carried through its local transforms up to the hand node, then into the world: always valid,
		// where a fresh model's world bound may not be updated yet
		[[nodiscard]] RE::NiPoint3 CentreOf(const RE::BSGeometry& a_model, const RE::NiAVObject& a_hand)
		{
			RE::NiPoint3 point = a_model.modelBound.center;
			for (const RE::NiAVObject* object = &a_model; object && object != &a_hand; object = object->parent) {
				point = object->local * point;
			}
			return a_hand.world * point;
		}

		// our lights under a weapon go to the middle of the weapon: the glow comes from the blade, and a light in the fist at the grip
		// throws the fingers' shadow across the floor. Light Placer's own node for the light moves when it has one (it rewrites a
		// flickering light's place, never its node's). True when a light of ours was placed.
		bool PlaceGlowsUnder(RE::NiAVObject* a_hand)
		{
			const auto* model = a_hand ? WeaponModel(a_hand) : nullptr;
			if (!model || model->modelBound.radius <= 0.0f) {
				return false;
			}
			const auto centre = CentreOf(*model, *a_hand);
			bool       placed = false;
			RE::BSVisit::TraverseScenegraphLights(a_hand, [&centre, &placed](RE::NiPointLight* a_light) {
				const char* name = a_light->name.c_str();
				// Light Placer's lights of our rows, and the record route's own weapon light (ArtLights.cpp, hung at the part's origin)
				if (!a_light->parent || !name || (!StrayLights::IsOurs(name) && std::string_view(name) != "IlluminatedArtLight")) {
					return RE::BSVisit::BSVisitControl::kContinue;
				}
				RE::NiAVObject* moved = a_light;
				if (const char* holder = a_light->parent->name.c_str(); holder && std::string_view(holder).starts_with("LP_Node")) {
					moved = a_light->parent;
				}
				if (moved->parent) {
					moved->local.translate = moved->parent->world.Invert() * centre;
					moved->world.translate = centre;
					placed = true;
				}
				return RE::BSVisit::BSVisitControl::kContinue;
			});
			return placed;
		}

		// each actor whose weapon changed, both of his models, for a few frames or until every place has its light; a place already
		// placed never ends the look for a light another place gets later. Armed by the game's draw and equip events, ending itself
		void PlaceWeaponGlows()
		{
			if (gGlowQueued.exchange(false, std::memory_order_relaxed)) {
				const std::lock_guard lock(gGlowLock);
				for (const auto id : gGlowQueue) {
					if (const auto it = std::ranges::find(gGlowWatch, id, &GlowWatch::id); it != gGlowWatch.end()) {
						*it = GlowWatch{ .id = id, .looks = kGlowLooks, .placed = 0 };
					} else {
						gGlowWatch.push_back(GlowWatch{ .id = id, .looks = kGlowLooks, .placed = 0 });
					}
				}
				gGlowQueue.clear();
			}
			if (gGlowWatch.empty()) {
				return;
			}
			std::erase_if(gGlowWatch, [](GlowWatch& a_watch) {
				auto*       actor = RE::TESForm::LookupByID<RE::Actor>(a_watch.id);
				const auto* state = actor ? actor->AsActorState() : nullptr;
				// a weapon put away hangs no glow on its hand nodes; its next draw notes it again
				if (!state || state->GetWeaponState() == RE::WEAPON_STATE::kSheathed) {
					return true;
				}
				std::uint8_t place = 1;
				for (const bool first : { false, true }) {
					auto* root = actor && actor->Is3DLoaded() ? actor->Get3D(first) : nullptr;
					for (const char* node : { "WEAPON", "SHIELD" }) {
						if (root && (a_watch.placed & place) == 0 && PlaceGlowsUnder(root->GetObjectByName(node))) {
							a_watch.placed |= place;
						}
						place = static_cast<std::uint8_t>(place << 1);
					}
				}
				return a_watch.placed == kAllPlaces || --a_watch.looks <= 0;
			});
		}
	}

	void WatchWeaponGlows()
	{
		static GlowSinks sinks;
		if (auto* actions = SKSE::GetActionEventSource()) {
			actions->AddEventSink(&sinks);
		}
		if (auto* events = RE::ScriptEventSourceHolder::GetSingleton()) {
			events->AddEventSink<RE::TESEquipEvent>(&sinks);
		}
	}

	void NoteWeaponGlow(RE::FormID a_id)
	{
		const std::lock_guard lock(gGlowLock);
		gGlowQueue.push_back(a_id);
		gGlowQueued.store(true, std::memory_order_relaxed);
	}

	void PlaceHerShadow()
	{
		// every lighting: Light Placer's weapon rows and the record route's weapon lights both hang inside WEAPON / SHIELD, and the
		// draw / equip sinks (WatchWeaponGlows) note the actors on both routes
		PlaceWeaponGlows();
		if (!ShadowDownload() || RecordRoute() || REL::Module::IsVR()) {
			return;
		}
		auto*       player = RE::PlayerCharacter::GetSingleton();
		const auto* strings = RE::FixedStrings::GetSingleton();
		if (!player || !strings) {
			return;
		}
		// her hands down and nothing of hers moved: nothing to do (a draw's first frames already light her hands)
		const auto* state = player->AsActorState();
		if (gMoved.empty() && (!state || state->GetWeaponState() == RE::WEAPON_STATE::kSheathed)) {
			return;
		}
		const LightKit::Cost::Pass pass{ 13, "her shadow" };
		// kept between frames so the player update allocates nothing once they have grown
		static std::vector<std::pair<RE::NiPointLight*, RE::NiPoint3>> between;  // this frame: a light and the world point it goes to
		static std::vector<RE::NiPointLight*>                          lights;
		between.clear();
		const bool apart = HandsApart();
		auto*      body = player->Get3D(false);
		for (const bool first : { false, true }) {
			auto* root = player->Get3D(first);
			auto* left = root ? root->GetObjectByName(strings->npcLMagicNode) : nullptr;
			auto* right = root ? root->GetObjectByName(strings->npcRMagicNode) : nullptr;
			if (apart) {
				// the first-person arms are not where her third-person torso is: only that view's lights are kept out of it
				if (first) {
					continue;
				}
				// each hand's lights stay at their hand, out of her torso; a light moved before and clear of it now goes back below
				lights.clear();
				OurLightsUnder(left, lights);
				OurLightsUnder(right, lights);
				for (auto* light : lights) {
					const auto own = OwnPlace(light);
					const auto out = LightKit::OutOfBody(body, own);
					if (out.GetDistance(own) > 0.01f) {
						between.emplace_back(light, out);
					}
				}
				continue;
			}
			lights.clear();
			OurLightsUnder(left, lights);
			const std::size_t leftCount = lights.size();
			OurLightsUnder(right, lights);
			if (leftCount == 0 || leftCount == lights.size()) {
				continue;
			}
			const auto point = (left->world * LightKit::kHandPoint + right->world * LightKit::kHandPoint) * 0.5f;
			for (auto* light : lights) {
				between.emplace_back(light, point);
			}
		}
		// a light no longer between her hands goes back to its own place
		std::erase_if(gMoved, [](const Moved& a_m) {
			const bool still = std::ranges::any_of(between, [&a_m](const auto& a_b) { return a_b.first == a_m.light.get(); });
			if (!still && a_m.light->parent && a_m.light->parent == a_m.parent) {
				a_m.light->local.translate = a_m.placed;
				a_m.light->world.translate = a_m.light->parent->world * a_m.placed;
			}
			return !still;
		});
		for (auto& [light, point] : between) {
			if (!WasMoved(light)) {
				gMoved.push_back({ RE::NiPointer<RE::NiPointLight>(light), light->parent, light->local.translate });
			}
			light->local.translate = light->parent->world.Invert() * point;
			light->world.translate = point;
		}
	}

	void DropHerShadow()
	{
		gMoved.clear();
	}
}
