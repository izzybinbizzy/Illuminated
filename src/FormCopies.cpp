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

	RE::TESObjectLIGH* SteadyCopy(const RE::TESObjectLIGH* a_src)
	{
		using enum RE::TES_LIGHT_FLAGS;
		constexpr auto kMoving = static_cast<std::uint32_t>(kFlicker) | static_cast<std::uint32_t>(kFlickerSlow) |
		                         static_cast<std::uint32_t>(kPulse) | static_cast<std::uint32_t>(kPulseSlow);
		if (!a_src || (a_src->data.flags.underlying() & kMoving) == 0) {
			return nullptr;
		}
		auto* out = CopyLight(a_src);
		if (out) {
			out->data.flags = static_cast<RE::TES_LIGHT_FLAGS>(out->data.flags.underlying() & ~kMoving);
		}
		return out;
	}

}
