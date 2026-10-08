// Illuminated - SKSE plugin
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE.txt and the notice at the top of main.cpp.
//
// The Illuminated pages in SKSE Menu Framework's Mod Control Panel: one page per settings page, a heading per
// group, a checkbox or a pick-one list per setting. A change is saved and shows in game within a second. The look is
// the shared MenuStyle.h in candle gold.

#ifndef WIN32_LEAN_AND_MEAN
#	define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#	define NOMINMAX  // the build defines it too (lagen.XMAKE_EDITS)
#endif
#include "Plugin.h"

#include "SKSEMenuFramework.h"
#include "Translation.h"
#include "MenuStyle.h"

namespace Plugin
{
	namespace
	{
		constexpr std::size_t    kMaxPages = 24;
		std::vector<std::string> gPages;
		std::string              gCSLight;  // the CS Light plugin that is loaded, if one is

		const ImGuiMCP::ImVec4 kWarn{ 1.0f, 0.72f, 0.3f, 1.0f };
		// every shown line goes through T(): Translation.json beside Settings.txt (lagen.py writes the English one)
		using Translation::T;

		void DrawSetting(std::size_t a_index, Setting& a_s)
		{
			ImGuiMCP::PushID(static_cast<int>(a_index));
			if (!SettingAvailable(a_s)) {
				ImGuiMCP::TextDisabled(T("%s - not installed"), T(a_s.label.c_str()));
				ImGuiMCP::PopID();
				return;
			}
			if (a_s.isSlider) {
				// the slider moves freely while it is held; the value is saved, on its step, when it is let go
				static std::unordered_map<std::size_t, int> held;
				int                                         v = held.contains(a_index) ? held[a_index] : a_s.value;
				ImGuiMCP::SliderInt(T(a_s.label.c_str()), &v, a_s.minValue, a_s.maxValue, "%d%%");
				v = AllowedValue(a_s, v);
				if (ImGuiMCP::IsItemActive()) {
					held[a_index] = v;
				} else {
					held.erase(a_index);
				}
				if (ImGuiMCP::IsItemDeactivated() || (!ImGuiMCP::IsItemActive() && v != a_s.value)) {
					held.erase(a_index);
					if (v != a_s.value) {
						SetSetting(a_index, v);
					}
				}
				if (!a_s.tips.empty() && !a_s.tips[0].empty()) {
					ImGuiMCP::SetItemTooltip("%s", T(a_s.tips[0].c_str()));
				}
			} else if (a_s.isChoice) {
				std::vector<const char*> items;
				for (const auto& c : a_s.choices) {
					items.push_back(T(c.c_str()));
				}
				int v = a_s.value;
				if (ImGuiMCP::Combo(T(a_s.label.c_str()), &v, items.data(), static_cast<int>(items.size()))) {
					SetSetting(a_index, v);
				}
				const auto shown = static_cast<std::size_t>(std::clamp(a_s.value, 0, static_cast<int>(a_s.tips.size()) - 1));
				if (!a_s.tips.empty() && !a_s.tips[shown].empty()) {
					ImGuiMCP::SetItemTooltip("%s", T(a_s.tips[shown].c_str()));
				}
			} else {
				bool on = a_s.value != 0;
				if (ImGuiMCP::Checkbox(T(a_s.label.c_str()), &on)) {
					SetSetting(a_index, on ? 1 : 0);
				}
				if (!a_s.tips.empty() && !a_s.tips[0].empty()) {
					ImGuiMCP::SetItemTooltip("%s", T(a_s.tips[0].c_str()));
				}
			}
			if (a_s.restart) {
				ImGuiMCP::SameLine();
				ImGuiMCP::TextDisabled("%s", T("(takes effect the next time the game starts)"));
			}
			ImGuiMCP::PopID();
		}

