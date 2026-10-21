# RESTRICTEDFOODS hand-in (2026-10-19 / 2026-10-20)

Branch `ext-food-eating`, based on `main` `84b3ea0` (the DESIGN.md rulings of 2026-10-20). The task: an extension that lets the player choose foods that citizens do not eat, with an in-game window to choose them. **Result: `ext.restricted_foods`, one class C toggle (locked for the game, per-town state recorded in the save), off by default, with a Foods page in the shared in-game extension window.
Built and unit-tested; the page opened in a running game and a restricted food was not eaten (a Windows run by the author, 2026-10-19). the checked list survived a save, quit and reload (a Windows run by the author, 2026-10-20). NOT yet looked at: the second page's tab, the resave golden under the official fixture (section 7).**

Built to the rulings of DESIGN.md of 2026-10-20: the lock is on activation, a live window editing class C state is fine, and per-town state travels in the tail record.

## 1. What it does
Chosen Edible materials stop being EATEN by citizens while staying Edible for everything else (storage by category, markets, recipes, trade). The motivating case: raw Wheat is directly edible, so citizens eat it from the barn instead
of the Bread a mod's bakery makes from it.

Retail decides "may a citizen eat this" in exactly two places, both a test of `RawMaterialDescription::_flags` bit 5 (read in the retail exe):

| place | retail | what the extension does |
|---|---|---|
| `CitizenComp_Entity_impl_ef40`, category 0 (`citizencomponent.cpp`) | the storage item picker the eating action uses; two passes over the same bit test | a blocked material's bit counts as clear in BOTH passes, so it is never eligible and never picked |
| `ResidenceComponent::GetFoodCount` (`citizenaicomponent.cpp`, 0x14012a9d0) | the Edible count of a home's storage; gates "does my home have food", the starving icon, the home's food check | a blocked material is not counted |

Nothing else changes: the material keeps its flag, so it is still stored, sold, traded and cooked. With the row off, or on with nothing checked, both functions are retail's own, in retail's order.

## 2. Class C: the state, and where it lives
The state is the town's **rules list**: which foods are not eaten. It is per-town, so the test in DESIGN.md ("must a reload restore it for the town to play on as saved?") makes the row class C, and the list never lives in `settings.cfg`.

