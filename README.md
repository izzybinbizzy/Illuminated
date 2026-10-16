# Illuminated - SKSE plugin

Copyright (C) 2026 izzydoingit. GPL-3.0-or-later, see `LICENSE`.

The plugin of Illuminated - Spell & Weapon Light NG; it ships with the mod and is required by it.

## What it does
- Lights spells, enchanted weapons, staves, projectiles, explosions and hazards in each spell's own color. The lighting
  (Community Shaders, ENB or Vanilla) is detected automatically - nothing to pick.
- **With Light Placer** (Community Shaders): Light Placer hangs the lights on the effect models; the plugin takes the
  game's own magic lights off where the mod provides one, so nothing is lit twice.
- **Without Light Placer**: lights through the game's own light records, with a ground light at the caster's feet.
- **Sprays, streams and bolts**: each projectile wears a raised copy of its own light record, the way
  [ReLight](https://github.com/TrumanGIT/ReLight) by Truman lights them (GPL-3.0-or-later, used with permission).
- Fading, Dynamic Lighting, dim in daylight, a light budget for big fights, Lights by Item, presets, Brightness and
  Reach, the HUD (reticle and charge gems), VAER Reborn's swirls kept, and wards when Dynamic Wards is not installed.

## Settings
SKSE Menu Framework > Illuminated. Every setting is live. Values the menu does not show are in
`SKSE\Plugins\Illuminated\Illuminated - Advanced Settings.ini`. The charge gems' pre-rendered art is `HUD/*.dds`,
installed to `SKSE\Plugins\Illuminated\HUD\` (without it the plugin draws a plain gem).

## Requires
SKSE, Address Library for SKSE Plugins and SKSE Menu Framework (the settings page and the HUD).

## Third-party files
`src/DevBenchAPI.h` and `src/DevBenchAPI.cpp` are devbench's own interface files, MIT licensed by
their author (see `src/DevBenchAPI.LICENSE.txt`) and copied unchanged. devbench is not required.

`src/SKSEMenuFramework.h` is SKSE Menu Framework's own header by Thiago Kaique, copied unchanged from
[SKSE-Menu-Framework-3-Example](https://github.com/QTR-Modding/SKSE-Menu-Framework-3-Example), MIT licensed
(see `src/SKSEMenuFramework.LICENSE.txt`).
