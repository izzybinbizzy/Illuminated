# Luminous Arcana - SKSE plugin

Copyright (C) 2026 izzydoingit. GPL-3.0-or-later, see `../LICENSE`.

Ships with Luminous Arcana and is required by it. When the game has loaded its plugins it reads the
Luminous Arcana configs under `Data\LightPlacer\Luminous Arcana\` (at any depth) and, in memory:

0. makes its own copies of twenty of the game's magic lights, carrying the look Luminous Arcana's lights
   were made with, and points every magic effect, projectile, explosion and hazard that used one at the
   copy (the Light Placer configs state their own colors, so the game's light records keep theirs);
1. removes the game's own casting light from magic effects whose casting art those configs light;
2. does the same for projectiles, explosions and hazards whose model is lit, keeping cone and flame
   projectiles lit and always removing poison spray lights;
3. gives the Dragonborn poison rune the casting art its lit hand needs;
4. with the Spray Lights setting on, gives each spray projectile a stretched copy of its light,
   colored from the spray markers in its settings file;
5. leaves only the first light on an enchantment that carries two or more lit shaders (the originals are
   restored while the Crafting Menu is open and while a save is written, so a save never stores an
   in-memory copy).

Every choice is a setting: a game global each light's conditions read (`GetGlobalValue`), so a change
shows in game within a second. The settings come from `Data\SKSE\Plugins\Luminous Arcana\*.txt`,
the picks are saved to `Data\MCM\Settings\Luminous Arcana.ini`, and the pages are added to SKSE Menu
Framework's Mod Control Panel (the MCM uses the same INI). Passes 1 and 2 run again on every change.
The Brightness and Reach sliders set the fade and radius of in-memory copies of the lights the configs name
(`src/LightCopies.cpp`); Light Placer reads a light with no fade or radius of its own from its light record.

It changes nothing if Let There Be Glow's plugin (`LetThereBeGlow.dll`) is loaded - the two mods are
never used together. Every change, and how long the load pass took, is written
to `LuminousArcana.log`.

The source is generated from the Let There Be Glow plugin's source by the mod's build tool.

## Source layout

| file | what it holds |
|---|---|
| `src/main.cpp` | the order of work: what runs when the game loads, saves and loads a save, and the SKSE entry point |
| `src/Plugin.h` | what the files share: every function another file calls, and the structs they pass around |
| `src/PCH.h` | the headers every file includes, compiled once |
| `src/Text.cpp` | small text helpers |
| `src/EditorIDs.cpp` | editor IDs, recorded as each form loads because the game throws most of them away |
| `src/Configs.cpp` | reads the Light Placer configs and collects the models and shaders they light |
| `src/SprayMarkers.cpp` | reads the spray markers from the settings file |
| `src/Settings.cpp` | the settings: their globals, the INI, and the call the MCM makes when one changes |
| `src/LightCopies.cpp` | the light copies the Brightness and Reach sliders set |
| `src/Menu.cpp` | the Luminous Arcana pages in SKSE Menu Framework |
| `src/SKSEMenuFramework.h` | SKSE Menu Framework's own header (Thiago099/SKSE-Menu-Framework-3, GPL-3.0) |
| `src/FormCopies.cpp` | in-memory copies of lights, effect shaders and magic effects |
| `src/LightSettings.cpp` | pass 0 |
| `src/CastingLights.cpp` | pass 1 |
| `src/EffectLights.cpp` | pass 2 |
| `src/PoisonRune.cpp` | pass 3 |
| `src/SprayLights.cpp` | pass 4 |
| `src/Enchantments.cpp` | pass 5, and putting the original effects back while a save is written or the Crafting Menu is open |
