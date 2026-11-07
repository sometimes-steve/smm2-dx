# Changelog

## 0.2.0 - 2026-11-07

The in-game menu is redesigned, installed mods now work, and a lot of small things around starting, restarting and updating are
fixed.

### Menu

- **New look, modelled on Ship of Harkinian's menu.** Settings pages are one pane: full rows, and a row's description appears as a
  tooltip when you point at it or select it with a controller. The description pane is kept for the Mods page only.
- **A General tab** (first in the list) with **Appearance** (Theme, Menu Size) and **Game** (Skip Intro, Skip Mode Select, Show
  Recomp Version). Theme and Skip Intro moved here from Video.
- **Menu Size presets:** Compact, Small, Normal, Large, Extra Large. The menu now grows more slowly than the window, so it no longer
  fills a small window or becomes huge on 4K, and a narrow window shrinks it instead of cutting it off.
- **Proper capitalization everywhere:** labels, headings and choices are in Title Case (Skip Mode Select, Match Display), and
  descriptions are sentences, instead of the old ALL CAPS.
- **Rows fit their content** instead of spanning the window, with the same minimum width on every page and in both themes (the NES
  theme's smaller font used to make its boxes half the size). Headings have a thin line under them, and rows only get a box when you
  point at them.
- **The panel is sized from the window and the top row**, so every tab is the same size and the whole row of tabs always shows.
- **Controls tab rebuilt** with the same rows as the other pages: Button Prompts, System Language, Controllers, one row per
  controller button and keyboard key (select it, then press the new one), reset buttons and the menu shortcuts.
- **Quit tab:** Restart the Game and Quit to Desktop, with the About section (Version, Check for Updates, New Version Available)
  below them. The update rows moved here from Online.
- **Tooltips checked against the game.** Corrected the Theme text, and removed text about other platforms and features this game
  doesn't have. The Aspect Ratio row is gone: it did nothing in this game, which always renders at its own 16:9.

### Restarting

- **Restart the Game** reboots the game inside the app, after a confirmation. Settings that apply at the next start take effect.
- **After you install a mod, the menu asks whether to restart the game now**, and "Restart the game to apply" no longer goes back to
  the setup screen.
- **Skip Mode Select now runs again after Restart the Game**, using whatever the setting is at that moment. Turning it on in the menu
  does nothing until the next start or restart, instead of starting its button presses wherever you happen to be.
- **Removed Quit to Start Screen and Game Files.** The game files are set up once, on first run.

### Mods

- **Installed mods now load.** Mods from the Mods tab were installed and listed as on, but never reached the game's files.
- **A mod's packs are merged into the game's, not swapped in.** Mods made for an older game version only carry that version's models;
  replacing the whole pack removed the newer ones and crashed the game (for example "Peachette over Toadette", which stopped the game
  a few seconds after the title screen). Members the mod has replace the game's, extra ones are added, and several mods stack in
  order. Packs ending in .pack and .sarc are merged; Yaz0-compressed .szs files still replace the whole file.
- **GameBanana pages show far more.** Hovering or selecting a mod shows its first screenshot, submitter, likes, downloads,
  developers, licence and description. A mod's own page has a Screenshot row to step through up to six images, a Likes row and a
  button for its video. **Sort By** (most downloaded, liked, viewed, newest, recently updated) and **Category** rows filter the list.
- **Installed mods from GameBanana show the same pane** as when you browsed them.

### Version and updates

- **Show Recomp Version** (General > Game): the title screen's Ver. box can show this release instead of the game's 3.0.3. With Skip
  Intro or Skip Mode Select on, the same box also shows in the bottom right while the game starts, then goes away.
- **The update notice** now appears as soon as the check has its answer and just says "New version available". It's always its normal
  size whatever Menu Size is, its box fits its text, and it sits above the loading screen's coffee line.
- **Test builds count as older than their release.** A build like 0.1.0+dev gets the notice for 0.1.0; the leading "v" is optional.

### Fixes

- **Skip Mode Select sometimes did nothing.** Its first button press could go out before the title screen took input, so the rest of
  the sequence went nowhere and you stayed on the title screen. It now waits longer.
- **Skip Intro is greyed out and says "Unavailable" while Skip Mode Select is on**, instead of showing a misleading Off.
- **The offline notice** points at the Online tab and Restart the Game instead of the setup screen.

## 0.1.0 - Initial release

First release of smm2-dx: Super Mario Maker 2 (3.0.3) as a native Windows program.

- Runs on your graphics card through Vulkan. No emulator, Windows 10 or 11 only.
- Setup screen that unpacks your own game (folders, NSP or XCI), `prod.keys` and the firmware font for you.
- Online play, Course World and uploads through your own [OpenCourse World](https://opencourse.world) account.
- Mouse and keyboard in the Course Maker: right click to erase, middle click to drag the course, number keys for the
  palette, Ctrl+Z to undo.
- Sharper graphics and smoother scaling: internal resolution presets, AMD FSR 1 upscaling, SMAA, 120 fps frame
  generation, unlocked aspect ratio.
- Mods: drop-in changes made by the community.
- In-game menu (F1 / Esc), drawn with Dear ImGui: Controls, Video, Audio, Mods and Online tabs.
- Build Helper: `build-helper.cmd` fetches every tool it needs and builds the app in one step, nothing to install.
