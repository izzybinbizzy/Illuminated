// Illuminated - SKSE plugin
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE.txt and the notice at the top of main.cpp.
//
// Makes in-memory copies of lights, effect shaders and magic effects.

#include "Plugin.h"

namespace Plugin
{
	// ------------------------------------------------------------------ making copies in memory
	void CopyLightInto(RE::TESObjectLIGH* a_out, const RE::TESObjectLIGH* a_src)
	{
		a_out->data = a_src->data;
		a_out->fade = a_src->fade;
		a_out->emittanceColor = a_src->emittanceColor;
		a_out->lensFlare = a_src->lensFlare;
		a_out->sound = a_src->sound;
		a_out->SetModel(a_src->GetModel());
	}

	RE::TESObjectLIGH* CopyLight(const RE::TESObjectLIGH* a_src)
	{
		auto* out = a_src ? NewForm<RE::TESObjectLIGH>() : nullptr;
		if (!out) {
			return nullptr;
		}
		CopyLightInto(out, a_src);
		return out;
	}

	RE::TESEffectShader* CopyShader(const RE::TESEffectShader* a_src)
	{
		auto* out = a_src ? NewForm<RE::TESEffectShader>() : nullptr;
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
		auto* out = a_src ? NewForm<RE::EffectSetting>() : nullptr;
		if (!out) {
			return nullptr;
		}
		out->Copy(a_src);
		return out;
	}
}
