// Illuminated - SKSE plugin
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE.txt and the notice at the top of main.cpp.
//
// The Brightness and Reach sliders. A Light Placer light that states no fade, radius or cutoff of its own takes them
// from its light record, and reads them each time it makes a light (Light Placer's LightData.cpp: GetFade, GetRadius,
// GetCutoff). So every light the configs name is an in-memory copy made here - one per light record, fade, radius and
// cutoff the configs used - and the sliders set those three on the copy. A copy is found by its editor ID, which Light
// Placer looks up when it reads its configs, after this runs. The copies are listed in the settings files ([light]
// blocks), written by lagen.py. Pass 0's own copies of the game's magic lights (LightSettings.cpp) and pass 4's spray
// lights (SprayLights.cpp) join the list too, so every light this mod makes follows the sliders.
//
// How the two sliders reach a light:
//   Brightness  the fade.
//   Reach       the radius - and, for an inverse-square light, the cutoff. Community Shaders works such a light's reach
//               out from its fade and its cutoff alone (measured 2026-10-01: the stated radius changed nothing, and a
//               brighter light reached further). The cutoff is the made-with one x Brightness / Reach², which moves the
//               reach by Reach and holds it still while Brightness changes.
//               On ENB and Vanilla (Lighting.cpp) an inverse-square copy is made plain when it is made: no cutoff, the
//               reach it had drawn as a radius, and Reach moves that radius.
//
// A light that is already lit follows at once: on the frame a slider changes, every lit light that names one of our
// copies takes its copy's cutoff, and a steady light's fade moves by the change (Frame, on the main thread).
//
// A flickering light (`flicker=1`) is the one kind the record cannot carry: Light Placer writes its fade from the
// config's keys every frame, never from the record. Those lights are kept in a list, and each is scaled by Brightness
// straight after Light Placer wrote it - Light Placer does that in five places, most of them on the game's worker
// threads (traced 2026-10-01), and each is wrapped here, after Light Placer's own hook:
//   a caster's update        the hands of every actor (the player's among them)
//   an effect's update       enchantment and hit effects
//   an explosion's update    } SE/AE only: these three are calls inside game functions, at the places Light Placer
//   a hazard's update        } (powerof3/LightPlacer, Hooks/Update.cpp) hooks them. In VR these lights keep
//   a cell's animations      } their made-with strength.
// Light Placer skips a culled or distant light, so a value it did not rewrite is never scaled twice. Nothing in this
// part runs while Brightness is 100%.

#include "Plugin.h"

namespace Plugin
{
	namespace
	{
		constexpr std::string_view kBrightness = "IlluminatedBrightness";
		constexpr std::string_view kReach = "IlluminatedReach";
		constexpr float            kLowestCutoff = 0.01f, kHighestCutoff = 1.0f;  // what Light Placer keeps a cutoff within
		constexpr std::uint32_t    kInverseSquare = 1u << 14;                      // Community Shaders' flag on a light record

		std::atomic<bool>  gRefresh{ false };
		std::atomic<float> gBrightness{ 1.0f };

		// what a lit light of a config's copy needs, by the copy's editor ID. Written at data load and on the main thread.
		struct Lit
		{
			bool  flicker{ false };
			float cutoff{ 0.0f };  // 0: not an inverse-square light
		};
		struct NameHash
		{
			using is_transparent = void;
			std::size_t operator()(std::string_view a_name) const noexcept { return std::hash<std::string_view>{}(a_name); }
		};
		std::unordered_map<std::string, Lit, NameHash, std::equal_to<>> gLit;

		// the flickering lights lit now. The light is held, so its address is never another light's while it is listed.
		struct Flicker
		{
			RE::NiPointer<RE::NiPointLight> light;
			RE::FormID                      owner{ 0 };       // the reference it hangs on
			const RE::TESObjectCELL*        cell{ nullptr };  // where that reference was when the light was found
			float                           written{ -1.0f }; // what the light held after our last write
		};
		std::unordered_map<const RE::NiPointLight*, Flicker> gFlickers;
		std::mutex                                           gFlickerLock;

