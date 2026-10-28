// Illuminated - SKSE plugin
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE.txt and the notice at the top of main.cpp.
//
// Lights fade while sneaking. With "Lights fade while sneaking" a sneaking caster's lights glide down to Sneaking brightness
// over half a second, and back up when they stand. With "While invisible or in shadow" on "Lights fade out" an invisible
// caster's lights glide all the way out the same way, and hers while she carries a perk that needs the dark.
// Light Placer can only cut a light at once, so the menu build's rows carry
// no sneak test (lagen.py drop_sneak_tests) and the fade is done here, per caster:
//   Light Placer's lights of our copies and our own art / weapon / flesh lights, by the actor they hang on - found once a frame
//   in the scene's lists (the main thread); a rune's (a placed spell's lobbed projectile) by the one who laid it, unless
//   "Runes" says they stay lit
//   the game's own casting light on the record route (Vanilla / ENB, or no Light Placer), by its caster - SneakDim from the
//   caster's update, after every other pass wrote it
//   the lights StreamLights.cpp hangs on a projectile (a bolt's row, the ward dome) by its shooter; a cone's cloud light is
//   dimmed where it is moved (SneakDim)
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
		constexpr std::string_view kInvisible = "IlluminatedWhileInvisible";  // 0 fade out (the default), 1 stay on
		constexpr std::string_view kRunes = "IlluminatedSneakingRunes";       // 0 fade with their caster (the default), 1 stay lit
		constexpr std::string_view kArtLight = "IlluminatedArtLight";         // ArtLights.cpp
		constexpr std::string_view kRowLight = "IlluminatedStreamRow";        // StreamLights.cpp: a bolt's row
		constexpr std::string_view kWardLight = "IlluminatedStream";          // StreamLights.cpp: the ward dome
		constexpr float            kTopCutoff = 1.0f;                         // the highest cutoff Light Placer and our lights write
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

		[[nodiscard]] bool  Fades() { return SettingValue(kSneaking, 0) == 0; }
		[[nodiscard]] bool  FadesInvisible() { return SettingValue(kInvisible, 0) == 0; }
		[[nodiscard]] bool  RunesFade() { return SettingValue(kRunes, 0) == 0; }
		[[nodiscard]] float SneakLow() { return static_cast<float>(std::clamp(SettingValue(kSneakBrightness, 0), 0, 100)) / 100.0f; }

		// a perk or ability that only pays off in the dark (a Shadow birthsign: spells cheaper and quieter "in shadows") reads the
		// light level, which our lights on her raise - so while she carries one her lights fade out (his pick). Found once: every
		// perk whose conditions, or an entry point's, ask for a LOW light level, and every ability with such an effect
		[[nodiscard]] bool ReadsLightLevel(const RE::TESCondition& a_condition)
		{
			using Op = RE::CONDITION_ITEM_DATA::OpCode;
			for (const auto* item = a_condition.head; item; item = item->next) {
				const auto op = item->data.flags.opCode;
				if (item->data.functionData.function.get() == RE::FUNCTION_DATA::FunctionID::kGetLightLevel &&
					item->data.object.get() == RE::CONDITIONITEMOBJECT::kSelf && (op == Op::kLessThan || op == Op::kLessThanOrEqualTo)) {
					return true;
				}
			}
			return false;
		}

		struct ShadowSigns
		{
			std::vector<RE::BGSPerk*>   perks;
			std::vector<RE::SpellItem*> abilities;
		};

		[[nodiscard]] const ShadowSigns& Signs()
		{
			static const ShadowSigns signs = [] {
				ShadowSigns found;
				auto*       dh = RE::TESDataHandler::GetSingleton();
				if (!dh) {
					return found;
				}
				for (auto* perk : dh->GetFormArray<RE::BGSPerk>()) {
					const bool reads = perk && (ReadsLightLevel(perk->perkConditions) || std::ranges::any_of(perk->perkEntries, [](const auto* a_entry) {
						const auto* point = a_entry && a_entry->GetType() == RE::PERK_ENTRY_TYPE::kEntryPoint ?
						                        static_cast<const RE::BGSEntryPointPerkEntry*>(a_entry) :
						                        nullptr;
						// the perk owner's own tab only (the first): an entry point's other tabs ask about a target or a weapon
						return point && point->conditions.size() > 0 && ReadsLightLevel(point->conditions[0]);
					}));
					if (reads) {
						found.perks.push_back(perk);
					}
				}
				for (auto* spell : dh->GetFormArray<RE::SpellItem>()) {
					if (spell && spell->GetSpellType() == RE::MagicSystem::SpellType::kAbility &&
						std::ranges::any_of(spell->effects, [](const auto* a_effect) {
							return a_effect && (ReadsLightLevel(a_effect->conditions) || (a_effect->baseEffect && ReadsLightLevel(a_effect->baseEffect->conditions)));
						})) {
						found.abilities.push_back(spell);
					}
				}
				SKSE::log::info("sneak fade: {} perk(s) and {} abilit(y/ies) read the light level (her lights fade while she carries one)",
					found.perks.size(), found.abilities.size());
				return found;
			}();
			return signs;
		}

		std::atomic<bool> gShadowSign{ false };  // she carries one of Signs() - read once a frame on the main thread

		[[nodiscard]] bool CarriesShadowSign(const RE::Actor& a_actor)
		{
			const auto& signs = Signs();
			return std::ranges::any_of(signs.perks, [&](auto* a_perk) { return a_actor.HasPerk(a_perk); }) ||
			       std::ranges::any_of(signs.abilities, [&](auto* a_spell) { return a_actor.HasSpell(a_spell); });
		}

		// the level a caster's lights glide to: Sneaking brightness while it sneaks, out while it is invisible (the Shadow Stone,
		// Invisibility - every effect of that kind raises the Invisibility value) or, for her, while she carries a shadow sign;
		// else full
		[[nodiscard]] float TargetOf(const RE::Actor& a_actor, float a_low)
		{
			float target = 1.0f;
			if (Fades() && a_actor.IsSneaking()) {
				target = a_low;
			}
			if (FadesInvisible() && (a_actor.AsActorValueOwner()->GetActorValue(RE::ActorValue::kInvisibility) > 0.0f ||
										(a_actor.IsPlayerRef() && gShadowSign.load(std::memory_order_relaxed)))) {
				target = 0.0f;
			}
			return target;
		}

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
			// pulls the light in (measured: 30 % on the record lit 7 % of the ground). The cutoff moves with it, as Brightness does.
			// Off Community Shaders those two words are the ambient color, never touched
			auto* light = a_d.hold.get();
			a_d.inverseSquare = InverseSquare() && LightKit::Isl::On(light);
			if (a_d.inverseSquare) {
				a_d.preCutoff = LightKit::Isl::Cutoff(light);
				LightKit::Isl::SetCutoff(light, a_d.preCutoff * a_level, 0.01f, kTopCutoff);
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
				LightKit::Isl::SetCutoff(light, a_d.preCutoff, 0.01f, kTopCutoff);
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

		struct Caster
		{
			RE::FormID id{ 0 };
			float      target{ 1.0f };  // TargetOf
		};

		// the caster a light of ours fades with: the actor it hangs on, the shooter of the projectile a row or ward light of ours
		// hangs on, or the one who laid a rune (its shooter, else whoever set the chain off) - an enemy's rune or a trap's (laid by
		// nobody) never fades because she sneaks, and with Runes on "Stay lit" none does
		[[nodiscard]] std::optional<Caster> CasterOf(const RE::NiLight& a_light, float a_low, bool a_runesFade)
		{
			const auto of = [a_low](const RE::Actor* a_actor) {
				return a_actor ? std::optional<Caster>{ Caster{ a_actor->GetFormID(), TargetOf(*a_actor, a_low) } } : std::nullopt;
			};
			const auto* ref = OwnerOf(&a_light);
			if (!ref) {
				return std::nullopt;
			}
			if (const auto* actor = ref->As<RE::Actor>()) {
				return of(actor);
			}
			const auto* projectile = ref->As<RE::Projectile>();
			if (!projectile) {
				return std::nullopt;
			}
			const std::string_view name = a_light.name.c_str();
			if (name != kRowLight && name != kWardLight) {
				const auto* base = ref->GetBaseObject();
				const auto* data = base ? base->As<RE::BGSProjectile>() : nullptr;
				if (!data || !data->IsGrenade() || !projectile->GetProjectileRuntimeData().spell) {
					return std::nullopt;
				}
				// a rune proper is set off by someone coming near (its proximity trigger); a timed burst or a bloom has none
				if (!a_runesFade && data->data.explosionProximity > 0.0f) {
					return std::nullopt;
				}
			}
			if (const auto shooter = projectile->GetProjectileRuntimeData().shooter.get()) {
				return of(shooter->As<RE::Actor>());
			}
			const auto* cause = ref->GetActorCause();
			const auto  by = cause ? cause->actor.get() : RE::NiPointer<RE::Actor>{};
			return of(by.get());
		}

		[[nodiscard]] bool Ours(RE::NiLight* a_light)
		{
			auto* point = a_light ? netimmerse_cast<RE::NiPointLight*>(a_light) : nullptr;
			if (!point) {
				return false;
			}
			const std::string_view name = point->name.c_str();
			return name == kArtLight || name == kRowLight || name == kWardLight || IsOurLightPlacerLight(*point);
		}

		struct Found
		{
			RE::NiLight* light{ nullptr };
			RE::FormID   owner{ 0 };
			float        target{ 1.0f };
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

	void FindShadowSigns() { static_cast<void>(Signs()); }

	void SneakFrameEnd(float a_delta)
	{
		const bool              fades = Fades() || FadesInvisible();
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
		const float low = SneakLow();
		const bool  runesFade = RunesFade();
		const float step = std::clamp(a_delta, 0.0f, 0.25f) / kFadeSeconds;
		if (const auto* player = RE::PlayerCharacter::GetSingleton()) {
			gShadowSign.store(FadesInvisible() && CarriesShadowSign(*player), std::memory_order_relaxed);
		}

		// outside the lock: the scene's lists and the game's forms (the main thread)
		static std::vector<Found> found;  // its storage kept between frames
		found.clear();
		if (auto* scene = RE::BSShaderManager::State::GetSingleton().shadowSceneNode[0]) {
			const auto walk = [&](const auto& a_list) {
				for (const auto& bs : a_list) {
					auto* light = bs ? bs->light.get() : nullptr;
					if (const auto caster = Ours(light) ? CasterOf(*light, low, runesFade) : std::nullopt) {
						found.push_back({ light, caster->id, caster->target });
					}
				}
			};
			walk(scene->GetRuntimeData().activeLights);
			walk(QueuedLights(scene));  // copied under its lock: the game and Light Placer add to it (the audit's M5)
		}
		std::unordered_map<RE::FormID, float> targets;  // a caster still here: the level it glides to now
		std::vector<RE::FormID>               gone;
		for (const auto id : listed) {
			if (const auto* actor = RE::TESForm::LookupByID<RE::Actor>(id)) {
				targets[id] = TargetOf(*actor, low);
			} else {
				gone.push_back(id);
			}
		}
		for (const auto& f : found) {
			targets[f.owner] = f.target;
		}

		std::lock_guard l{ gLock };
		for (const auto id : gone) {
			gLevels.erase(id);
		}
		for (const auto& f : found) {
			if (f.target < 1.0f) {
				gLevels.try_emplace(f.owner, 1.0f);
			}
		}
		// each caster one step toward its target (Sneaking brightness, out while invisible, else full); one listed by a caster's
		// update since the look above takes its first step next frame
		std::erase_if(gLevels, [&](auto& a_kv) {
			const auto s = targets.find(a_kv.first);
			if (s == targets.end()) {
				return false;
			}
			const float target = s->second;
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
			if (TargetOf(*a_caster, SneakLow()) >= 1.0f) {
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
