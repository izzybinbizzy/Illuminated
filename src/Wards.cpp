// Illuminated - SKSE plugin
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE.txt and the notice at the top of main.cpp.
//
// The wards: one dome per ward, and 360 Ward's sphere in the vanilla blue. Steps down when DynamicWards.dll is loaded,
// which dresses every ward itself (his call: "have the whole ward system step down if dynamic wards is present").
//
// ONE DOME. A ward spell's rank is the effect that accumulates Ward Power; every OTHER effect on the same spell that wears
// the vanilla ward art draws a second dome and a second hand light (vanilla's ShieldConcSelf, Odin's Ward Regeneration,
// Mysticism's Dummy rows - measured in his game). Those lose their art and their light. An effect on a mod's
// own ward art (a vampire's blood ward) is never touched.
//
// THE BLUE SPHERE. 360 Ward's own sphere is orange (its mesh uses the fire gradient); with 360 Ward loaded the vanilla
// dome art form and 360 Ward's hit flash point at our copies under `meshes\magic\Glow Wards\Vanilla\` (his report,
// "the 360 ward is orange"; the look below). Nothing is saved. The ward hand wears our copy under `Glow Wards\Blue\`, and
// its light row claims that copy beside the vanilla mesh (lagen.py WARD_HAND_COPIES), so it is lit on either route.
//
// THE WARD LOOK (his words 2026-10-09: "change the blue ward to vanilla blue. add transparent ward with vanilla outline as a
// preset for wards, no slider just pure transparent with outline."): the setting IlluminatedWardLook - 0 the vanilla ward's own
// blue (`Glow Wards\Vanilla\`: the sphere and flash retinted to gradwardcolor's hue; the old `Blue\` sphere was Dynamic Wards'
// deeper Adept Blue), 1 Transparent (`Glow Wards\Transparent\`: no fill, the rim glow as the outline; without 360 Ward our
// vanilla-blue dome with its bright layer turned into a rim). The worn wards (Spellbreaker, Crusader, Reman) follow it too
// (his answer 2026-10-13): their looping dome has a Transparent copy. Applied at data load and on every settings refresh
// (ApplyWardLook): an art form's model is read when a ward is raised, so it shows on the next ward.

#include "Plugin.h"

namespace Plugin
{
	namespace
	{
		constexpr std::uint32_t    kShieldConcSelf = 0x0FCC62;  // Skyrim.esm
		constexpr std::uint32_t    kDomeArt = 0x018124;         // Skyrim.esm WardHitEffect: magic\wardbodyfx.nif
		constexpr std::uint32_t    kFlashArt = 0x000802;        // 360 Ward.esp: magic\wardShieldHitFX.nif
		constexpr const char*      k360Plugin = "360 Ward.esp";
		constexpr const char*      kEmptyModel = "Effects\\FXEmptyObject.nif";
		constexpr const char*      kBlueSphere = "Magic\\Glow Wards\\Vanilla\\wardbodyfx360.nif";
		constexpr const char*      kBlueFlash = "Magic\\Glow Wards\\Vanilla\\wardshieldhitfx.nif";
		constexpr const char*      kClearSphere = "Magic\\Glow Wards\\Transparent\\wardbodyfx360.nif";
		constexpr const char*      kClearDome = "Magic\\Glow Wards\\Transparent\\wardbodyfx.nif";
		constexpr const char*      kBlueWornDome = "Magic\\Glow Wards\\Blue\\wardbodyfxworn.nif";
		constexpr const char*      kClearWornDome = "Magic\\Glow Wards\\Transparent\\wardbodyfxworn.nif";
		constexpr const char*      kBlueWornHand = "Magic\\Glow Wards\\Blue\\wardinhandfxworn.nif";
		constexpr std::string_view kWardLook = "IlluminatedWardLook";  // 0 vanilla blue, 1 transparent
		int                        gLookApplied = -1;                  // the look last put on the forms (-1 none yet)
		// his call: the dome (without 360 Ward) and the hand (always) wear OUR vanilla-blue copies (their own textures, so no
		// other mod's loose textures reach them)
		constexpr std::uint32_t kHandArt = 0x0253F1;  // Skyrim.esm: magic\wardinhandfx.nif
		constexpr const char*   kBlueDome = "Magic\\Glow Wards\\Blue\\wardbodyfx.nif";
		constexpr const char*   kBlueHand = "Magic\\Glow Wards\\Blue\\wardinhandfx.nif";

