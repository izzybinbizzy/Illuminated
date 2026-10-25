// Illuminated - SKSE plugin
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE.txt and the notice at the top of main.cpp.
//
// The settings: what the player picks in the menu instead of in an installer.
//
// Every setting is a game global. Light Placer reads it in each light's conditions ("GetGlobalValue"), so a
// change shows in game within a second with no restart. The globals are made here, in memory, before Light Placer
// reads its configs. The player's picks live in Data\SKSE\Plugins\Illuminated\Illuminated.ini, beside the list of
// settings (*.txt, written by lagen.py).

#include "Fade.h"
#include "Plugin.h"

namespace Plugin
{
	namespace
	{
		std::vector<Setting>            gSettings;
		Fade::StringMap<std::size_t>    gIndex;  // lower-case id -> index
		std::vector<MenuNote>           gNotes;
		std::vector<SprayMarker>        gMarkers;
		std::vector<LightCopy>          gLights;
		std::unordered_set<std::string> gLightIds;   // lower-case ids of gLights, for the duplicate check
		std::vector<char>               gAvailable;  // per setting: are the mods it needs loaded (worked out once, main thread)
		// THREADS: the lists above are filled once, by LoadSettings at data load on the main thread, before the menu is
		// registered or DevBench is answered; after that only each setting's value changes (a relaxed atomic), so readers
		// take no lock. The lock keeps two writers (the menu, DevBench on the main thread) from writing the file at once.
		std::recursive_mutex gSettingsLock;

		// the route global (his answer 2026-10-13, Q7 - RecordLights.cpp DecideRecordRoute): every Light Placer row waits on it
		// first (lagen.py route_switch). Not a setting: no settings file names it, the menu never shows it, nothing saves it
		constexpr const char* kRouteGlobal = "IlluminatedLightPlacerRoute";
		RE::TESGlobal*        gRoute = nullptr;
		float                 gRouteValue = 0.0f;
		// his Performance switch: "a toggle to turn off lights completely for the entire mod" - Light Placer then
		// draws none of ours (the route global held at 0) and every pass treats every config as unlit (Coverage::ModelLit)
		constexpr std::string_view kLightsOff = "IlluminatedLightsOff";
		constexpr std::string_view kQuality = "IlluminatedQuality";  // 0 Quality, 1 Performance (his order 2026-10-20)
		constexpr std::string_view kShadowLights = "IlluminatedShadowLights";

		[[nodiscard]] float RouteNow() { return LightsOff() ? 0.0f : gRouteValue; }

		constexpr std::string_view kSettingsSection = "Settings";
		constexpr std::string_view kDetectedSection = "Detected";
		constexpr std::string_view kDetectedKey = "sPlugins";

		fs::path SettingsFolder() { return OurFolder(); }
		fs::path IniPath() { return SettingsFolder() / (std::string(kOurFolder) + ".ini"); }

		std::string Unescape(std::string_view a_text)
		{
			std::string out;
			for (std::size_t i = 0; i < a_text.size(); ++i) {
				if (a_text[i] == '\\' && i + 1 < a_text.size()) {
					++i;
					out.push_back(a_text[i] == 'n' ? '\n' : a_text[i]);
				} else {
					out.push_back(a_text[i]);
				}
			}
			return out;
		}

