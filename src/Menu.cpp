// Luminous Arcana - SKSE plugin
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE.txt and the notice at the top of main.cpp.
//
// The Luminous Arcana pages in SKSE Menu Framework's Mod Control Panel: one page per settings page, a heading per
// group, a checkbox or a pick-one list per setting. A change is saved and shows in game within a second.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "Plugin.h"

#include "SKSEMenuFramework.h"

namespace Plugin
{
	namespace
	{
		constexpr std::size_t    kMaxPages = 24;
		std::vector<std::string> gPages;
		std::string              gCSLight;  // the CS Light plugin that is loaded, if one is

		const ImGuiMCP::ImVec4 kWarn{ 1.0f, 0.72f, 0.3f, 1.0f };

		void DrawSetting(std::size_t a_index, Setting& a_s)
		{
			ImGuiMCP::PushID(static_cast<int>(a_index));
			if (!SettingAvailable(a_s)) {
				ImGuiMCP::TextDisabled("%s - not installed", a_s.label.c_str());
				ImGuiMCP::PopID();
				return;
			}
			if (a_s.isChoice) {
				std::vector<const char*> items;
				for (const auto& c : a_s.choices) {
					items.push_back(c.c_str());
				}
				int v = a_s.value;
				if (ImGuiMCP::Combo(a_s.label.c_str(), &v, items.data(), static_cast<int>(items.size()))) {
					SetSetting(a_index, v);
				}
				const auto shown = static_cast<std::size_t>(std::clamp(a_s.value, 0, static_cast<int>(a_s.tips.size()) - 1));
				if (!a_s.tips.empty() && !a_s.tips[shown].empty()) {
					ImGuiMCP::SetItemTooltip("%s", a_s.tips[shown].c_str());
				}
			} else {
				bool on = a_s.value != 0;
				if (ImGuiMCP::Checkbox(a_s.label.c_str(), &on)) {
					SetSetting(a_index, on ? 1 : 0);
				}
				if (!a_s.tips.empty() && !a_s.tips[0].empty()) {
					ImGuiMCP::SetItemTooltip("%s", a_s.tips[0].c_str());
				}
			}
			if (a_s.restart) {
				ImGuiMCP::SameLine();
				ImGuiMCP::TextDisabled("(takes effect the next time the game starts)");
			}
			ImGuiMCP::PopID();
		}

		void DrawPage(std::size_t a_page)
		{
			if (a_page >= gPages.size()) {
				return;
			}
			const auto& page = gPages[a_page];
			if (a_page == 0 && !gCSLight.empty()) {
				ImGuiMCP::TextColored(kWarn, "%s is loaded.", gCSLight.c_str());
				ImGuiMCP::TextWrapped("%s", "Luminous Arcana does not need CS Light. If you keep CS Light for its world lights, untick its Magic FX, "
											"Mysticsm, Bound Weapons, Praedy Staves, Regular soulgems, Spiders, Misc Effects and Dwarven "
											"Spiders options in its own installer, or those lights glow twice.");
				ImGuiMCP::Separator();
			}
			if (a_page == 0) {
				ImGuiMCP::TextDisabled("Settings: %zu from Luminous Arcana.esp, %zu made in memory. Changes show in game within a second.",
					GlobalsFromPlugin(), GlobalsMadeInMemory());
			}
			std::string group;
			auto&       settings = Settings();
			for (const auto& n : Notes()) {
				if (n.page == page) {
					ImGuiMCP::TextColored(kWarn, "%s", n.label.c_str());
					ImGuiMCP::TextWrapped("%s", n.text.c_str());
				}
			}
			for (std::size_t i = 0; i < settings.size(); ++i) {
				auto& s = settings[i];
				if (s.page != page) {
					continue;
				}
				if (s.group != group) {
					group = s.group;
					ImGuiMCP::SeparatorText(group.c_str());
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
		if (!SKSEMenuFramework::IsInstalled()) {
			SKSE::log::warn("SKSE Menu Framework is not installed: the Luminous Arcana settings can only be changed in the MCM or the INI");
			return;
		}
		for (const auto name : { "CS Light.esp", "CS Light.esl" }) {
			if (PluginLoaded(name)) {
				gCSLight = name;
			}
		}
		gPages.clear();
		for (const auto& s : Settings()) {
			if (std::find(gPages.begin(), gPages.end(), s.page) == gPages.end()) {
				gPages.push_back(s.page);
			}
		}
		if (gPages.size() > kMaxPages) {
			SKSE::log::warn("settings name {} pages; the menu shows the first {}", gPages.size(), kMaxPages);
			gPages.resize(kMaxPages);
		}
		SKSEMenuFramework::SetSection(std::string(kOurFolder));
		for (std::size_t i = 0; i < gPages.size(); ++i) {
			SKSEMenuFramework::AddSectionItem(gPages[i], kPageFunctions[i]);
		}
		SKSE::log::info("menu: {} page(s) added to SKSE Menu Framework {}", gPages.size(), SKSEMenuFramework::GetMenuFrameworkVersion());
	}
}
