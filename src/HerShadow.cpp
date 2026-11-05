// HerShadow.cpp - the optional Bottled Shaders shadow download makes her own hand lights shadow casters, and two lit hands
// throw two crossed shadows. While both hands are lit their lights sit at one point between her hands, so they throw one
// shadow and light as much as before; with one hand lit that hand's lights go back where Light Placer put them.

#include "Plugin.h"
#include "StrayLights.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
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
	}

	void PlaceHerShadow()
	{
		if (!ShadowDownload() || RecordRoute() || REL::Module::IsVR()) {
			return;
		}
		auto*       player = RE::PlayerCharacter::GetSingleton();
		const auto* strings = RE::FixedStrings::GetSingleton();
		if (!player || !strings) {
			return;
		}
		// kept between frames so the player update allocates nothing once they have grown
		static std::vector<std::pair<RE::NiPointLight*, RE::NiPoint3>> between;  // this frame: a light and the world point it goes to
		static std::vector<RE::NiPointLight*>                          lights;
		between.clear();
		for (const bool first : { false, true }) {
			auto* root = player->Get3D(first);
			auto* left = root ? root->GetObjectByName(strings->npcLMagicNode) : nullptr;
			auto* right = root ? root->GetObjectByName(strings->npcRMagicNode) : nullptr;
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
