# Fixes and extensions -- the design (decided 2026-10-11 with the user)

The port is faithful: `OpenBA.exe` plays retail's game byte for byte. This sheet says how we
change that game on purpose -- fixing retail's bugs, adding features retail never had -- without
losing the faithful build, its gates, or anyone's saves. Rules first, then the shape, then what
every agent must know. Status and the inventory live beside this file (`INVENTORY.md`, `STATUS.md`),
never here.

## 1. Facts the design rests on

- **The save format is retail's and stays retail's.** Every class serializes under its own small
  version (`IO::Serializer(stream, version)`, branches on `GetVersion()` on read). A save carries a
  mod list: one `Save::SaveLoad::ModInfo` per mod (name, path, versionMajor, versionMinor,
  gameVersion). On load `Save::SaveLoad::CheckMod` (retail 0x1800c1090) turns a mismatch into the
  player's known modals: 1 modsMissing, 4 modsChanged, 3 modsVersion, 2 modsOutOfDate.
- **A `.pkm` mod is data only** (compiled `.rsc`, a priority, version pair, overridable flag). No
  code, no dependency list. Load order: `.pkg` / `.pkm` pack stack first, then loose files for
  names NO package holds (`IO::FileSystem::OpenFile`, `filesystem_openfile.cpp:337`): a loose file
  never overrides a packaged object, so every object we ship gets a new name (AOSPIKE S5).
- **`settings.cfg`** (`src/engine/io/settings_file.h`) is a flat key=value map read once at boot by
  `IO::Persistence::ReadSettings` and flushed by `WriteSettings`. `BANISHED_*` env flags are
  harness-only (`src/app/options.h`) and frozen: no new ones (DIRECTION.md §2).
- **The recorded candidates**: `docs/retail_bugs.md` (3 bugs, each with its "faithful lever"),
  the ~59 `DEVIATION` comments in `src/`, and the user's rulings in `docs/known_port_issues.md`
  (rows 452, 1162, 1295 among them).

## 2. The three tiers

| Tier | What | Save format | Default | Where the gate runs |
|---|---|---|---|---|
| 0 Faithful | retail's behaviour | retail's | -- | here, and only here |
| 1 Fix | a retail bug corrected at its faithful lever | retail's, unchanged | **ON** | boot/stress/quit only |
| 2 Extension | a feature retail never had | by class (below) | **class A ON, B / C OFF** (ruled 2026-10-18; `eye_camera` is class A but OFF: it changes how the game is played) | boot/stress/quit only |

A **fix** is one branch at the one site `retail_bugs.md` names as the lever, retail's path kept
in the other arm. It never changes what is serialized. It may change sim outcomes (that is the
bug), so a save written under fixes loads in retail and in faithful mode with no warning.

An **extension** carries a **class**, which is part of its type and shown in the menu, because
the class says what a toggle costs the player:

| Class | Save impact | Toggle | Examples |
|---|---|---|---|
| **A** | none: no serialized state, no sim effect | live, any time | eye-level citizen camera, renderer work, sharp UI at high scale |
| **B** | sim-divergent, format-neutral | locked for the game's lifetime; recorded in the save | pathfinding changes |
| **C** | format-extending | locked for the game's lifetime; recorded in the save | anything with new per-entity or per-town state |

Class C state is written in a chunk at the **tail of block 1 (the compressed game-state block)**,
after the last faithful Serialize and before the stream closes (`gameupdateloop_serialize.cpp`,
the save arm of `GameUpdateLoop::SaveRestore`), never inside a retail class's serialization. Retail's
loader stops where it always did and ignores the tail; the block size and CRC cover it automatically.
NOT after the file end: retail checks the exact file size and marks such a save bad (SAVETAIL §1).
Every enabled B/C extension (B included) writes a small manifest record (id, format version) into
that tail; our loader locks the game's extension set from the manifest, because the scenario,
tutorial and autoload routes never read the save's mod list (SAVETAIL §4).

**The save-compat mechanism for B and C.** An enabled B/C extension writes itself into the save's
existing mod list as a pseudo-mod: path `ext:<id>` (a reserved prefix no real `.pkm` path can
carry; an id never contains `.`, so it cannot match a file on Linux either), name = its display
name, ModInfo+0x18 = the extension's format version (the field retail's modal displays), +0x14 =
0x010007ff, +0x10 = 0 (it is the package CRC, never compared for a missing mod). Retail then loads
such a save through the standard modsMissing path -- a Yes/No modal per entry, Yes continues (retail
0x1400b61e0, SAVETAIL §3) -- the same warning a missing `.pkm` gives, which is the paradigm the player
already knows; the tail chunk is never read. Our own loader splits the `ext:` entries out before
CheckMod and shows its own notice for a missing or disabled extension. No header field changes.
Toggling a B/C extension mid-game reuses `Dialog/Reload.rsc:game`, as the Mods dialog does.
Both write seams (pseudo-mods in `Save::SaveLoad::SaveMetaData`, the tail chunk) write ZERO bytes
when no B/C extension is enabled: the resave gate byte-compares the whole decompressed block 1.

