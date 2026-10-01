// Illuminated - SKSE plugin
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE.txt and the notice at the top of main.cpp.
//
// DevBench, when it is in the load order: `inspect kind=illuminated` returns every setting's value and every light
// copy's fade, radius and cutoff as they are now next to the values it was made with, and `menu action=invoke name=illuminated
// set=setting key=<setting id or INI key> value=<n>` changes a setting exactly as the menu does (saved, applied at the
// next frame). Nothing here runs unless DevBench asks.

#include "Plugin.h"

#include "DevBenchAPI.h"

#include <nlohmann/json.hpp>

namespace Plugin
{
	namespace
	{
		constexpr const char* kKey = "illuminated";
		constexpr unsigned    kNeedsBuild = 10500;  // DevBench 1.5.0: RegisterToolExtension

		constexpr const char* kInspect =
			R"({"description":"Illuminated - every setting's value, and every light copy (id, base light, flicker, the fade, radius and cutoff it holds now and the ones it was made with). Read only.","inputSchema":{"type":"object","properties":{}},"readOnly":true})";

		constexpr const char* kMenu =
			R"({"description":"Illuminated - set=setting key=<setting id or INI key> value=<whole number> changes a setting exactly as the menu does: saved, and applied at the next frame.","inputSchema":{"type":"object","properties":{"set":{"type":"string"},"key":{"type":"string"},"value":{"type":"number"}}}})";

		using json = nlohmann::json;

		void InspectNow(void* a_sink, DevBenchAPI::WriteFn a_write)
		{
			json out;
			out["settings"] = json::object();
			for (const auto& s : Settings()) {
				out["settings"][s.id] = s.value;
			}
			out["copies"] = json::array();
			for (const auto& c : LightCopies()) {
				if (c.form) {
					out["copies"].push_back({ { "id", c.id }, { "base", c.base }, { "flicker", c.flicker }, { "fade", c.form->fade },
						{ "radius", c.form->data.radius }, { "cutoff", c.form->data.fallofExponent }, { "madeFade", c.startFade },
						{ "madeRadius", c.startRadius }, { "madeCutoff", c.startCutoff } });
				}
			}
			// every string escaped, and a number that is not finite written as null: the reply is always valid JSON
			a_write(a_sink, out.dump(-1, ' ', false, json::error_handler_t::replace).c_str());
		}

		bool MenuNow(const char* a_args)
		{
			const auto args = json::parse(a_args ? a_args : "", nullptr, false);  // a discarded value (not an exception) if bad
			if (!args.is_object() || args.value("set", "") != "setting" || !args.contains("value") || !args["value"].is_number()) {
				return false;
			}
			const auto  key = Lower(args.value("key", ""));
			const auto  value = static_cast<int>(std::lround(args["value"].get<double>()));
			const auto& settings = Settings();
			for (std::size_t i = 0; i < settings.size(); ++i) {
				if (Lower(settings[i].id) == key || Lower(settings[i].ini) == key) {
					SetSetting(i, value);
					return true;
				}
			}
			return false;
		}

		// DevBench calls these across the DLL boundary: nothing may be thrown back through it
		void Inspect(void*, const char*, void* a_sink, DevBenchAPI::WriteFn a_write) noexcept
		{
			if (!a_write) {
				return;
			}
			try {
				InspectNow(a_sink, a_write);
			} catch (...) {
				a_write(a_sink, R"({"error":"Illuminated could not build its report"})");
			}
		}

		void Menu(void*, const char* a_args, void* a_sink, DevBenchAPI::WriteFn a_write) noexcept
		{
			bool ok = false;
			try {
				ok = MenuNow(a_args);
			} catch (...) {
			}
			if (a_write) {
				a_write(a_sink, ok ? R"({"queued":true})" : R"({"queued":false,"error":"set=setting key=<setting id or INI key> value=<whole number>"})");
			}
		}
	}

	void OfferToDevBench()
	{
		auto* devbench = DevBenchAPI::GetDevBenchInterface001();
		if (!devbench || devbench->GetBuildNumber() < kNeedsBuild) {
			return;  // DevBench is not in this load order, or too old; nothing depends on it
		}
		devbench->RegisterToolExtension("inspect", kKey, kInspect, Inspect, nullptr);
		devbench->RegisterMenuHandler(kKey, kMenu, Menu, nullptr);
		SKSE::log::info("DevBench: inspect kind={} and the settings (menu invoke name={}) registered", kKey, kKey);
	}
}
