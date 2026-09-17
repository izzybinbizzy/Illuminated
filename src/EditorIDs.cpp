// Luminous Arcana - SKSE plugin
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE.txt and the notice at the top of main.cpp.
//
// Editor IDs: recorded as each form loads, because the game throws most of them away.

#include "Plugin.h"

namespace Plugin
{
	// ------------------------------------------------------------------ editor IDs
	// The game throws most editor IDs away while it loads. The passes' rules are written against them,
	// so every form type these passes read has its editor ID recorded as it arrives.
	std::unordered_map<const RE::TESForm*, std::string> gEditorIDs;
	RE::BSSpinLock                                      gEditorIDLock;

	std::string EditorID(const RE::TESForm* a_form)
	{
		if (!a_form) {
			return {};
		}
		{
			RE::BSSpinLockGuard guard(gEditorIDLock);
			if (const auto it = gEditorIDs.find(a_form); it != gEditorIDs.end()) {
				return it->second;
			}
		}
		const char* own = a_form->GetFormEditorID();
		return own ? std::string(own) : std::string();
	}

	std::string Label(const RE::TESForm* a_form)
	{
		const auto id = EditorID(a_form);
		const auto* file = a_form ? a_form->GetFile(0) : nullptr;
		return std::format("{} | {:08X} | {}", id.empty() ? "(no editor ID)" : id, a_form ? a_form->GetFormID() : 0,
			file ? file->GetFilename() : "(created)");
	}

	void RememberEditorID(const RE::TESForm* a_form, const char* a_id)
	{
		if (a_form && a_id && *a_id) {
			RE::BSSpinLockGuard guard(gEditorIDLock);
			gEditorIDs[a_form] = a_id;
		}
	}

	void ForgetEditorIDs()
	{
		RE::BSSpinLockGuard guard(gEditorIDLock);
		gEditorIDs.clear();
		gEditorIDs.rehash(0);
	}
}
