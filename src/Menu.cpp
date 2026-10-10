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
#include "Fade.h"
#include "Plugin.h"

// SKSE Menu Framework's own header (theirs, MIT): its warnings are not ours, and ours are errors (xmake.lua)
#pragma warning(push, 0)
#include "SKSEMenuFramework.h"
#pragma warning(pop)
#include "Translation.h"
#include "MenuStyle.h"

namespace Plugin
{
	namespace
	{
		constexpr std::size_t    kMaxPages = 24;
		std::vector<std::string> gPages;
		// his word 2026-10-09: "put praedy staves last" - after the fading module's pages too (RegisterMenuTail)
		constexpr std::string_view kLastPage = "Praedy's Staves";
		std::string                gCSLight;  // the CS Light plugin that is loaded, if one is

		const ImGuiMCP::ImVec4 kWarn{ 1.0f, 0.72f, 0.3f, 1.0f };
		// every shown line goes through T(): Translation.json beside Settings.txt (lagen.py writes the English one)
		using Translation::T;

		// HIS MENU OF 2026-10-09 ~11:05: "Default to ENB Light Settings" sits under the Version heading and is "greyed out if enb
		// is not selected". 🔁 HIS WORD 2026-10-10: "found by itself needs to be changed to "Automatic"" and "users to not be able
		// to manually switch between versions" - the heading shows what was found (Lighting.cpp), there is no pick; an old
		// settings file's Lighting line is not drawn
		constexpr std::string_view kOldLightingId = "IlluminatedLighting";
		constexpr std::string_view kYieldENBId = "IlluminatedYieldENBLight";
		constexpr std::string_view kVersionGroup = "Version";  // lagen.py: the first heading on Lights

		bool EnbFound() { return LightingPick() == Lighting::kEnb; }

		void DrawVersion()
		{
			ImGuiMCP::Text("%s", T("Lighting"));
			ImGuiMCP::SameLine();
			ImGuiMCP::TextColored(ImGuiMCP::ImVec4{ 1.0f, 0.86f, 0.55f, 1.0f }, T("Automatic - %s"), T(LightingName(LightingPick())));
			ImGuiMCP::SetItemTooltip("%s", T("Illuminated finds your lighting by itself when the game starts: Community Shaders when its inverse "
											 "square lighting is installed, else ENB when an ENB is in the game folder, else Vanilla. Your presets "
											 "are kept apart for each one."));
		}

		// the hover text: the setting's own tip, and a restart-only setting says so there (no line of text under it - his
		// word 2026-10-09: "all of the subtext in lights menu is not needed")
		void SettingTip(const Setting& a_s, std::size_t a_tip)
		{
			const char* tip = a_tip < a_s.tips.size() && !a_s.tips[a_tip].empty() ? T(a_s.tips[a_tip].c_str()) : "";
			if (a_s.restart) {
				ImGuiMCP::SetItemTooltip("%s%s%s", tip, *tip ? "\n" : "", T("(takes effect the next time the game starts)"));
			} else if (*tip) {
				ImGuiMCP::SetItemTooltip("%s", tip);
			}
		}

		void DrawSetting(std::size_t a_index, Setting& a_s)
		{
			ImGuiMCP::PushID(static_cast<int>(a_index));
			if (!SettingAvailable(a_s)) {
				ImGuiMCP::TextDisabled(T("%s - not installed"), T(a_s.label.c_str()));
				ImGuiMCP::PopID();
				return;
			}
			const bool greyed = a_s.id == kYieldENBId && !EnbFound();
			ImGuiMCP::BeginDisabled(greyed);
			if (a_s.isSlider) {
				// the slider moves freely while it is held; the value is saved, on its step, when it is let go
				static std::unordered_map<std::size_t, int> held;
				int                                         v = held.contains(a_index) ? held[a_index] : a_s.value.load();
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
				SettingTip(a_s, 0);
			} else if (a_s.isChoice) {
				std::vector<const char*> items;
				for (const auto& c : a_s.choices) {
					items.push_back(T(c.c_str()));
				}
				int v = a_s.value.load();
				if (ImGuiMCP::Combo(T(a_s.label.c_str()), &v, items.data(), static_cast<int>(items.size()))) {
					SetSetting(a_index, v);
				}
				SettingTip(a_s, static_cast<std::size_t>(std::clamp(a_s.value.load(), 0, std::max(0, static_cast<int>(a_s.tips.size()) - 1))));
			} else {
				bool on = a_s.value != 0;
				if (ImGuiMCP::Checkbox(T(a_s.label.c_str()), &on)) {
					SetSetting(a_index, on ? 1 : 0);
				}
				SettingTip(a_s, 0);
			}
			ImGuiMCP::EndDisabled();
			ImGuiMCP::PopID();
		}

