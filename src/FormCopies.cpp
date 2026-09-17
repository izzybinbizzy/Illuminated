// Luminous Arcana - SKSE plugin
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE.txt and the notice at the top of main.cpp.
//
// Makes in-memory copies of lights, effect shaders and magic effects.

#include "Plugin.h"

namespace Plugin
{
	// ------------------------------------------------------------------ making copies in memory
	RE::TESObjectLIGH* CopyLight(const RE::TESObjectLIGH* a_src)
	{
		auto* out = NewForm<RE::TESObjectLIGH>();
		if (!out) {
			return nullptr;
		}
		out->data = a_src->data;
		out->fade = a_src->fade;
		out->emittanceColor = a_src->emittanceColor;
		out->lensFlare = a_src->lensFlare;
		out->sound = a_src->sound;
		out->SetModel(a_src->GetModel());
		return out;
	}

	RE::TESEffectShader* CopyShader(const RE::TESEffectShader* a_src)
	{
		auto* out = NewForm<RE::TESEffectShader>();
		if (!out) {
			return nullptr;
		}
		out->data = a_src->data;
		out->fillTexture.textureName = a_src->fillTexture.textureName;
		out->particleShaderTexture.textureName = a_src->particleShaderTexture.textureName;
		out->holesTexture.textureName = a_src->holesTexture.textureName;
		out->membranePaletteTexture.textureName = a_src->membranePaletteTexture.textureName;
		out->particlePaletteTexture.textureName = a_src->particlePaletteTexture.textureName;
		return out;
	}

	RE::EffectSetting* CopyEffect(RE::EffectSetting* a_src)
	{
		auto* out = NewForm<RE::EffectSetting>();
		if (!out) {
			return nullptr;
		}
		out->Copy(a_src);
		return out;
	}
}