		void ReadSettingsFile(const fs::path& a_file)
		{
			std::ifstream in(a_file, std::ios::binary);
			if (!in) {
				return;
			}
			std::map<std::string, std::string> block;
			std::string                        kind;
			const auto                         flush = [&]() {
				if (kind == "setting") {
					Setting s;
					s.id = block["id"];
					s.ini = block["ini"];
					s.page = block["page"];
					s.group = block["group"];
					s.label = block["label"];
					s.isChoice = block["kind"] == "choice";
					s.isSlider = block["kind"] == "slider";
					s.choices = Split(block["choices"], '|');
					ParseInt(block["default"], s.defaultValue);
					if (s.isSlider) {
						ParseInt(block["min"], s.minValue);
						ParseInt(block["max"], s.maxValue);
						ParseInt(block["step"], s.stepValue);
					}
					s.restart = block["restart"] == "1";
					s.needs = Split(block["requires"], '|');
					const auto& autoText = block["auto"];
					if (const auto colon = autoText.find(':'); colon != std::string::npos) {
						s.autoAll = autoText.substr(0, colon) == "and";
						s.autoPlugins = Split(std::string_view(autoText).substr(colon + 1), '|');
					}
					for (std::size_t i = 0; i < (std::max)(s.choices.size(), std::size_t{ 1 }); ++i) {
						s.tips.push_back(Unescape(block["tip" + std::to_string(i)]));
					}
					const bool shapeOk = s.isChoice ? s.choices.size() >= 2 :
					                     s.isSlider ? s.stepValue > 0 && s.minValue < s.maxValue && (s.maxValue - s.minValue) % s.stepValue == 0 :
					                                  true;
					if (!s.id.empty() && !gIndex.contains(Lower(s.id)) && shapeOk) {
						s.value = s.defaultValue;
						gIndex[Lower(s.id)] = gSettings.size();
						gSettings.push_back(std::move(s));
					}
				} else if (kind == "light") {
					LightCopy c;
					c.id = block["id"];
					c.base = block["base"];
					if (!ParseFloat(block["fade"], c.fade) || !std::isfinite(c.fade)) {
						c.fade = 0.0f;  // not given, or not a finite number
					}
					ParseInt(block["radius"], c.radius);
					if (!ParseFloat(block["cutoff"], c.cutoff) || !std::isfinite(c.cutoff)) {
						c.cutoff = 0.0f;
					}
					c.flicker = block["flicker"] == "1";
					if (!c.id.empty() && !c.base.empty() && gLightIds.insert(Lower(c.id)).second) {
						gLights.push_back(std::move(c));
					}
				} else if (kind == "note") {
					gNotes.push_back({ block["page"], block["group"], block["label"], Unescape(block["text"]) });
				} else if (kind == "marker") {
					SprayMarker m;
					m.name = Lower(block["name"]);
					m.when = ParseConditions(Split(block["when"], ';'));
					m.values = block["values"];
					gMarkers.push_back(std::move(m));
				}
				block.clear();
				kind.clear();
			};
			std::string line;
			while (std::getline(in, line)) {
				const auto t = Trim(line);
				if (t.empty() || t[0] == '#') {
					continue;
				}
				if (t.front() == '[' && t.back() == ']') {
					flush();
					kind = t.substr(1, t.size() - 2);
					continue;
				}
				const auto eq = t.find('=');
				if (eq != std::string::npos) {
					block[t.substr(0, eq)] = t.substr(eq + 1);
				}
			}
			flush();
		}

		bool Loaded(std::string_view a_plugin)
		{
			auto* dh = RE::TESDataHandler::GetSingleton();
			return dh && (dh->LookupLoadedModByName(a_plugin) || dh->LookupLoadedLightModByName(a_plugin));
		}

		RE::TESGlobal* MakeGlobal(const std::string& a_id)
		{
			auto* g = NewForm<RE::TESGlobal>();
			if (!g) {
				return nullptr;
			}
			g->value = 0.0f;
			return RegisterEditorID(g, a_id) && RE::TESForm::LookupByEditorID<RE::TESGlobal>(a_id) == g ? g : nullptr;
		}

		std::map<std::string, std::map<std::string, std::string>> ReadIni()
		{
			std::map<std::string, std::map<std::string, std::string>> out;
			std::ifstream                                             in(IniPath());
			std::string                                               line, section;
			while (std::getline(in, line)) {
				const auto t = Trim(line);
				if (t.empty() || t[0] == ';' || t[0] == '#') {
					continue;
				}
				if (t.front() == '[' && t.back() == ']') {
					section = Lower(t.substr(1, t.size() - 2));
					continue;
				}
				const auto eq = t.find('=');
				if (eq != std::string::npos) {
					out[section][Lower(Trim(t.substr(0, eq)))] = Trim(t.substr(eq + 1));
				}
			}
			return out;
		}

		// the saved picks onto the settings; a setting the INI does not name keeps its default
		void ApplyIni()
		{
			const auto            ini = ReadIni();
			const auto            sec = ini.find(Lower(kSettingsSection));
			std::set<std::string> detectedBefore;
			if (const auto d = ini.find(Lower(kDetectedSection)); d != ini.end()) {
				if (const auto k = d->second.find(Lower(kDetectedKey)); k != d->second.end()) {
					for (auto& p : Split(k->second, '|')) {
						detectedBefore.insert(Lower(p));
					}
				}
			}
			for (auto& s : gSettings) {
				int  v = s.value;
				bool fromIni = false;
				if (sec != ini.end()) {
					if (const auto it = sec->second.find(Lower(s.ini)); it != sec->second.end()) {
						int parsed = 0;
						if (ParseInt(it->second, parsed)) {
							v = parsed;
							fromIni = true;
						}
					}
				}
				if (!s.autoPlugins.empty()) {
					std::size_t have = 0;
					bool        newlySeen = false;
					for (auto& p : s.autoPlugins) {
						if (Loaded(p)) {
							++have;
							newlySeen |= !detectedBefore.contains(Lower(p));
						}
					}
					const bool met = s.autoAll ? have == s.autoPlugins.size() : have > 0;
					// the installer ticked a detected mod for you: so does this, unless the player already decided
					// while that mod was installed
					if (met && (!fromIni || newlySeen)) {
						v = 1;
					}
				}
				s.value = AllowedValue(s, v);
			}
		}
	}

