# Rocksmith 2014 Mac Fixes

Fixes for **Rocksmith 2014 Remastered** on macOS (Steam version). They work on Apple Silicon
and Intel Macs, tested on an Apple Silicon Mac with current macOS and on an Intel Mac mini with
macOS 10.15. On older macOS, use v1.0.4 or later: earlier versions didn't load there.

- **Much faster startup with lots of DLC or custom songs.** With about 2,400 songs, the game
  spent about two minutes enumerating them every time it started. With the fixes it takes
  about 25 seconds.
- **End works in the song list.** It jumps to the last song, as Home jumps to the first. Before,
  it did nothing.
- **Space works in the song search**, so you can search for "black sabbath". Before, spaces
  were ignored in the search box.
- **The keyboard works in the Guitarcade games**, such as Scale Racer. Before, arrows, Enter,
  Esc and other keys did nothing in their menus.

The fixes don't touch your songs, saves or settings. They work alongside other mods, such as
the custom-song patch.

## Install

1. Download `rs-mac-fixes.zip` from the [releases](../../releases) page and open it; you
   get the program `rs-mac-fixes`.
2. Quit Rocksmith if it is running.
3. Double-click `rs-mac-fixes`. The first time, macOS says it can't verify the developer (the
   program isn't notarized by Apple). Go to **System Settings → Privacy & Security**, scroll
   down, click **Open Anyway** next to rs-mac-fixes, and confirm. On older macOS versions,
   right-click (or Control-click) the file, choose **Open**, then **Open** again.
4. A Terminal window shows where the game is and what is installed. Type `i` and press Return.
5. Start Rocksmith from Steam as usual.

Prefer Terminal? These do the same:

```bash
chmod +x rs-mac-fixes
./rs-mac-fixes install
./rs-mac-fixes status
./rs-mac-fixes uninstall
./rs-mac-fixes report
```

The installer finds the game in your Steam libraries, including ones on other drives (connect
the drive first). If it can't find the game, it asks for it: drag the `Rocksmith2014` folder
or `Rocksmith2014.app` from Finder into the Terminal window and press Return. In Terminal you
can also give the location up front with `--game "/path/to/Rocksmith2014"` (the folder that
holds `Rocksmith2014.app`, or the app itself).

## Uninstall

Open `rs-mac-fixes` again and type `u`, or run `./rs-mac-fixes uninstall`. This removes the
fixes and puts the game back exactly as it was.

## After a Steam update or "Verify integrity of game files"

Steam may put back the original game files. The fixes are then simply not loaded; nothing
breaks. Run the installer again to get them back.

## What to expect

- **While the songs load at startup, the screen updates slowly, and the menu shows up only
  once all songs are loaded.** This is the price of the speed-up: the game normally loads
  one song per screen refresh, and the fixes have it load as many as fit in 50 ms. The time
  you wait is much shorter overall, but the screen stutters during those seconds. You can
  trade speed for a smoother screen (see Settings).
- If your game build is different from the one the fixes were made for, they stay inactive
  and the game runs as before. The installer warns about this.

## Settings

Settings are files in `~/Library/Application Support/rs-mac-fixes/` (in Finder: Go → Go to
Folder…, then paste the path). Create the folder if it isn't there.

| File | Effect |
|---|---|
| `budget_ms` | How many milliseconds per screen refresh the game spends loading songs at startup. Default 50. Lower is smoother but slower; `0` loads one song per refresh, as the game normally does. Example: `echo 20 > ~/Library/Application\ Support/rs-mac-fixes/budget_ms` |
| `disabled` | If this file exists, the fixes do nothing. An empty file is enough: `touch ~/Library/Application\ Support/rs-mac-fixes/disabled` |

The fixes write a short log there too, `rs-mac-fixes.log`.

## Something not working?

Start Rocksmith once with the fixes installed and quit it. Then open `rs-mac-fixes` again and
type `r` (or run `./rs-mac-fixes report`). This writes `rs-mac-fixes-report.txt` to your
Desktop and shows it in Finder; attach that file to your bug report. It lists your macOS
version, Mac type, what is installed in the game folder (other mods included) and the end of
the fixes' log. Your home folder is shown as `~`, so it doesn't include your user name.

## What the installer changes

- It copies `librsmacfixes.dylib` into `Rocksmith2014.app/Contents/MacOS/`.
- It adds one line to the game executable telling it to load that file. The executable is
  backed up first, as `Rocksmith2014.rs-mac-fixes-backup` in the same folder.

Uninstalling removes both again and leaves the backup in place; you can delete it. Nothing else
in the game folder is changed, and song files are never touched.

## Why it was slow

The game's code is encrypted on disk, so the causes were found by analysing the running game.
Startup had three problems, each worse the more songs you have:

- The game mounts one song archive per screen refresh. With 2,400 archives, that alone took
  2,400 frames, each also drawing the menu.
- Every time the game checks whether a file exists, it asks each mounted archive in turn, and
  each one recalculates the same fingerprint (MD5) of the file name. With all songs mounted,
  that came to about 30 million MD5 calculations per startup.
- The game stores every number it reads from song data in a table that puts all numbers
  between 0 and 1 in the same slot, so each new number was compared against a growing list.

Mounting faster also meant keeping more song files open at once than macOS allows a game by
default (256), so the fixes raise that limit; otherwise songs would go missing from the list.

The keyboard problems were gaps in the game's key tables. macOS's End key had no entry in the
table the menus use. The game's own keyboard, which the song search and the Guitarcade menus
read, only knew letters and digits, and keys such as the arrows didn't even reach it intact.
The fixes fill in the missing keys.

Details for developers: [docs/technical.md](docs/technical.md).

## Building from source

Needs the Xcode Command Line Tools (`xcode-select --install`).

```bash
./build.sh
```

This builds `build/rs-mac-fixes`, the installer with the library embedded, and
`build/rs-mac-fixes.zip` for releases (the zip keeps the file executable).

## Disclaimer

This is an unofficial fan project, not affiliated with or endorsed by Ubisoft. It changes how
the game runs; use it at your own risk. Uninstalling restores the game executable exactly.

## License

MIT, see [LICENSE](LICENSE).