		// one setting by its id, the way the menu sets it; a setting that is not installed or not there is left alone
		void SetById(std::string_view a_id, int a_value)
		{
			auto& settings = Settings();
			for (std::size_t i = 0; i < settings.size(); ++i) {
				if (settings[i].id == a_id && SettingAvailable(settings[i])) {
					if (const int v = AllowedValue(settings[i], a_value); v != settings[i].value) {
						SetSetting(i, v);
					}
					return;
				}
			}
		}

		// ------------------------------------------------------------------ the player's own presets, one set per lighting
		// HIS WORD 2026-10-10: "any presets users make need to be version dependant so they can have presets per version if they
		// switch to say, enb to community shaders mid save." A preset is every setting's value, saved under a name in
		// Presets.ini beside the settings file, in a section of the lighting it was made on ([Vanilla|My preset]); the menu
		// shows only the presets of the lighting found now. The file is read and written on the menu's thread, under a lock.
		struct UserPreset
		{
			std::string                        name;
			std::map<std::string, std::string> values;  // a setting's ini key (lower case) -> its value
		};
		std::mutex        gPresetLock;
		std::atomic<bool> gPresetsDirty{ true };  // the file changed (or was never read): read it at the next draw

		fs::path PresetsPath() { return fs::current_path() / "Data" / "SKSE" / "Plugins" / std::string(kOurFolder) / "Presets.ini"; }

		// every preset in the file, by section "<lighting>|<name>", in file order
		std::vector<std::pair<std::string, UserPreset>> ReadPresets()
		{
			std::vector<std::pair<std::string, UserPreset>> out;
			std::ifstream                                   in(PresetsPath());
			std::string                                     line;
			while (std::getline(in, line)) {
				const auto t = Trim(line);
				if (t.empty() || t[0] == ';' || t[0] == '#') {
					continue;
				}
				if (t.front() == '[' && t.back() == ']') {
					const auto section = t.substr(1, t.size() - 2);
					const auto bar = section.find('|');
					if (bar != std::string::npos && bar + 1 < section.size()) {
						out.push_back({ section.substr(0, bar), UserPreset{ section.substr(bar + 1), {} } });
					}
					continue;
				}
				const auto eq = t.find('=');
				if (eq != std::string::npos && !out.empty()) {
					out.back().second.values[Lower(Trim(t.substr(0, eq)))] = Trim(t.substr(eq + 1));
				}
			}
			return out;
		}

		void WritePresets(const std::vector<std::pair<std::string, UserPreset>>& a_all)
		{
			std::ostringstream text;
			text << "; " << kOurFolder << " - the presets you saved in the menu, one set per lighting. Written by the menu.\n";
			for (const auto& [lighting, p] : a_all) {
				text << "\n[" << lighting << "|" << p.name << "]\n";
				for (const auto& [k, v] : p.values) {
					text << k << "=" << v << "\n";
				}
			}
			std::error_code ec;
			fs::create_directories(PresetsPath().parent_path(), ec);
			const auto tmp = fs::path(PresetsPath()).concat(".tmp");
			{
				std::ofstream out(tmp, std::ios::trunc);
				out << text.str();
				if (!out.flush()) {
					SKSE::log::warn("presets: {} could not be written", Fade::PathText(PresetsPath()));
					fs::remove(tmp, ec);
					return;
				}
			}
			fs::rename(tmp, PresetsPath(), ec);
			if (ec) {
				SKSE::log::warn("presets: {} could not be replaced ({})", Fade::PathText(PresetsPath()), ec.message());
				fs::remove(tmp, ec);
			}
		}

		void SavePreset(const std::string& a_name)
		{
			std::lock_guard   lock(gPresetLock);
			const std::string lighting = LightingName(LightingPick());
			UserPreset        p{ a_name, {} };
			for (const auto& s : Settings()) {
				p.values[Lower(s.ini)] = std::to_string(s.value.load());
			}
			auto all = ReadPresets();
			std::erase_if(all, [&](const auto& e) { return e.first == lighting && Lower(e.second.name) == Lower(a_name); });
			all.push_back({ lighting, std::move(p) });
			WritePresets(all);
			gPresetsDirty = true;
			SKSE::log::info("presets: '{}' saved for {}", a_name, lighting);
		}

		void LoadPreset(const UserPreset& a_preset)
		{
			auto& settings = Settings();
			for (std::size_t i = 0; i < settings.size(); ++i) {
				const auto it = a_preset.values.find(Lower(settings[i].ini));
				int        v = 0;
				if (it != a_preset.values.end() && ParseInt(it->second, v) && SettingAvailable(settings[i])) {
					if (const int allowed = AllowedValue(settings[i], v); allowed != settings[i].value) {
						SetSetting(i, allowed);
					}
				}
			}
			SKSE::log::info("presets: '{}' loaded", a_preset.name);
		}