	// ------------------------------------------------------------------ conditions
	bool ConditionsHold(const std::vector<std::vector<Clause>>& a_tests)
	{
		for (const auto& test : a_tests) {
			bool any = false;
			for (const auto& c : test) {
				const auto it = gIndex.find(c.global);
				// a setting no settings file names: treat it as 0, which is what the game reads from a missing global
				const int value = it == gIndex.end() ? 0 : gSettings[it->second].value.load();
				if (value == c.value) {
					any = true;
					break;
				}
			}
			if (!any) {
				return false;
			}
		}
		return true;
	}

	// ------------------------------------------------------------------ loading
	void LoadSettings()
	{
		std::lock_guard l{ gSettingsLock };
		gSettings.clear();
		gIndex.clear();
		gNotes.clear();
		gMarkers.clear();
		gLights.clear();
		gLightIds.clear();
		std::error_code       ec;
		std::vector<fs::path> files;
		if (fs::is_directory(SettingsFolder(), ec)) {
			// the error code form: stepping on through a folder that cannot be read ends the walk instead of throwing
			for (auto it = fs::directory_iterator(SettingsFolder(), ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
				std::error_code one;  // one unreadable entry is skipped, the walk goes on
				if (it->is_regular_file(one) && Lower(Fade::PathText(it->path().extension())) == ".txt") {
					files.push_back(it->path());
				}
			}
		}
		std::ranges::sort(files);  // Settings.txt (the main download) before Praedy's Staves Settings.txt
		// stable: the rest keep the order above (a plain sort left it unspecified, and the first file to name a setting wins)
		std::ranges::stable_sort(files, [](const fs::path& a, const fs::path& b) {
			return (Lower(Fade::PathText(a.filename())) != "settings.txt") < (Lower(Fade::PathText(b.filename())) != "settings.txt");
		});
		for (const auto& f : files) {
			ReadSettingsFile(f);
		}
		std::size_t made = 0;
		for (auto& s : gSettings) {
			s.global = MakeGlobal(s.id);
			if (s.global) {
				++made;
			} else {
				SKSE::log::warn("[SETTING-FAILED] {} | could not make its global; lights that read it stay at 0", s.id);
			}
		}
		// the route global (his answer Q7): made here with the settings' globals, before Light Placer reads its configs'
		// conditions; DecideRecordRoute sets its value once the lighting is known (0 until then)
		if (!gRoute) {
			gRoute = MakeGlobal(kRouteGlobal);
			if (!gRoute) {
				SKSE::log::warn("[SETTING-FAILED] {} | could not make the route global; Illuminated lights the game's own records instead",
					kRouteGlobal);
			}
		}
		// the load order is fixed after data load: the menu (render thread) reads this instead of the data handler
		gAvailable.assign(gSettings.size(), 1);
		for (std::size_t i = 0; i < gSettings.size(); ++i) {
			const auto& needs = gSettings[i].needs;
			gAvailable[i] = needs.empty() || std::ranges::any_of(needs, [](const std::string& p) { return Loaded(p); });
		}
		ApplyIni();
		ApplyGlobals();
		SaveSettings();  // so the file shows what the detected mods turned on
		SKSE::log::info("settings: {} read from {} file(s), {} notes, {} spray markers, {} light copies; {} globals made in memory",
			gSettings.size(), files.size(), gNotes.size(), gMarkers.size(), gLights.size(), made);
		for (const auto& s : gSettings) {
			const int v = s.value;
			SKSE::log::info("[SETTING] {} = {}{}", s.id, v,
				s.isChoice && v >= 0 && v < static_cast<int>(s.choices.size()) ? " (" + s.choices[static_cast<std::size_t>(v)] + ")" : s.isSlider ? "%" :
																																					"");
		}
	}

	void ApplyGlobals()
	{
		std::lock_guard l{ gSettingsLock };
		for (auto& s : gSettings) {
			if (s.global) {
				s.global->value = static_cast<float>(s.value);
			}
		}
		if (gRoute) {
			gRoute->value = RouteNow();  // a save holds the globals as they were when it was made: the route found now wins
		}
	}

	bool SetLightPlacerRoute(bool a_lightPlacerDraws)
	{
		std::lock_guard l{ gSettingsLock };
		// without the global Light Placer reads 0 and draws none of our lights: the caller then takes the record route
		gRouteValue = a_lightPlacerDraws && gRoute ? 1.0f : 0.0f;
		if (gRoute) {
			gRoute->value = RouteNow();
		}
		SKSE::log::info("[SETTING] {} = {} (Light Placer {} Illuminated's lights)", kRouteGlobal, gRouteValue > 0.5f ? 1 : 0,
			gRouteValue > 0.5f ? "draws" : "draws none of");
		return gRoute != nullptr;
	}

	void SaveSettings()
	{
		std::lock_guard l{ gSettingsLock };
		std::error_code ec;
		fs::create_directories(IniPath().parent_path(), ec);
		// keep every line of a section this plugin does not own
		const auto         old = ReadIni();
		std::ostringstream out;  // written beside the file and moved over it (below)
		out << "[" << kSettingsSection << "]\n";
		for (const auto& s : gSettings) {
			out << s.ini << "=" << s.value.load() << "\n";
		}
		// the detected mods this save saw, so a mod installed later is ticked for the player the way the installer would
		std::set<std::string> seen;
		std::string           list;
		// a mod seen once stays on the list, so removing and reinstalling it does not undo the player's choice
		if (const auto d = old.find(Lower(kDetectedSection)); d != old.end()) {
			if (const auto k = d->second.find(Lower(kDetectedKey)); k != d->second.end()) {
				for (const auto& p : Split(k->second, '|')) {
					if (seen.insert(Lower(p)).second) {
						list += (list.empty() ? "" : "|") + p;
					}
				}
			}
		}
		for (const auto& s : gSettings) {
			for (const auto& p : s.autoPlugins) {
				if (Loaded(p) && seen.insert(Lower(p)).second) {
					list += (list.empty() ? "" : "|") + p;
				}
			}
		}
		out << "\n[" << kDetectedSection << "]\n"
			<< kDetectedKey << "=" << list;
		out << "\n";
		for (const auto& [section, keys] : old) {
			if (section == Lower(kSettingsSection) || section == Lower(kDetectedSection) || section.empty()) {
				continue;
			}
			out << "\n[" << section << "]\n";
			for (const auto& [k, v] : keys) {
				out << k << "=" << v << "\n";
			}
		}
		// a crash or a full disk mid-write leaves the old file, never half of one
		const auto tmp = fs::path(IniPath()).concat(".tmp");
		{
			std::ofstream file(tmp, std::ios::trunc);
			file << out.str();
			if (!file.flush()) {
				SKSE::log::warn("settings: {} could not be written", Fade::PathText(IniPath()));
				fs::remove(tmp, ec);
				return;
			}
		}
		fs::rename(tmp, IniPath(), ec);
		if (ec) {
			SKSE::log::warn("settings: {} could not be replaced ({})", Fade::PathText(IniPath()), ec.message());
			fs::remove(tmp, ec);
		}
	}

	void SetSetting(std::size_t a_index, int a_value, bool a_save)
	{
		{
			std::lock_guard l{ gSettingsLock };
			if (a_index >= gSettings.size()) {
				return;
			}
			auto& s = gSettings[a_index];
			s.value = AllowedValue(s, a_value);  // a relaxed atomic: the menu shows it at once
			SKSE::log::info("[SETTING-CHANGED] {} = {}", s.id, s.value.load());
		}
		// The menu calls this on the RENDER thread: the game's globals, the file and the lit scene are the main thread's, so
		// they follow in ONE SKSE task - a slider dragged over many steps queues one task, not one per step (the re-score's
		// Illuminated issue 1)
		// a step of a slider still held is applied at once and written once it is let go (a_save false): the file is not rewritten
		// every frame of a drag
		static std::atomic<bool> queued{ false }, saveWanted{ false };
		if (a_save) {
			saveWanted = true;
		}
		if (queued.exchange(true)) {
			return;
		}
		const auto apply = [] {
			queued = false;
			ApplyGlobals();
			if (saveWanted.exchange(false)) {
				SaveSettings();
			}
			RequestRefresh();
		};
		if (auto* tasks = SKSE::GetTaskInterface()) {
			tasks->AddTask(apply);
		} else {
			// no way to reach the main thread: the globals are never written from the menu's thread; the new value
			// applies with the next refresh
			queued = false;
			SKSE::log::warn("[SETTING-CHANGED] no SKSE task interface - setting {} applies with the next refresh", a_index);
		}
	}

	int AllowedValue(const Setting& a_setting, int a_value)
	{
		if (a_setting.isSlider) {
			const int step = (std::max)(a_setting.stepValue, 1);
			const int v = std::clamp(a_value, a_setting.minValue, a_setting.maxValue);
			// a value between steps (an INI edited by hand) goes to the nearest step: the configs only name the steps
			const int k = (v - a_setting.minValue + step / 2) / step;
			return std::clamp(a_setting.minValue + k * step, a_setting.minValue, a_setting.maxValue);
		}
		const int top = a_setting.isChoice ? static_cast<int>(a_setting.choices.size()) - 1 : 1;
		return std::clamp(a_value, 0, top);
	}

	int SettingValue(std::string_view a_id, int a_fallback)
	{
		// called every frame: an id is lowered on the stack, no string made (every id is far shorter than this)
		std::array<char, 96> low{};
		if (a_id.size() > low.size()) {
			return a_fallback;
		}
		std::ranges::transform(a_id, low.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		const auto it = gIndex.find(std::string_view(low.data(), a_id.size()));
		return it == gIndex.end() ? a_fallback : gSettings[it->second].value.load();
	}

	bool LightsOff()
	{
		return SettingValue(kLightsOff, 0) != 0;
	}

	bool PerformanceMode()
	{
		return SettingValue(kQuality, 0) != 0;
	}

	bool ShadowLights()
	{
		return SettingValue(kShadowLights, 0) != 0 && !PerformanceMode() && !LightsOff();
	}

	bool RegisterEditorID(RE::TESForm* a_form, const std::string& a_id)
	{
		if (!a_form) {
			return false;
		}
		a_form->SetFormEditorID(a_id.c_str());
		const auto& [map, lock] = RE::TESForm::GetAllFormsByEditorID();
		RE::BSWriteLockGuard guard{ lock };
		if (!map) {
			return false;
		}
		map->insert({ RE::BSFixedString(a_id.c_str()), a_form });
		return true;
	}

	std::vector<LightCopy>&         LightCopies() { return gLights; }
	std::vector<Setting>&           Settings() { return gSettings; }
	const std::vector<MenuNote>&    Notes() { return gNotes; }
	const std::vector<SprayMarker>& SprayMarkers() { return gMarkers; }
	bool                            PluginLoaded(std::string_view a_plugin) { return Loaded(a_plugin); }

	bool PluginLoadedStartingWith(std::initializer_list<std::string_view> a_lowerPrefixes)
	{
		const auto* dh = RE::TESDataHandler::GetSingleton();
		const auto  match = [&](const RE::TESFile* a_file) {
			const auto name = a_file ? LightKit::Lower(a_file->GetFilename()) : std::string();
			return std::ranges::any_of(a_lowerPrefixes, [&name](std::string_view p) { return !name.empty() && name.starts_with(p); });
		};
		for (std::uint8_t i = 0; dh && dh->GetLoadedMods() && i < dh->GetLoadedModCount(); ++i) {
			if (match(dh->GetLoadedMods()[i])) {
				return true;
			}
		}
		for (std::uint16_t i = 0; dh && dh->GetLoadedLightMods() && i < dh->GetLoadedLightModCount(); ++i) {
			if (match(dh->GetLoadedLightMods()[i])) {
				return true;
			}
		}
		return false;
	}

	bool SettingAvailable(const Setting& a_setting)
	{
		if (a_setting.needs.empty()) {
			return true;
		}
		if (!gSettings.empty() && &a_setting >= gSettings.data() && &a_setting < gSettings.data() + gSettings.size()) {
			const auto i = static_cast<std::size_t>(&a_setting - gSettings.data());
			if (i < gAvailable.size()) {
				return gAvailable[i] != 0;
			}
		}
		return std::any_of(a_setting.needs.begin(), a_setting.needs.end(), [](const std::string& p) { return Loaded(p); });
	}
}
