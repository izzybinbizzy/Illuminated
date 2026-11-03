// HerShadow.cpp - the optional Bottled Shaders shadow download makes her own hand lights shadow casters; with both hands lit
// they threw two crossed shadows (his checklist 430). While both hands are lit, their lights sit at one point between her
// hands, so they throw one shadow; with one hand lit that hand's lights go back where Light Placer put them. RELight's
// Hooks.cpp PlaceHerShadow is the same idea for its own lights.

#include "Plugin.h"
#include "StrayLights.h"

#include <algorithm>
#include <filesystem>
#include <system_error>
#include <utility>
#include <vector>

namespace Plugin
{
	namespace
	{
		struct Moved
		{
			RE::NiPointer<RE::NiPointLight> light;
			RE::NiPoint3                    placed{};  // Light Placer's place for it, in its parent's frame
		};

		std::vector<Moved> gMoved;  // main thread only

		// the shadow download's marker file: without it nothing here moves a light
		[[nodiscard]] bool ShadowDownload()
		{
			static const bool on = [] {
				std::error_code ec;
				return std::filesystem::exists("Data/SKSE/Plugins/Illuminated/Bottled Shaders Shadows.ini", ec);
			}();
			return on;
		}

		void OurLightsUnder(RE::NiAVObject* a_magic, std::vector<RE::NiPointLight*>& a_out)
		{
			if (!a_magic) {
				return;
			}
			RE::BSVisit::TraverseScenegraphLights(a_magic, [&a_out](RE::NiPointLight* a_light) {
				const char* name = a_light->name.c_str();
				if (a_light->parent && name && StrayLights::IsOurs(name)) {
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
		std::vector<std::pair<RE::NiPointLight*, RE::NiPoint3>> between;  // this frame: a light and the world point it goes to
		for (const bool first : { false, true }) {
			auto*                          root = player->Get3D(first);
			auto*                          left = root ? root->GetObjectByName(strings->npcLMagicNode) : nullptr;
			auto*                          right = root ? root->GetObjectByName(strings->npcRMagicNode) : nullptr;
			std::vector<RE::NiPointLight*> lights;
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
		std::erase_if(gMoved, [&between](const Moved& a_m) {
			const bool still = std::ranges::any_of(between, [&a_m](const auto& a_b) { return a_b.first == a_m.light.get(); });
			if (!still && a_m.light->parent) {
				a_m.light->local.translate = a_m.placed;
				a_m.light->world.translate = a_m.light->parent->world * a_m.placed;
			}
			return !still;
		});
		for (auto& [light, point] : between) {
			if (!WasMoved(light)) {
				gMoved.push_back({ RE::NiPointer<RE::NiPointLight>(light), light->local.translate });
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