		void DeletePreset(const std::string& a_name)
		{
			std::lock_guard   lock(gPresetLock);
			const std::string lighting = LightingName(LightingPick());
			auto              all = ReadPresets();
			std::erase_if(all, [&](const auto& e) { return e.first == lighting && e.second.name == a_name; });
			WritePresets(all);
			gPresetsDirty = true;
		}

		// HIS GO-TO PICK "presets" (2026-10-07 ~20:45; "do all of them" 2026-10-08): one click sets the sliders and the switches
		// that shape the look together. -1 leaves a setting as the player has it. 🔁 HIS WORD 2026-10-10: "remove performance
		// preset, only subtle default and cinematic(change dramatic to cinematic)".
		void DrawPresets()
		{
			struct Preset
			{
				const char* name;
				const char* tip;
				int         brightness, reach, flicker, hands, streams;
			};
			const Preset presets[] = {
				{ T("Subtle"), T("Softer lights that stay close to the spell."), 75, 80, 1, -1, -1 },
				{ T("Default"), T("The lights as the mod was made."), 100, 100, 1, 0, 1 },
				{ T("Cinematic"), T("Brighter lights that reach further."), 150, 120, 1, -1, -1 },
			};
			MenuStyle::Header(MenuStyle::Icon::kBulb, T("Presets"));
			for (std::size_t i = 0; i < std::size(presets); ++i) {
				const auto& p = presets[i];
				if (i) {
					ImGuiMCP::SameLine();
				}
				if (ImGuiMCP::Button(p.name)) {
					SetById("IlluminatedBrightness", p.brightness);
					SetById("IlluminatedReach", p.reach);
					SetById("IlluminatedDynamicLighting", p.flicker);
					if (p.hands >= 0) {
						SetById("IlluminatedHandLights", p.hands);
					}
					if (p.streams >= 0) {
						SetById("IlluminatedStreamLights", p.streams);
					}
				}
				ImGuiMCP::SetItemTooltip("%s", p.tip);
			}
			// the player's own presets, for the lighting found now
			static char name[48]{};
			ImGuiMCP::SetNextItemWidth(220.0f);
			ImGuiMCP::InputTextWithHint("##presetName", T("Name a preset"), name, sizeof(name));
			ImGuiMCP::SameLine();
			const std::string typed = Trim(name);
			ImGuiMCP::BeginDisabled(typed.empty() || typed.find_first_of("[]|=") != std::string::npos);
			if (ImGuiMCP::Button(T("Save as preset"))) {
				SavePreset(typed);
				name[0] = '\0';
			}
			ImGuiMCP::EndDisabled();
			ImGuiMCP::SetItemTooltip(T("Saves every setting as it is now under this name, for %s. Each lighting keeps its own presets."),
				T(LightingName(LightingPick())));
			std::vector<UserPreset> mine;
			{
				// the file is read again only after a save or a delete (not every frame the page is drawn)
				std::lock_guard                                        lock(gPresetLock);
				static std::vector<std::pair<std::string, UserPreset>> cache;
				if (gPresetsDirty.exchange(false)) {
					cache = ReadPresets();
				}
				const std::string lighting = LightingName(LightingPick());
				for (const auto& [l, p] : cache) {
					if (l == lighting) {
						mine.push_back(p);
					}
				}
			}
			for (std::size_t i = 0; i < mine.size(); ++i) {
				ImGuiMCP::PushID(static_cast<int>(1000 + i));
				if (ImGuiMCP::Button(mine[i].name.c_str())) {
					LoadPreset(mine[i]);
				}
				ImGuiMCP::SetItemTooltip("%s", T("Load this preset."));
				ImGuiMCP::SameLine();
				if (ImGuiMCP::SmallButton(T("Delete"))) {
					DeletePreset(mine[i].name);
				}
				ImGuiMCP::PopID();
			}
		}

		// HIS MENU OF 2026-10-09: "soul gems needs to have 1 submenu with praedy's in the dropdown along with vanilla and none".
		// The two settings the installers made stay (the configs' conditions read both); the menu shows them as ONE pick:
		// None = no soul gem lights, Vanilla = the vanilla gems, Praedy's = the vanilla gems + Praedy's (offered only with
		// Praedy's SoulGems.esp loaded, as its own switch was).
		constexpr std::string_view kSoulGems = "IlluminatedSoulGems";
		constexpr std::string_view kPraedysGems = "IlluminatedPraedys";