		// Light Placer names a light "LP_Light[<config>|<light record's editor ID>]..."
		const Lit* LitOf(const RE::NiPointLight& a_light)
		{
			const std::string_view name = a_light.name.c_str();
			if (!name.starts_with("LP_Light[")) {
				return nullptr;
			}
			const auto bar = name.find('|');
			const auto end = bar == std::string_view::npos ? bar : name.find(']', bar);
			if (end == std::string_view::npos) {
				return nullptr;
			}
			const auto it = gLit.find(name.substr(bar + 1, end - bar - 1));
			return it == gLit.end() ? nullptr : &it->second;
		}

		const RE::TESObjectREFR* OwnerOf(const RE::NiAVObject* a_object)
		{
			for (; a_object; a_object = a_object->parent) {
				if (const auto* ref = a_object->GetUserData()) {
					return ref;
				}
			}
			return nullptr;
		}

		// gFlickerLock is held by every caller
		void Scale(Flicker& a_flicker)
		{
			auto& fade = a_flicker.light->GetLightRuntimeData().fade;
			if (fade != a_flicker.written) {
				fade *= gBrightness.load(std::memory_order_relaxed);  // this frame's own value from Light Placer
			}
			a_flicker.written = fade;
		}

		bool Scaling()
		{
			return gBrightness.load(std::memory_order_relaxed) != 1.0f;
		}

		// straight after Light Placer wrote the lights of one reference, or of one cell
		void ScaleOwned(const RE::TESObjectREFR* a_owner)
		{
			if (!a_owner || !Scaling()) {
				return;
			}
			const auto      id = a_owner->GetFormID();
			std::lock_guard l{ gFlickerLock };
			for (auto& [light, flicker] : gFlickers) {
				if (flicker.owner == id) {
					Scale(flicker);
				}
			}
		}

		void ScaleCell(const RE::TESObjectCELL* a_cell)
		{
			if (!a_cell || !Scaling()) {
				return;
			}
			std::lock_guard l{ gFlickerLock };
			for (auto& [light, flicker] : gFlickers) {
				if (flicker.cell == a_cell) {
					Scale(flicker);
				}
			}
		}

		// an explosion's or a hazard's lights are found here as well, so even their first frame is at the slider's strength
		void ScaleUnder(RE::TESObjectREFR* a_owner)
		{
			auto* root = a_owner && Scaling() ? a_owner->Get3D() : nullptr;
			if (!root) {
				return;
			}
			std::lock_guard l{ gFlickerLock };
			RE::BSVisit::TraverseScenegraphLights(root, [&](RE::NiPointLight* a_light) {
				if (const auto* lit = a_light ? LitOf(*a_light) : nullptr; lit && lit->flicker) {
					auto [it, added] = gFlickers.try_emplace(a_light);
					if (added) {
						it->second = { RE::NiPointer<RE::NiPointLight>(a_light), a_owner->GetFormID(), a_owner->GetParentCell() };
					}
					Scale(it->second);
				}
				return RE::BSVisit::BSVisitControl::kContinue;
			});
		}

		// the main thread's look at the scene's lights: new flickering lights are listed; on the frame a slider changed
		// (Brightness by a_ratio) every lit light takes its copy's cutoff and a steady light's fade moves with it
		template <class List>
		void VisitList(const List& a_list, bool a_changed, float a_ratio)
		{
			for (const auto& bsLight : a_list) {
				auto*      light = bsLight ? netimmerse_cast<RE::NiPointLight*>(bsLight->light.get()) : nullptr;
				const Lit* lit = light ? LitOf(*light) : nullptr;
				if (!lit) {
					continue;
				}
				auto& data = light->GetLightRuntimeData();
				if (a_changed && lit->cutoff > 0.0f) {
					data.ambient.green = lit->cutoff;  // where Light Placer put the cutoff when it made the light
				}
				if (!lit->flicker) {
					if (a_changed) {
						data.fade *= a_ratio;
					}
				} else if (auto [it, added] = gFlickers.try_emplace(light); added) {
					const auto* owner = OwnerOf(light);
					it->second = { RE::NiPointer<RE::NiPointLight>(light), owner ? owner->GetFormID() : 0, owner ? owner->GetParentCell() : nullptr };
				}
			}
		}

