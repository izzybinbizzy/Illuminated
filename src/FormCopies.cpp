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

	void HoldFlicker(RE::TESObjectLIGH* a_light, bool a_flickers)
	{
		using enum RE::TES_LIGHT_FLAGS;
		constexpr auto                                                     kMoving = static_cast<std::uint32_t>(kFlicker) | static_cast<std::uint32_t>(kFlickerSlow) |
		                                                                             static_cast<std::uint32_t>(kPulse) | static_cast<std::uint32_t>(kPulseSlow);
		static std::unordered_map<const RE::TESObjectLIGH*, std::uint32_t> madeWith;  // each record's moving bits as the game made it
		if (!a_light) {
			return;
		}
		const auto bits = a_light->data.flags.underlying();
		const auto kept = madeWith.try_emplace(a_light, bits & kMoving).first->second;
		if (kept != 0) {
			a_light->data.flags = static_cast<RE::TES_LIGHT_FLAGS>((bits & ~kMoving) | (a_flickers ? kept : 0));
		}
	}

}