		void DrawSoulGems(std::size_t a_gems, const Setting& a_s)
		{
			auto&       settings = Settings();
			std::size_t praedy = settings.size();
			for (std::size_t i = 0; i < settings.size(); ++i) {
				if (settings[i].id == kPraedysGems) {
					praedy = i;
				}
			}
			const bool  withPraedy = praedy < settings.size() && SettingAvailable(settings[praedy]);
			const char* items[] = { T("None"), T("Vanilla"), T("Praedy's") };
			int         v = a_s.value == 0 ? 0 : (withPraedy && settings[praedy].value != 0) ? 2 :
			                                                                                   1;
			ImGuiMCP::PushID(static_cast<int>(a_gems));
			if (ImGuiMCP::Combo(T("Soul gems"), &v, items, withPraedy ? 3 : 2)) {
				SetSetting(a_gems, v == 0 ? 0 : 1);
				if (withPraedy) {
					SetSetting(praedy, v == 2 ? 1 : 0);
				}
			}
			ImGuiMCP::SetItemTooltip("%s", T("Which soul gems glow. Vanilla uses one mesh for a full gem and an empty one, so empty gems "
											 "stay dark. Praedy's: Praedy's Soul Gems, meshes and all (delete Soulgems.json from the "
											 "Subtle Soul Gems mod folder if you have it)."));
			ImGuiMCP::PopID();
		}

		void DrawPage(std::size_t a_page)
		{
			if (a_page >= gPages.size()) {
				return;
			}
			const MenuStyle::Page style;
			const auto&           page = gPages[a_page];
			// HIS MENU OF 2026-10-09 ~11:05: "start the menu with (lightbulb) Presets", and "all of the subtext in lights menu is
			// not needed" - the status lines that stood here are gone (the Lighting pick shows what is in use; devbench and the
			// log still say it); CS Light loaded stays one warning line, its how-to in the hover text
			// 🔁 HIS WORD 2026-10-09 ~11:40: the Version heading (the Lighting pick + "Default to ENB Light Settings") "goes before
			// presets as the first thing in the menu" - Presets is drawn when the first heading after Version starts
			bool       presetsDrawn = a_page != 0;
			const auto presets = [&] {
				if (presetsDrawn) {
					return;
				}
				presetsDrawn = true;
				DrawPresets();
				if (!gCSLight.empty()) {
					ImGuiMCP::TextColored(kWarn, T("%s is loaded."), gCSLight.c_str());
					ImGuiMCP::SetItemTooltip("%s", T("Illuminated does not need CS Light. If you keep CS Light for its world lights, untick its "
													 "Magic FX, Mysticsm, Bound Weapons, Praedy Staves, Regular soulgems, Spiders, Misc Effects "
													 "and Dwarven Spiders options in its own installer, or those lights glow twice."));
				}
			};
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
				if (s.id == kPraedysGems || s.id == kOldLightingId) {
					continue;  // drawn inside the one soul gem pick; the old Lighting pick is gone (Automatic)
				}
				if (s.group != group) {
					if (s.group != kVersionGroup) {
						presets();
					}
					group = s.group;
					MenuStyle::Header(MenuStyle::Icon::kBulb, T(group.c_str()));
					if (group == kVersionGroup) {
						DrawVersion();
					}
				}
				if (s.id == kSoulGems) {
					DrawSoulGems(i, s);
					continue;
				}
				DrawSetting(i, s);
			}
			presets();
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
		// the framework may keep the pointer: the text lives for the whole session (a temporary's c_str() would dangle)
		static const std::string kSection(kOurFolder);
		SKSEMenuFramework::SetSection(T(kSection.c_str()));
		for (std::size_t i = 0; i < gPages.size(); ++i) {
			if (gPages[i] != kLastPage) {
				SKSEMenuFramework::AddSectionItem(T(gPages[i].c_str()), kPageFunctions[i]);
			}
		}
		SKSE::log::info("menu: {} page(s) added to SKSE Menu Framework {}", gPages.size(), SKSEMenuFramework::GetMenuFrameworkVersion());
	}

	// after the fading module's pages: the last page goes at the bottom of the section
	void RegisterMenuTail()
	{
		if (!SKSEMenuFramework::IsInstalled()) {
			return;
		}
		static const std::string kSection(kOurFolder);
		for (std::size_t i = 0; i < gPages.size(); ++i) {
			if (gPages[i] == kLastPage) {
				SKSEMenuFramework::SetSection(T(kSection.c_str()));
				SKSEMenuFramework::AddSectionItem(T(gPages[i].c_str()), kPageFunctions[i]);
			}
		}
	}
}
