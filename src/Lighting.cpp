// Illuminated - SKSE plugin
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE.txt and the notice at the top of main.cpp.
//
// Which lighting the game draws with, and how a light made for Community Shaders is drawn without it. Ported from
// Dynamic Wards' Lighting.cpp (the same three kinds, the same house light).
//
//   Community Shaders   inverse square lighting: a light's reach is worked out from its fade and its cutoff (LightCopies.cpp)
//   ENB                 } the game's own lighting: the inverse square flag means nothing, a light reaches its radius. Every
//   Vanilla             } inverse-square light this mod makes or copies is turned into a plain one (LightKit::PlainOf)
//
// AUTOMATIC ONLY - HIS WORD: "i also want the detection to be automatic and users to not be able to manually switch
// between versions" (it was a menu pick and an installer's Lighting.txt). The game is looked at once, at data load:
// Community Shaders' inverse square shader -> Community Shaders, an ENB's settings in the game folder -> ENB, else Vanilla.
//
// The plain light: the reach an inverse-square light has (reach² = K x fade / cutoff - size²) as the game's own lighting
// draws it - Dynamic Wards' house light, LTBG section 4's reach 133 drawn at radius 178 with fade 1.14 (wardgen.plain_light).
// Never shorter than the radius the light already states: a plain light is not made dimmer than the game's own. The maths
// is LightMath.h's PlainOf / PlainAt, the one copy both mods use and the fading tests check.

#include "Plugin.h"

namespace Plugin
{
	namespace
	{
		constexpr const char* kIslShader = "Data/Shaders/InverseSquareLighting/InverseSquareLighting.hlsli";
		// a Community Shaders that builds inverse square lights into Light Limit Fix (Bottled Shaders 1.0.10 dropped the
		// separate shader above): its attenuation reads each light's InverseSquare flag
		constexpr const char* kLlfAttenuation = "Data/Shaders/LightLimitFix/Attenuation.hlsli";
		constexpr const char* kEnbFiles[] = { "enbseries.ini", "enblocal.ini" };

		std::atomic<Lighting> gFound{ Lighting::kShaders };
		std::atomic<bool>     gIslShader{ false };
		std::atomic<bool>     gEnbFiles{ false };

		bool EnbInstalled()
		{
			std::error_code ec;
			return std::ranges::any_of(kEnbFiles, [&](const char* a_file) { return fs::exists(fs::current_path() / a_file, ec); });
		}

		bool IslBuiltIntoLightLimitFix()
		{
			std::ifstream in(fs::current_path() / kLlfAttenuation, std::ios::binary);
			if (!in) {
				return false;
			}
			const std::string text{ std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
			return text.find("InverseSquare") != std::string::npos;
		}
	}

	void ReadLighting()
	{
		std::error_code ec;
		gIslShader = fs::exists(fs::current_path() / kIslShader, ec) || IslBuiltIntoLightLimitFix();
		gEnbFiles = EnbInstalled();
		gFound = gIslShader ? Lighting::kShaders : gEnbFiles ? Lighting::kEnb :
		                                                       Lighting::kVanilla;
		SKSE::log::info("lighting: {} (detected - inverse square shader {}, ENB settings in the game folder {}); lights drawn {}",
			LightingName(gFound), gIslShader ? "installed" : "not installed", gEnbFiles ? "yes" : "no", InverseSquare() ? "inverse square" : "plain");
	}

	Lighting LightingFound()
	{
		return gFound.load();
	}

	const char* LightingName(Lighting a_lighting)
	{
		switch (a_lighting) {
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
		return gFound.load() == Lighting::kShaders && gIslShader.load();
	}

	std::string LightingReport()
	{
		return std::format(R"({{"pick":"{}","detected":true,"inverseSquareShader":{},"enbFiles":{},"inverseSquare":{}}})", LightingName(gFound),
			gIslShader.load(), gEnbFiles.load(), InverseSquare());
	}
}
