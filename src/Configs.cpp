// Luminous Arcana - SKSE plugin
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE.txt and the notice at the top of main.cpp.
//
// Reads the Light Placer configs and collects what they light: model paths, shader names, and for every model the
// settings its lights wait on, so the passes can follow the menu.

#include "Plugin.h"

namespace Plugin
{
	namespace
	{
		// ------------------------------------------------------------------ a small JSON reader for the configs
		struct Json
		{
			enum class Kind
			{
				kNull,
				kBool,
				kNumber,
				kString,
				kArray,
				kObject
			};
			Kind                                      kind{ Kind::kNull };
			std::string                               text;
			std::vector<Json>                         items;
			std::vector<std::pair<std::string, Json>> fields;

			const Json* Get(std::string_view a_key) const
			{
				for (const auto& [k, v] : fields) {
					if (k == a_key) {
						return &v;
					}
				}
				return nullptr;
			}
		};

		struct Reader
		{
			const std::string& s;
			std::size_t        i{ 0 };
			bool               bad{ false };

			void Space()
			{
				while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) {
					++i;
				}
			}

			std::string String()
			{
				std::string out;
				++i;  // the opening quote
				while (i < s.size() && s[i] != '"') {
					if (s[i] == '\\' && i + 1 < s.size()) {
						++i;
						const char c = s[i];
						if (c == 'u' && i + 4 < s.size()) {
							i += 4;  // no config names a model or a shader with an escaped character
							out.push_back('?');
						} else {
							out.push_back(c == 'n' ? '\n' : c == 't' ? '\t' : c);
						}
					} else {
						out.push_back(s[i]);
					}
					++i;
				}
				if (i >= s.size()) {
					bad = true;
				}
				++i;  // the closing quote
				return out;
			}

			Json Value(int a_depth = 0)
			{
				Json v;
				Space();
				if (i >= s.size() || a_depth > 64) {
					bad = true;
					return v;
				}
				const char c = s[i];
				if (c == '{') {
					v.kind = Json::Kind::kObject;
					++i;
					Space();
					if (i < s.size() && s[i] == '}') {
						++i;
						return v;
					}
					while (!bad) {
						Space();
						if (i >= s.size() || s[i] != '"') {
							bad = true;
							break;
						}
						auto key = String();
						Space();
						if (i >= s.size() || s[i] != ':') {
							bad = true;
							break;
						}
						++i;
						v.fields.emplace_back(std::move(key), Value(a_depth + 1));
						Space();
						if (i < s.size() && s[i] == ',') {
							++i;
							continue;
						}
						if (i < s.size() && s[i] == '}') {
							++i;
							break;
						}
						bad = true;
					}
				} else if (c == '[') {
					v.kind = Json::Kind::kArray;
					++i;
					Space();
					if (i < s.size() && s[i] == ']') {
						++i;
						return v;
					}
					while (!bad) {
						v.items.push_back(Value(a_depth + 1));
						Space();
						if (i < s.size() && s[i] == ',') {
							++i;
							continue;
						}
						if (i < s.size() && s[i] == ']') {
							++i;
							break;
						}
						bad = true;
					}
				} else if (c == '"') {
					v.kind = Json::Kind::kString;
					v.text = String();
				} else {
					const auto start = i;
					while (i < s.size() && s[i] != ',' && s[i] != ']' && s[i] != '}' && !std::isspace(static_cast<unsigned char>(s[i]))) {
						++i;
					}
					v.text = s.substr(start, i - start);
					v.kind = v.text == "true" || v.text == "false" ? Json::Kind::kBool : v.text == "null" ? Json::Kind::kNull : Json::Kind::kNumber;
				}
				return v;
			}
		};

		Coverage gCoverage;

		void ReadConfig(const fs::path& a_file, Coverage& a_cov)
		{
			std::ifstream in(a_file, std::ios::binary);
			if (!in) {
				return;
			}
			std::stringstream buf;
			buf << in.rdbuf();
			std::string text = buf.str();
			if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF) {
				text = text.substr(3);  // a byte order mark
			}
			Reader r{ text };
			const auto root = r.Value();
			if (r.bad || root.kind != Json::Kind::kArray) {
				SKSE::log::warn("[CONFIG-UNREADABLE] {} | not a list of entries this plugin can read; its lights are not counted", a_file.filename().string());
				return;
			}
			++a_cov.files;
			for (const auto& entry : root.items) {
				if (entry.kind != Json::Kind::kObject) {
					continue;
				}
				// each light of the entry: the settings it waits on
				std::vector<std::vector<std::vector<Clause>>> tests;
				if (const auto* lights = entry.Get("lights"); lights && lights->kind == Json::Kind::kArray) {
					for (const auto& light : lights->items) {
						std::vector<std::string> conds;
						if (const auto* data = light.Get("data")) {
							if (const auto* c = data->Get("conditions"); c && c->kind == Json::Kind::kArray) {
								for (const auto& item : c->items) {
									if (item.kind == Json::Kind::kString) {
										conds.push_back(item.text);
									}
								}
							}
						}
						tests.push_back(ParseConditions(conds));
					}
				}
				if (tests.empty()) {
					tests.emplace_back();
				}
				if (const auto* models = entry.Get("models"); models && models->kind == Json::Kind::kArray) {
					for (const auto& m : models->items) {
						const auto low = NormalPath(m.text);
						if (low.size() > 4 && low.ends_with(".nif")) {
							a_cov.models.insert(low);
							auto& list = a_cov.modelTests[low];
							list.insert(list.end(), tests.begin(), tests.end());
						}
					}
				}
				if (const auto* ids = entry.Get("formIDs"); ids && ids->kind == Json::Kind::kArray) {
					for (const auto& f : ids->items) {
						const auto low = NormalPath(f.text);
						if (low.size() > 2) {
							a_cov.shaders.insert(low);
						}
					}
				}
			}
		}
	}

	// ------------------------------------------------------------------ what the configs light
	fs::path LightPlacerDir(std::string_view a_folder)
	{
		return fs::current_path() / "Data" / "LightPlacer" / fs::path(std::string(a_folder));
	}

	bool Coverage::ModelLit(const std::string& a_model) const
	{
		const auto it = modelTests.find(a_model);
		if (it == modelTests.end()) {
			return false;
		}
		return std::any_of(it->second.begin(), it->second.end(), [](const auto& t) { return ConditionsHold(t); });
	}

	const Coverage& ReadCoverage()
	{
		gCoverage = Coverage{};
		for (const auto folder : { kOurFolder, kCSFolder }) {
			std::error_code ec;
			const auto      dir = LightPlacerDir(folder);
			if (!fs::is_directory(dir, ec)) {
				continue;
			}
			for (const auto& entry : fs::recursive_directory_iterator(dir, ec)) {
				if (entry.is_regular_file() && Lower(entry.path().extension().string()) == ".json") {
					ReadConfig(entry.path(), gCoverage);
				}
			}
		}
		return gCoverage;
	}
}
