// Illuminated - SKSE plugin
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE.txt and the notice at the top of main.cpp.
//
// Reads the Light Placer configs and collects what they light: model paths, shader names, and for every model the
// settings its lights wait on, so the passes can follow the menu. Without Light Placer (RecordLights.cpp) it also keeps
// every light row - the copy it names, its color, its flicker - until the game's own light records are made from them.

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
			struct Field;  // a key and its value; completed below, once Json is (a std::pair of an incomplete Json is not allowed)

			Kind               kind{ Kind::kNull };
			std::string        text;
			std::vector<Json>  items;
			std::vector<Field> fields;

			const Json* Get(std::string_view a_key) const;
		};

		struct Json::Field
		{
			std::string key;
			Json        value;
		};

		const Json* Json::Get(std::string_view a_key) const
		{
			for (const auto& [k, v] : fields) {
				if (k == a_key) {
					return &v;
				}
			}
			return nullptr;
		}

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
					// a run with no escape is copied in one piece (the speed pass, 2026-10-08: one push per character was the
					// reader's cost)
					if (const auto stop = s.find_first_of("\"\\", i); stop != std::string::npos && stop > i) {
						out.append(s, i, stop - i);
						i = stop;
						continue;
					}
					if (s[i] == '\\' && i + 1 < s.size()) {
						++i;
						const char c = s[i];
						if (c == 'u' && i + 4 < s.size()) {
							i += 4;  // no config names a model or a shader with an escaped character
							out.push_back('?');
						} else {
							out.push_back(c == 'n' ? '\n' : c == 't' ? '\t' :
																	   c);
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
					v.kind = v.text == "true" || v.text == "false" ? Json::Kind::kBool : v.text == "null" ? Json::Kind::kNull :
					                                                                                        Json::Kind::kNumber;
				}
				return v;
			}
		};

		Coverage       gCoverage;
		bool           gKeepLights = false;
		ConfigLightMap gConfigLights;

		[[nodiscard]] float Number(const Json* a_value, float a_fallback)
		{
			float out = a_fallback;
			if (!a_value || a_value->kind != Json::Kind::kNumber || !ParseFloat(a_value->text, out) || !std::isfinite(out)) {
				return a_fallback;
			}
			return out;
		}

		// A fadeController's keys: a flicker loops over them, a flash runs once and ends at 0 (its last key, far in the
		// future, holds 0). The steady fade a record light can carry is the mean over the time it runs, linear between keys.
		void ReadFadeController(const Json& a_controller, ConfigLight& a_out)
		{
			const auto* keys = a_controller.Get("keys");
			if (!keys || keys->kind != Json::Kind::kArray || keys->items.empty()) {
				return;
			}
			std::vector<std::pair<float, float>> points;  // time, fade
			for (const auto& k : keys->items) {
				if (k.kind == Json::Kind::kObject) {
					points.emplace_back(Number(k.Get("time"), -1.0f), (std::max)(Number(k.Get("value"), 0.0f), 0.0f));
				}
			}
			std::erase_if(points, [](const auto& a_p) { return a_p.first < 0.0f; });
			if (points.empty()) {
				return;
			}
			std::sort(points.begin(), points.end());
			a_out.controller = true;
			a_out.flash = points.back().second == 0.0f;
			if (a_out.flash) {
				// up to the first key after which every key is 0: the far sentinel key is not part of the flash
				while (points.size() > 1 && points[points.size() - 2].second == 0.0f) {
					points.pop_back();
				}
			}
			float area = 0.0f;
			for (std::size_t i = 1; i < points.size(); ++i) {
				area += (points[i].first - points[i - 1].first) * (points[i].second + points[i - 1].second) * 0.5f;
			}
			const float span = points.back().first - points.front().first;
			a_out.meanFade = span > 0.0f ? area / span : points.front().second;
			if (!a_out.flash && points.size() > 1 && span > 0.0f) {
				a_out.keys = std::move(points);  // a flicker, played by the record light every frame (RecordLights.cpp)
				if (const auto* i = a_controller.Get("interpolation"); i && i->kind == Json::Kind::kString) {
					const auto how = Lower(i->text);
					a_out.interpolation = how == "step" ? 0 : how == "cubic" ? 2 :
					                                                           1;
				}
			}
		}

		[[nodiscard]] ConfigLight ReadConfigLight(const Json& a_light, std::vector<std::vector<Clause>> a_test)
		{
			ConfigLight out;
			out.test = std::move(a_test);
			const auto* data = a_light.Get("data");
			if (!data) {
				return out;
			}
			if (const auto* l = data->Get("light"); l && l->kind == Json::Kind::kString) {
				out.light = l->text;
			}
			if (const auto* c = data->Get("color"); c && c->kind == Json::Kind::kArray && c->items.size() == 3) {
				const auto channel = [&](std::size_t a_i) {
					return static_cast<std::uint8_t>(std::clamp(std::lround(Number(&c->items[a_i], 0.0f)), 0L, 255L));
				};
				out.r = channel(0);
				out.g = channel(1);
				out.b = channel(2);
				out.hasColor = true;
			}
			if (const auto* f = data->Get("flags"); f && f->kind == Json::Kind::kString) {
				out.portalStrict = Contains(Lower(f->text), "portalstrict");
			}
			if (const auto* fc = data->Get("fadeController"); fc && fc->kind == Json::Kind::kObject) {
				ReadFadeController(*fc, out);
			}
			return out;
		}

		using Clock = std::chrono::steady_clock;
		double gReadMs = 0.0, gParseMs = 0.0;  // the load's two costs, logged by ReadCoverage (the speed pass)

		void ReadConfig(const fs::path& a_file, Coverage& a_cov)
		{
			const auto    t0 = Clock::now();
			std::ifstream in(a_file, std::ios::binary | std::ios::ate);
			if (!in) {
				return;
			}
			// the whole file in one read into one buffer
			const auto  size = static_cast<std::size_t>((std::max)(std::streamoff{ 0 }, static_cast<std::streamoff>(in.tellg())));
			std::string text(size, '\0');
			in.seekg(0);
			in.read(text.data(), static_cast<std::streamsize>(size));
			text.resize(static_cast<std::size_t>((std::max)(std::streamsize{ 0 }, in.gcount())));
			if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF) {
				text.erase(0, 3);  // a byte order mark
			}
			const auto t1 = Clock::now();
			gReadMs += std::chrono::duration<double, std::milli>(t1 - t0).count();
			struct Timed
			{
				Clock::time_point start;
				~Timed() { gParseMs += std::chrono::duration<double, std::milli>(Clock::now() - start).count(); }
			} timed{ t1 };
			Reader     r{ text };
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
				// each light of the entry: the settings it waits on (and, for the game's own light records, the light itself)
				std::vector<std::vector<std::vector<Clause>>> tests;
				ConfigEntry                                   rows;
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
						if (gKeepLights) {
							rows.push_back(ReadConfigLight(light, tests.back()));
						}
					}
				}
				if (tests.empty()) {
					tests.emplace_back();
				}
				if (const auto* models = entry.Get("models"); models && models->kind == Json::Kind::kArray) {
					// one copy of the entry's tests (and rows), shared by every model it names
					const auto shared = std::make_shared<const Coverage::Tests>(std::move(tests));
					const auto sharedRows = rows.empty() ? nullptr : std::make_shared<const ConfigEntry>(std::move(rows));
					for (const auto& m : models->items) {
						const auto low = NormalPath(m.text);
						if (low.size() > 4 && low.ends_with(".nif")) {
							a_cov.models.insert(low);
							a_cov.modelTests[low].push_back(shared);
							if (sharedRows) {
								gConfigLights[low].push_back(sharedRows);
							}
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
		return std::any_of(it->second.begin(), it->second.end(), [](const auto& a_entry) {
			return std::any_of(a_entry->begin(), a_entry->end(), [](const auto& t) { return ConditionsHold(t); });
		});
	}

	void KeepConfigLights(bool a_keep)
	{
		gKeepLights = a_keep;
	}

	const ConfigLightMap& ConfigLights()
	{
		return gConfigLights;
	}

	void DropConfigLights()
	{
		ConfigLightMap{}.swap(gConfigLights);  // the memory goes back, not only the entries
	}

	const Coverage& ReadCoverage()
	{
		gCoverage = Coverage{};
		DropConfigLights();
		gReadMs = gParseMs = 0.0;
		const auto started = Clock::now();
		for (const auto folder : { kOurFolder, kCSFolder }) {
			std::error_code ec;
			const auto      dir = LightPlacerDir(folder);
			if (!fs::is_directory(dir, ec)) {
				continue;
			}
			// the error code form throughout: a folder that cannot be stepped into (locked, broken under MO2's virtual
			// files) ends the walk instead of throwing out of the game's message handler
			for (auto it = fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec);
				!ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
				std::error_code one;  // one unreadable entry is skipped, the walk goes on
				if (it->is_regular_file(one) && Lower(it->path().extension().string()) == ".json") {
					ReadConfig(it->path(), gCoverage);
				}
			}
		}
		const double total = std::chrono::duration<double, std::milli>(Clock::now() - started).count();
		SKSE::log::info("configs: read in {:.1f} ms - files {:.1f} ms, parsing {:.1f} ms, finding them {:.1f} ms", total, gReadMs, gParseMs,
			total - gReadMs - gParseMs);
		return gCoverage;
	}
}
