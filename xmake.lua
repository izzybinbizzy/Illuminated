-- Illuminated - SKSE plugin. GPL-3.0-or-later, see LICENSE.txt.
set_xmakever("3.0.0")
set_project("Illuminated")
set_version("1.0.0")
set_license("GPL-3.0-or-later")
set_arch("x64")
set_languages("c++23")
-- the DLL carries its own Visual C++ runtime, so an older runtime on a player's PC cannot stop it loading
set_runtimes("MT")
add_rules("mode.releasedbg")
set_defaultmode("releasedbg")

-- GLOW_VR=1 (set by PC Runner\pluginbuild.py for the optional VR download) builds for Skyrim VR only;
-- anything else is the SE + AE build every main download ships
local vr_only = os.getenv("GLOW_VR") == "1"
set_config("skyrim_se", not vr_only)
set_config("skyrim_ae", not vr_only)
set_config("skyrim_vr", vr_only)

includes("lib/commonlibsse-ng")

add_requires("nlohmann_json v3.12.0")

target("Illuminated", function()
    add_deps("commonlibsse-ng")
    add_packages("nlohmann_json")
    add_defines("NOMINMAX")  -- windows.h min/max macros break std::min/max and numeric_limits::max
    -- GLOW_FADE_DEBUG=1 (PC Runner\pluginbuild.py, debug=True) builds the TESTING DLL with the fading module's Debug page:
    -- it ships only in the PDB and Source download (his call 2026-10-09), never in the main one
    if os.getenv("GLOW_FADE_DEBUG") == "1" then
        add_defines("FADE_DEBUG_PAGE")
    end
    add_rules("commonlibsse-ng.plugin", {
        name = "Illuminated",
        author = "izzydoingit",
        description = "Illuminated - sets its magic lights and takes the game's own light off what Illuminated lights, in memory",
    })
    -- the source is split by job (see the file map at the top of src/main.cpp); every .cpp in src is built
    add_files("src/*.cpp")
    add_headerfiles("src/*.h")
    add_includedirs("src")
    set_pcxxheader("src/PCH.h")
    -- every warning in OUR files is an error (his C++ standard, 2026-10-07: "/W4 /WX ... fix all warnings"); the
    -- headers included with <...> (CommonLib, the standard library) are external and quiet
    set_warnings("allextra", "error")
    add_cxflags("/external:anglebrackets", "/external:W0")
end)
