// BurnEmConfig.h - Illuminated's side of the shared BurningBanners.cpp: the light's name and its numbers under Illuminated's sliders.
#pragma once

#include "Plugin.h"

namespace Plugin::BurnEm
{
	inline constexpr const char* kLightName = "IlluminatedBannerFire";
	inline constexpr const char* kModName = "Illuminated";

	// Brightness with the lighting's own strength, x Ground effects; 0 with every light of the mod off
	[[nodiscard]] inline float LightStrength() { return LightsOff() ? 0.0f : BrightnessNow() * KindStrength(LightKind::kGround); }
	[[nodiscard]] inline float ReachScale() { return static_cast<float>(SettingValue("IlluminatedReach", 100)) / 100.0f; }
	// "Burn'em Banners lights its banners" off: our light, theirs put out
	[[nodiscard]] inline bool OursLight() { return !BurnEmBannersLightsItself(); }
}
