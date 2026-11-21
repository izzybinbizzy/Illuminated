// BurningBanners.cpp - Burn'em Banners (GhdOne, Nexus 118694) makes Skyrim's banners burn. It lights a burning banner itself with
// Community Shaders and Light Placer (a row on the model's ENBLight01 node) and on ENB (an ENB light on that node); on Vanilla it gives
// none, so there Illuminated hangs a fire light on that node while the banner burns. The game says when a banner starts and stops
// burning (its destruction stage) and when it leaves the loaded world, so nothing here looks round each frame. The light is the
// burning-ground fire's: its hazard light's color (Skyrim.esm 0x3E075), fade 1.0 and reach 500 (gen.py's GROUND_FIRE values), drawn
// plain, under Brightness, Reach, Ground effects and [Ground Effects] fire.

#include "GroundElement.h"
#include "Plugin.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace RE
{
	// the game's event (ScriptEventSourceHolder +0x3C8), which our CommonLib only declares; its fields as two public SKSE plugins
	// read them (ObjectImpactFramework, Apache-2.0; DbSkseFunctions)
	struct TESDestructionStageChangedEvent
	{
		NiPointer<TESObjectREFR> target;
		std::uint32_t            oldStage{ 0 };
		std::uint32_t            newStage{ 0 };
	};
	static_assert(sizeof(TESDestructionStageChangedEvent) == 0x10);
}

namespace Plugin
{
	namespace
	{
		constexpr const char*      kName = "IlluminatedBannerFire";
		constexpr std::string_view kPlugin = "BurnEmBanners.esp";
		constexpr std::string_view kModels = "burnembanners\\";  // every model of its banners lives under this folder
		constexpr RE::FormID       kFireHazard = 0x3E075;        // Skyrim.esm's burning-ground fire, whose light gives the color
		constexpr float            kFade = 1.0f, kReach = 500.0f;
		constexpr std::size_t      kMost = 16;  // banners burning at once that get a light; the game lights few at once off Community Shaders

		struct Lit
		{
			RE::NiPointer<RE::NiPointLight> light;
			RE::NiPointer<RE::BSLight>      bs;
		};

		std::atomic<bool>                        gOn{ false };  // decided once at data load
		RE::NiColor                              gColor{ 1.0f, 0.6f, 0.3f };
		std::mutex                               gNotedLock;
		std::vector<std::pair<RE::FormID, bool>> gNoted;  // a banner and whether it burns now (gNotedLock)
		std::atomic<bool>                        gAnyNoted{ false };
		std::unordered_map<RE::FormID, Lit>      gLit;  // main thread

		[[nodiscard]] bool OurBanner(const RE::TESObjectREFR* a_ref)
		{
			const auto* base = a_ref ? a_ref->GetBaseObject() : nullptr;
			const auto* model = base ? base->As<RE::TESModel>() : nullptr;
			const char* path = model ? model->GetModel() : nullptr;
			if (!path) {
				return false;
			}
			std::string_view p(path);
			if (p.size() >= 7 && _strnicmp(p.data(), "meshes\\", 7) == 0) {
				p.remove_prefix(7);
			}
			return p.size() > kModels.size() && _strnicmp(p.data(), kModels.data(), kModels.size()) == 0;
		}

		// a sink only notes (it may run off the main thread); the player update acts
		void Note(RE::FormID a_id, bool a_burning)
		{
			const std::lock_guard l(gNotedLock);
			gNoted.emplace_back(a_id, a_burning);
			gAnyNoted.store(true, std::memory_order_relaxed);
		}