## 3. Shape in the tree

```
src/ext/                      the only home of non-faithful code
  registry.h / registry.cpp   the one table: id, tier, class, default, display name, description,
                              format version (C only); Ext() -- the read-once struct of toggles,
                              filled from settings.cfg at boot (keys `ext.<id>=0|1`, `fix.<id>=0|1`)
  fixes/<id>.cpp              a fix's helper, if its lever needs more than a line
  <id>/                       one directory per extension (its bodies, its .rsc sources)
src/game/Dialog/ExtensionsDialog.*   the menu, cloned from the Mods dialog shape
```

**Hook rules (performance and readability, decided 2026-10-11):**

1. A **fix** is `if (Fix().<id>) { corrected } else { retail }` at the lever, nothing else moved.
   The branch is tiny by construction (a load moved, a different component looked up).
2. A **class A extension** replaces a whole function, selected at the caller or at a seam that
   already exists (the camera controller, the UI layout pass). Live toggles are fine: not hot,
   not sim-affecting.
3. **Class B / C** replace whole bodies, selected at the highest level that works, **once per
   game** (at new game / load, when the class lock is taken). The faithful bodies are never edited.
4. **No toggle is read inside a hot inner loop.** Hoist the decision above the loop or split the
   function. No function-pointer tables, no hook vtables, no indirect-call-everywhere: an indirect
   call blocks inlining of the faithful body at every site and makes the faithful code unreadable.
5. **An engine-side seam is a null-by-default static data pointer** that the game side sets (the
   pseudo-mod provider, `Graphics::SunLight::_extShadowFitView`): `src/engine` never includes
   `src/ext`, the faithful path pays one null test, and the retail body it steers is unchanged. Never
   a virtual, never a function-pointer table.
6. **Measured.** Every extension's hand-in reports the stress run faithful vs. everything-on. Perf is
   recorded, not chased (the user's rule) -- EXCEPT a regression on the faithful profile, which is
   a defect: the faithful path must stay exactly as fast as today.
7. **No game content is distributed** (the user, 2026-10-14). The tree, the exe and anything built
   for handing out hold only our own code and our own resources: no retail shader source (the kit's
   `.srsl`) or DXBC, no hand port or translation of either, no retail texture, model, sound, string
   table or `.rsc` copy, and nothing derived from them. The kit and the `.pkg` are references to read
   (as Ghidra is) and inputs at build or run time on the user's machine. An extension that needs a
   changed version of retail data transforms the user's installed copy at load (the FOLIAGE_AA
   bytecode rewriter is the pattern). It never ships the changed copy. Hand-ins state it.

**Rules that carry over unchanged.** No doors (`tools/acceptance/acceptance.sh` at target: nothing
registers or resolves a body by retail name or address). No null guard on a faithful crash. Every
fix cites the retail address and quotes the lever from `retail_bugs.md`. No `getenv`. No new
`BANISHED_*` flags: a toggle is a `settings.cfg` key and a menu entry.

**Gates.** The existing gate (`tools/build.sh run`, resave x5 against the golden, unit, castmetric,
acceptance, worldcmp) runs with the **faithful profile** forced (`fix.*=0`, `ext.*=0` for the run).
A second, cheaper gate runs everything on and only has to boot, run the stress town and quit clean
(exit 0, no crash / hang log). A class C extension that ships gets its own resave golden under its
profile.

**Menu.** The front end gets an Extensions button beside Mods with its own icon (a new `.rsc` and
sprite is fine). The dialog lists fixes and extensions with the class letter, the lock state
("live" / "locked for this game"), the toggle, and the description. A B/C toggle in-game shows the
same restart/reload notice the Mods dialog shows.

## 4. Order of work

1. **Scaffold** (block EXTSCAFFOLD): `src/ext/registry`, the settings keys, `Ext()` / `Fix()`, the
   Extensions dialog + button + icon, the faithful-profile force for the gate, unit tests for the
   registry and the settings keys.
2. **Fix pilots** (block FIXPILOT): the three recorded retail bugs as `fix.icon_slide`,
   `fix.residence_goto`, `fix.tavern_counter`, each at its lever.
3. **Verification** (block SAVETAIL): DONE, see §5.
4. **Inventory** (block EXTINVENTORY): every `DEVIATION` comment, every RULED row and every
   retail bug classified restore / fix / extension(A|B|C) with the lever, into `INVENTORY.md`.
5. Then the first extensions, in the user's order: **A** eye-level citizen camera; **B**
   pathfinding; **A** sharp UI at high scale (font / UI scaling or integer upscaling).

## 5. Open until verified

- SAVETAIL: DONE 2026-10-11 (`handins/SAVETAIL.md`, all five VERIFIED): the chunk moved from the file
  tail to block 1's tail (above); modsMissing continues on Yes; `ext:` never matches a `.pkm`. Not
  clicked through in retail, proved from the bytes and the ToolKit's dialog resource.
- Whether a loose text `.rsc` (the Extensions dialog, its icon) is loaded by the shipped loader
  or must be compiled (`Tools-x64.exe`) into a `.pkg`; the scaffold block finds out and records it.