		void DrawPage(std::size_t a_page)
		{
			if (a_page >= gPages.size()) {
				return;
			}
			const MenuStyle::Page style;
			const auto&           page = gPages[a_page];
			if (a_page == 0) {
				ImGuiMCP::TextColored(MenuStyle::kMuted, T("Lighting: %s"), T(LightingName(LightingPick())));
				if (!InverseSquare()) {
					ImGuiMCP::TextWrapped("%s", T("Lights are drawn by the game's own lighting: each reaches as far as it does with "
												  "Community Shaders' inverse square lighting. Reach and Brightness still apply."));
				}
				if (RecordRoute()) {
					ImGuiMCP::TextWrapped("%s", T("Light Placer is not loaded: Illuminated gives each spell's own hand, bolt, explosion and "
												  "hazard light its color and reach instead. Brightness and Reach apply from the next cast."));
				}
				ImGuiMCP::Separator();
			}
			if (a_page == 0 && !gCSLight.empty()) {
				ImGuiMCP::TextColored(kWarn, T("%s is loaded."), gCSLight.c_str());
				ImGuiMCP::TextWrapped("%s", T("Illuminated does not need CS Light. If you keep CS Light for its world lights, untick its Magic FX, "
											  "Mysticsm, Bound Weapons, Praedy Staves, Regular soulgems, Spiders, Misc Effects and Dwarven "
											  "Spiders options in its own installer, or those lights glow twice."));
				ImGuiMCP::Separator();
			}
			if (a_page == 0) {
				MenuStyle::Note(T("Changes show in game within a second."));
			}
			std::string group;
			auto&       settings = Settings();
			for (const auto& n : Notes()) {
				if (n.page == page) {
					ImGuiMCP::TextColored(kWarn, "%s", T(n.label.c_str()));
					ImGuiMCP::TextWrapped("%s", T(n.text.c_str()));
				}
			}
			for (std::size_t i = 0; i < settings.size(); ++i) {
				auto& s = settings[i];
				if (s.page != page) {
					continue;
				}
				if (s.group != group) {
					group = s.group;
					MenuStyle::Header(MenuStyle::Icon::kBulb, T(group.c_str()));
				}
				DrawSetting(i, s);
			}
		}

		template <std::size_t I>
		void __stdcall Page()
		{
			DrawPage(I);
		}

		constexpr SKSEMenuFramework::Model::RenderFunction kPageFunctions[kMaxPages] = {
			Page<0>, Page<1>, Page<2>, Page<3>, Page<4>, Page<5>, Page<6>, Page<7>, Page<8>, Page<9>, Page<10>, Page<11>,
			Page<12>, Page<13>, Page<14>, Page<15>, Page<16>, Page<17>, Page<18>, Page<19>, Page<20>, Page<21>, Page<22>, Page<23>
		};
	}

	void RegisterMenu()
	{
		SKSE::log::info("{}", Translation::Load(fs::current_path() / "Data" / "SKSE" / "Plugins" / std::string(kOurFolder) / "Translation.json"));
		if (!SKSEMenuFramework::IsInstalled()) {
			SKSE::log::warn("SKSE Menu Framework is not installed, so there is no settings page - the settings INI still applies");
			return;
		}
		for (const auto name : { "CS Light.esp", "CS Light.esl" }) {
			if (PluginLoaded(name)) {
				gCSLight = name;
			}
		}
		gPages.clear();
		std::unordered_set<std::string> seen;  // the pages in the order they are first named, each once
		for (const auto& s : Settings()) {
			if (seen.insert(s.page).second) {
				gPages.push_back(s.page);
			}
		}
		if (gPages.size() > kMaxPages) {
			SKSE::log::warn("settings name {} pages; the menu shows the first {}", gPages.size(), kMaxPages);
			gPages.resize(kMaxPages);
		}
		MenuStyle::gTheme = MenuStyle::MakeTheme(0xFFC94D);  // candle gold
		SKSEMenuFramework::SetSection(T(std::string(kOurFolder).c_str()));
		for (std::size_t i = 0; i < gPages.size(); ++i) {
			SKSEMenuFramework::AddSectionItem(T(gPages[i].c_str()), kPageFunctions[i]);
		}
		SKSE::log::info("menu: {} page(s) added to SKSE Menu Framework {}", gPages.size(), SKSEMenuFramework::GetMenuFrameworkVersion());
	}
}