		// HIS REPORT (Spellbreaker - the old fix, lost in a handoff purge): a WORN ward - Spellbreaker, the Divine
		// Crusader shield, the Shield of Reman - never drives a cast ward's transitions, so the vanilla meshes sit idle: only
		// the swirl, and the dome flashes as the ward ends. They wear copies of our dome and hand on Magic\\BeginLoopEnd.hkx
		// (`wardgen.py warddomes`), in art forms of their own, so the cast wards keep theirs.
		struct WornRow
		{
			const char* plugin;
			RE::FormID  id;
		};
		constexpr WornRow kWorn[] = {
			{ "Skyrim.esm", 0x07DCDB },                        // Spellbreaker's Ward - Deflection
			{ "ccmtysse001-knightsofthenine.esl", 0x00081D },  // the Divine Crusader shield's
			{ "LegacyoftheDragonborn.esm", 0x124E5B },         // Reman's Ward
		};
		RE::BGSArtObject* gWornDome = nullptr;
		RE::BGSArtObject* gWornHand = nullptr;
		// the worn wards' models, loaded once and held for the session (his look 2026-10-19, checklist 67: "ward stays up but is
		// not instant like dynamic wards"): read off disk when a worn ward came up, its dome showed late, while Dynamic Wards keeps
		// every ward model it dresses loaded and its dome shows at once
		std::vector<RE::NiPointer<RE::NiNode>> gWornModels;

		// the bare mesh name, lower case: "Magic\WardBodyFX.nif" -> "wardbodyfx"
		std::string MeshName(const char* a_model)
		{
			auto path = NormalPath(a_model ? a_model : "");
			if (const auto slash = path.find_last_of("\\/"); slash != std::string::npos) {
				path.erase(0, slash + 1);
			}
			if (const auto dot = path.rfind('.'); dot != std::string::npos) {
				path.erase(dot);
			}
			return path;
		}

		bool IsVanillaWardArt(const RE::BGSArtObject* a_art)
		{
			if (!a_art) {
				return false;
			}
			const auto name = MeshName(a_art->GetModel());
			return name == "wardinhandfx" || name == "wardbodyfx" || name == "wardbodyfx360";
		}

		bool WearsWardArt(const RE::EffectSetting* a_e)
		{
			return a_e && (IsVanillaWardArt(a_e->data.castingArt) || IsVanillaWardArt(a_e->data.hitEffectArt));
		}

		bool IsRank(const RE::EffectSetting* a_e)
		{
			return a_e->data.archetype == RE::EffectArchetypes::ArchetypeID::kAccumulateMagnitude &&
			       a_e->data.primaryAV == RE::ActorValue::kWardPower;
		}

		// the empty art a silenced effect wears, made on first need
		RE::BGSArtObject* EmptyArt()
		{
			static RE::BGSArtObject* const empty = [] {
				auto* art = NewForm<RE::BGSArtObject>();
				if (art) {
					art->SetModel(kEmptyModel);
				}
				return art;
			}();
			return empty;
		}

		// a second dome's effect: no art, no hand light (its own work - Ward Power, Stamina - is left as it is)
		bool Silence(RE::EffectSetting* a_e)
		{
			auto* empty = EmptyArt();
			if (!empty) {
				return false;
			}
			a_e->data.castingArt = empty;
			a_e->data.hitEffectArt = empty;
			a_e->data.enchantEffectArt = nullptr;
			a_e->data.light = nullptr;
			SKSE::log::info("[WARD-SILENT] {}", Label(a_e));
			return true;
		}

		// main thread, data load; not on VR (the loader's address is read off the SE / AE Address Library only, as LightKit.h
		// ArtColor's)
		void HoldWornModels()
		{
			if (REL::Module::IsVR() || !gWornModels.empty()) {
				return;
			}
			for (const auto* model : { kBlueWornHand, kBlueWornDome, kClearWornDome }) {
				RE::NiPointer<RE::NiNode> root;
				if (RE::BSModelDB::Demand(model, root, RE::BSModelDB::DBTraits::ArgsType{}) == RE::BSResource::ErrorCode::kNone && root) {
					gWornModels.push_back(std::move(root));
				} else {
					SKSE::log::warn("wards: {} could not be loaded ahead - a worn ward reads it when it comes up", model);
				}
			}
			SKSE::log::info("wards: {} worn ward model(s) held loaded for the session", gWornModels.size());
		}
	}

