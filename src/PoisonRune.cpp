// Illuminated - SKSE plugin
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE.txt and the notice at the top of main.cpp.
//
// Pass 3: the Dragonborn poison rune gets the casting art its lit hand needs.

#include "Plugin.h"

namespace Plugin
{
	// ------------------------------------------------------------------ pass 3: the poison rune's casting art
	constexpr std::string_view kPoisonRuneEffect = "DLC2PoisonRuneFFLocation";
	constexpr std::string_view kPoisonRuneArtModel = "magic\\poisonrunefxhand01.nif";
	constexpr RE::FormID       kGameRuneArt = 0x0006DE85;  // ParalyzeFXHand01 (Skyrim.esm), the art Dragonborn gives the rune

	void PoisonRuneArt()
	{
		RE::EffectSetting* rune = nullptr;
		for (auto* effect : LightKit::FormsOf<RE::EffectSetting>()) {
			if (effect && EditorID(effect) == kPoisonRuneEffect) {
				rune = effect;
				break;
			}
		}
		if (!rune) {
			SKSE::log::info("poison rune: {} is not in this load order; nothing to do", kPoisonRuneEffect);
			return;
		}
		// another mod's casting art on the rune is that mod's pick: replaced only while it is the game's own, or none (L7)
		if (const auto* had = rune->data.castingArt; had && had->GetFormID() != kGameRuneArt) {
			SKSE::log::info("poison rune: {} keeps its casting art {:08X}, set by another mod", Label(rune), had->GetFormID());
			return;
		}
		auto* art = NewForm<RE::BGSArtObject>();
		if (!art) {
			SKSE::log::warn("poison rune: could not create an art object; it keeps what it had");
			return;
		}
		art->SetModel(kPoisonRuneArtModel.data());
		art->data.artType = RE::BGSArtObject::ArtType::kMagicCastingArt;
		rune->data.castingArt = art;
		// its casting light is pass 1's now (main.cpp runs this pass first): off while a setting lights the art, given back when
		// none does, a record light without Light Placer
		SKSE::log::info("poison rune: {} now plays {}", Label(rune), kPoisonRuneArtModel);
	}
}
