// Luminous Arcana - SKSE plugin
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE.txt and the notice at the top of main.cpp.
//
// The settings: what the player picks in the menu instead of in an installer.
//
// Every setting is a game global. Light Placer reads it in each light's conditions ("GetGlobalValue"), so a
// change shows in game within a second with no restart. The optional Luminous Arcana.esp holds the globals;
// without it this file makes them in memory before Light Placer reads its configs. The player's picks live in
// Data\MCM\Settings\Luminous Arcana.ini, the file MCM Helper keeps, so the SKSE Menu Framework page and the
// MCM always agree. The list of settings is Data\SKSE\Plugins\Luminous Arcana\*.txt, written by lagen.py.

#include "Plugin.h"

namespace Plugin
{
	namespace
	{
		std::vector<Setting>                         gSettings;
		std::unordered_map<std::string, std::size_t> gIndex;  // lower-case id -> index
		std::vector<MenuNote>                        gNotes;
		std::vector<SprayMarker>                     gMarkers;
		std::size_t                                  gMadeGlobals = 0;
		std::size_t                                  gFileGlobals = 0;
		std::recursive_mutex                         gSettingsLock;

		constexpr std::string_view kSettingsSection = "Settings";
		constexpr std::string_view kDetectedSection = "Detected";
		constexpr std::string_view kDetectedKey = "sPlugins";

		fs::path SettingsFolder() { return fs::current_path() / "Data" / "SKSE" / "Plugins" / std::string(kOurFolder); }
		fs::path IniPath() { return fs::current_path() / "Data" / "MCM" / "Settings" / (std::string(kOurFolder) + ".ini"); }

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

