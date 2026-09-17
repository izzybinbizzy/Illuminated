// Luminous Arcana - SKSE plugin
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE.txt and the notice at the top of main.cpp.
//
// Pass 5: an enchantment carrying two or more lit shaders keeps the light of its first one only.

#include "Plugin.h"

namespace Plugin
{
	// ------------------------------------------------------------------ pass 5: enchantments with two lit shaders
	// An enchantment carrying two or more lit shaders keeps the light of its first one; each other lit
	// effect is pointed at an in-memory copy of its magic effect whose shader is an unlit copy.
	//
	// A form made in memory has no place in a save, so no saved form may ever point at one:
	//   - enchantments from plugins are never written to a save, so they keep their copies;
	//   - enchantments made during play (the enchanting table) ARE saved, so they get their copies
	//     only between saves: the originals go back when a save starts and the copies return on the
	//     next frame, after the save has been written;
	//   - while the Crafting Menu is open every enchantment carries its originals, so the table builds
	//     a new enchantment from effects that exist in a plugin.
	struct Swap
	{
		RE::Effect*        effect;
		RE::EffectSetting* original;
		RE::EffectSetting* quiet;
	};
	std::vector<Swap> gSwaps;         // plugin enchantments, made once at data load
	std::vector<Swap> gCreatedSwaps;  // enchantments made during play, rebuilt after every restore

	std::unordered_set<const RE::TESEffectShader*>                 gLitShaders;
	std::unordered_map<const RE::TESEffectShader*, std::string>    gLitShaderNames;
	std::unordered_map<RE::EffectSetting*, RE::EffectSetting*>     gQuietEffects;
	std::unordered_map<RE::TESEffectShader*, RE::TESEffectShader*> gQuietShaders;
	std::recursive_mutex                                           gSwapLock;
	bool                                                           gCrafting = false;

	const RE::TESEffectShader* LitShaderOf(const RE::Effect* a_effect)
	{
		const auto* base = a_effect ? a_effect->baseEffect : nullptr;
		const auto* shader = base ? base->data.enchantShader : nullptr;
		return shader && gLitShaders.contains(shader) ? shader : nullptr;
	}

	RE::EffectSetting* QuietCopyOf(RE::EffectSetting* a_original)
	{
		auto& quiet = gQuietEffects[a_original];
		if (!quiet) {
			auto*& shader = gQuietShaders[a_original->data.enchantShader];
			if (!shader) {
				shader = CopyShader(a_original->data.enchantShader);
			}
			quiet = shader ? CopyEffect(a_original) : nullptr;
			if (quiet) {
				quiet->data.enchantShader = shader;
			}
		}
		return quiet && quiet->data.enchantShader != a_original->data.enchantShader ? quiet : nullptr;
	}

	struct FixResult
	{
		std::size_t dropped{ 0 }, failed{ 0 };
		const RE::TESEffectShader* kept{ nullptr };
	};

	// One enchantment, the keep-the-first rule. `a_log` writes a row per moved effect.
	FixResult FixEnchantment(RE::EnchantmentItem* a_ench, std::vector<Swap>& a_out, bool a_log)
	{
		FixResult r;
		std::size_t lit = 0;
		for (auto* eff : a_ench->effects) {
			if (const auto* shader = LitShaderOf(eff)) {
				if (!r.kept) r.kept = shader;
				++lit;
			}
		}
		if (lit < 2) {
			r.kept = nullptr;
			return r;
		}
		bool keptOne = false;
		for (std::size_t i = 0; i < a_ench->effects.size(); ++i) {
			auto*       eff = a_ench->effects[i];
			const auto* shader = LitShaderOf(eff);
			if (!shader) {
				continue;
			}
			if (shader == r.kept && !keptOne) {
				keptOne = true;
				continue;
			}
			auto* original = eff->baseEffect;
			auto* quiet = QuietCopyOf(original);
			if (!quiet) {
				++r.failed;
				SKSE::log::warn("[ENCH-FAILED] {} | effect {} | no quiet copy of {}", Label(a_ench), i, Label(original));
				continue;
			}
			eff->baseEffect = quiet;
			a_out.push_back({ eff, original, quiet });
			++r.dropped;
			if (a_log) {
				SKSE::log::info("[ENCH-DROPPED] {} | effect {} | {} | {} now plays an unlit copy of its shader", Label(a_ench), i,
					gLitShaderNames[shader], EditorID(original));
			}
		}
		return r;
	}