| | |
|---|---|
| row | `restricted_foods`, `Tier::Extension`, `Class::C`, default OFF, `formatVersion` 1 (`registry.cpp`) |
| record | the row's own record in the tail chunk (`savetail.h`); written whenever the row is active for the game, **even with an empty list** (the lock set must not depend on content); zero bytes for a faithful game |
| codec | `Ext::RestrictedFoods::Codec()` (`src/ext/restricted_foods/save_state.cpp`), registered in `src/ext/state_codecs.cpp` (`kCodecs`, one line) |
| wire format v1 | little-endian: `u32 count`, then `count` x { `u32 length`, `u8 entry[length]` }; each entry is one rule in the grammar below |
| by name | entries name a material by internal name, a food group, `all`, an `allow:` exemption or `blocking=off`. Never an index: mods change the load order between sessions. A name the game no longer has matches nothing |
| load | `ExtSave_ReadTail`: after the lock took the saved set, `SaveTail::ApplyState` hands every record's payload to its codec; a payload that does not parse (cut short, bytes left over, a layout this build does not know, a rule it cannot read) is fatal through the same box as a damaged manifest |
| reset | `SaveTail::ResetAllState()` runs after `LockForGame` (a new game or a load starts empty) and after `UnlockForGame` (the town's state goes with the town) |
| row off for the town | the codec is never called; the next save carries no record |

Because the save names `ext:restricted_foods`, retail (and a build without the extension) shows the missing-mod notice instead of playing the town under different rules; with the extension disabled our loader plays the town without the feature and drops the payload at the next save.

### The rules grammar
One entry per rule:

| entry | meaning |
|---|---|
| `Wheat` | block one material by its internal name (`RawMaterialDescription::_name`) |
| `Template\RawMaterialWheat.rsc:rawmaterial` | the same, written as the resource path (the name is cut out of it) |
| `group:Grain` | block every Edible material in a food group: `Fruit`, `Vegetable`, `Protein`, `Grain`, or a number (`0x2`) |
| `all` | block every Edible material |
| `allow:Fish` | exempt a material; beats every blocking rule |
| `blocking=off` | disables the whole list |

The page writes names, groups, `all`, `allow:` and `blocking=off`. Unchecking a food that a `group:` or `all` rule blocks adds an `allow:` entry instead of losing the rule. With no rules the row blocks nothing.

## 3. The page (the shared extension window)
Per DESIGN.md "Surfaces", the in-game controls are not a button per feature: the main toolbar gets ONE extension button, which opens one window with a tab per active extension page. This is the first extension that needs it, so this hand-in also builds the host
(`src/ext/toolbar/`, `toolbar.h` has the contract):

| | |
|---|---|
| button | one button in the Reports group (Status Bar, Limits, Town Hall, Help), sort priority 950, between Town Hall and Help; built from the toolbar's utility-button template (template 3). The toolbar keeps a **null** entry in `toolbarItems` for it and the retail readers of that array skip it. Not added when no page is offered (no active contributor), outside a running game, or under the gate's command-line faithful profile: then the toolbar is retail's. No hotkey (the group's ten are taken). Icon `Toolbar_extensions.png`, the Extensions menu's cog at 32 px |
| window | `Dialog/ExtensionsWindow.rsc`: a utility-style window like Limits and Status Bar (not modal, does not pause the game, the game's own title bar, remembers its place, closes with its close button or the toolbar button). Its body is ONE tab control with no tabs of its own; `UI::TabElement::AddTab` adds one per active page when it opens, as the Options dialog's Enhanced tab is added |
| page | `Contribution { id, caption, page, open, changed, close }`: the page is offered only while its row is active for the game. The controls are **live**: `changed` writes the state at once (no OK / Apply) |
| this page | `Dialog/RestrictedFoodsPage.rsc:pageRestrictedFoods`, controller `src/ext/restricted_foods/toolbar_page.cpp` |

**The Foods page.** One cell per edible food of the running game (every loaded `RawMaterialDescription` with the Edible bit, once per internal name, sorted by the name the game shows), laid out like the Town Hall's Inventory tab: a four-column table of
cells, each a check box plus the Town Hall's compact item label (20 px icon, small font), so a modded game's hundreds of foods are a short scroll. Checking a food applies it at once; the cell tints red while the food is not eaten.

**The filter.** The page opens on the foods **the town has had** (`CitizenBehavior::field_0x110`, the town's known-material list: `RawMaterialComponent::Startup` appends a template the first time an entity of it is created, nothing removes it, so it is the Town Hall inventory's
"ever made" list). A button under the list flips to **all foods** (needed to block a food before the town makes it) and back; the choice is kept for the session. Checked foods never disappear, and a flip rebuilds the cells but keeps every check. A town with no known foods opens on all.

Two small differences from the sketch in DESIGN.md, because the page needed them: `Contribution` carries `caption` (the tab's text, a key in `ExtensionsStringTable.rsc`) and `changed` takes the element id and returns whether the page owned it (the window forwards a click to each page in turn).

## 4. Where
| file | change |
|---|---|
| `src/ext/restricted_foods/restricted_foods.{h,cpp}` (new) | `ParseRules`, `FormatRules`, `Blocks`, `SetBlocked`; the live state: `g_on`, `SetRulesText`, `CurrentRules`, `ApplyRules`, `IsBlockedNow`, `BeginFrame` (reads the row once per frame), `Blocked` / `BlockedSlow` (a per-description-pointer verdict cache dropped on rule change and on a lock-epoch change) |
| `src/ext/restricted_foods/save_state.cpp` (new), `src/ext/state_codecs.cpp` (new) | the codec and its registration (section 2) |
| `src/ext/restricted_foods/toolbar_page.cpp` (new) | the Foods page (section 3) |
| `src/ext/savetail.{h,cpp}` | `StateCodec`, `Writer` / `Reader` (promoted for the first codec), `FindCodec`, `ResetAllState`, `ApplyState`; `ActiveRecords` calls the codec's `encode` |
| `src/ext/toolbar/toolbar.{h,cpp}`, `toolbar_window.cpp` (new) | the shared host: the page list, `Offered`, the window, `InsertAt` / `Style` / `Toggle` for the button |
| `src/ext/ui/find_element.h` (new) | `Ext::FindElement<T>`, the generic form of `EnhancedTab::FindIn` |
| `src/game/Dialog/toolbardialog.cpp` | the button's seams: the null item, `InsertAt` / `Style` while the Reports group is built, `Toggle` on the click, the null guards in the three readers |
| `src/game/Component/citizencomponent.cpp`, `citizenaicomponent.cpp` | the two eating seams (3 lines), each after retail's own bit test |
| `src/game/Game/gameupdateloop.cpp`, `gameupdateloop_load.cpp`, `gameupdateloop_serialize.cpp` | `BeginFrame()` beside `SpreadWarmth::BeginFrame()`; `ResetAllState()` after the lock and after the unlock; `ApplyState` after the manifest's lock in `ExtSave_ReadTail` |
| `src/ext/registry.{h,cpp}` | the row (class C, version 1, OFF) |
| `src/ext/ui/Dialog/ExtensionsWindow.rsc`, `RestrictedFoodsPage.rsc` (new), `ExtensionsStringTable.rsc`, `ExtensionsSprites.rsc`, `CMakeLists.txt`, `src/ext/resources/resources.cpp` | the window and page resources, their texts (`ExtensionsWindow*`, `RestrictedFoods*`), the embed list and the provider prefixes |
| `tools/ext_icons/restrictedfoods.py`, `tools/ext_sprites.py`, `Entry_restricted_foods.png`, `Toolbar_extensions.png`, `ext_sprites_data.h` | the row icon and the toolbar button's icon (programmer art), regenerated with `tools/ext_sprites.py` and `tools/ext_sprites_bake.py` |
| `tests/unit/ext_restricted_foods_test.cpp`, `ext_toolbar_test.cpp` (new), `ext_rsc_text_test.cpp`, `tests/unit/sources.cmake` | the tests (section 7) |

No engine change and no new setting keys beyond `ext.restricted_foods` (the row's own toggle).

## 5. Behaviour notes
* **Off is retail.** Each seam tests `Ext::RestrictedFoods::g_on` first (a load and a not-taken branch); with the row off, or on with no rules, `g_on` is false and both functions are retail's own, in retail's order. Under the faithful profile the row is forced off.
* **Identity is the internal name**, not the file path (OpenBA's `RawMaterialDescription` does not carry its resource path). Two materials from different mods with the same internal name are treated alike (the page lists one cell for both).
* **The verdict cache** is keyed by the material's description pointer, a side table in the extension (no field in a retail class), dropped when the rules change and when the lock epoch changes (a new game, a load, a mod reload).
* **Starvation.** `all` (or a list that blocks every food a town has) leaves citizens with nothing they may eat; they starve as in a town with no food. That is what the rule asks for.
* **The row cannot be switched on in a running town** (DESIGN.md: the lock is on activation). Turn it on in the Extensions menu before starting the town; what the page edits is live from then on.

## 6. What changed from the first version
The class (A to C), the storage (`settings.cfg` to the save tail), the control (a Foods window opened from a Foods button in the main / pause menu and a Foods button in the toolbar, to one page in the shared window under one extension button), and the names (`RestrictedFoods` to `Ext::RestrictedFoods`, the string keys and element names to the `<Id><What>` standard). The pause-menu button and its seams in `mainmenudialog.cpp` are gone, as is `Persistence::SetString`.

## 7. Verified / not verified
* VERIFIED on the rebased branch (`main` `84b3ea0`, MinGW on Windows): unit 1210 / 0 failures / 6 skipped, `tools/acceptance/acceptance.sh` 0 lines off target; `OpenBA.exe` builds with every seam and the window code. Tests: the registry row (class C, off, version 1, the faithful profile turns it off); the codec's round trip (stable: encode(decode(x)) = x), an empty list is still a valid record, a cut-short / over-long / unknown-version / unreadable-rule payload is refused; the state travels in the tail through `ActiveRecords`, `Encode`, `Decode`, `Validate`, `ApplyState` and is gone after `ResetAllState`; the rules grammar and matching; the seam is inert by default;
  the page list is offered only while the row is active and never under the faithful gate; the embedded `.rsc` files parse and use only types `Extensions.rsc` already uses.
* LOOKED AT in a running game (a Windows run by the author, 2026-10-19): the window opens from the toolbar button and shows the foods; a restricted food is not eaten. (The earlier standalone Foods window, the check boxes, the filter and the four-column list were looked at in a running game before the rework.)
* LOOKED AT (a Windows run by the author, 2026-10-20): three foods checked, saved, quit to the menu and loaded again: all three were still checked.
* LOOKED AT (a Windows run by the author, 2026-10-20): a new town with the row off has no extension button (the toolbar is retail's); loading the save made with the row on shows the notice that the extension is being turned back on because the save needs it, and the three checked foods come back.
* RESAVE (2026-10-20, measured on the branch before its rebase onto `main` `84b3ea0`; `tools/resave_gate/resave_gate.sh` in roundtrip mode, in a scratch game directory, `/profile:faithful` for the first leg; the repo's own `Gate.sav` fixture was not available, so the fixture is a copy of the author's 2026-10-18 `Auto.sav`, which carries a class B manifest):
  * **Faithful leg, this branch against a build of `main` `319a6f5`** (the baseline is its own reference, since no golden exists for this fixture): the resaved image has the SAME size (31,712,550 bytes) and every difference (4,284 bytes, three regions) is the same bytes in a different order: records keyed by raw memory address, which the gate's README says move between builds (the `CitizenBehavior` bucket trees). The same neighbourhoods vary by 22 to 26 bytes between two runs of ONE build. So the faithful path is not byte-identical to `main` across builds on this fixture, and the differences are order-only; a same-build golden under the official fixture is the check that settles it.
  * **Class C leg** (the author's food save, `ext:restricted_foods` in its manifest; per-entry profile, the manifest locks the set): the resave is byte-identical across two runs (PASS against its own golden, 44,440,446 bytes), the tail record is 242 bytes, and the `restricted_foods` payload (31 bytes, wire v1) decodes to `bean`, `fish`, `pumpkin`, the three foods the user checked. A save made with the row off carries no `restricted_foods` record.
* LOOKED AT in RETAIL (a Windows run by the author, 2026-10-20): a save made with the row on (and the other class B rows) loads in the retail game. Retail shows its missing-mod dialog once per extension ("The following mod can not be found or cannot be loaded", e.g. `Better camera [ext:better_camera]`, "Saved with version 1"), and after Yes on each the game loads fine: it reads the save and ignores the tail. A town saved from retail and loaded again here has no `restricted_foods` record (retail writes only its own state), so it plays as a town without the extension, as DESIGN.md describes for an extension that is not available.
* NOT VERIFIED: a second extension's tab (only one page exists, so `AddTab` has only ever added one); the faithful gate (resave x5) under the official `Gate.sav` fixture, and a class C golden checked into `docs/runtime_port/resave_gate/golden/` under it. `tools/acceptance` (0 off target) and the ABI audit (OK) pass on this branch.
* To try it: build, turn **Restricted foods** on in the Extensions menu, start a town, press the new button in the Reports group of the toolbar, check a food. The log lines to read are `[ExtToolbar]`, `[RestrictedFoods]` and `[ExtSave]` on stderr.
