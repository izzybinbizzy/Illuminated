// Illuminated - SKSE plugin
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE.txt and the notice at the top of main.cpp.
//
// THE VFX SWITCHER (his order 2026-10-19: "i want spell mods to be selectable and changable", and his yes to the design the same
// day: "yes add the vfx switcher"). A spell VFX pack copied by `vfx pack convert.py` sits under paths of its own
// (meshes\VFX Packs\<pack>\...) with a list beside it, Data\SKSE\Plugins\VFX Packs\<pack>.json: its element, a word of the art
// pick it belongs to, and each original model -> its copy. At data load every spell form wearing one of those originals is noted
// with its own model; the art pick of its element (Fire / Frost / Shock / Healing Art) then decides which model it wears - the
// pack's copy when the pick names that pack, its own otherwise - at load and on every settings refresh. The next cast shows it;
// art already out in the world keeps its model until it ends (measured on a projectile, `vfx switch probe 2026-10-19`). World
// objects (statics, activators, lights, movable statics) are never touched: this switches spells, not a torch on a wall.
// NormalPath (Text.cpp) reads a pack's copy as the model it stands in for, so every pass still finds a switched spell.

#include "Plugin.h"

#include <nlohmann/json.hpp>

namespace Plugin
{
	namespace
	{
		// OFF in this release (his order 2026-10-19: "keep the spell switcher turned off in illuminated for now so we can ship with
		// it in the code but not playable"): no pack list is read and no model is changed - the art picks keep only their light color
		constexpr bool kSwitcherOn = false;

		constexpr std::string_view kPackFolder = "VFX Packs";

		// the art picks the packs follow: the element a pack's json names, and the menu setting that picks for it
		struct Pick
		{
			std::string_view element, setting;
		};
		constexpr std::array<Pick, 4> kPicks{ { { "fire", "IlluminatedFireArt" },
			{ "frost", "IlluminatedFrostArt" },
			{ "shock", "IlluminatedShockArt" },
			{ "healing", "IlluminatedHealingArt" } } };

		struct Pack
		{
			std::string                                  name, choice;  // choice: a word of the art pick's choice it belongs to
			std::size_t                                  pick{ 0 };     // into kPicks
			std::unordered_map<std::string, std::string> models;        // the original (a model key) -> the pack's copy
		};

		// one model slot of a spell form: what it came with, and what it wears now
		struct Slot
		{
			RE::TESModel* model{ nullptr };
			std::string   original;  // as the game had it
			std::string   key;       // ModelKey(original)
			std::size_t   pick{ 0 };
			std::string   worn;  // the pack copy it wears, "" = its own
		};

		std::vector<Pack> gPacks;
		std::vector<Slot> gSlots;

		// a model path as the packs key it: lower case, backslashes, no leading "meshes\"
		std::string ModelKey(std::string_view a_model)
		{
			std::string key = NormalPath(a_model);
			if (key.starts_with("meshes\\")) {
				key.erase(0, 7);
			}
			return key;
		}

		std::optional<std::size_t> PickOf(std::string_view a_element)
		{
			const auto at = std::ranges::find_if(kPicks, [&](const Pick& a_p) { return LightKit::SameText(a_p.element, a_element); });
			return at == kPicks.end() ? std::nullopt : std::optional{ static_cast<std::size_t>(at - kPicks.begin()) };
		}

		[[maybe_unused]] void ReadPacks()  // used only while kSwitcherOn
		{
			const auto      folder = fs::current_path() / "Data" / "SKSE" / "Plugins" / std::string(kPackFolder);
			std::error_code ec;
			if (!fs::is_directory(folder, ec)) {
				return;
			}
			for (const auto& entry : fs::directory_iterator(folder, ec)) {
				if (!entry.is_regular_file(ec) || Lower(entry.path().extension().string()) != ".json") {
					continue;
				}
				try {
					std::ifstream file(entry.path());
					const auto    doc = nlohmann::json::parse(file);
					Pack          pack;
					pack.name = doc.at("pack").get<std::string>();
					pack.choice = doc.at("choice").get<std::string>();
					const auto pick = PickOf(doc.at("element").get<std::string>());
					if (!pick || pack.choice.empty()) {
						SKSE::log::warn("[VFX-PACK] {}: no art pick for its element or no choice word - left out", entry.path().filename().string());
						continue;
					}
					pack.pick = *pick;
					for (const auto& [original, copy] : doc.at("models").items()) {
						pack.models.emplace(ModelKey(original), copy.get<std::string>());
					}
					SKSE::log::info("[VFX-PACK] {} ({} Art, choice word \"{}\"): {} model(s)", pack.name, kPicks[pack.pick].element, pack.choice,
						pack.models.size());
					gPacks.push_back(std::move(pack));
				} catch (const std::exception& e) {
					SKSE::log::warn("[VFX-PACK] {} could not be read ({}) - left out", entry.path().filename().string(), e.what());
				}
			}
		}

