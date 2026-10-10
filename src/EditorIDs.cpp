// Illuminated - SKSE plugin
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE.txt and the notice at the top of main.cpp.
//
// Editor IDs: the game throws most of them away while it loads, so they are recorded as each form arrives - by the
// fading module's recorder (FadeEditorIDs.cpp), the one hook on that vtable slot; main.cpp asks it for every form type
// the passes read. This file is the passes' way in, and the log's label.

#include "Fade.h"
#include "Plugin.h"

namespace Plugin
{
	std::string EditorID(const RE::TESForm* a_form) { return Fade::EditorID(a_form); }

	std::string Label(const RE::TESForm* a_form)
	{
		const auto  id = EditorID(a_form);
		const auto* file = a_form ? a_form->GetFile(0) : nullptr;
		return std::format("{} | {:08X} | {}", id.empty() ? "(no editor ID)" : id, a_form ? a_form->GetFormID() : 0,
			file ? file->GetFilename() : "(created)");
	}

	void RememberEditorID(const RE::TESForm* a_form, const char* a_id) { Fade::RememberEditorID(a_form, a_id); }
}
