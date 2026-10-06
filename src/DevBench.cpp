// Illuminated - SKSE plugin
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE.txt and the notice at the top of main.cpp.
//
// DevBench, when it is in the load order (it is optional; nothing here runs without it):
//   inspect kind=illuminated     every setting's value, and every light copy's fade, radius and cutoff now next to the
//                                values it was made with.
//   illuminated.control          action = setting (key = a setting id or INI key, value = a whole number: changed exactly
//                                as the menu does - saved, applied at the next frame; the reply is the value it holds) |
//                                settings (every setting with its value, range and page).
//   menu invoke name=illuminated set=setting key=... value=... - the same, kept for scripts that already use it.
//   event illuminated.settingChanged {id, ini, value} for each change made through DevBench.

#include "Plugin.h"

#include "DevBenchGlue.h"

namespace Plugin
{
	namespace
	{
		using DevBenchGlue::json;

		constexpr const char* kKey = "illuminated";

		constexpr const char* kInspect =
			R"json({"description":"Illuminated - every setting's value, and every light copy (id, base light, flicker, the fade, radius and cutoff it holds now and the ones it was made with). Read only.","inputSchema":{"type":"object","properties":{}},"readOnly":true})json";

		constexpr const char* kTool =
			R"json({"description":"Illuminated (Light Placer spell, weapon and effect lights with an SKSE menu): change its settings. action=setting sets one setting by its id or INI key to a whole number exactly as the menu does (saved, applied at the next frame) and replies with the value it now holds; action=settings lists every setting with its id, INI key, value, range and menu page. Read the lights with inspect kind=illuminated.","inputSchema":{"type":"object","properties":{"action":{"type":"string","enum":["setting","settings"]},"key":{"type":"string","description":"setting: a setting id or INI key (action=settings lists them)"},"value":{"type":"number","description":"setting: a whole number (a switch 0/1, a choice's index, a slider's percent)"}},"required":["action"]}})json";

		constexpr const char* kMenu =
			R"json({"description":"Illuminated - set=setting key=<setting id or INI key> value=<whole number> changes a setting exactly as the menu does: saved, and applied at the next frame. The same as the illuminated.control tool.","inputSchema":{"type":"object","properties":{"set":{"type":"string"},"key":{"type":"string"},"value":{"type":"number"}}}})json";

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
			DevBenchGlue::Reply(a_sink, a_write, out);
		}

		json SettingJson(const Setting& a_s)
		{
			return json{ { "id", a_s.id }, { "ini", a_s.ini }, { "value", a_s.value }, { "default", a_s.defaultValue }, { "min", a_s.minValue },
				{ "max", a_s.maxValue }, { "page", a_s.page }, { "label", a_s.label }, { "restart", a_s.restart } };
		}

		json Act(const json& a_args)
		{
			auto action = DevBenchGlue::Text(a_args, "action");
			if (action.empty() || action == "invoke") {  // `menu invoke` hands over its own args: action=invoke, the verb in set
				action = DevBenchGlue::Text(a_args, "set");
			}
			auto& settings = Settings();
			if (action == "settings") {
				json all = json::array();
				for (const auto& s : settings) {
					all.push_back(SettingJson(s));
				}
				return json{ { "ok", true }, { "settings", all } };
			}
			if (action != "setting") {
				return DevBenchGlue::Refusal("unknown action '" + action + "'", json::array({ "setting", "settings" }));
			}
			const auto key = Lower(DevBenchGlue::Text(a_args, "key"));
			const auto v = DevBenchGlue::Number(a_args, "value");
			for (std::size_t i = 0; i < settings.size(); ++i) {
				if (Lower(settings[i].id) == key || Lower(settings[i].ini) == key) {
					if (!v) {
						return DevBenchGlue::Refusal("setting needs value: a whole number");
					}
					SetSetting(i, static_cast<int>(std::lround(*v)));
					const auto now = SettingJson(settings[i]);
					DevBenchGlue::Emit("illuminated.settingChanged", json{ { "id", settings[i].id }, { "ini", settings[i].ini }, { "value", settings[i].value } });
					return json{ { "ok", true }, { "setting", now }, { "note", "applied to the lights at the next frame" } };
				}
			}
			json ids = json::array();
			for (const auto& s : settings) {
				ids.push_back(s.id);
			}
			return DevBenchGlue::Refusal("unknown setting '" + key + "'", ids);
		}

		// DevBench calls these across the DLL boundary: nothing may be thrown back through it
		void Inspect(void*, const char*, void* a_sink, DevBenchAPI::WriteFn a_write) noexcept
		{
			try {
				InspectNow(a_sink, a_write);
			} catch (...) {
				DevBenchGlue::Reply(a_sink, a_write, DevBenchGlue::Refusal("Illuminated could not build its report"));
			}
		}

		void Control(void*, const char* a_args, void* a_sink, DevBenchAPI::WriteFn a_write) noexcept
		{
			try {
				DevBenchGlue::Reply(a_sink, a_write, Act(DevBenchGlue::Args(a_args)));
			} catch (...) {
				DevBenchGlue::Reply(a_sink, a_write, DevBenchGlue::Refusal("Illuminated could not handle that"));
			}
		}
	}

	void OfferToDevBench()
	{
		auto* devbench = DevBenchAPI::GetDevBenchInterface001();
		if (!devbench || devbench->GetBuildNumber() < DevBenchGlue::kNeedsBuild) {
			return;  // DevBench is not in this load order, or too old; nothing depends on it
		}
		devbench->RegisterToolExtension("inspect", kKey, kInspect, Inspect, nullptr);
		devbench->RegisterTool("illuminated.control", kTool, Control, nullptr);
		devbench->RegisterMenuHandler(kKey, kMenu, Control, nullptr);
		SKSE::log::info("DevBench: inspect kind={}, the tool illuminated.control and menu invoke name={} registered", kKey, kKey);
	}
}
