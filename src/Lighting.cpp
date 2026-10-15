// Illuminated - SKSE plugin
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE.txt and the notice at the top of main.cpp.
//
// Which lighting the game draws with, and how a light made for Community Shaders is drawn without it. Ported from
// Dynamic Wards' Lighting.cpp (the same three kinds, the same house light); RELight - Spell Addon carries the same file.
//
//   Community Shaders   inverse square lighting: a light's reach is worked out from its fade and its cutoff (LightCopies.cpp)
//   ENB                 } the game's own lighting: the inverse square flag means nothing, a light reaches its radius. Every
//   Vanilla             } inverse-square light this mod makes or copies is turned into a plain one (LightKit::PlainOf)
//
// AUTOMATIC ONLY - HIS WORD: "i also want the detection to be automatic and users to not be able to manually switch
// between versions" (it was a menu pick and an installer's Lighting.txt). The game is looked at once, at data load:
// Community Shaders (its inverse square shader, Light Limit Fix's built-in one, or CommunityShaders.dll loaded) -> Community
// Shaders; an ENB that is running, or its settings in the game folder -> ENB; else Vanilla.
//
// COMMUNITY SHADERS' TWO WORDS (RELight 2.1's user report 2026-10-14, Bottled Shaders + KittyVFX: frost and shock hand lights
// dark): Light Limit Fix reads the first two words of EVERY light's runtime data as its own flags and cutoff
// (LLFCommon::RuntimeLightDataExt - bit 9 Disabled, bit 10 InverseSquare), whatever the shader files say. RELight looked only for
// the separate shader file, which Bottled Shaders 1.0.10+ no longer has, drew its lights plain, and the ambient color it wrote
// there read as flags (a red of 81-122 sets Disabled). So whenever CommunityShaders.dll is loaded (Bottled Shaders keeps the
// name) the lights are written in Community Shaders' words, never with an ambient color; and leftover ENB settings never make it
// ENB while Community Shaders runs (the same report).
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
		std::atomic<bool>     gIslShader{ false };  // the lights are written in Community Shaders' inverse square words
		std::atomic<bool>     gCsLoaded{ false }, gEnbRunning{ false }, gEnbFiles{ false };

		[[nodiscard]] bool EnbFilesLeft()
		{
			std::error_code ec;
			return std::ranges::any_of(kEnbFiles, [&](const char* a_file) { return std::filesystem::exists(std::filesystem::current_path() / a_file, ec); });
		}

		[[nodiscard]] bool IslBuiltIntoLightLimitFix()
		{
			std::ifstream in(std::filesystem::current_path() / kLlfAttenuation, std::ios::binary);
			if (!in) {
				return false;
			}
			const std::string text{ std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
			return text.find("InverseSquare") != std::string::npos;
		}

		// Community Shaders, or a fork that keeps its DLL name (Bottled Shaders)
		[[nodiscard]] bool CommunityShadersLoaded() { return REX::W32::GetModuleHandleA("CommunityShaders.dll") != nullptr; }

		// an ENB that is running: its d3d11.dll exports its SDK (one loaded through an injector under another name is found by its
		// settings files instead)
		[[nodiscard]] bool EnbRunning()
		{
			for (const char* dll : { "d3d11.dll", "dxgi.dll", "d3d9.dll" }) {
				if (const auto handle = REX::W32::GetModuleHandleA(dll); handle && REX::W32::GetProcAddress(handle, "ENBGetSDKVersion")) {
					return true;
				}
			}
			return false;
		}
	}

	void ReadLighting()
	{
		std::error_code ec;
		const bool      shaderFile = std::filesystem::exists(std::filesystem::current_path() / kIslShader, ec);
		const bool      inLlf = IslBuiltIntoLightLimitFix();
		gCsLoaded = CommunityShadersLoaded();
		gIslShader = shaderFile || inLlf || gCsLoaded;
		gEnbRunning = EnbRunning();
		gEnbFiles = EnbFilesLeft();
		gFound = gIslShader ? Lighting::kShaders : gEnbRunning || gEnbFiles ? Lighting::kEnb :
		                                                                      Lighting::kVanilla;
		SKSE::log::info(
			"lighting: {} (detected - CommunityShaders.dll {}, inverse square shader {}, in Light Limit Fix {}, ENB running {}, ENB "
			"settings in the game folder {}); lights drawn {}",
			LightingName(gFound), gCsLoaded ? "loaded" : "not loaded", shaderFile ? "installed" : "not installed", inLlf ? "yes" : "no",
			gEnbRunning ? "yes" : "no", gEnbFiles ? "yes" : "no", InverseSquare() ? "inverse square" : "plain");
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
		return std::format(
			R"({{"pick":"{}","detected":true,"inverseSquareShader":{},"communityShadersLoaded":{},"enbRunning":{},"enbFiles":{},"inverseSquare":{}}})",
			LightingName(gFound), gIslShader.load(), gCsLoaded.load(), gEnbRunning.load(), gEnbFiles.load(), InverseSquare());
	}
}
