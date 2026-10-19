# UI scale default on Retina

Facts (verified by running, build/mac-ui, docs_metal/ui_r2_*.png, ui_native.png):

* `_uiScale = GameSettings::uiScale / 100` is layout units per device pixel. A SMALLER value
  gives a BIGGER UI. The earlier note in INPUT_WINDOW.md ("raise to 200") had the direction
  backwards: 200 makes the UI smaller still. On a density-2 window the physical-size match
  is uiScale 50 (= 100 / density).
* Retail's slider range is 75..150, hard-coded in `OptionsDialog` (code, not Options.rsc).
* Change: on first run (no `GameUIScale` key in settings.cfg) and pixel density > 1,
  `GameSettings::Load` sets uiScale to `MacDisplay::DefaultUiScalePercent()` (50 at density 2).
  Density 1 and any saved value are untouched. The Metal backend records the density
  (`src/platform/posix/mac_display_defaults.h`).
* The Options slider range is retail's divided by the density (38..75 at density 2) so 50 is
  reachable; identity at density 1.
* Caveat: a value saved at density 2 (e.g. 50) is a large UI if the same settings.cfg is later
  used at density 1.
