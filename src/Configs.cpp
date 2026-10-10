// Illuminated - SKSE plugin
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE.txt and the notice at the top of main.cpp.
//
// Reads the Light Placer configs and collects what they light: model paths, shader names, and for every model the
// settings its lights wait on, so the passes can follow the menu. Without Light Placer (RecordLights.cpp) it also keeps
// every light row - the copy it names, its color, its flicker - until the game's own light records are made from them.

#include "Fade.h"
#include "Plugin.h"

namespace Plugin
{
	namespace
	{
		// a claim written as "0xID~Plugin.esp" (a vanilla record a mod renames, gen.RENAMED_BASE_FORMS): its form in this load
		// order, or nothing (not that form of claim, or its plugin is not loaded)
		[[nodiscard]] std::optional<RE::FormID> FormOfToken(std::string_view a_text)
		{
			const auto tilde = a_text.find('~');
			if (tilde == std::string_view::npos || tilde < 3 || !(a_text.starts_with("0x") || a_text.starts_with("0X"))) {
				return std::nullopt;
			}
			const auto    hex = a_text.substr(2, tilde - 2);
			std::uint32_t local = 0;
			const auto    r = std::from_chars(hex.data(), hex.data() + hex.size(), local, 16);
			if (r.ec != std::errc{} || r.ptr != hex.data() + hex.size()) {
				return std::nullopt;
			}
			auto*      dh = RE::TESDataHandler::GetSingleton();
			const auto id = dh ? dh->LookupFormID(local, a_text.substr(tilde + 1)) : 0;
			return id ? std::optional<RE::FormID>(id) : std::nullopt;
		}

		// the configs' JSON reader, numbers and fadeController keys: ConfigText.h (game-free, tested off the game)
		using ConfigText::Json;
		using ConfigText::Number;
		using ConfigText::Reader;
		using ConfigText::ReadFadeController;

		Coverage       gCoverage;
		bool           gKeepLights = false;
		ConfigLightMap gConfigLights;

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
				SKSE::log::warn("[CONFIG-UNREADABLE] {} | not a list of entries this plugin can read; its lights are not counted", Fade::PathText(a_file.filename()));
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
				// one copy of the entry's tests (and rows), shared by every model and form it names
				const auto shared = std::make_shared<const Coverage::Tests>(std::move(tests));
				const auto sharedRows = rows.empty() ? nullptr : std::make_shared<const ConfigEntry>(std::move(rows));
				if (const auto* models = entry.Get("models"); models && models->kind == Json::Kind::kArray) {
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
							// without Light Placer an explosion, projectile or hazard a config names by editor ID gets its record
							// light too (Explosions.json's formIDs entries were dark on Vanilla) - kept under its
							// own key, which EffectLights.cpp asks by the form's editor ID
							if (sharedRows) {
								// a vanilla record a mod renames is claimed by its id, "0x4046D~Skyrim.esm" (gen.RENAMED_BASE_FORMS,
								// Frenzy's explosion was dark without Mysticism): kept under the form itself
								const auto id = FormOfToken(f.text);
								const auto key = id ? FormIdKey(*id) : FormKey(low);
								a_cov.modelTests[key].push_back(shared);
								gConfigLights[key].push_back(sharedRows);
							}
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

	const Coverage& LastCoverage()
	{
		return gCoverage;
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
				if (it->is_regular_file(one) && Lower(Fade::PathText(it->path().extension())) == ".json") {
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