		// a spell form's model: noted when a pack holds a copy of it (one element only - a model two elements' packs both claim is
		// left as it is and logged)
		[[maybe_unused]] void Note(RE::TESModel* a_model, std::size_t& a_conflicts)  // used only while kSwitcherOn
		{
			const char* model = a_model ? a_model->GetModel() : nullptr;
			if (!model || !*model) {
				return;
			}
			const auto                 key = ModelKey(model);
			std::optional<std::size_t> pick;
			for (const auto& pack : gPacks) {
				if (!pack.models.contains(key)) {
					continue;
				}
				if (pick && *pick != pack.pick) {
					++a_conflicts;
					SKSE::log::warn("[VFX-PACK] {} is claimed by a {} pack and a {} pack - left as it is", key, kPicks[*pick].element,
						kPicks[pack.pick].element);
					return;
				}
				pick = pack.pick;
			}
			if (pick) {
				gSlots.push_back({ a_model, model, key, *pick, {} });
			}
		}

		// every form of one spell type (read as plain TESForms and cast one by one - rule file 7: GetFormArray<T>() is wrong for a
		// form whose TESForm part is not at its start)
		template <class T>
		void NoteAll(RE::TESDataHandler* a_dh, std::size_t& a_conflicts)
		{
			for (auto* form : a_dh->GetFormArray(T::FORMTYPE)) {
				auto* typed = form ? form->template As<T>() : nullptr;
				if (!typed) {
					continue;
				}
				Note(static_cast<RE::TESModel*>(typed), a_conflicts);
				if constexpr (std::is_same_v<T, RE::BGSProjectile>) {
					Note(&typed->muzzleFlashModel, a_conflicts);
				}
			}
		}

		// the pack the art pick names now for one element, or nullptr (its own models)
		const Pack* Chosen(std::size_t a_pick)
		{
			const auto& settings = Settings();
			const auto  at = std::ranges::find_if(settings, [&](const Setting& a_s) { return LightKit::SameText(a_s.id, kPicks[a_pick].setting); });
			if (at == settings.end() || !at->isChoice) {
				return nullptr;
			}
			const int v = at->value.load();
			if (v < 0 || v >= static_cast<int>(at->choices.size())) {
				return nullptr;
			}
			const auto label = Lower(at->choices[static_cast<std::size_t>(v)]);
			const auto pack = std::ranges::find_if(gPacks, [&](const Pack& a_p) { return a_p.pick == a_pick && Contains(label, Lower(a_p.choice)); });
			return pack == gPacks.end() ? nullptr : &*pack;
		}
	}

	// data load, after every pass (main.cpp): the packs read and the spell forms that wear their originals noted, then dressed
	void LoadVfxPacks()
	{
		if constexpr (!kSwitcherOn) {
			SKSE::log::info("[VFX-PACK] the spell art switcher is off in this build - no pack is read, no spell model changes");
		} else {
			ReadPacks();
			auto* dh = RE::TESDataHandler::GetSingleton();
			if (gPacks.empty() || !dh) {
				return;
			}
			std::size_t conflicts = 0;
			NoteAll<RE::BGSArtObject>(dh, conflicts);
			NoteAll<RE::BGSProjectile>(dh, conflicts);
			NoteAll<RE::BGSExplosion>(dh, conflicts);
			NoteAll<RE::BGSHazard>(dh, conflicts);
			NoteAll<RE::BGSImpactData>(dh, conflicts);
			NoteAll<RE::BGSAddonNode>(dh, conflicts);
			SKSE::log::info("[VFX-PACK] {} pack(s), {} spell model slot(s) they can dress, {} left alone (claimed by two elements)",
				gPacks.size(), gSlots.size(), conflicts);
			ApplyVfxPacks();
		}
	}

	// main thread: each slot wears the copy of the pack its element's art pick names, or its own model (at data load and on every
	// settings refresh - EffectLights.cpp RefreshLights)
	void ApplyVfxPacks()
	{
		if (gSlots.empty()) {
			return;
		}
		std::array<const Pack*, kPicks.size()> chosen{};
		for (std::size_t i = 0; i < kPicks.size(); ++i) {
			chosen[i] = Chosen(i);
		}
		std::array<std::size_t, kPicks.size()> worn{}, changed{};
		for (auto& slot : gSlots) {
			std::string want;
			if (const auto* pack = chosen[slot.pick]) {
				if (const auto it = pack->models.find(slot.key); it != pack->models.end()) {
					want = it->second;
				}
			}
			worn[slot.pick] += want.empty() ? 0u : 1u;
			if (want == slot.worn) {
				continue;
			}
			slot.model->SetModel((want.empty() ? slot.original : want).c_str());
			slot.worn = std::move(want);
			++changed[slot.pick];
		}
		for (std::size_t i = 0; i < kPicks.size(); ++i) {
			if (changed[i] > 0) {
				SKSE::log::info("[VFX-PACK] {} Art: {} - {} form model(s) wear its copies ({} changed now)", kPicks[i].element,
					chosen[i] ? chosen[i]->name : "the game's own", worn[i], changed[i]);
			}
		}
	}
}
