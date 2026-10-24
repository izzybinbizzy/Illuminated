// Illuminated - SKSE plugin
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE.txt and the notice at the top of main.cpp.
//
// Lights fade while sneaking. With "Lights fade while sneaking" a sneaking caster's lights glide down to Sneaking brightness
// over half a second, and back up when they stand. Light Placer can only cut a light at once, so the menu build's rows carry
// no sneak test (lagen.py drop_sneak_tests) and the fade is done here, per caster:
//   Light Placer's lights of our copies and our own art / weapon / flesh lights, by the actor they hang on - found once a frame
//   in the scene's lists (the main thread); a rune's (a placed spell's lobbed projectile) by the player, as its row asked
//   the game's own casting light on the record route (Vanilla / ENB, or no Light Placer), by its caster - SneakDim from the
//   caster's update, after every other pass wrote it
// Our write is the last word on a dimmed light and never a base for anyone else's: each light remembers the fade the others
// left it (`pre`) and what we wrote over it. At the start of the player update, and around a pass of ours that writes fades
// later in the frame (SneakPause: a caster's update, an effect's, the cell's animations), the light gets its `pre` back if
// nobody wrote since; after them it is dimmed again. So Brightness, the fading module, Light Placer and the game only ever read
// their own numbers. A listed light is held, so its address is never another light's while it is listed.

#include "Plugin.h"

namespace Plugin
{
	namespace
	{
		constexpr std::string_view kSneaking = "IlluminatedSneaking";  // 0 fade (the default), 1 stay on
		constexpr std::string_view kSneakBrightness = "IlluminatedSneakingBrightness";
		constexpr std::string_view kArtLight = "IlluminatedArtLight";  // ArtLights.cpp
		constexpr float            kFadeSeconds = 0.5f;
		constexpr std::uint32_t    kForgetFrames = 3;

		struct Dimmed
		{
			RE::NiPointer<RE::NiLight> hold;
			RE::FormID                 owner{ 0 };
			float                      pre{ 0.0f };
			float                      wrote{ 0.0f };
			float                      preCutoff{ 0.0f };  // an inverse-square light's cutoff, moved with the fade (below)
			float                      wroteCutoff{ 0.0f };
			std::uint32_t              seen{ 0 };
			bool                       inverseSquare{ false };
			bool                       down{ false };    // our write is on the light now
			bool                       paused{ false };  // a SneakPause took it off and puts it back
		};

		std::unordered_map<RE::FormID, float>    gLevels;  // each caster's level: 1 = not dimmed
		std::unordered_map<RE::NiLight*, Dimmed> gDim;
		std::mutex                               gLock;
		std::atomic<bool>                        gDimming{ false };  // gDim holds a light
		std::uint32_t                            gFrame{ 0 };

		[[nodiscard]] bool Fades() { return SettingValue(kSneaking, 0) == 0; }

		[[nodiscard]] float LevelNow(RE::FormID a_owner)
		{
			const auto it = gLevels.find(a_owner);
			return it == gLevels.end() ? 1.0f : it->second;
		}

		void DimOne(Dimmed& a_d, float a_level)
		{
			auto& fade = a_d.hold->GetLightRuntimeData().fade;
			if (a_d.down && fade == a_d.wrote) {
				return;
			}
			a_d.pre = fade;
			a_d.wrote = fade * a_level;
			fade = a_d.wrote;
			// Community Shaders works an inverse-square light's reach out of its fade and its cutoff, so a dimmer fade alone also
			// pulls the light in (measured: 30 % on the record lit 7 % of the ground). The cutoff moves with it, as Brightness does
			auto* light = a_d.hold.get();
			a_d.inverseSquare = LightKit::Isl::On(light);
			if (a_d.inverseSquare) {
				a_d.preCutoff = LightKit::Isl::Cutoff(light);
				LightKit::Isl::SetCutoff(light, a_d.preCutoff * a_level);
				a_d.wroteCutoff = LightKit::Isl::Cutoff(light);
			}
			a_d.down = true;
		}

		void Restore(Dimmed& a_d)
		{
			if (!a_d.down) {
				return;
			}
			auto& fade = a_d.hold->GetLightRuntimeData().fade;
			if (fade == a_d.wrote) {
				fade = a_d.pre;  // nobody wrote it since: back to what the others left it
			}
			auto* light = a_d.hold.get();
			if (a_d.inverseSquare && LightKit::Isl::On(light) && LightKit::Isl::Cutoff(light) == a_d.wroteCutoff) {
				LightKit::Isl::SetCutoff(light, a_d.preCutoff);
			}
			a_d.down = false;
		}

		void DimAll()
		{
			for (auto& d : gDim | std::views::values) {
				DimOne(d, LevelNow(d.owner));
			}
		}

		[[nodiscard]] const RE::TESObjectREFR* OwnerOf(const RE::NiAVObject* a_object)
		{
			for (; a_object; a_object = a_object->parent) {
				if (const auto* ref = a_object->GetUserData()) {
					return ref;
				}
			}
			return nullptr;
		}

		// the caster a light of ours fades with: the actor it hangs on, or the player for a rune
		[[nodiscard]] const RE::Actor* CasterOf(const RE::NiLight& a_light)
		{
			const auto* ref = OwnerOf(&a_light);
			if (!ref) {
				return nullptr;
			}
			if (const auto* actor = ref->As<RE::Actor>()) {
				return actor;
			}
			const auto* projectile = ref->As<RE::Projectile>();
			const auto* base = projectile ? ref->GetBaseObject() : nullptr;
			const auto* data = base ? base->As<RE::BGSProjectile>() : nullptr;
			if (data && data->IsGrenade() && projectile->GetProjectileRuntimeData().spell) {
				return RE::PlayerCharacter::GetSingleton();
			}
			return nullptr;
		}

