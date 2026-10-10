// Illuminated - SKSE plugin
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE.txt and the notice at the top of main.cpp.
//
// DevBench, when it is in the load order (it is optional; nothing here runs without it):
//   inspect kind=illuminated     every setting's value, and every light copy's fade, radius and cutoff now next to the
//                                values it was made with; the lighting pick; the record lights (made without Light Placer).
//   illuminated.control          action = setting (key = a setting id or INI key, value = a whole number: changed exactly
//                                as the menu does - saved, applied at the next frame; the reply is the value it holds) |
//                                settings (every setting with its value, range and page) | item (form = a spell or weapon's
//                                form id, choice = auto, off or r,g,b: its Lights by Item choice, as the page sets it).
//   menu invoke name=illuminated set=setting key=... value=... - the same, kept for scripts that already use it.
//   event illuminated.settingChanged {id, ini, value} for each change made through DevBench.

#include "Plugin.h"
#include "Fade.h"

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
			R"json({"description":"Illuminated (spell, weapon and effect lights on Vanilla, ENB or Community Shaders, with an SKSE menu): change its settings. action=setting sets one setting by its id or INI key to a whole number exactly as the menu does (saved, applied at the next frame) and replies with the value it now holds; action=settings lists every setting with its id, INI key, value, range and menu page; action=item sets one spell's or weapon's Lights by Item choice (form, choice = auto, off or r,g,b) as the page does. Read the lights with inspect kind=illuminated.","inputSchema":{"type":"object","properties":{"action":{"type":"string","enum":["setting","settings","item"]},"key":{"type":"string","description":"setting: a setting id or INI key (action=settings lists them)"},"value":{"type":"number","description":"setting: a whole number (a switch 0/1, a choice's index, a slider's percent)"},"form":{"type":"string","description":"item: the spell's or weapon's form id, e.g. 0x0002B96B"},"choice":{"type":"string","description":"item: auto, off, or a color r,g,b (0-255 each)"}},"required":["action"]}})json";

		constexpr const char* kMenu =
			R"json({"description":"Illuminated - set=setting key=<setting id or INI key> value=<whole number> changes a setting exactly as the menu does: saved, and applied at the next frame. The same as the illuminated.control tool.","inputSchema":{"type":"object","properties":{"set":{"type":"string"},"key":{"type":"string"},"value":{"type":"number"}}}})json";

		[[nodiscard]] json InspectNow()
		{
			json out;
			out["settings"] = json::object();
			for (const auto& s : Settings()) {
				out["settings"][s.id] = s.value.load();
			}
			out["lighting"] = json::parse(LightingReport());
			out["records"] = json::parse(RecordReport());                                             // the game's own light records, lit without Light Placer
			out["groundLights"] = { { "now", GroundLightCount() }, { "made", GroundLightsMade() } };  // GroundLights.cpp
			out["copies"] = json::array();
			for (const auto& c : LightCopies()) {
				if (c.form) {
					out["copies"].push_back({ { "id", c.id }, { "base", c.base }, { "flicker", c.flicker }, { "fade", c.form->fade },
						{ "radius", c.form->data.radius }, { "cutoff", c.form->data.fallofExponent }, { "madeFade", c.startFade },
						{ "madeRadius", c.startRadius }, { "madeCutoff", c.startCutoff } });
				}
			}
			return out;
		}

		json SettingJson(const Setting& a_s)
		{
			return json{ { "id", a_s.id }, { "ini", a_s.ini }, { "value", a_s.value.load() }, { "default", a_s.defaultValue }, { "min", a_s.minValue },
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
			if (action == "item") {
				const auto    form = DevBenchGlue::Text(a_args, "form");
				std::uint32_t id = 0;
				const auto    digits = form.starts_with("0x") || form.starts_with("0X") ? std::string_view(form).substr(2) : std::string_view(form);
				if (std::from_chars(digits.data(), digits.data() + digits.size(), id, 16).ec != std::errc{} || !RE::TESForm::LookupByID(id)) {
					return DevBenchGlue::Refusal("item needs form: a loaded spell's or weapon's form id");
				}
				if (!Fade::Items::SetFromText(id, DevBenchGlue::Text(a_args, "choice"))) {
					return DevBenchGlue::Refusal("item needs choice: auto, off or r,g,b");
				}
				return json{ { "ok", true }, { "form", form }, { "choice", DevBenchGlue::Text(a_args, "choice") }, { "note", "applied to the lights at the next frame" } };
			}
			if (action != "setting") {
				return DevBenchGlue::Refusal("unknown action '" + action + "'", json::array({ "setting", "settings", "item" }));
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
					DevBenchGlue::Emit("illuminated.settingChanged", json{ { "id", settings[i].id }, { "ini", settings[i].ini }, { "value", settings[i].value.load() } });
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
				// the light copies are game forms the main thread writes: read them there
				const auto now = DevBenchGlue::OnMainThread([]() { return InspectNow(); });
				DevBenchGlue::Reply(a_sink, a_write, now ? *now : DevBenchGlue::NotRunYet());
			} catch (...) {
				DevBenchGlue::Reply(a_sink, a_write, DevBenchGlue::Refusal("Illuminated could not build its report"));
			}
		}

		void Control(void*, const char* a_args, void* a_sink, DevBenchAPI::WriteFn a_write) noexcept
		{
			try {
				// a change is made on the main thread, as the game's forms and the lit scene are its
				const auto now = DevBenchGlue::OnMainThread([args = DevBenchGlue::Args(a_args)]() { return Act(args); });
				DevBenchGlue::Reply(a_sink, a_write, now ? *now : DevBenchGlue::NotRunYet());
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
