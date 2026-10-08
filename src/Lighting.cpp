// Illuminated - SKSE plugin
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE.txt and the notice at the top of main.cpp.
//
// Which lighting the game draws with, and how a light made for Community Shaders is drawn without it. Ported from
// Dynamic Wards' Lighting.cpp (the same three picks, the same file, the same house light).
//
//   Community Shaders   inverse square lighting: a light's reach is worked out from its fade and its cutoff (LightCopies.cpp)
//   ENB                 } the game's own lighting: the inverse square flag means nothing, a light reaches its radius. Every
//   Vanilla             } inverse-square light this mod makes or copies is turned into a plain one (Plain, below)
//
// The pick is one word in `SKSE\Plugins\Illuminated\Lighting.txt`, which the installer's option installs. With no file the
// game is looked at: Community Shaders' inverse square shader -> Community Shaders, an ENB's settings in the game folder ->
// ENB, else Vanilla. Inverse square lighting is used only on the Community Shaders pick, and only while its shader is there.
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
		constexpr float       kK = 3918.88f;                  // the house K, 0.8 * 69.99² (StreamLights.cpp rule 4, gen.py)
		constexpr float       kPlainReach = 178.0f / 133.0f;  // Dynamic Wards' house light: reach 133 drawn at radius 178
		constexpr float       kPlainFade = 1.14f;

		std::atomic<Lighting> gPick{ Lighting::kShaders };
		std::atomic<bool>     gFromFile{ false };
		std::atomic<bool>     gIslShader{ false };

		fs::path PickPath() { return fs::current_path() / "Data" / "SKSE" / "Plugins" / std::string(kOurFolder) / "Lighting.txt"; }

		std::optional<Lighting> ReadPick()
		{
			std::ifstream in(PickPath());
			std::string   line;
			while (in && std::getline(in, line)) {
				if (line.size() >= 3 && line.compare(0, 3, "\xEF\xBB\xBF") == 0) {
					line.erase(0, 3);  // a UTF-8 BOM some editors write
				}
				const auto word = Lower(Trim(line.substr(0, line.find_first_of(" \t\r#"))));
				if (word.empty()) {
					continue;
				}
				if (word == "enb") {
					return Lighting::kEnb;
				}
				if (word == "vanilla") {
					return Lighting::kVanilla;
				}
				if (word == "cs" || word == "communityshaders" || word == "shaders") {
					return Lighting::kShaders;
				}
				SKSE::log::warn("lighting: Lighting.txt says '{}', which is not cs, enb or vanilla; the game is looked at instead", word);
				return std::nullopt;
			}
			return std::nullopt;
		}

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
		const auto pick = ReadPick();
		gFromFile = pick.has_value();
		gPick = pick ? *pick : gIslShader ? Lighting::kShaders :
		                   EnbInstalled() ? Lighting::kEnb :
		                                    Lighting::kVanilla;
		if (gPick == Lighting::kShaders && !gIslShader) {
			SKSE::log::warn("lighting: Community Shaders was picked but its inverse square lighting is not installed; lights are drawn plain");
		}
		SKSE::log::info("lighting: {} ({}); inverse square shader {}; lights drawn {}", LightingName(gPick), gFromFile ? "the installer's pick" : "no Lighting.txt, looked at the game", gIslShader ? "installed" : "not installed", InverseSquare() ? "inverse square" : "plain");
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
		return std::format(R"({{"pick":"{}","fromInstaller":{},"inverseSquareShader":{},"inverseSquare":{}}})", LightingName(gPick),
			gFromFile.load(), gIslShader.load(), InverseSquare());
	}
}