		[[nodiscard]] bool Ours(RE::NiLight* a_light)
		{
			auto* point = a_light ? netimmerse_cast<RE::NiPointLight*>(a_light) : nullptr;
			return point && (std::string_view(point->name.c_str()) == kArtLight || IsOurLightPlacerLight(*point));
		}

		struct Found
		{
			RE::NiLight* light{ nullptr };
			RE::FormID   owner{ 0 };
			bool         sneaking{ false };
		};
	}

	void SneakFrameStart()
	{
		std::lock_guard l{ gLock };
		++gFrame;
		for (auto& d : gDim | std::views::values) {
			Restore(d);
		}
	}

	void SneakFrameEnd(float a_delta)
	{
		const bool              fades = Fades();
		std::vector<RE::FormID> listed;
		{
			std::lock_guard l{ gLock };
			if (!fades && gLevels.empty()) {
				return;
			}
			listed.reserve(gLevels.size());
			for (const auto id : gLevels | std::views::keys) {
				listed.push_back(id);
			}
		}
		const float low = static_cast<float>(std::clamp(SettingValue(kSneakBrightness, 0), 0, 100)) / 100.0f;
		const float step = std::clamp(a_delta, 0.0f, 0.25f) / kFadeSeconds;

		// outside the lock: the scene's lists and the game's forms (the main thread)
		static std::vector<Found> found;  // its storage kept between frames
		found.clear();
		if (auto* scene = RE::BSShaderManager::State::GetSingleton().shadowSceneNode[0]) {
			const auto& lists = scene->GetRuntimeData();
			for (const auto* list : { &lists.activeLights, &lists.lightQueueAdd }) {
				for (const auto& bs : *list) {
					auto*       light = bs ? bs->light.get() : nullptr;
					const auto* caster = Ours(light) ? CasterOf(*light) : nullptr;
					if (caster) {
						found.push_back({ light, caster->GetFormID(), caster->IsSneaking() });
					}
				}
			}
		}
		std::unordered_map<RE::FormID, bool> sneaks;  // a caster still here: sneaking now?
		std::vector<RE::FormID>              gone;
		for (const auto id : listed) {
			if (const auto* actor = RE::TESForm::LookupByID<RE::Actor>(id)) {
				sneaks[id] = actor->IsSneaking();
			} else {
				gone.push_back(id);
			}
		}
		for (const auto& f : found) {
			sneaks[f.owner] = f.sneaking;
		}

		std::lock_guard l{ gLock };
		for (const auto id : gone) {
			gLevels.erase(id);
		}
		if (fades) {
			for (const auto& f : found) {
				if (f.sneaking) {
					gLevels.try_emplace(f.owner, 1.0f);
				}
			}
		}
		// each caster one step toward Sneaking brightness while it sneaks, else full; one listed by a caster's update since the
		// look above takes its first step next frame
		std::erase_if(gLevels, [&](auto& a_kv) {
			const auto s = sneaks.find(a_kv.first);
			if (s == sneaks.end()) {
				return false;
			}
			const float target = fades && s->second ? low : 1.0f;
			float&      now = a_kv.second;
			now = now < target ? (std::min)(now + step, target) : (std::max)(now - step, target);
			return now >= 1.0f && target >= 1.0f;
		});
		for (const auto& f : found) {
			if (LevelNow(f.owner) >= 1.0f) {
				continue;
			}
			auto& d = gDim[f.light];
			if (!d.hold) {
				d.hold.reset(f.light);
				d.owner = f.owner;
			}
			d.seen = gFrame;
		}
		std::erase_if(gDim, [](auto& a_kv) {
			auto& d = a_kv.second;
			if (LevelNow(d.owner) < 1.0f && gFrame - d.seen <= kForgetFrames) {
				return false;
			}
			Restore(d);
			return true;
		});
		DimAll();
		gDimming.store(!gDim.empty(), std::memory_order_release);
	}

	void SneakDim(RE::NiLight* a_light, const RE::Actor* a_caster)
	{
		if (!a_light || !a_caster) {
			return;
		}
		const auto      id = a_caster->GetFormID();
		std::lock_guard l{ gLock };
		auto            it = gLevels.find(id);
		if (it == gLevels.end()) {
			if (!a_caster->IsSneaking() || !Fades()) {
				return;
			}
			it = gLevels.emplace(id, 1.0f).first;
		}
		if (it->second >= 1.0f) {
			return;
		}
		auto& d = gDim[a_light];
		if (!d.hold) {
			d.hold.reset(a_light);
			d.owner = id;
		}
		d.seen = gFrame;
		DimOne(d, it->second);
		gDimming.store(true, std::memory_order_release);
	}

	void DropSneakFade()
	{
		std::lock_guard l{ gLock };
		gDim.clear();
		gLevels.clear();
		gDimming.store(false, std::memory_order_release);
	}

	SneakPause::SneakPause(const RE::TESObjectREFR* a_owner) :
		owner_(a_owner ? a_owner->GetFormID() : 0)
	{
		if (!gDimming.load(std::memory_order_acquire)) {
			return;
		}
		gLock.lock();
		held_ = true;
		for (auto& d : gDim | std::views::values) {
			d.paused = d.down && (owner_ == 0 || d.owner == owner_);
			if (d.paused) {
				Restore(d);
			}
		}
	}

	SneakPause::~SneakPause()
	{
		if (!held_) {
			return;
		}
		for (auto& d : gDim | std::views::values) {
			if (d.paused) {
				DimOne(d, LevelNow(d.owner));
				d.paused = false;
			}
		}
		gLock.unlock();
	}
}
