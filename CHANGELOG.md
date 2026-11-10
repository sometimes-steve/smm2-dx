# Changelog

## 0.5.2 - 2026-11-09

### Fixes

- **Your input is ignored while Skip mode select runs.** The sequence presses buttons for you, and a click, a key or a stick
  moved in the middle of it spoiled it. While it runs, key presses, mouse clicks and movement, the wheel, touches and
  controller buttons are dropped (and so are the menu's keys, so the menu can't open); F11 and Alt+Enter still work. It lifts
  when the sequence is done, or after 120 seconds. Pressing a button no longer cancels the sequence.

### Build Helper

- **Copy Error Details:** when setup stops, a button next to "Setup stopped." puts the version, the error, the hint, the log's
  path and the end of the log on the clipboard, so a report doesn't have to be retyped (#12).

## 0.5.1 - 2026-11-09

### Title screen

- **The corner line on the title screen now reads "© 2026 sometimes_steve"** instead of "© 2019 Nintendo" (#14). That line is
  not text but a picture in the game's title layout (the texture `Title_Claim_00`, 384 x 60), so it is swapped for this
  project's own picture when the game loads it; the game's files are never changed or shipped. The staff roll still carries
  Nintendo's copyright notice. `recomp/tools/gen_title_claim.py` draws the new picture (it needs Pillow; a normal build does
  not, the result is part of the source).

## 0.5.0 - 2026-11-09

### Menu

