// Illuminated - SKSE plugin
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE.txt and the notice at the top of main.cpp.
//
// A soft glow on every atronach the HexRim add-on names, on every lighting. The add-on's settings file lists each race and its
// color ([atronach] blocks); without that file nothing here runs. iLluminous Elementals keeps the atronachs its own files light.
//
// CREDIT: a light is made and registered the way ReLight by Truman does it (github.com/TrumanGIT/ReLight, GPL-3.0-or-later,
// used with his permission): a master NiPointLight cloned for every use and handed to the shadow scene node.

#include "Fade.h"
#include "Plugin.h"

namespace Plugin
{
	namespace
	{
		constexpr const char*      kName = "IlluminatedAtronachLight";
		constexpr std::string_view kSwitch = "IlluminatedHexRimAtronachs";
		constexpr std::size_t      kMostLights = 16;
		constexpr float            kFade = 1.0f;
		constexpr float            kReach = 300.0f;
		constexpr float            kHeight = 90.0f;    // about its chest, above the feet its root stands on
		constexpr std::uint32_t    kRefusedWait = 60;  // a full scene: never a clone every frame

		struct Lit
		{
			RE::NiPointer<RE::NiNode>       root;
			RE::NiPointer<RE::NiPointLight> light;
			RE::NiPointer<RE::BSLight>      bs;
			RE::NiColor                     color;
			bool                            seen{ false };
		};
		std::unordered_map<RE::FormID, Lit>                 gLit;  // main thread only, by the actor's form id
		std::unordered_map<const RE::TESRace*, RE::NiColor> gRaces;
		std::uint32_t                                       gWait{ 0 };
		std::size_t                                         gRoom{ 0 };  // this frame's share of the glow limit

		// the races another mod's Light Placer files light, only while Light Placer runs: its AtronachFlame.json is AtronachFlameRace
		[[nodiscard]] std::unordered_set<std::string> ElementalRaces()
		{
			std::unordered_set<std::string> out;
			if (!REX::W32::GetModuleHandleA("po3_LightPlacer.dll")) {
				return out;
			}
			std::error_code ec;
			const auto      dir = std::filesystem::current_path(ec) / "Data" / "LightPlacer" / "iLluminousElementals";
			for (auto it = std::filesystem::recursive_directory_iterator(dir, std::filesystem::directory_options::skip_permission_denied, ec);
				!ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
				if (const auto stem = Lower(Fade::PathText(it->path().stem())); stem.starts_with("atronach")) {
					out.insert(stem + "race");
				}
			}
			return out;
		}

		void Drop(Lit& a_lit, RE::ShadowSceneNode* a_scene)
		{
			if (a_scene && a_lit.bs) {
				a_scene->RemoveLight(a_lit.bs);
			}
			if (a_lit.light && a_lit.light->parent) {
				a_lit.light->parent->DetachChild(a_lit.light.get());
			}
			a_lit = {};
		}

		void Dress(Lit& a_lit)
		{
			const auto  turn = LightKit::Turn();
			auto*       light = a_lit.light.get();
			auto&       data = light->GetLightRuntimeData();
			const float strength = kFade * BrightnessNow();
			const float reach = kReach * static_cast<float>(SettingValue("IlluminatedReach", 100)) / 100.0f;
			data.diffuse = a_lit.color;
			if (InverseSquare()) {
				data.fade = strength;
				if (data.radius.x != reach) {
					data.radius = { reach, reach, LightKit::kLightSize };
					light->SetLightAttenuation(reach);
				}
				LightKit::Isl::SetOwn(light);
				LightKit::Isl::SetCutoff(light, LightKit::CutoffFor(strength, reach));
			} else {
				const auto plain = LightKit::PlainAt(strength, reach, reach);
				data.fade = plain.fade;
				if (data.radius.x != plain.radius) {
					data.radius = { plain.radius, plain.radius, LightKit::kLightSize };
					light->SetLightAttenuation(plain.radius);
				}
				data.ambient = LightKit::Tenth(a_lit.color);
			}
		}

