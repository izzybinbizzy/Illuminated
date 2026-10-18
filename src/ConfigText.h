// Illuminated - SKSE plugin
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE.txt and the notice at the top of main.cpp.
//
// The plain-text part of reading the Light Placer configs and the settings file: trimming, whole-field numbers, the
// settings a light waits on (ParseConditions), the configs' small JSON reader and a fadeController's keys. Nothing of the
// game in here, so tests/test_fade.cpp builds and checks it off the game (PC Runner\fade tests.py).
//
//   Trim, ParseInt, ParseFloat, Split   text helpers; a number is read only when the WHOLE field is one ("12abc" is not 12)
//   Clause, ParseConditions             the GetGlobalValue items of a light's conditions, as tests of OR-runs
//   DynamicRow, TwinOf, LightPlacerKey,
//   SwapTwins                           the Dynamic Lighting switch at once (LightCopies.cpp SwapDynamicTwins)
//   Json, Reader, Number                the configs' JSON reader (never throws, depth capped)
//   ReadFadeController                  a fadeController's keys: flicker or flash, its mean fade

#pragma once

#include "LightMath.h"

#include <charconv>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <limits>
#include <optional>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

namespace ConfigText
{
	using LightKit::Lower;

	[[nodiscard]] inline std::string Trim(std::string_view a_text)
	{
		const auto first = a_text.find_first_not_of(" \t\r\n");
		if (first == std::string_view::npos) {
			return {};
		}
		const auto last = a_text.find_last_not_of(" \t\r\n");
		return std::string(a_text.substr(first, last - first + 1));
	}

	// a_out keeps what it held unless the whole (trimmed) field is a number - so a caller may ignore the answer and keep its
	// default (the settings file's optional fields)
	template <class T>
	bool ParseNumber(std::string_view a_text, T& a_out)
	{
		const auto t = Trim(a_text);
		T          v{};
		const auto r = std::from_chars(t.data(), t.data() + t.size(), v);
		if (t.empty() || r.ec != std::errc() || r.ptr != t.data() + t.size()) {
			return false;
		}
		if constexpr (std::is_floating_point_v<T>) {
			if (!std::isfinite(v)) {
				return false;  // "nan" / "inf" read as whole fields: never a value
			}
		}
		a_out = v;
		return true;
	}

	inline bool ParseInt(std::string_view a_text, int& a_out) { return ParseNumber(a_text, a_out); }
	inline bool ParseFloat(std::string_view a_text, float& a_out) { return ParseNumber(a_text, a_out); }

