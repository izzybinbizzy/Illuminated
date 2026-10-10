// Illuminated - SKSE plugin
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE.txt and the notice at the top of main.cpp.
//
// Which lighting the game draws with, and how a light made for Community Shaders is drawn without it. Ported from
// Dynamic Wards' Lighting.cpp (the same three kinds, the same house light).
//
//   Community Shaders   inverse square lighting: a light's reach is worked out from its fade and its cutoff (LightCopies.cpp)
//   ENB                 } the game's own lighting: the inverse square flag means nothing, a light reaches its radius. Every
//   Vanilla             } inverse-square light this mod makes or copies is turned into a plain one (Plain, below)
//
// AUTOMATIC ONLY - HIS WORD 2026-10-10: "i also want the detection to be automatic and users to not be able to manually switch
// between versions" (it was a menu pick and an installer's Lighting.txt). The game is looked at once, at data load:
// Community Shaders' inverse square shader -> Community Shaders, an ENB's settings in the game folder -> ENB, else Vanilla.
//
// The plain light: the reach an inverse-square light has (reach² = K x fade / cutoff - size²) as the game's own lighting
// draws it - Dynamic Wards' house light, LTBG section 4's reach 133 drawn at radius 178 with fade 1.14 (wardgen.plain_light).
// Never shorter than the radius the light already states: a plain light is not made dimmer than the game's own.

#include "Plugin.h"

namespace Plugin
{
	namespace
	{
		constexpr const char* kIslShader = "Data/Shaders/InverseSquareLighting/InverseSquareLighting.hlsli";
		constexpr const char* kEnbFiles[] = { "enbseries.ini", "enblocal.ini" };
		constexpr float       kK = LightKit::kK;              // the house K, 0.8 * 69.99² (LightKit.h, gen.py)
		constexpr float       kPlainReach = 178.0f / 133.0f;  // Dynamic Wards' house light: reach 133 drawn at radius 178
		constexpr float       kPlainFade = 1.14f;

		std::atomic<Lighting> gPick{ Lighting::kShaders };
		std::atomic<bool>     gIslShader{ false };
		std::atomic<bool>     gEnbFiles{ false };

		bool EnbInstalled()
		{
			std::error_code ec;
			return std::ranges::any_of(kEnbFiles, [&](const char* a_file) { return fs::exists(fs::current_path() / a_file, ec); });
		}
	}

	void ReadLighting()
	{
		std::error_code ec;
		gIslShader = fs::exists(fs::current_path() / kIslShader, ec);
		gEnbFiles = EnbInstalled();
		gPick = gIslShader ? Lighting::kShaders : gEnbFiles ? Lighting::kEnb :
		                                                      Lighting::kVanilla;
		SKSE::log::info("lighting: {} (detected - inverse square shader {}, ENB settings in the game folder {}); lights drawn {}",
			LightingName(gPick), gIslShader ? "installed" : "not installed", gEnbFiles ? "yes" : "no", InverseSquare() ? "inverse square" : "plain");
	}

	Lighting LightingPick()
	{
		return gPick.load();
	}

	const char* LightingName(Lighting a_pick)
	{
		switch (a_pick) {
		case Lighting::kEnb:
			return "ENB";
		case Lighting::kVanilla:
			return "Vanilla";
		default:
			return "Community Shaders";
		}
	}

	bool InverseSquare()
	{
		return gPick.load() == Lighting::kShaders && gIslShader.load();
	}

	float IslReach(float a_fade, float a_cutoff, float a_size)
	{
		if (a_cutoff <= 0.0f || a_fade <= 0.0f) {
			return 0.0f;
		}
		return std::sqrt((std::max)(kK * a_fade / a_cutoff - a_size * a_size, 0.0f));
	}

	PlainLight Plain(float a_fade, float a_radius, float a_reach)
	{
		return { a_fade * kPlainFade, (std::max)(a_radius, a_reach * kPlainReach) };
	}

	std::string LightingReport()
	{
		return std::format(R"({{"pick":"{}","detected":true,"inverseSquareShader":{},"enbFiles":{},"inverseSquare":{}}})", LightingName(gPick),
			gIslShader.load(), gEnbFiles.load(), InverseSquare());
	}
}