		void Visit(RE::Actor* a_actor, RE::ShadowSceneNode* a_scene)
		{
			const auto race = gRaces.find(a_actor->GetRace());
			auto*      root = race != gRaces.end() && !a_actor->IsDead() && a_actor->Is3DLoaded() ? a_actor->GetCurrent3D() : nullptr;
			auto*      node = root ? root->AsNode() : nullptr;
			if (!node) {
				return;
			}
			auto it = gLit.find(a_actor->GetFormID());
			if (it != gLit.end() && it->second.root.get() != node) {
				Drop(it->second, a_scene);
				gLit.erase(it);
				it = gLit.end();
			}
			if (it == gLit.end()) {
				if (gWait > 0 || ConfigText::GlowWaits(gLit.size(), gRoom, false)) {
					return;
				}
				auto* light = LightKit::CloneLight();
				if (!light) {
					return;
				}
				light->name = kName;
				light->local.translate = { 0.0f, 0.0f, kHeight };
				light->GetLightRuntimeData().radius.x = -1.0f;  // the clone's own may equal ours: Dress attenuates it at once
				Lit lit;
				lit.light.reset(light);
				lit.root.reset(node);
				lit.color = race->second;
				Dress(lit);
				lit.bs.reset(LightKit::Hang(node, light, a_scene));
				if (!lit.bs) {
					gWait = kRefusedWait;
					return;
				}
				it = gLit.emplace(a_actor->GetFormID(), std::move(lit)).first;
			} else {
				Dress(it->second);
			}
			it->second.seen = true;
		}
	}

	void ReadAtronachRaces()
	{
		auto* data = RE::TESDataHandler::GetSingleton();
		if (AtronachRows().empty() || !data) {
			return;
		}
		const auto theirs = ElementalRaces();
		for (auto* race : data->GetFormArray<RE::TESRace>()) {
			const char* id = race ? race->formEditorID.c_str() : nullptr;
			const auto  row = id ? std::ranges::find_if(AtronachRows(), [id](const AtronachRow& a_r) { return LightKit::SameText(a_r.race, id); }) : AtronachRows().end();
			if (row != AtronachRows().end() && !theirs.contains(Lower(id))) {
				gRaces.emplace(race, row->color);
			}
		}
		SKSE::log::info("atronach lights: {} of {} race(s) in this load order, {} name(s) left to iLluminous Elementals", gRaces.size(), AtronachRows().size(), theirs.size());
	}

	void TickAtronachLights()
	{
		if (gRaces.empty()) {
			return;
		}
		auto* scene = LightKit::Scene();
		auto* lists = RE::ProcessLists::GetSingleton();
		if (!scene || !lists || LightsOff() || SettingValue(kSwitch, 0) == 0) {
			for (auto& [id, lit] : gLit) {
				Drop(lit, scene);
			}
			gLit.clear();
			return;
		}
		gWait -= gWait > 0 ? 1 : 0;
		// one limit with the art glows, which go first; a lowered limit puts out the lights past it at once
		const auto cap = GlowLimit();
		gRoom = (std::min)(kMostLights, cap > ArtGlowsLit() ? cap - ArtGlowsLit() : std::size_t{ 0 });
		for (auto& [id, lit] : gLit) {
			lit.seen = false;
		}
		for (auto& handle : lists->highActorHandles) {
			if (auto actor = handle.get()) {
				Visit(actor.get(), scene);
			}
		}
		std::erase_if(gLit, [scene](auto& a_entry) {
			if (a_entry.second.seen) {
				return false;
			}
			Drop(a_entry.second, scene);
			return true;
		});
		for (auto it = gLit.begin(); gLit.size() > gRoom && it != gLit.end();) {
			Drop(it->second, scene);
			it = gLit.erase(it);
		}
	}

	void DropAtronachLights()
	{
		for (auto& [id, lit] : gLit) {
			Drop(lit, LightKit::Scene());
		}
		gLit.clear();
	}
}