	// Dynamic Wards 2.0 and later is a DLL only (no plugin); it dresses every ward itself
	bool DynamicWardsLoaded()
	{
		static const bool loaded = REX::W32::GetModuleHandleA("DynamicWards.dll") != nullptr;
		return loaded;
	}

	void ApplyWardLook()
	{
		if (DynamicWardsLoaded()) {
			return;
		}
		const int look = std::clamp(SettingValue(kWardLook, 0), 0, 1);
		if (look == gLookApplied) {
			return;
		}
		gLookApplied = look;
		// the worn wards follow the look (his answer 2026-10-13): their own looping dome, which they wear without 360 Ward - set
		// first, so a missing vanilla dome art form below never leaves it without a model
		if (gWornDome) {
			gWornDome->SetModel(look == 1 ? kClearWornDome : kBlueWornDome);
		}
		auto* dh = RE::TESDataHandler::GetSingleton();
		auto* dome = dh ? dh->LookupForm<RE::BGSArtObject>(kDomeArt, "Skyrim.esm") : nullptr;
		if (!dome) {
			return;
		}
		// loaded, not merely present: LookupModByName also finds a plugin that is installed but not enabled
		if (PluginLoaded(k360Plugin)) {
			dome->SetModel(look == 1 ? kClearSphere : kBlueSphere);
			if (auto* flash = dh->LookupForm<RE::BGSArtObject>(kFlashArt, k360Plugin)) {
				flash->SetModel(kBlueFlash);
			}
			SKSE::log::info("wards: 360 Ward is loaded - its sphere {} ({}), its flash in the vanilla blue",
				look == 1 ? "transparent" : "in the vanilla blue", look == 1 ? kClearSphere : kBlueSphere);
		} else {
			dome->SetModel(look == 1 ? kClearDome : kBlueDome);
			SKSE::log::info("wards: the dome {} ({})", look == 1 ? "transparent" : "in our vanilla blue", look == 1 ? kClearDome : kBlueDome);
		}
	}