	// the trimmed, non-empty pieces between separators
	[[nodiscard]] inline std::vector<std::string> Split(std::string_view a_text, char a_sep)
	{
		std::vector<std::string> out;
		std::size_t              start = 0;
		while (start <= a_text.size()) {
			const auto end = a_text.find(a_sep, start);
			auto       piece = Trim(a_text.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start));
			if (!piece.empty()) {
				out.push_back(std::move(piece));
			}
			if (end == std::string_view::npos) {
				break;
			}
			start = end + 1;
		}
		return out;
	}

	// ------------------------------------------------------------------ the settings a light waits on
	struct Clause
	{
		std::string global;  // lower case
		int         value{ 0 };
	};

	// THE ROUTE (his answer 2026-10-13, Q7): every Light Placer row waits first on this global == 1 (lagen.py route_switch), so
	// Light Placer draws a row only on Community Shaders. The plugin reads a row only for its own work, the route where Light
	// Placer draws none of them: a route clause standing alone is the gate and is read as if it were not written, and inside an
	// OR run it is the one item that cannot hold here, so the run's other items decide (CodeRabbit on Illuminated #20).
	inline constexpr std::string_view kRouteGlobal = "illuminatedlightplacerroute";

	// Only GetGlobalValue items are kept. A run of OR items is one test, as the game reads it; a test that holds anything
	// else (IsSneaking OR a setting) cannot be decided from the settings, so it is left out: it may be true.
	[[nodiscard]] inline std::vector<std::vector<Clause>> ParseConditions(const std::vector<std::string>& a_conditions)
	{
		std::vector<std::vector<Clause>> out;
		std::vector<Clause>              run;
		bool                             runHasOther = false;
		for (const auto& raw : a_conditions) {
			const auto words = Split(raw, ' ');
			if (words.empty()) {
				continue;
			}
			const bool isOr = Lower(words.back()) == "or";
			auto       fn = words.begin();
			while (fn != words.end() && Lower(*fn) != "getglobalvalue") {
				++fn;
			}
			if (fn != words.end() && std::distance(fn, words.end()) >= 5 && *(fn + 2) == "NONE" && *(fn + 3) == "==") {
				int v = 0;
				if (ParseInt(*(fn + 4), v)) {
					// a route clause is left out: alone, its run is empty and so no test (the gate); in an OR run the other items decide
					if (auto global = Lower(*(fn + 1)); global != kRouteGlobal) {
						run.push_back({ std::move(global), v });
					}
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

	// ------------------------------------------------------------------ the Dynamic Lighting twins
	// Every Light Placer row that waits on Dynamic Lighting has exactly one steady twin (lagen.py: the same claim, place and other
	// conditions - only the light, its controllers and that clause differ; 821 pairs in 1.3). Light Placer names a light
	// "LP_Light[<config>|<light record's editor ID>]..." and keeps its verdict on the light's conditions in the top byte of its
	// ambient red (its LightData.cpp CullLight: 1 conditions, 2 game, 4 script; hidden while any is set), and writes it again only
	// when that verdict changes - so a pair swapped the way its next test will swap it stays as written.
	inline constexpr std::string_view kDynamicGlobal = "illuminateddynamiclighting";
	inline constexpr std::uint8_t     kRowDynamic = 1, kRowSteady = 2, kRowNeither = 4;

	// the kind of row a light's tests make it: Dynamic Lighting on, its steady twin (off), or neither
	[[nodiscard]] inline std::uint8_t DynamicRow(const std::vector<std::vector<Clause>>& a_tests)
	{
		for (const auto& run : a_tests) {
			if (run.size() == 1 && run.front().global == kDynamicGlobal) {
				return run.front().value != 0 ? kRowDynamic : kRowSteady;
			}
		}
		return kRowNeither;
	}

	enum class Twin : std::uint8_t
	{
		kNeither,
		kDynamic,
		kSteady
	};

	// a "<config>|<light>" by the kinds of rows that name it: named only by Dynamic Lighting rows, the dynamic one; named by a
	// steady twin, the steady one - even when a row of neither kind shares it (the pairing by place tells those apart)
	[[nodiscard]] constexpr Twin TwinOf(std::uint8_t a_rows)
	{
		if (a_rows == kRowDynamic) {
			return Twin::kDynamic;
		}
		return (a_rows & kRowSteady) && !(a_rows & kRowDynamic) ? Twin::kSteady : Twin::kNeither;
	}

	// "LP_Light[<config>|<light>]..." -> its "<config>|<light>"; empty for any other light
	[[nodiscard]] constexpr std::string_view LightPlacerKey(std::string_view a_name)
	{
		constexpr std::string_view head = "LP_Light[";
		if (!a_name.starts_with(head)) {
			return {};
		}
		const auto bar = a_name.find('|', head.size());
		const auto end = bar == std::string_view::npos ? bar : a_name.find(']', bar);
		return end == std::string_view::npos ? std::string_view{} : a_name.substr(head.size(), end - head.size());
	}

	inline constexpr std::uint32_t kHideConditions = 1u << 24, kHideOther = 6u << 24, kHidden = 0xFFu << 24;

	struct TwinBits
	{
		std::uint32_t dynamic{ 0 };
		std::uint32_t steady{ 0 };
	};

	// the pair's hide bits once the setting says a_on: the light it wants shows if either showed, the other hides; nothing when
	// either is hidden for another reason (left to Light Placer's own test)
	[[nodiscard]] constexpr std::optional<TwinBits> SwapTwins(TwinBits a_now, bool a_on)
	{
		if ((a_now.dynamic | a_now.steady) & kHideOther) {
			return std::nullopt;
		}
		const bool shown = !(a_now.dynamic & kHideConditions) || !(a_now.steady & kHideConditions);
		const auto hide = [](std::uint32_t a_bits, bool a_hide) { return a_hide ? a_bits | kHideConditions : a_bits & ~kHideConditions; };
		return TwinBits{ hide(a_now.dynamic, !(a_on && shown)), hide(a_now.steady, !(!a_on && shown)) };
	}

	// ------------------------------------------------------------------ a small JSON reader for the configs
	// Its own reader, not nlohmann (which the DevBench glue and the fading rule files use): the configs are ~150 files and
	// 12 MB read at every start, and this one keeps only what the passes ask for and copies strings in runs (measured: the
	// configs' parse 89 -> 67 ms). It never throws: a file it cannot read is marked bad, and its depth is capped (64) so no
	// file can run it out of stack.
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

		[[nodiscard]] const Json* Get(std::string_view a_key) const;
	};

	struct Json::Field
	{
		std::string key;
		Json        value;
	};

	inline const Json* Json::Get(std::string_view a_key) const
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

		static bool IsSpace(char a_c) { return std::isspace(static_cast<unsigned char>(a_c)) != 0; }

		void Space()
		{
			while (i < s.size() && IsSpace(s[i])) {
				++i;
			}
		}

		std::string String()
		{
			std::string out;
			++i;  // the opening quote
			while (i < s.size() && s[i] != '"') {
				// a run with no escape is copied in one piece
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
					v.fields.push_back(Json::Field{ std::move(key), Value(a_depth + 1) });
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
				while (i < s.size() && s[i] != ',' && s[i] != ']' && s[i] != '}' && !IsSpace(s[i])) {
					++i;
				}
				v.text = s.substr(start, i - start);
				v.kind = v.text == "true" || v.text == "false" ? Json::Kind::kBool : v.text == "null" ? Json::Kind::kNull :
				                                                                                        Json::Kind::kNumber;
			}
			return v;
		}
	};

	// a number field, or the fallback (missing, not a number, not finite)
	[[nodiscard]] inline float Number(const Json* a_value, float a_fallback)
	{
		float out = a_fallback;
		if (!a_value || a_value->kind != Json::Kind::kNumber || !ParseFloat(a_value->text, out) || !std::isfinite(out)) {
			return a_fallback;
		}
		return out;
	}

	// A fadeController's keys: a flicker loops over them, a flash runs once and ends at 0 (its last key, far in the future,
	// holds 0). The steady fade a record light can carry is the mean over the time it runs, linear between keys. a_out is
	// any type with the fields controller, flash, meanFade, keys and interpolation (Plugin::ConfigLight).
	template <class Out>
	void ReadFadeController(const Json& a_controller, Out& a_out)
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
		std::ranges::sort(points);
		const bool flash = points.back().second == 0.0f;
		if (flash) {
			// up to the first key after which every key is 0: the far sentinel key is not part of the flash
			while (points.size() > 1 && points[points.size() - 2].second == 0.0f) {
				points.pop_back();
			}
		}
		// summed in double: keys far apart in time with large values can pass a float's range
		double area = 0.0;
		for (std::size_t i = 1; i < points.size(); ++i) {
			const double dt = static_cast<double>(points[i].first) - static_cast<double>(points[i - 1].first);
			const double sum = static_cast<double>(points[i].second) + static_cast<double>(points[i - 1].second);
			area += dt * sum * 0.5;
		}
		const double span = static_cast<double>(points.back().first) - static_cast<double>(points.front().first);
		const double mean = span > 0.0 ? area / span : static_cast<double>(points.front().second);
		if (!std::isfinite(mean) || mean < 0.0 || mean > static_cast<double>((std::numeric_limits<float>::max)())) {
			return;  // not a fade a light can carry: the controller is left out
		}
		a_out.controller = true;
		a_out.flash = flash;
		a_out.meanFade = static_cast<float>(mean);
		if (!flash && points.size() > 1 && span > 0.0) {
			a_out.keys = std::move(points);  // a flicker, played by the record light every frame (RecordLights.cpp)
			if (const auto* how = a_controller.Get("interpolation"); how && how->kind == Json::Kind::kString) {
				const auto low = Lower(how->text);
				a_out.interpolation = low == "step" ? 0 : low == "cubic" ? 2 :
				                                                           1;
			}
		}
	}
}