		// once a frame, on the main thread
		void Frame()
		{
			const bool  changed = gRefresh.exchange(false);
			const float before = gBrightness;
			if (changed) {
				RefreshLights();
			}
			StreamLightsFrame();
			auto* scene = RE::BSShaderManager::State::GetSingleton().shadowSceneNode[0];
			if (!scene || (!changed && !Scaling())) {
				return;
			}
			const auto&     lights = scene->GetRuntimeData();
			std::lock_guard l{ gFlickerLock };
			VisitList(lights.activeLights, changed, gBrightness / before);
			VisitList(lights.lightQueueAdd, changed, gBrightness / before);  // added before the next draw
			if (Scaling()) {
				std::erase_if(gFlickers, [](const auto& a_kv) { return a_kv.second.light->GetRefCount() <= 1; });  // only we hold it: gone
			} else {
				gFlickers.clear();  // Light Placer's own values stand from here on
			}
		}

		// ------------------------------------------------------------------ the hooks
		// All installed at data load, long after Light Placer installed its own at plugin load, so each wraps Light
		// Placer's: ours runs after it, on the values it just wrote.
		struct PlayerUpdate
		{
			static void thunk(RE::PlayerCharacter* a_this, float a_delta)
			{
				func(a_this, a_delta);
				Frame();
			}
			static inline REL::Relocation<decltype(thunk)> func;
			static void                                    Install()
			{
				REL::Relocation<std::uintptr_t> vtbl{ RE::PlayerCharacter::VTABLE[0] };
				func = vtbl.write_vfunc(REL::Relocate(0xAD, 0xAD, 0xAF), thunk);  // Actor::Update
			}
		};

		struct CasterUpdate
		{
			static void thunk(RE::ActorMagicCaster* a_this, float a_delta)
			{
				func(a_this, a_delta);
				ScaleOwned(a_this->GetCasterAsActor());
			}
			static inline REL::Relocation<decltype(thunk)> func;
			static void                                    Install()
			{
				REL::Relocation<std::uintptr_t> vtbl{ RE::ActorMagicCaster::VTABLE[0] };
				func = vtbl.write_vfunc(REL::Relocate(0x1D, 0x1D, 0x1F), thunk);  // MagicCaster::Update
			}
		};

		template <class T>
		struct EffectUpdate
		{
			static void thunk(T* a_this)
			{
				func(a_this);
				if (Scaling()) {
					ScaleOwned(a_this->target.get().get());
				}
			}
			static inline REL::Relocation<decltype(thunk)> func;
			static void                                    Install()
			{
				REL::Relocation<std::uintptr_t> vtbl{ T::VTABLE[0] };
				func = vtbl.write_vfunc(0x3B, thunk);  // ReferenceEffect::UpdatePosition
			}
		};

		// a 5-byte call inside a game function, wrapped where it stands; left alone if the call is not there
		template <class Thunk>
		void WrapCall(REL::Relocation<std::uintptr_t> a_site, std::string_view a_what)
		{
			if (*reinterpret_cast<const std::uint8_t*>(a_site.address()) != 0xE8) {
				SKSE::log::warn("{}: the expected call is not there (another plugin rewrote it?); its flickering lights keep their made-with strength",
					a_what);
				return;
			}
			Thunk::func = SKSE::GetTrampoline().write_call<5>(a_site.address(), Thunk::thunk);
		}