	void Wards()
	{
		if (DynamicWardsLoaded()) {
			SKSE::log::info("wards: Dynamic Wards is loaded - it dresses the wards, so this pass leaves them alone");
			return;
		}
		auto* dh = RE::TESDataHandler::GetSingleton();
		if (!dh) {
			return;
		}

		// one dome
		std::set<RE::EffectSetting*> extras;
		if (auto* shield = dh->LookupForm<RE::EffectSetting>(kShieldConcSelf, "Skyrim.esm"); shield && WearsWardArt(shield)) {
			extras.insert(shield);
		}
		for (auto* spell : dh->GetFormArray<RE::SpellItem>()) {
			if (!spell) {
				continue;
			}
			bool                            rank = false;
			std::vector<RE::EffectSetting*> others;
			for (auto* effect : spell->effects) {
				auto* base = effect ? effect->baseEffect : nullptr;
				if (!WearsWardArt(base)) {
					continue;
				}
				if (IsRank(base)) {
					rank = true;
				} else {
					others.push_back(base);
				}
			}
			if (rank) {
				extras.insert(others.begin(), others.end());
			}
		}
		for (auto* e : extras) {
			Silence(e);  // the second hand light goes with the second dome
		}
		SKSE::log::info("wards: {} effect(s) wear the ward art beside a ward's rank and show no second dome", extras.size());

		if (auto* hand = dh->LookupForm<RE::BGSArtObject>(kHandArt, "Skyrim.esm")) {
			hand->SetModel(kBlueHand);
			SKSE::log::info("wards: the hand wears our vanilla blue ({})", kBlueHand);
		}
		// the worn wards: their own looping hand always, their own looping dome unless 360 Ward's sphere is the dome (the
		// dome's model is the ward look's, set by ApplyWardLook below)
		if (!gWornDome) {
			gWornDome = NewForm<RE::BGSArtObject>();
		}
		if (!gWornHand) {
			gWornHand = NewForm<RE::BGSArtObject>();
		}
		if (gWornDome && gWornHand) {
			gWornHand->SetModel(kBlueWornHand);
			const bool                         has360 = PluginLoaded(k360Plugin);
			auto*                              dome = dh->LookupForm<RE::BGSArtObject>(kDomeArt, "Skyrim.esm");
			std::set<const RE::EffectSetting*> wornEffects;
			for (const auto& r : kWorn) {
				if (auto* e = dh->LookupForm<RE::EffectSetting>(r.id, r.plugin)) {
					e->data.castingArt = gWornHand;
					e->data.hitEffectArt = has360 ? dome : gWornDome;
					wornEffects.insert(e);
				}
			}
			// one dome on a shield too (checklist 67, read 2026-10-19): Artificer's Spellbreaker enchantment carries the worn ward
			// beside two effects of its own in the vanilla ward art (Ward Power, Stamina) - those drew two idle domes and two more
			// hand lights; like a spell's extras they lose their art and light
			// an effect a castable spell also carries keeps its art (silencing it would take the art off that spell everywhere)
			std::set<const RE::EffectSetting*> castable;
			for (const auto* spell : dh->GetFormArray<RE::SpellItem>()) {
				if (spell && spell->GetSpellType() != RE::MagicSystem::SpellType::kEnchantment) {
					for (const auto* effect : spell->effects) {
						if (effect && effect->baseEffect) {
							castable.insert(effect->baseEffect);
						}
					}
				}
			}
			std::size_t shieldExtras = 0;
			for (auto* ench : dh->GetFormArray<RE::EnchantmentItem>()) {
				if (!ench) {
					continue;
				}
				bool carries = false;
				for (const auto* effect : ench->effects) {
					carries = carries || (effect && wornEffects.contains(effect->baseEffect));
				}
				if (!carries) {
					continue;
				}
				for (auto* effect : ench->effects) {
					auto* base = effect ? effect->baseEffect : nullptr;
					if (base && !wornEffects.contains(base) && !castable.contains(base) && WearsWardArt(base) && Silence(base)) {
						++shieldExtras;
					}
				}
			}
			HoldWornModels();
			SKSE::log::info("wards: {} worn ward(s) (Spellbreaker, Crusader, Reman) wear the looping copies; {} other ward-art effect(s) on their enchantments show no dome",
				wornEffects.size(), shieldExtras);
		}

		// the sphere / dome and the worn dome: the ward look
		gLookApplied = -1;
		ApplyWardLook();
	}

	// ------------------------------------------------------------------ the shield's ward at once
	// HIS LOOK (checklist 67): "ward stays up but is not instant like dynamic wards which needs to be fixed as a user also
	// suggested." and again 2026-10-19: "I looked its not as fast as dynamic wards". Measured at 30 pictures a second:
	// Spellbreaker's dome shows 0.1-1.0 s after block is pressed. The shield raises its ward through its enchantment, whose effects carry the
	// condition IsBlocking == 1; the game re-checks an active effect's conditions only on its own timer
	// (fActiveEffectConditionUpdateInterval), so the ward waits for the next check. Here, when the player's block starts, every
	// one of her active effects that comes from an ENCHANTMENT and whose conditions ask IsBlocking is checked at once
	// (ActiveEffect::EvaluateConditions, the effect's own virtual - no address needed), and once more on the next frame in case
	// the game has not yet marked her blocking when the animation event comes. Only those effects: nothing else changes timing.
	// It runs with or without Dynamic Wards (the wait is the game's, not the dome's), so it is not behind DynamicWardsLoaded.
	namespace
	{
		// the animation event the player's graph sends as a block begins (read off the game's own events 2026-10-19 - a block sends
		// "blockStartOut"; "blockStart" is the input event's name and never reached the graph)
		constexpr std::string_view kBlockTag = "blockStartOut";

		// an enchantment's effect whose conditions ask IsBlocking (the shield wards: Spellbreaker, Artificer's two extra
		// Spellbreaker effects, the Crusader shields, any mod's shield that works the same way)
		[[nodiscard]] bool WaitsForBlock(const RE::ActiveEffect* a_effect)
		{
			using Flag = RE::ActiveEffect::Flag;
			if (!a_effect || !a_effect->effect || !a_effect->spell || a_effect->flags.none(Flag::kHasConditions) ||
				a_effect->flags.any(Flag::kDispelled) || a_effect->spell->GetSpellType() != RE::MagicSystem::SpellType::kEnchantment) {
				return false;
			}
			for (const auto* item = a_effect->effect->conditions.head; item; item = item->next) {
				if (item->data.functionData.function.get() == RE::FUNCTION_DATA::FunctionID::kIsBlocking) {
					return true;
				}
			}
			return false;
		}

