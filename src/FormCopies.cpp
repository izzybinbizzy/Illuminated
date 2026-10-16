// Illuminated - SKSE plugin
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE.txt and the notice at the top of main.cpp.
//
// Makes in-memory copies of lights (the effect-shader and magic-effect copies went with pass 5, 2026-10-16).

#include "Plugin.h"

namespace Plugin
{
	// ------------------------------------------------------------------ making copies in memory
	void CopyLightInto(RE::TESObjectLIGH* a_out, const RE::TESObjectLIGH* a_src)
	{
		if (!a_out || !a_src) {
			return;
		}
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

}