		std::vector<std::string> Split(std::string_view a_text, char a_sep)
		{
			std::vector<std::string> out;
			std::size_t              start = 0;
			while (start <= a_text.size()) {
				const auto end = a_text.find(a_sep, start);
				const auto piece = Trim(a_text.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start));
				if (!piece.empty()) {
					out.push_back(piece);
				}
				if (end == std::string_view::npos) {
					break;
				}
				start = end + 1;
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
                    s.choices = Split(block["choices"], '|');
                    ParseInt(block["default"], s.defaultValue);
                    s.restart = block["restart"] == "1";
                    s.needs = Split(block["requires"], '|');
                    const auto& autoText = block["auto"];
                    if (const auto colon = autoText.find(':'); colon != std::string::npos) {
                        s.autoAll = autoText.substr(0, colon) == "and";
                        s.autoPlugins = Split(std::string_view(autoText).substr(colon + 1), '|');
                    }
                    for (std::size_t i = 0; i < s.choices.size(); ++i) {
                        s.tips.push_back(Unescape(block["tip" + std::to_string(i)]));
                    }
                    if (!s.id.empty() && !gIndex.contains(Lower(s.id)) && (s.isChoice ? s.choices.size() >= 2 : true)) {
                        s.value = s.defaultValue;
                        gIndex[Lower(s.id)] = gSettings.size();
                        gSettings.push_back(std::move(s));
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
			g->SetFormEditorID(a_id.c_str());
			g->value = 0.0f;
			const auto& [map, lock] = RE::TESForm::GetAllFormsByEditorID();
			{
				RE::BSWriteLockGuard guard{ lock };
				if (map) {
					map->insert({ RE::BSFixedString(a_id.c_str()), g });
				}
			}
			return RE::TESForm::LookupByEditorID<RE::TESGlobal>(a_id) == g ? g : nullptr;
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

		bool ApplyIni(bool a_firstLoad)
		{
			const auto ini = ReadIni();
			const auto sec = ini.find(Lower(kSettingsSection));
			std::set<std::string> detectedBefore;
			if (const auto d = ini.find(Lower(kDetectedSection)); d != ini.end()) {
				if (const auto k = d->second.find(Lower(kDetectedKey)); k != d->second.end()) {
					for (auto& p : Split(k->second, '|')) {
						detectedBefore.insert(Lower(p));
					}
				}
			}
			bool changed = false;
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
					} else if (!fromIni && a_firstLoad) {
						v = met ? 1 : s.defaultValue;
					}
				} else if (!fromIni && a_firstLoad) {
					v = s.defaultValue;
				}
				const int top = s.isChoice ? static_cast<int>(s.choices.size()) - 1 : 1;
				v = std::clamp(v, 0, top);
				changed |= v != s.value;
				s.value = v;
			}
			return changed;
		}
	}

	// ------------------------------------------------------------------ conditions
	std::vector<std::vector<Clause>> ParseConditions(const std::vector<std::string>& a_conditions)
	{
		// Only GetGlobalValue items are kept. A run of OR items is one test, as the game reads it; a test that holds
		// anything else (IsSneaking OR a setting) cannot be decided from the settings, so it is left out: it may be true.
		std::vector<std::vector<Clause>> out;
		std::vector<Clause>              run;
		bool                             runHasOther = false;
		for (const auto& raw : a_conditions) {
			const auto words = Split(raw, ' ');
			if (words.empty()) {
				continue;
			}
			const bool isOr = Lower(words.back()) == "or";
			const auto fn = std::find_if(words.begin(), words.end(), [](const std::string& w) { return Lower(w) == "getglobalvalue"; });
			if (fn != words.end() && std::distance(fn, words.end()) >= 5 && *(fn + 2) == "NONE" && *(fn + 3) == "==") {
				int v = 0;
				if (ParseInt(*(fn + 4), v)) {
					run.push_back({ Lower(*(fn + 1)), v });
				} else {
					runHasOther = true;
				}
			} else {
				runHasOther = true;
			}
			if (!isOr) {
				if (!runHasOther && !run.empty()) {
					out.push_back(run);
				}
				run.clear();
				runHasOther = false;
			}
		}
		if (!runHasOther && !run.empty()) {
			out.push_back(run);
		}
		return out;
	}

	bool ConditionsHold(const std::vector<std::vector<Clause>>& a_tests)
	{
		std::lock_guard l{ gSettingsLock };
		for (const auto& test : a_tests) {
			bool any = false;
			for (const auto& c : test) {
				const auto it = gIndex.find(c.global);
				// a setting no settings file names: treat it as 0, which is what the game reads from a missing global
				const int value = it == gIndex.end() ? 0 : gSettings[it->second].value;
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
		std::error_code ec;
		std::vector<fs::path> files;
		if (fs::is_directory(SettingsFolder(), ec)) {
			for (const auto& entry : fs::directory_iterator(SettingsFolder(), ec)) {
				if (entry.is_regular_file() && Lower(entry.path().extension().string()) == ".txt") {
					files.push_back(entry.path());
				}
			}
		}
		std::sort(files.begin(), files.end());  // Settings.txt (the main download) before Praedy's Staves Settings.txt
		std::sort(files.begin(), files.end(), [](const fs::path& a, const fs::path& b) {
			return (Lower(a.filename().string()) != "settings.txt") < (Lower(b.filename().string()) != "settings.txt");
		});
		for (const auto& f : files) {
			ReadSettingsFile(f);
		}
		gMadeGlobals = gFileGlobals = 0;
		for (auto& s : gSettings) {
			s.global = RE::TESForm::LookupByEditorID<RE::TESGlobal>(s.id);
			if (s.global) {
				++gFileGlobals;
				continue;
			}
			s.global = MakeGlobal(s.id);
			if (s.global) {
				++gMadeGlobals;
			} else {
				SKSE::log::warn("[SETTING-FAILED] {} | could not make its global; lights that read it stay at 0", s.id);
			}
		}
		ApplyIni(true);
		ApplyGlobals();
		SaveSettings();  // so the MCM shows what the detected mods turned on
		SKSE::log::info("settings: {} read from {} file(s), {} notes, {} spray markers; globals: {} from Luminous Arcana.esp, {} made in memory",
			gSettings.size(), files.size(), gNotes.size(), gMarkers.size(), gFileGlobals, gMadeGlobals);
		for (const auto& s : gSettings) {
			SKSE::log::info("[SETTING] {} = {}{}", s.id, s.value, s.isChoice && s.value < static_cast<int>(s.choices.size()) ? " (" + s.choices[s.value] + ")" : "");
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
	}

	bool ReloadSettingsIni()
	{
		std::lock_guard l{ gSettingsLock };
		const bool changed = ApplyIni(false);
		ApplyGlobals();
		return changed;
	}

	void SaveSettings()
	{
		std::lock_guard l{ gSettingsLock };
		std::error_code ec;
		fs::create_directories(IniPath().parent_path(), ec);
		// keep every line of a section this plugin does not own (MCM Helper may keep more there one day)
		const auto               old = ReadIni();
		std::ofstream            out(IniPath(), std::ios::trunc);
		out << "[" << kSettingsSection << "]\n";
		for (const auto& s : gSettings) {
			out << s.ini << "=" << s.value << "\n";
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
		out << "\n[" << kDetectedSection << "]\n" << kDetectedKey << "=" << list;
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
	}

	void SetSetting(std::size_t a_index, int a_value)
	{
		{
			std::lock_guard l{ gSettingsLock };
			if (a_index >= gSettings.size()) {
				return;
			}
			auto& s = gSettings[a_index];
			const int top = s.isChoice ? static_cast<int>(s.choices.size()) - 1 : 1;
			s.value = std::clamp(a_value, 0, top);
			if (s.global) {
				s.global->value = static_cast<float>(s.value);
			}
			SKSE::log::info("[SETTING-CHANGED] {} = {}", s.id, s.value);
		}
		SaveSettings();
		// the forms are the game's: change them on its main thread, not the menu's
		SKSE::GetTaskInterface()->AddTask([]() { RefreshLights(); });
	}

	std::vector<Setting>&           Settings() { return gSettings; }
	const std::vector<MenuNote>&    Notes() { return gNotes; }
	const std::vector<SprayMarker>& SprayMarkers() { return gMarkers; }
	std::size_t                     GlobalsFromPlugin() { return gFileGlobals; }
	std::size_t                     GlobalsMadeInMemory() { return gMadeGlobals; }
	bool                            PluginLoaded(std::string_view a_plugin) { return Loaded(a_plugin); }

	bool SettingAvailable(const Setting& a_setting)
	{
		if (a_setting.needs.empty()) {
			return true;
		}
		return std::any_of(a_setting.needs.begin(), a_setting.needs.end(), [](const std::string& p) { return Loaded(p); });
	}

	// ------------------------------------------------------------------ Papyrus: the MCM tells the plugin its INI changed
	namespace
	{
		void PapyrusRefresh(RE::StaticFunctionTag*)
		{
			const bool changed = ReloadSettingsIni();
			SaveSettings();
			if (changed) {
				SKSE::GetTaskInterface()->AddTask([]() { RefreshLights(); });
			}
		}

		bool RegisterPapyrus(RE::BSScript::IVirtualMachine* a_vm)
		{
			a_vm->RegisterFunction("SettingsChanged", "LuminousArcanaNative", PapyrusRefresh);
			return true;
		}
	}

	void InstallPapyrus()
	{
		if (const auto* papyrus = SKSE::GetPapyrusInterface()) {
			papyrus->Register(RegisterPapyrus);
		}
	}
}