		class Sinks final :
			public RE::BSTEventSink<RE::TESDestructionStageChangedEvent>,
			public RE::BSTEventSink<RE::TESCellAttachDetachEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const RE::TESDestructionStageChangedEvent* a_event, RE::BSTEventSource<RE::TESDestructionStageChangedEvent>*) override
			{
				LightKit::Guarded("banner burning", [a_event]() {
					if (a_event && a_event->target && OurBanner(a_event->target.get())) {
						Note(a_event->target->GetFormID(), a_event->newStage >= 1);
					}
				});
				return RE::BSEventNotifyControl::kContinue;
			}
			RE::BSEventNotifyControl ProcessEvent(const RE::TESCellAttachDetachEvent* a_event, RE::BSTEventSource<RE::TESCellAttachDetachEvent>*) override
			{
				// a banner leaving the loaded world takes its light with it; one coming back burns again only at its next stage
				LightKit::Guarded("banner detach", [a_event]() {
					if (a_event && !a_event->attached && a_event->reference && OurBanner(a_event->reference.get())) {
						Note(a_event->reference->GetFormID(), false);
					}
				});
				return RE::BSEventNotifyControl::kContinue;
			}
		};

		// this light's numbers under the sliders now (main thread)
		void Dress(RE::NiPointLight* a_light)
		{
			const float strength = LightsOff() ? 0.0f : BrightnessNow() * KindStrength(LightKind::kGround) * GroundElement::StrengthOf(GroundElement::Element::kFire);
			const float reach = kReach * static_cast<float>(SettingValue("IlluminatedReach", 100)) / 100.0f * GroundElement::ReachOf(GroundElement::Element::kFire);
			const auto  plain = LightKit::PlainAt(kFade, 0.0f, reach);
			auto&       data = a_light->GetLightRuntimeData();
			a_light->SetLightAttenuation(plain.radius);
			data.diffuse = gColor;
			data.ambient = LightKit::Tenth(gColor);
			data.fade = plain.fade * strength;
		}

		void PutOut(RE::FormID a_id)
		{
			if (const auto it = gLit.find(a_id); it != gLit.end()) {
				auto* scene = LightKit::Scene();
				LightKit::TakeOff(scene, it->second.bs);
				if (auto* parent = it->second.light ? it->second.light->parent : nullptr) {
					parent->DetachChild(it->second.light.get());
				}
				gLit.erase(it);
			}
		}

		void Light(RE::FormID a_id)
		{
			if (gLit.size() >= kMost) {
				return;
			}
			auto* ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(a_id);
			auto* root = ref && ref->Is3DLoaded() ? ref->Get3D() : nullptr;
			if (!root) {
				return;
			}
			// its own light's node, where Burn'em Banners hangs its light on the other lightings; else the banner's root
			auto* at = root->GetObjectByName("ENBLight01");
			auto* node = at && at->AsNode() ? at->AsNode() : (at && at->parent ? at->parent : root->AsNode());
			auto* light = node ? LightKit::CloneLight() : nullptr;
			if (!light) {
				return;
			}
			light->name = kName;
			light->local.translate = node == (at ? at->parent : nullptr) ? at->local.translate : RE::NiPoint3{};
			light->local.scale = 1.0f;
			Dress(light);
			if (auto* bs = LightKit::Hang(node, light, LightKit::Scene(), 1.0f, false)) {
				gLit.emplace(a_id, Lit{ RE::NiPointer<RE::NiPointLight>(light), RE::NiPointer<RE::BSLight>(bs) });
			}
		}
	}

	void WatchBurningBanners()
	{
		auto* dh = RE::TESDataHandler::GetSingleton();
		if (!dh || !dh->LookupModByName(kPlugin)) {
			return;
		}
		// its own light runs with Light Placer and on ENB: there nothing of ours is added, so no banner is lit twice
		if (!RecordRoute() || LightingFound() != Lighting::kVanilla) {
			SKSE::log::info("Burn'em Banners: its own light runs on this setup, so Illuminated adds none to burning banners");
			return;
		}
		if (const auto* hazard = dh->LookupForm<RE::BGSHazard>(kFireHazard, "Skyrim.esm"); hazard && hazard->data.light) {
			const auto& c = hazard->data.light->data.color;
			gColor = { c.red / 255.0f, c.green / 255.0f, c.blue / 255.0f };
		}
		static Sinks sinks;
		if (auto* events = RE::ScriptEventSourceHolder::GetSingleton()) {
			events->AddEventSink<RE::TESDestructionStageChangedEvent>(&sinks);
			events->AddEventSink<RE::TESCellAttachDetachEvent>(&sinks);
			gOn.store(true, std::memory_order_relaxed);
			SKSE::log::info("Burn'em Banners: a burning banner gets a fire light on Vanilla");
		}
	}

	void TickBurningBanners()
	{
		if (!gAnyNoted.exchange(false, std::memory_order_relaxed)) {
			return;
		}
		std::vector<std::pair<RE::FormID, bool>> noted;
		{
			const std::lock_guard l(gNotedLock);
			noted.swap(gNoted);
		}
		for (const auto& [id, burning] : noted) {
			// each new stage may swap the banner's model: its light is hung again on the 3D it has now
			PutOut(id);
			if (burning) {
				Light(id);
			}
		}
	}

	void RetuneBurningBanners()
	{
		for (auto& [id, lit] : gLit) {
			if (lit.light) {
				Dress(lit.light.get());
			}
		}
	}

	void DropBurningBanners()
	{
		while (!gLit.empty()) {
			PutOut(gLit.begin()->first);
		}
		const std::lock_guard l(gNotedLock);
		gNoted.clear();
	}
}
