// Illuminated - SKSE plugin
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE.txt and the notice at the top of main.cpp.
//
// Which lighting the game draws with: the shared GameLighting.h finds it once, at data load (Community Shaders, ENB or Vanilla,
// never picked); this file keeps the answer for the rest of the plugin. Off Community Shaders every inverse-square light this mod
// makes or copies is drawn plain (LightKit::PlainOf: reach 133 drawn at radius 178 with fade 1.14, LightMath.h).

#include "Plugin.h"

#include <Windows.h>

#include "GameLighting.h"

namespace Plugin
{
	static_assert(static_cast<int>(Lighting::kShaders) == static_cast<int>(GameLighting::Lighting::kShaders) &&
				  static_cast<int>(Lighting::kEnb) == static_cast<int>(GameLighting::Lighting::kEnb) &&
				  static_cast<int>(Lighting::kVanilla) == static_cast<int>(GameLighting::Lighting::kVanilla));

	namespace
	{
		std::atomic<Lighting> gFound{ Lighting::kVanilla };
		std::atomic<bool>     gInverseSquare{ false }, gCommunityShaders{ false }, gEnb{ false }, gEnbFiles{ false };
	}

	void ReadLighting()
	{
		const auto found = GameLighting::Read();
		gCommunityShaders = found.communityShaders;
		gInverseSquare = found.inverseSquare;
		gEnb = found.enb;
		gEnbFiles = found.enbFilesLeft;
		gFound = static_cast<Lighting>(found.lighting);
		GameLighting::Log(found);
	}

	Lighting LightingFound()
	{
		return gFound.load();
	}

	const char* LightingName(Lighting a_lighting)
	{
		return GameLighting::Name(static_cast<GameLighting::Lighting>(a_lighting));
	}

	bool InverseSquare()
	{
		return gInverseSquare.load();
	}

	bool LightLimitFix()
	{
		return gCommunityShaders.load();
	}

	std::string LightingReport()
	{
		return std::format(
			R"({{"pick":"{}","detected":true,"inverseSquareShader":{},"communityShadersLoaded":{},"enbRunning":{},"enbFiles":{},"inverseSquare":{}}})",
			LightingName(gFound), gInverseSquare.load(), gCommunityShaders.load(), gEnb.load(), gEnbFiles.load(), InverseSquare());
	}
}