		class ShieldWards : public RE::MagicTarget::ForEachActiveEffectVisitor
		{
		public:
			RE::BSContainer::ForEachResult Accept(RE::ActiveEffect* a_effect) override
			{
				if (WaitsForBlock(a_effect)) {
					found.push_back(a_effect);
				}
				return RE::BSContainer::ForEachResult::kContinue;
			}

			std::vector<RE::ActiveEffect*> found;
		};

		// main thread: the shield wards' conditions read now, not on the game's next check. Collected first, then checked, so
		// the list is never walked while an effect changes state
		void RecheckShieldWards()
		{
			auto* player = RE::PlayerCharacter::GetSingleton();
			auto* target = player ? player->AsMagicTarget() : nullptr;
			if (!target) {
				return;
			}
			ShieldWards wards;
			target->VisitEffects(wards);  // SE, AE and VR alike (FadeCharge.cpp walks them the same way)
			for (auto* effect : wards.found) {
				effect->EvaluateConditions(0.0f, true);
			}
		}

		class BlockWatch : public RE::BSTEventSink<RE::BSAnimationGraphEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const RE::BSAnimationGraphEvent* a_event,
				RE::BSTEventSource<RE::BSAnimationGraphEvent>*) override
			{
				LightKit::Guarded("block watch", [a_event]() {
					const std::string_view tag = a_event ? a_event->tag.c_str() : "";
					if (!LightKit::SameText(tag, kBlockTag)) {
						return;
					}
					static std::atomic_bool told{ false };  // the graph's events may come off the main thread
					if (!told.exchange(true)) {
						SKSE::log::info("wards: a block started - the shield wards are checked at once");
					}
					// the graph's events may come off the main thread: the effects are read on it
					if (auto* tasks = SKSE::GetTaskInterface()) {
						tasks->AddTask([]() {
							LightKit::Guarded("shield ward check", []() {
								RecheckShieldWards();
								// once more a frame later: IsBlocking may only read true after this frame's update
								if (auto* again = SKSE::GetTaskInterface()) {
									again->AddTask([]() { LightKit::Guarded("shield ward check (next frame)", RecheckShieldWards); });
								}
							});
						});
					}
				});
				return RE::BSEventNotifyControl::kContinue;
			}
		};
		BlockWatch gBlockWatch;
		// the graph the watch is on (main thread only): a transformation (werewolf, Vampire Lord) or a 3D reset builds the player a
		// new one, and a new game has none at its first message - KeepBlockWatch puts the watch on whatever graph she has now
		const RE::BSAnimationGraphManager* gWatchedGraph = nullptr;
		float                              gWatchClock = 0.0f;
	}

	// a game loaded or a new one started (main.cpp), or her graph was rebuilt (KeepBlockWatch): the watch is put on the graph
	// she has (taken off first, so it is never on twice)
	void WatchBlocking()
	{
		auto* player = RE::PlayerCharacter::GetSingleton();
		if (!player) {
			return;
		}
		RE::BSTSmartPointer<RE::BSAnimationGraphManager> graph;
		player->GetAnimationGraphManager(graph);
		player->RemoveAnimationGraphEventSink(&gBlockWatch);
		const bool added = graph && player->AddAnimationGraphEventSink(&gBlockWatch);
		gWatchedGraph = added ? graph.get() : nullptr;
		SKSE::log::info("wards: the block watch is {} the player's animation graph", added ? "on" : "NOT on (no graph yet)");
	}

	// player update (LightCopies.cpp), main thread: once a second, a graph that is not the one the watch is on gets the watch
	void KeepBlockWatch(float a_delta)
	{
		gWatchClock += std::max(a_delta, 0.0f);
		if (gWatchClock < 1.0f) {
			return;
		}
		gWatchClock = 0.0f;
		auto*                                            player = RE::PlayerCharacter::GetSingleton();
		RE::BSTSmartPointer<RE::BSAnimationGraphManager> graph;
		if (player && player->GetAnimationGraphManager(graph) && graph && graph.get() != gWatchedGraph) {
			WatchBlocking();
		}
	}
}