- **Footer hints:** the menu shows what the buttons do (Select, Back, Switch Tab; Choose, Cancel while a list is open),
  named for your Button Icons setting (A/B, Cross/Circle, L/R, LB/RB, L1/R1), or Enter, Esc and Q/E when you last used the
  keyboard. Hidden on touch (#11).
- **The menu remembers where you were:** each page's scroll position and selected row come back when you reopen the menu,
  along with the tab and the Installed / GameBanana page (#11).
- **Collapsible sections:** Display, Rendering and Shaders on the Video tab fold with A or a click on their heading (#11).
- **Live previews:** Theme and Menu size open a list on A or a click and show each choice as you move over it; B or Esc puts
  the old one back (#11).
- The Quit tab no longer has a "Reset Page to Defaults" row.

### Mods

- **Clear search** buttons on Installed and GameBanana, counts in the headings ("Installed mods (3 of 12)", "Results (30)"),
  and a "Loading from GameBanana..." line with moving dots while a fetch is out (#10).

### Online

- **The Online tab shows your status:** a globe that spins and pulses green while you are online, turns amber when a changed
  config waits for a restart, and sits grey with no config.
- **Your profile:** name, maker code, courses played and cleared, attempts, lives lost, world records and first clears,
  read from OpenCourse World's own totals. The in-game Play History uses the same totals; before, it summed only the first
  page of your played list and came out short.
- **Change Config File...** picks the `ocw-config.json` you downloaded and **Get Your Config** opens
  opencourse.world/account. The Address row is gone from the menu (an `ip` entry in the config file still works).
- **Fix Broken Miis** (on by default): some players' Miis arrive from OpenCourse World blank, which showed as a broken Mii
  or the grey silhouette wherever they appeared, including the course list. The blank Mii is replaced, before the game reads
  the server's answer, by a generated one that stays the same for that player until the app closes. Your own Mii is never
  changed. Turn it off to see Miis as OpenCourse World sends them (#13).

### Fixes

- **Skip mode select no longer stops on the title screen.** Its waits counted frames, but the frames run unpaced and unseen
  at several hundred per second, so the L+R press came before the title screen took input. It now also waits 2.5 s of real
  time after the logo goes quiet and 1 s between presses, and holds L+R for 30 frames. The log says how many times the game
  read each press and why a sequence ended.

### Build

- The release zip no longer includes `smm2.ini` (your settings), `ocw-maker.txt` (your maker code) or `ocw-config.json` (your
  token), and the packager refuses to ship them.

## 0.4.2 - 2026-11-09

### Menu

- Settings that only apply after the game restarts (Skip intro, Skip mode select, Language, Prepare at full speed) now say
  "(restart)" in their name, and changing one opens a "Restart the game?" window (Restart now / Not now); after Not now,
  a notice reminds you as the menu closes. Quit > Restart the Game, Quit to Desktop and Reset Page to Defaults ask
  in the same kind of window (#9).

### Build

- The helper scripts are Python now and live in `recomp/tools/`: `rebuild_confirm.py` (was `rebuild-confirm.ps1`) and
  `package_release.py` (was `package-release.ps1`).

## 0.4.1 - 2026-11-09

### Mii

- **A fresh save no longer wipes your Mii on OpenCourse World.** The game keeps the player's profile in its own save, and a few
  seconds after logging in it sends that profile to OCW. A new save has no profile Mii yet (it is blank), so the very first
  start replaced the Mii you set on OCW with a blank one, and the Maker panel on the first Course World screen was empty. Now,
  just before the game sends its profile (`RegisterUser` and `SyncUserProfile`), a blank profile Mii, or one that is the default
  Mii, is replaced with your Mii from OCW. A Mii you chose in the game is never touched.

### Menu

- **Changed settings are marked, and can be reset.** A setting that differs from its default shows a small dot before its name.
  Right-click it (X on a controller) to put just that one back, or use **Reset Page to Defaults** at the bottom of each settings
  page (it asks once more before it does anything).
- **Rebuild Game Code** (General > Game): deletes the compiled game code and its build cache, restarts, and the setup screen
  compiles it all again from scratch. It takes several minutes; saves and settings are not touched. It is greyed out when this
  copy has no recompiler.
- **Show recomp version is on by default** for new installs (the title screen's Ver. box shows this recomp's version). If you
  already have a `smm2.ini`, your own choice stays.

### Build

- `recomp/tools/rebuild_confirm.py` does a clean build and checks it: every file compiled again, both exes rewritten, and the text your
  uncommitted changes added is in the runtime. `--version` sets the version string, `--install` copies the runtime into another
  copy's data folder.
- Two diagnostics for the Mii and the online connection, off unless asked for with an environment variable or an empty file of
  that name in the data folder: `RECOMP_TRACE_UPLOAD` (a call chain for each write to a TLS connection) and `RECOMP_MII_TRACE`
  (where in memory the game holds the Mii it was served; `RECOMP_MII_PROBE.txt` looks for a given pattern around it).

## 0.4.0 - 2026-11-08

### Mii

- **Your OCW Mii shows from the very first start.** Before the game boots, the setup asks OpenCourse World whose account your
  config belongs to (the token alone is enough: nothing to type) and remembers the maker code in `ocw-maker.txt`, so the Mii the
  game asks for at boot is already yours. Until now that was only known from the second start. A small lookup tool
  (`tools/games/smm2/ocw_identity.py`) does it with a few more Python packages in the bundled Python (about 21 MB; hashes in
  `recomp/tools/requirements-ocw.txt`). It only talks to the domain in your config, is instant once the code is known, and any
  failure leaves things as they were. `RECOMP_NO_OCW_LOOKUP=1` turns it off.
- **The game no longer puts the default Mii back on OCW.** It keeps its own copy of the player's Mii (from its save) and uploads
  that a few seconds after logging in, so a save made while only the default Mii was served overwrote the Mii you set on OCW with
  the default. The default copies the game holds are now replaced with your Mii (only exact copies of the default Mii are
  touched; the game then saves the right one). `RECOMP_MII_NO_HEAL=1` turns it off.
- The Mii database logs what the game asks for (`Get`, `BuildDefault`, `BuildRandom`).

### Build

- **The Build Helper unpacks a fresh Python** when the one it set up earlier lacks the packages the account lookup needs.

## 0.3.0 - 2026-11-07

### Mii

- **Your Mii on OCW is the default.** Without a `mii.charinfo` file, the game's Mii list holds the Mii you set on OpenCourse World, not a
  built-in one. Whose it is is learned from the game: the first profile it fetches in a session is yours, remembered in
  `ocw-maker.txt` (with a fingerprint of your account, never the token; delete the file to forget it). Until it is known, the
  default Mii is shown.
- **A `mii.charinfo`** in the data folder (or where `RECOMP_MII_CHARINFO` says) replaces it; one that is not a valid CharInfo is
  skipped. The game is told when the Mii changes.
- **A Mute toggle** on the Audio tab.

### Faster first-time setup

- **The generator uses several processes** (a quarter of the cores, 1 to 6 per module; `RECOMP_GEN_WORKERS` sets it), and holds less
  memory per process.
- **Keep Build Cache** (General > Game, off by default). The compiled game code is deleted once it is built. With this on, about
  800 MB is kept next to the game files, by a hash of what it was built from, and a new version that changes only some of the game
  code recompiles just those parts. Turning it off deletes it. `RECOMP_NO_OBJCACHE=1` skips the cache entirely and
  `RECOMP_KEEP_OBJCACHE=1` keeps it without the setting.
- **The biggest files compile first,** so the build no longer ends on one long compile, and **more compilers run once the
  generators are done** (their memory is free then).
- `RECOMP_GAMEBUILD_CFLAGS` adds compiler flags for trying them out.

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
