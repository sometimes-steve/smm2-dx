# Changelog

## 0.7.10 "Quick Retry" - 2026-11-23

### Game

- **Deaths are counted in courses that retry quickly.** Some courses put Mario back within a few frames of being taken down, which the death counter
  took for the same attempt still going, so those deaths were missed (seen in Super Mario 3D World courses). A course that is taken down and starts
  ticking again after a short pause (more than three frames, within ten seconds) is now counted as a retry. Courses that took longer were counted
  before and still are.

### Support

- **RecompLauncher.** The release zip carries a `recomplauncher.json` next to the exe, so [RecompLauncher](https://recomplauncher.com) can start the
  game.
- **Log.** The log notes each time a course is taken down and how long the course's ticks paused after it, which shows why a death was or was not
  counted.
- **README.** It says the project is AI assisted.

## 0.7.9 "Body Count" - 2026-11-22

### Game

- **Deaths are counted for every course (#33).** Each time Mario dies the game adds one to that course's count, kept in `course-deaths.txt` in the
  data folder (one line per course: its OpenCourse World data id and the deaths), on this device only. The game has no single step for a death, so
  a death is told by what follows: the course is taken down and the same course is played again within ten seconds. Start Over, the pause menu,
  Exit Course, a skip, a clear and a test play in the Course Maker are not deaths. A death that ends an Endless run (no lives left, no retry) is
  not counted. OpenCourse World keeps a death count per course only for players with a record on it, so it could not supply this.
- **Show course deaths.** The Extras page has a new **Show course deaths** switch (off by default, applies at once): while you play a course, in
  Course World or the Endless Challenge, your deaths in it are shown as bold white text at the top centre of the screen, next to the Endless
  skips if those are shown too. Both texts are a little smaller and sit side by side near the top edge.

### Support

- **The test driver never closes your game.** `recomp/tools/autoplay.py` used to stop a game by its program name, which could close one you had open.
  It now notes the process ids of the game it starts and ends only those, and it refuses to start while another game is running.
- **Release packaging.** The release zip also leaves out `course-deaths*.txt`.
- **Tests.** `recomp/tools/test_course_deaths.py` gets into a course in a hidden game, waits with nothing pressed (no death may be counted), runs Mario until he dies and checks the log and the saved
  count (the test is skipped if he does not die in two and a half minutes). `autoplay.py` can now walk into the Endless Challenge by itself
  (`enter_endless`).

## 0.7.8 "Skip Tally" - 2026-11-21

### Game

- **Endless Challenge skips are counted (#32).** Every course you skip in the Endless Challenge (holding Minus) is added to a total, shown on the
  Stats tab in a new Endless Challenge card (kept in `endless-skips.txt` in the data folder, on this device only). A tap of Minus that does not
  skip, a Start Over, a clear or the first course of a run does not count. The game itself is not changed: the count only watches the one step of
  the game that runs when a skip happens.
- **A new Extras page.** A new Extras entry in the main menu holds small additions that are off until you ask for them. The first is **Show Endless
  skips**: while you play the Endless Challenge, the skips of this visit are shown in bold white text with a black edge at the top centre of the
  screen (Course World and the rest of the game never show it). The count starts again when you come back after a minute away from the Endless
  Challenge; the all-time total is on the Stats page. It applies at once.

### Support

- **Steering a test run without the desktop.** `RECOMP_CTL=<file>` (used by `recomp/tools/autoplay.py`) makes the game read commands from a file:
  hold buttons, save a picture, put a mark in the log, read the game's memory (`peek`), count the game's function entries over a stretch (`fnprof start` / `fnprof stop <path>`, which
  also say which ran first), quit. A run with it ignores the keyboard, mouse and controllers of whoever is at the computer and opens its window
  without taking the focus.
- **Release packaging.** The release zip leaves out the skip total (`endless-skips.txt`), profiler dumps and `.bak` files.
- **Tests.** `recomp/tools/test_endless_skips.py` plays into an Endless course in a hidden game, taps Minus, skips twice and checks the log and the
  saved total.

## 0.7.7 "Head Start" - 2026-11-21

### Game

- **A course starts downloading as soon as the server names it (#3).** When you skip, clear or start an Endless or Course World course, the game asks
  OpenCourse World which course is next, makes two more calls, and only then opens its own connection to download it. The course is known from the
  first answer, so it is now fetched in the background right away and handed to the game when the game asks for it, even while it is still arriving.
  The game's own calls are unchanged (the same bytes go to the server and come back), so play history and challenge counts are what they were. In
  measurements on Windows this saves about a second on a skip that needs the network; it is not the whole wait between courses: the game keeps a
  reserve course (every other skip needs no network), and most of a skip that does is the game's own work after the download, about 8 of 12 seconds.
  If an early download fails or the server answers in a way it does not follow, the game asks for the course itself, as before. `RECOMP_OCW_PREFETCH=0`
  turns it off.

### Support

- **Network timing in the log.** The log says how long each burst of OpenCourse World traffic took (`[net] ... first byte after ... ms, last after ... ms`),
  each course download (the request, how long to the first byte and to the last) and the TLS handshake, and what the early downloads did (`[ocw-http]`).
  Only the request line and the names of the headers are logged, never a value: one of them is an id of yours.
- **Finding what a hitch is doing.** `LARECOMP_FNPROF_LONG=<ms>` counts the game's function calls frame by frame and saves the counts of any frame
  that took longer, with one ordinary frame to compare; `recomp/tools/fnprof_diff.py` names what the hitch ran extra from the game's symbols.
- **Tests.** `recomp/tools/test_ocw_proxy.py` runs the early-download proxy in front of a fake, slow, chunked server and the reader of the server's
  answers on synthetic WebSocket and PRUDP frames.

## 0.7.6 "Many Tongues" - 2026-11-21

### Game

- **Japanese, Chinese, Korean and Cyrillic text draws in the name and comment dialogs (#53).** The overlay's keyboard dialog showed `?` for every
  character outside Latin-1, even though the game received the text intact. The dialog now adds the characters it is shown to its font when they
  appear, rebuilding the font once between frames, using Segoe UI where it has them and your own Windows fonts (Yu Gothic or MS Gothic, Microsoft
  YaHei or SimSun, Microsoft JhengHei or MingLiU, Malgun Gothic) for the rest; no font is shipped. Kana or Hangul in the text, or your Windows display
  language, decide which font a Han character comes from. The set is limited to 512 characters per session; one past that, or an emoji (the
  overlay's font handles the basic plane only), stays a `?` instead of crashing. Pasting works; typing with an IME (composition, the candidate
  window) and Japanese on the controller's on-screen keyboard are not done yet. Only the keyboard dialog's text is covered: the menu window's own font
  and its other text fields still show `?`.
- **The game gets stable text from a prompt.** A lone surrogate or invalid UTF-8 now becomes U+FFFD instead of junk bytes, and a name is never cut
  inside a character or an emoji's surrogate pair.

### Support

- **Tests for the text prompts.** `recomp/tools/test_text_utf.py` builds the UTF-8 / UTF-16 conversion and the character limits
  (`runtime/text_utf.c`) and checks them against Python's codecs, with Japanese, Chinese, Korean, emoji and broken input.
- **`LARECOMP_KBD_AUTO` takes Unicode.** The test hook that answers a text prompt at once read the environment in the ANSI code page, which turned
  any non-Latin text into `?`.

## 0.7.5 "Flight Recorder" - 2026-11-18

### Game

- **Skip mode select is one setting with three values.** General > Game > **Skip mode select (restart)** is now **Off**, **Course World** or
  **Make**, in one row, in place of the switch and the "goes to" row 0.7.4 had. Your saved choice carries over.

### Support

- **Copy or save a support report (#55).** Quit > Support has **Copy Support Report** and **Export Support Report**. The report holds this
  program's version, your PC (Windows, CPU, memory, graphics), the settings that matter (graphics, sound, input, start-up, language), the mods that
  are on and their order, the end of this session's log and of the last one's, and the crash log of a session that stopped on an error. Your name,
  your folders, your OpenCourse World domain, token and maker code, e-mail addresses, IP addresses and Course / Maker IDs are taken out before
  anything is written, and `prod.keys`, the firmware, the game's files, saves and OpenCourse World's configuration are never part of it. Export
  saves it as a text file in `smm2-dx_log` and says where; nothing is sent anywhere, you send it yourself. A session that ends on a fatal error now
  keeps its log as `crash-<date>.log` in `smm2-dx_log` (the newest five), so it is not lost after the three sessions the log rotation holds, and the
  next start offers the report with a notice. The frame-time sample #50 will add is not part of the report yet.

## 0.7.4 "Fast Track" - 2026-11-18

### Game

- **Skip mode select can go to the Course Maker (#20).** General > Game > **Skip mode select goes to (restart)** picks where Skip mode select
  lands: **Course World** (what it did, and the default) or **Make**, the Course Maker, which is one `A` after `L+R` on the title screen. It is
  read when the game starts, like the setting itself, and is greyed out while Skip mode select is off. The loading screen says "Opening Make" for
  it. Course World is checked against the game and pressed again if it did not open; Make has nothing to check it by (the Course Maker opens
  without the scene manager changing), so its sequence ends 5 seconds after the last press and does not press anything more.

## 0.7.3 "Page Turner" - 2026-11-18

### Play Guide (#19)

- **The Play Guide's exclamation mark opens the game's own manual instead of crashing.** The game asks the Switch's web browser for its offline
  manual and stopped when nothing answered. The manual's pages (the Play Guide: Course Maker, Course World, Coursebot, Q&A, Settings and more, in
  the game's language) are in your own game files, so setup now takes them from your NSP / XCI, the update's pages over the base game's, into the
  data folder. They are never shipped. They open in a web view over the game window (Microsoft's WebView2, which comes with Windows 11 and most
  of Windows 10; WebView2Loader.dll comes with the program), and in your browser where that is not available. Hovering a part shows its text, as
  the Switch's cursor does; Esc, the controller's back button or the guide's own back button closes it, and the controller's D-pad and confirm
  button move and select. The button icons are drawn with the console's button font, which setup writes from your firmware next to the manual.
  An install set up before this runs the font step once more to get the pages (a source checkout in dev mode skips that check: run
  `games/smm2/sysdata.py --game <your game folder>` by hand there).

## 0.7.2 "Word Check" - 2026-11-18

### Fixes

- **Entering a comment no longer crashes the game (#44).** The game checks every comment against the console's text filter word lists (system
  titles 0806 and 0823), and stopped with an assertion when they were missing. Setup takes them from your own firmware again, next to the fonts
  (they are never shipped), and an install set up before this runs the font step once more to get them. If they are still missing, the error
  now says so. A source checkout (dev mode) skips that check: run `games/smm2/sysdata.py` by hand there.

## 0.7.1 "Time Check" - 2026-11-17

### Audio

- **Mute in Background (#51).** Settings > Audio > **Mute in Background** fades the game's sound out over a quarter of a second when you switch to
  another window or minimize the app, and back in when you return. On by default. Mute and Volume keep their values (a muted game stays muted),
  the game keeps running, and opening the menu does not count as leaving. It covers all of the game's sound, movies included.

### Menu

- **A clock in the menu (#56).** The computer's local time is shown at the bottom of every menu page, beside the time played, in the time zone and
  12 / 24-hour format Windows is set to. It follows the system clock as the menu stays open. Settings > Display > **Show Clock** turns it off (on
  by default). On a narrow window the button hints give way to it.

## 0.7.0 "Character Select" - 2026-11-16

### Mii Editor (#23)

- **New Player > Mii Editor: a library of Miis, and an editor for each.** Make a new Mii, import one, duplicate, rename or delete it (deleting asks
  first), and **Play as This Mii** to make it the one the game uses. Edits are saved as you make them, in the `miis` folder of `smm2-dx_data`. The Mii you
  play as is the one the game reads at its start, so restart the game after changing it.
- **Import from MiiCharacters.com.** Paste the link of a Mii (`miicharacters.com/mii/23844`) and it is added to the library, exact: the Mii's own
  QR picture on the page is read and decoded, so every part, colour and position comes across, along with the name. The Mii's picture is kept
  and shown in the library. The link button at the right of the heading opens the site. 3DS and Wii U Mii QR codes only.
- **A live preview** beside the fields, drawn by [Arian's Mii Renderer](https://mii-unsecure.ariankordi.net/) a moment after each change (the Mii's
  88 bytes are sent there, nothing else).
- **Pick parts from pictures.** Hair, eyebrows, eyes, nose, mouth, face shape, wrinkles and make-up open a grid of the console's own icons, with
  your current choice marked. The icons are taken from your firmware, so the next time the program starts it runs the firmware step once
  more to get them; without them the parts are numbers.
- **Add the Mii in Use** puts the Mii you play as now (`mii.charinfo`) into the library.

### Menu

- **The menu has a sidebar.** Prelaunch, Settings, Mods, OpenCourse World, Player and Quit are on the left, and their tabs sit within the page on the right.
- **Link buttons.** A small link icon at the right of a heading opens a web page: MiiCharacters.com above, and the OpenCourse World Account heading
  (in place of the Get Your Config row) opens opencourse.world/account.
- The Search tab's two cards start at the same height.

### Release package

- **The Miis you made or imported (`miis`) and the one you play as (`mii.charinfo`) are left out** of the release zip, as the rest of your own data is.

## 0.6.2 "Zero Hero" - 2026-11-15

### Window (#46)

- **The mouse cursor is hidden over the game while the menu is closed.** Opening the menu shows it again, closing hides it. The
  **Hide Mouse Cursor** option (Display, on by default) turns it off. It only affects the game's window, in windowed and fullscreen alike; other applications keep their cursor.

### Search tab (#45)

- **New filter: Exclude 0-Second Clear Checks.** On by default. When on, courses whose clear check time is exactly 0 are never picked.
  It uses the time as OpenCourse World records it, so a course with a positive time under a second still counts, and it works together with
  the other filters.

## 0.6.1 "Blackout" - 2026-11-13

### Search tab (#24)

- **Take to the Game shows the black loading screen** Skip Mode Select uses ("Opening the course", with its loading bar) while Course World is
  driven to the course's page: the game runs fast and unseen with its sound off, and the window shows the page once it is up.

## 0.6.0 "Roulette" - 2026-11-13

### Search tab (#24)

- **New Search tab: find a course nobody has cleared and you have never played, and take it to the game.** The filters are on the left, the
  Find box on the right. Uncleared (nobody has cleared it) and Unplayed (you never played it) are on by default; there are also Game Style,
  Theme, Tag, Clear Condition, Min. / Max. Attempts and Min. / Max. Clear Check time (the sites' filters, less the year). **Find a Course**
  picks one at random from the ones that match, checks it with OpenCourse World, and shows its picture with the upload date and time, its
  name, maker, style, tags, clears and attempts. **Find Another** draws again.
- **Take to the Game** (in Course World): closes the menu and opens the course's page there for you: it backs out to Course World's home,
  opens the ID search, and answers the game's Course ID prompt with the course's ID. Play is yours to press. Your own buttons are ignored
  for the few seconds it takes. When Course World is not what the game is showing, the button says so.
- The course list (about 30,000 courses; OpenCourse World's web API has no search) is read in the background when the game starts and kept in
  `ocw-courses.bin` in the data folder, so the next start has it at once and only tops it up; your played list is kept the same way. The
  circle at the top right of the Find box reads both again. Find a Course is greyed until they are in.
- The Course ID prompt of the game's own Course World search is answered with that ID only when you took a course there from the Search tab
  (for the next 15 minutes); typing an ID yourself works as before.

### Menu

- **Boxes are half the window's width on every tab** (as the Stats and Achievements cards are), more only when their rows need it.
- Stats: **Last 14 Days** and **When You Play** sit side by side in a wide window, the same height. The titles of the boxes on the Stats
  and Achievements tabs have a line under them, as the headings of the option pages do.
- Achievements: the refresh circle is in the Leaderboards box's top right corner.
- Mods: the help text on the right for the Show row and for "None yet" is gone.

## 0.5.11 "Maple Leaf" - 2026-11-13

### Maker flags (#35)

- **Makers have flags.** OpenCourse World does not store where a maker is from, so the game had no country to draw a flag from. With the
  new **Maker Flags** setting (General tab, Players, on by default) every maker gets one as the server's answer reaches the game: a guess
  from the language of their name (Japanese kana is Japan, Korean is Korea, and so on for Thai, Cyrillic, Greek, Hebrew, Devanagari,
  Vietnamese and a few letters of Latin-script languages), otherwise a pick that is the same for that maker every time, most often the US
  or Japan. **These flags are made up, not where anyone really is**; turn the setting off for none. Only flags the game has are used
  (110; it has none for China, Taiwan or Saudi Arabia, so Chinese characters alone give Japan).
- **Your Flag.** Pick the flag you are shown with, from the 110 countries by name. It starts on Canada. It only changes what you see:
  nothing is sent to OpenCourse World, so other players see theirs for you.
- Shown on the leaderboards that list makers (Course Makers) and on maker pages. The Players and Versus Players leaderboards use another
  layout and do not have flags yet.

### Names

- **New setting: Strip Discord Tags** (General tab, Players, on by default). Makers who came from Discord can have the old name tag's `#` or
  `#0` left on the end of their name; it is taken off. Other tags (`name#1234`) are left alone.

## 0.5.10 "Wall Clock" - 2026-11-13

### Leaderboards (#30)

- **The leaderboards show when each maker last played.** The "Last played" line used to stay blank (the layout's placeholder
  text, `WWWWWWWWWWWWWWWW`). OpenCourse World's login answer tells the game the server's time, and it sends a fixed one, 8 February
  2023. The game counts "now" on from it, so every maker's last play was in the future and no "today" / "N days ago" text fitted.
  When the time in that answer is more than a day away from your machine's clock, it is now replaced by the clock's before the
  game reads it, so the line reads "today", "one day ago", "N days ago" or "more than one year ago" in your own time zone. A
  correct time from the server is left alone, so this stops doing anything once OpenCourse World fixes it.

## 0.5.9 "Top 999" - 2026-11-11

### Achievements tab

- **New Achievements tab: where you stand on OpenCourse World's in-game leaderboards.** One box, "Leaderboards", with a row for
  Maker Points (Weekly), Maker Points (All Time), Clears, First Clear, World Records and Endless (Easy, Normal, Expert, Super
  Expert). It shows `#N` when you are on a board and a dash when you are not, looked for down to rank 999. It asks over NEX, as the
  game does, so the places are the ones the game shows. A small spinner stands in for a board until its place is known.
- **Filled in when the game goes online, refreshed only when you ask.** The places are looked up in the background as soon as the
  game is online (the last answer shows straight away on the next start), and not again until you press the refresh button at the
  top right of the tab.

### Stats tab

- **Game Styles (Clears)** now counts the courses you cleared in each style instead of the time played in it.

## 0.5.8 "Honest Globe" - 2026-11-11

### Online tab (#37)

- **The Online tab no longer says Online when OpenCourse World can't be reached.** It used to show the green globe whenever your
  config was loaded, even with the service down. It now checks in the background that the server actually answers (not only that
  its name resolves: a down server can still have a name), and while it does not the globe turns red and reads "Can't reach
  OpenCourse World". It checks again every few seconds, so the tab goes back to Online as soon as the service is back.
- **The game is told it is offline too, with a toast saying why.** While the server does not answer the game no longer tries to
  connect, and when it asks to go online you get a toast, "Can't reach OpenCourse World", instead of the game sitting on a dead
  connection. It connects as soon as the server answers again.

## 0.5.7 "Tidy Names" - 2026-11-10

### Play History

- **The totals in a maker's Play History are right wherever you open it from.** Opened from the leaderboards, a maker's profile showed
  the wrong numbers (Courses played had the clears, Courses cleared had the lives lost). The four totals went into the wrong slots of
  the game's record; they now go where the game reads them.

### Usernames

- **New setting, on by default: Clean Usernames** (General tab, Players). It takes the console's symbols (private-use characters
  the game draws as boxes) and line breaks out of other players' names, so a name that was three lines tall shows on one. What is
  left is shown as it was; a name made only of symbols shows as `?`. Off: names are shown as OpenCourse World sends them. Your own
  name in the Online tab is cleaned the same way.

## 0.5.6 "Full Stats" - 2026-11-10

### Stats tab (#25)

- **A new Stats tab, on this device only.** Cards for **Play Time** (this session, all time), **Sessions** (longest, average, how
  many), **Characters** and **Game Styles** (a bar for the time spent with each), the totals **OpenCourse World** keeps for you
  (courses played and cleared, attempts, lives lost, world records, first clears) and **Clear Stats** worked out from them
  (clear rate, attempts per clear, lives lost per clear). Under them, **Last 14 Days** (a bar a day) and an **hours of the week**
  heatmap across the full width. The cards sit in two columns when the window is wide enough.
- **Characters and game styles are counted from the moment you start.** The character is the one you have picked (Mario, Luigi,
  Toad, Toadette); the style is that of the level you are in (not counted in Course World or the menus). Nothing is kept from
  before this version.
- The numbers are kept in `character.txt` and `history.txt` in the data folder (written every minute and when the app closes;
  never sent anywhere, and left out of release zips).

### Menu

- **Play Time moved from the Online tab to the Stats tab**; the Online tab keeps your profile and account.
- **Fix Broken Miis moved to the General tab.**
- **Every tab's sections sit on shaded cards**, the Mods tab included.

### Skip mode select

- It now checks that Course World opened (the game's scene changes once it has) and presses A again, up to twice, if it did not
  after 6 s. The log says what happened.

## 0.5.5 "Play time" - 2026-11-09

- **How long you have played, kept on this device only.** The Online tab has a **Play Time** section with **This Session** and
  **All Time**, and the menu shows the all-time total at the bottom right (`Played  3 h 12 min`) on every page. Time counts while
  the game is running, its window has the focus and is not minimized, and the menu, the keyboard box and the loading screen
  are not up. There is one total for the install, in `playtime.txt` in the data folder (written every minute and when the app
  closes; never sent anywhere, and left out of release zips).

## 0.5.4 "Language Safe" - 2026-11-09

### Names in other scripts

- **Chinese, Taiwanese and Korean characters in names show in every language, with nothing to set.** 0.5.3 swapped the game's
  name font chain for its Chinese one, which also drew Japanese kanji with the Chinese font and needed a setting to pick the
  script. Now the game's own chain is left exactly as it is and the three fonts the game has for those scripts (simplified
  Chinese, traditional Chinese, Korean) are added on the end, so only what was a "?" box changes. The **Name Fallback Font**
  setting is gone. The chain is made at start-up from your own `Font/Fcpx.szs` (never edited in place, never shipped) and only
  if that file is the 3.0.3 one; `RECOMP_NO_FONT_FALLBACK=1` turns it off.

### Menu

- The Mods and Online tabs no longer end with "Reset Page to Defaults" (nothing on them is a page of settings to reset).

## 0.5.3 "Course Finder" - 2026-11-09

### Course ID / Maker ID search (#17)

- **Esc (and B) now cancel the box** instead of searching for nothing. A cancel is reported to the game as a cancel, not as
  empty text with success.
- **The box follows what an ID is:** capital letters, and only the characters an ID holds (digits and B C D F G H J K L M N P Q R
  S T V W X Y: checked against 570 IDs from OpenCourse World). The dashes show as you type, `AAA-BBB-CCC`; a typed or pasted
  dash goes in place. The controller's on-screen keys grey out what an ID can't hold. Other prompts (names, comments) are
  not changed.
- **Keycap hints under the field**, as the menu's footer: Enter Confirm and Esc Cancel with the count, or the controller's
  buttons (Type, Delete, OK) named for your Button Icons setting. The menu's other text fields get the same row.

### Names in other scripts

- **A maker named in simplified Chinese no longer shows a "?" box.** The game draws names with Japanese fonts, which lack
  many Chinese and Korean characters, and it only falls back to a Chinese font when its language is Chinese. The game's name
  font chain now ends in a Chinese font for every language (a patched copy of `Font/Fcpx.szs` is made from your own file;
  your game files are never changed or shipped). **General > Appearance > Name Fallback Font** picks which one: Simplified
  Chinese (default), Traditional Chinese, Korean, or the game's own. One script at a time.
- Setup now also copies the Chinese and Korean shared fonts from your firmware when it has them. An install set up before this
  runs the font step once more to look for them. `RECOMP_NO_FONT_FALLBACK=1` turns the name fallback off.

## 0.5.2 "Hands Off" - 2026-11-09

### Fixes

- **Your input is ignored while Skip mode select runs.** The sequence presses buttons for you, and a click, a key or a stick
  moved in the middle of it spoiled it. While it runs, key presses, mouse clicks and movement, the wheel, touches and
  controller buttons are dropped (and so are the menu's keys, so the menu can't open); F11 and Alt+Enter still work. It lifts
  when the sequence is done, or after 120 seconds. Pressing a button no longer cancels the sequence.

### Build Helper

- **Copy Error Details:** when setup stops, a button next to "Setup stopped." puts the version, the error, the hint, the log's
  path and the end of the log on the clipboard, so a report doesn't have to be retyped (#12).

## 0.5.1 "Our Corner" - 2026-11-09

### Title screen

- **The corner line on the title screen now reads "© 2026 sometimes_steve"** instead of "© 2019 Nintendo" (#14). That line is
  not text but a picture in the game's title layout (the texture `Title_Claim_00`, 384 x 60), so it is swapped for this
  project's own picture when the game loads it; the game's files are never changed or shipped. The staff roll still carries
  Nintendo's copyright notice. `recomp/tools/gen_title_claim.py` draws the new picture (it needs Pillow; a normal build does
  not, the result is part of the source).

## 0.5.0 "Menu Polish" - 2026-11-09

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

## 0.4.2 "Restart Prompt" - 2026-11-09

### Menu

- Settings that only apply after the game restarts (Skip intro, Skip mode select, Language, Prepare at full speed) now say
  "(restart)" in their name, and changing one opens a "Restart the game?" window (Restart now / Not now); after Not now,
  a notice reminds you as the menu closes. Quit > Restart the Game, Quit to Desktop and Reset Page to Defaults ask
  in the same kind of window (#9).

### Build

- The helper scripts are Python now and live in `recomp/tools/`: `rebuild_confirm.py` (was `rebuild-confirm.ps1`) and
  `package_release.py` (was `package-release.ps1`).

## 0.4.1 "Mii Safe" - 2026-11-09

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

## 0.4.0 "Mii From Boot" - 2026-11-08

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

## 0.3.0 "Faster Setup" - 2026-11-07

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

## 0.2.0 "New Menu" - 2026-11-07

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

## 0.1.0 "First Course" - Initial release

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