	void DoubledEnchantments(const Coverage& a_cov)
	{
		for (auto* shader : RE::TESDataHandler::GetSingleton()->GetFormArray<RE::TESEffectShader>()) {
			const auto id = shader ? Lower(EditorID(shader)) : std::string();
			if (!id.empty() && a_cov.shaders.contains(id)) {
				gLitShaders.insert(shader);
				gLitShaderNames[shader] = id;
			}
		}
		std::size_t scanned = 0, fixed = 0, dropped = 0, failed = 0;
		std::lock_guard lock(gSwapLock);
		for (auto* ench : RE::TESDataHandler::GetSingleton()->GetFormArray<RE::EnchantmentItem>()) {
			if (!ench) {
				continue;
			}
			++scanned;
			const auto r = FixEnchantment(ench, gSwaps, true);
			dropped += r.dropped;
			failed += r.failed;
			if (r.dropped) {
				++fixed;
				SKSE::log::info("[ENCH-KEPT] {} | {} | {} other light(s) removed", Label(ench), gLitShaderNames[r.kept], r.dropped);
			}
		}
		SKSE::log::info("enchantments: {} scanned, {} lit shader(s), {} fixed, {} effect(s) moved to {} unlit effect copies and {} "
						"shader copies, {} failed",
			scanned, gLitShaders.size(), fixed, dropped, gQuietEffects.size(), gQuietShaders.size(), failed);
	}

	// Enchantments made during play carry FormIDs in the FF range and live only in the form map.
	void FixCreatedEnchantments()
	{
		// timed on purpose: this walks every form in the game on each save and load, and whether that costs
		// anything is a measurement, not a guess (see the log line below)
		const auto started = std::chrono::steady_clock::now();
		std::vector<RE::EnchantmentItem*> created;
		{
			const auto& [map, mapLock] = RE::TESForm::GetAllForms();
			RE::BSReadLockGuard guard(mapLock.get());
			if (map) {
				for (const auto& [id, form] : *map) {
					if (form && (id >> 24) == 0xFF && form->GetFormType() == RE::FormType::Enchantment) {
						created.push_back(static_cast<RE::EnchantmentItem*>(form));
					}
				}
			}
		}
		std::size_t fixed = 0, dropped = 0, failed = 0;
		for (auto* ench : created) {
			const auto r = FixEnchantment(ench, gCreatedSwaps, false);
			dropped += r.dropped;
			failed += r.failed;
			if (r.dropped) {
				++fixed;
				SKSE::log::info("[ENCH-CREATED] {:08X} | {} | {} | {} other light(s) removed", ench->GetFormID(), ench->GetName(),
					gLitShaderNames[r.kept], r.dropped);
			}
		}
		const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
		SKSE::log::info("enchantments made during play: {} found, {} fixed, {} effect(s) moved, {} failed, in {:.1f} ms", created.size(),
			fixed, dropped, failed, ms);
	}

	// Every swapped effect back on the magic effect its plugin gave it.
	void UseOriginals(std::string_view a_why)
	{
		std::lock_guard lock(gSwapLock);
		for (auto& s : gSwaps) {
			s.effect->baseEffect = s.original;
		}
		for (auto& s : gCreatedSwaps) {
			s.effect->baseEffect = s.original;
		}
		SKSE::log::info("{}: {} plugin and {} crafted enchantment effect(s) back on their originals", a_why, gSwaps.size(),
			gCreatedSwaps.size());
		gCreatedSwaps.clear();
	}

	// The unlit copies back on, and every enchantment made during play checked again.
	void UseQuiet(std::string_view a_why)
	{
		std::lock_guard lock(gSwapLock);
		if (gCrafting) {
			return;
		}
		for (auto& s : gSwaps) {
			s.effect->baseEffect = s.quiet;
		}
		if (!gLitShaders.empty()) {
			FixCreatedEnchantments();
		}
		SKSE::log::info("{}: unlit copies back on", a_why);
	}

	class CraftingWatch : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
	{
	public:
		static CraftingWatch* Get()
		{
			static CraftingWatch watch;
			return &watch;
		}

		RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
		{
			if (a_event && a_event->menuName == RE::CraftingMenu::MENU_NAME) {
				if (a_event->opening) {
					{
						std::lock_guard lock(gSwapLock);
						gCrafting = true;
					}
					UseOriginals("crafting menu opened");
				} else {
					{
						std::lock_guard lock(gSwapLock);
						gCrafting = false;
					}
					UseQuiet("crafting menu closed");
				}
			}
			return RE::BSEventNotifyControl::kContinue;
		}
	};

	bool AnyLitShaders()
	{
		return !gLitShaders.empty();
	}

	void WatchCraftingMenu()
	{
		RE::UI::GetSingleton()->AddEventSink<RE::MenuOpenCloseEvent>(CraftingWatch::Get());
	}

	void ForgetCreatedEnchantments()
	{
		std::lock_guard lock(gSwapLock);
		gCreatedSwaps.clear();
	}
}
