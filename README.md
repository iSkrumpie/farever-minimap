# Farever Minimap & DPS

![Farever Minimap](minimap.gif)
![Farever DPS meter](dpsmeter.gif)

A drop-in overlay for Farever (Shiro Games) with two tools in one DLL:

* **Minimap**: compass with a heading arrow, world mosaic underneath,
  the POIs the game already tracks (obelisks, respawn points,
  dungeons, world activities, merchants), and 800+ optional
  collectible markers (chests, red orbs, plants, ores) you can
  toggle in.
* **DPS meter**: per-skill damage table with real in-game icons,
  fight history, and combat-state tracking. Only counts the damage
  numbers the game itself shows above mobs, so other party members,
  ambient world damage and bleeds on you are out of the picture by
  construction.

## How to install

1. Grab the latest `.zip` from the [Releases page](../../releases).
2. Extract straight into your Farever folder, the one that contains
   `Farever.exe`. Typical Steam path:
   `C:\Program Files (x86)\Steam\steamapps\common\Farever`.
   You should end up with `dinput8.dll` and a `data\` folder sitting
   next to the game's executable.
3. Launch Farever from Steam.

There is no injector. Windows resolves `dinput8.dll` from the game
folder before its own copy, so dropping the file next to
`Farever.exe` is enough. To uninstall, delete `dinput8.dll` and the
`data\` folder.

The overlay shows up a few seconds after the title screen, once
your character is loaded. It hides itself during loading screens
and zone transitions, and comes back as soon as the next player
Hero is spawned in the world.

## Controls

Default hotkeys (all rebindable in game, see below):

| Key | Action |
| --- | --- |
| F8  | Toggle the minimap |
| F10 | Toggle the DPS meter |
| F9  | Reset the current DPS pull |

To remap, click the small key icon on the minimap bezel. A
"Hotkeys" window pops up with three slots. Click a slot, press the
key you want, done. The binding is written to `data\keybinds.json`
so it persists across restarts. Esc cancels a rebind in progress.

The minimap bezel buttons (and yes, you can drag them around the
rim to wherever you like, see Layout below):

* Pin: hold and drag to move the minimap around the screen
* Square: cycles three sizes (small, medium, large)
* Funnel: opens the POI filter panel
* Padlock: locks / unlocks the whole overlay layout
* Key: opens the hotkey rebind window
* Chest: toggles all four collectible categories at once
* Plus and Minus: zoom between roughly 10x and 20x
* Minus on top: shrinks the minimap to a small puck; click the puck
  to expand it again

The DPS window has a custom padlock toggle at the very start of the
status line that does the same job as the bezel padlock. Clicking
either locks every overlay window in place. The standard ImGui
title-bar collapse arrow on the left also shrinks the window to one
row when you want it out of the way. Drag the title bar to move.

## Collectibles

The minimap also knows about 821 collectible spawn points across
the W1 world: 147 chests, 199 Red Orbs (the secret-world ones),
311 plants and 264 ore nodes. They are off by default. Toggle them
all at once with the chest button on the bezel, or pick categories
individually in the filter panel.

To mark a chest or red orb as "done", **right-click it on the
minimap**. The icon stays visible but dims, and the filter panel
shows a "12 / 147" counter so completionists know how many are
left. The done set is written to `data\poi_done.json` and persists
across sessions. Plants and ores respawn, so they have no counter
and can't be marked done.

POI icons grow about 80% on mouse hover, which makes hitting the
right one much easier when several are stacked.

## DPS meter

* **Real skill icons** for every class skill, weapon skill and
  basic attack. At the first damage event for each skill the mod
  reads the icon's atlas, cell coordinates and size out of the
  game's own data, then loads that atlas on demand. No per-class
  configuration needed.

* **Combat state** indicator next to the elapsed time. Red dot
  while in combat, grey dot when idle. The mod uses the game's own
  `isInCombat` flag, so dodging or blocking without dealing damage
  keeps the fight open instead of prematurely sealing it.

* **Bleeds and incoming damage are filtered out**. Every floating
  damage number whose target is your own character (mob hits,
  ground AoE, DOTs on you) is dropped before it reaches the meter,
  so the totals are exclusively your outgoing damage.

* **Fight history**: a collapsible block under the live table shows
  the last 10 sealed fights, with time, duration, total damage and
  DPS for each. Click a row to open a detail window with the full
  per-skill breakdown of that fight.

* **Responsive layout**: as you shrink the DPS window, columns
  drop progressively (Crit%, Max, Hits, Total, %). At the narrowest
  size you get just the icon and the DPS column.

## Atlas (prototype, v0.4.1)

New in 0.4.1: a built-in catalog of every item, unit, dungeon and
skill in the game, pulled from the localized CDB that ships in
`res.pak`. Click the **book button on the minimap bezel** to open it.

Tab labels inside the mod stay in German because that's the
localization we read out of the game's CDB; the English mapping is:

* **Ausrüstung** (Equipment): 462 items with names, flavor text
  and slot info, sub-filtered by Waffen (weapons) / Rüstung
  (armor) / Schmuck (jewelry).
* **Units**: 357 enemies and NPCs. Every named boss is in here:
  Krabbzilla, Königin Honigzabeth, König Ratsar, Fürstin Biene,
  Munster Truk, Phrixes.
* **Aktivitäten** (Activities): 122 activities, dungeons and
  world events.
* **Skills**: 576 skills with names and flavor.

Items show their icon and stats (level, iLevel, rarity, sell
price) once you have actually seen the item in the game once,
in an inventory, on a vendor, in a chest drop or on the ground.
Those captures persist to `data\item_captures.tsv` and grow
incrementally across sessions, so the longer you play with the mod
the more populated the atlas gets. Items you have not encountered
yet still show name and flavor text, just no icon or numbers.

**This is a prototype.** Drop sources ("which boss drops what")
are not wired in yet; that's the next step. Expect occasional
captures that fail to resolve an icon for the more unusual item
types (mounts, gliders, cosmetics) - those use a different
asset path than the equipment atlas.

## Layout

Click the padlock (either on the minimap bezel or in the DPS-meter
status line) to lock the overlay. While locked, nothing moves:
title-bar drags, the minimap pin, and the bezel button right-drag
described next are all disabled. Click the padlock again to unlock.

While unlocked, **right-click and drag** on any bezel button to
slide it around the minimap rim. Useful if you want the most-used
buttons in a different spot than the defaults. The layout is saved
to `data\ui_state.json` together with the lock flag. Delete that
file to reset to defaults.

Window positions for the DPS meter, the minimap, the hotkeys window
and the fight-detail view are saved to `data\farever_layout.ini`.
Drag your windows wherever once; they come back there next time.

## Troubleshooting

If the overlay starts hitching, F8 and F10 turn the two windows off
independently. The mod also auto-disables itself if it detects the
GPU stalling on the overlay for too long in a row, so you should
not need a game restart to recover from a bad transition.

If the minimap or DPS meter does not come back right after a
dungeon load, give it ten or fifteen seconds. The mod waits for
the new player Hero to spawn on the client and locks onto it as
soon as the allocation comes through, which on slow loads can take
a bit longer than the loading screen itself.

If the game still crashes after a while with this version, please
zip the `farever-mod.log` file from your Farever folder (close the
game first, do not restart it - the log gets overwritten on launch)
and open an issue with the file attached. The log records what the
mod was doing at the moment of the crash and is the fastest way to
narrow the cause.

## What's new in 0.4.1

This is a stability-focused update on top of 0.4.

* Hardened every long-lived pointer cache against the Boehm GC's
  slot-reuse pattern, which was responsible for the dungeon-entry
  access violations a few users reported. Every entity, chest and
  item the mod tracks is now compared against its expected HashLink
  type before any field is read; mismatches drop the pointer silently
  instead of walking a freed slot.
* Bounded all GPU fence waits on the texture loader to 2 seconds
  (previously unbounded), so a stalled atlas load can never freeze
  the game's Present.
* Added the Atlas window described above as a prototype.

## Notes

The mod is read only. It reads your own player state out of the
game process and renders its own swap chain on top of Direct3D 12.
It does not write to game memory, it does not touch the network,
and it does not look at other players' positions or damage.

This is fan made and is not affiliated with Shiro Games. All
Farever assets remain the property of their respective owners; the
release zip bundles a subset of the game's UI textures and map
tiles for runtime display only.