		struct ExplosionUpdate
		{
			static bool thunk(RE::Explosion* a_this)
			{
				const bool result = func(a_this);
				ScaleUnder(a_this);
				return result;
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct HazardUpdate
		{
			static bool thunk(RE::Hazard* a_this)
			{
				const bool result = func(a_this);
				ScaleUnder(a_this);
				return result;
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct CellAnimations
		{
			static void thunk(RE::TESObjectCELL* a_cell)
			{
				func(a_cell);
				ScaleCell(a_cell);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		void InstallHooks()
		{
			PlayerUpdate::Install();
			CasterUpdate::Install();
			EffectUpdate<RE::ShaderReferenceEffect>::Install();
			EffectUpdate<RE::ModelReferenceEffect>::Install();
			if (REL::Module::IsVR()) {
				return;
			}
			SKSE::AllocTrampoline(3 * 14);
			WrapCall<ExplosionUpdate>(REL::Relocation<std::uintptr_t>{ RELOCATION_ID(42664, 43836), 0x11 }, "explosion update");
			WrapCall<HazardUpdate>(REL::Relocation<std::uintptr_t>{ RELOCATION_ID(42791, 43959), REL::VariantOffset(0x11, 0x1F, 0x11) }, "hazard update");
			WrapCall<CellAnimations>(REL::Relocation<std::uintptr_t>{ RELOCATION_ID(18458, 18889), 0x52 }, "cell animations");
		}
	}

	void MakeLightCopies()
	{
		std::size_t made = 0, flickers = 0, missing = 0, failed = 0, plained = 0;
		for (auto& c : LightCopies()) {
			if (c.form) {
				continue;  // one of pass 0's own magic lights, made already (LightSettings.cpp)
			}
			auto* base = RE::TESForm::LookupByEditorID<RE::TESObjectLIGH>(c.base);
			if (!base) {
				++missing;
				SKSE::log::warn("[LIGHTCOPY-MISSING] {} | its light {} is not in this load order; the lights that name it stay dark", c.id, c.base);
				continue;
			}
			auto* copy = CopyLight(base);
			if (!copy || !RegisterEditorID(copy, c.id) || RE::TESForm::LookupByEditorID<RE::TESObjectLIGH>(c.id) != copy) {
				++failed;
				SKSE::log::warn("[LIGHTCOPY-FAILED] {} | could not make a findable copy of {}", c.id, c.base);
				continue;
			}
			c.form = copy;
			c.startFade = c.fade > 0.0f ? c.fade : base->fade;
			c.startRadius = c.radius > 0 ? static_cast<std::uint32_t>(c.radius) : base->data.radius;
			// the config's own cutoff, or the record's when the record itself is an inverse-square light
			c.startCutoff = c.cutoff > 0.0f ? c.cutoff : (base->data.flags.underlying() & kInverseSquare) ? base->data.fallofExponent : 0.0f;
			if (!InverseSquare() && c.startCutoff > 0.0f) {
				// ENB and Vanilla (Lighting.cpp): the reach it has under Community Shaders, drawn plain; Reach moves the radius
				const float size = base->data.fov >= 50.0f ? 1.414f : std::clamp(base->data.fov, 0.01f, 50.0f);  // Light Placer's GetSize
				const auto  plain = Plain(c.startFade, static_cast<float>(c.startRadius), IslReach(c.startFade, c.startCutoff, size));
				c.startFade = plain.fade;
				c.startRadius = static_cast<std::uint32_t>(std::lround(plain.radius));
				c.startCutoff = 0.0f;
				copy->data.flags = static_cast<RE::TES_LIGHT_FLAGS>(copy->data.flags.underlying() & ~kInverseSquare);
				++plained;
			}
			gLit[c.id] = { c.flicker, 0.0f };
			flickers += c.flicker;
			++made;
		}
		SKSE::log::info("light copies: {} made ({} flicker, {} drawn plain for {}), {} whose light is not in this load order, {} failed", made,
			flickers, plained, LightingName(LightingPick()), missing, failed);
		InstallHooks();
		ApplyLightStrength(true);
	}

	void RequestRefresh()
	{
		gRefresh = true;
	}

	void ApplyLightStrength(bool a_log)
	{
		const int   brightness = SettingValue(kBrightness, 100);
		const int   reach = SettingValue(kReach, 100);
		const float scale = static_cast<float>(brightness) / 100.0f;
		const float stretch = static_cast<float>(reach) / 100.0f;
		gBrightness = scale;
		std::size_t set = 0;
		for (auto& c : LightCopies()) {
			if (!c.form) {
				continue;
			}
			c.form->fade = c.flicker ? c.startFade : c.startFade * scale;  // a flicker's Brightness is applied as it is written
			c.form->data.radius = static_cast<std::uint32_t>(std::lround(static_cast<double>(c.startRadius) * reach / 100.0));
			if (c.startCutoff > 0.0f) {
				c.form->data.fallofExponent = c.startCutoff * scale / (stretch * stretch);
				if (const auto it = gLit.find(c.id); it != gLit.end()) {
					it->second.cutoff = std::clamp(c.form->data.fallofExponent, kLowestCutoff, kHighestCutoff);
				}
			}
			++set;
		}
		SKSE::log::info("light strength: brightness {}%, reach {}%, on {} light copies", brightness, reach, set);
		if (a_log) {
			for (const auto& c : LightCopies()) {
				if (c.form) {
					SKSE::log::info("[LIGHTCOPY] {} | {}{} | fade {} radius {} cutoff {}", c.id, c.base, c.flicker ? " | flicker" : "", c.form->fade,
						c.form->data.radius, c.startCutoff > 0.0f ? c.form->data.fallofExponent : 0.0f);
				}
			}
		}
	}
}
