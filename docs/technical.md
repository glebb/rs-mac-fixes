# Technical notes

For the Steam build of Rocksmith 2014 Remastered for macOS whose x86_64 `__TEXT` segment is
`0x17e7000` bytes (vmaddr `0x100000000`). Addresses below are unslid. The library checks this
layout and the exact original bytes at every patch site before changing anything.

## Installing

The installer looks for the game (Steam app 221680) in `~/Library/Application Support/Steam`,
then in each library listed in `libraryfolders.vdf` (using the `installdir` from the app's
`appmanifest_221680.acf`), then in a few `/Applications` locations. Failing that it asks for
the folder; `--game` sets it directly. It refuses to install or uninstall while the game runs.

The executable is not code signed, so the installer adds an `LC_LOAD_WEAK_DYLIB` command for
`@executable_path/librsmacfixes.dylib` to its x86_64 slice, using the zeroed space after the
existing load commands. Weak: a missing library is skipped by dyld. The executable is copied to
`Rocksmith2014.rs-mac-fixes-backup` before its first change. Uninstalling removes the command,
moves any later commands back and zeroes the freed bytes, so the executable is byte-identical to
the original; the backup is left in place.

## Loading

The constructor only acts in an executable named `Rocksmith2014` whose `__TEXT` matches the
size above and whose `__DATA` covers the tables used below.

The installer's weak load command requires no minimum library version (compatibility version
0), and the library itself has compatibility version 1.0. dyld before macOS 12 skips a library
whose compatibility version is lower than its load command requires, silently for a weak one.
Up to v1.0.3 the command required 1.0 and the library had 0.0, so on older macOS (such as 10.15
on Intel Macs) the fixes never loaded; newer dyld no longer checks this. Library 1.0 satisfies
those old commands too, so reinstalling is enough.

The game's `__text` is encrypted on disk and decrypted by its start routine (the
`LC_UNIXTHREAD` entry) after dyld has run the library initializers. The library's constructor
therefore starts a thread that polls (every 200 µs, up to 60 s) until the expected original
bytes of the startup fixes are present, then patches; the keyboard sites are checked once after
that. Patching uses `vm_protect(... VM_PROT_COPY)`; under Rosetta, the translated code is
invalidated by the write.

How the causes were found: stack samples of the game during the scan (`sample <pid>`), a copy
of the decrypted `__TEXT` read from the running process with `mach_vm_read`, and disassembly of
that copy.

## Startup: song enumeration

Everything runs on the render-loop thread (the CVDisplayLink callback).

### One archive per frame

`0x100a0d7e0`, called once per frame from `0x100a0d742`: while the byte at `this+0xa` is set,
it pops one path from the pending-package vector (`this+0x1a8`..`this+0x1b0`) and calls
`InstallContentPackage` (`0x100a0df80`). With the vector empty it clears `this+0xa` and sets
`this+0xc9`.

Fix: the `call rel32` at `0x100a0d742` is redirected to `batch_install_step`, which calls the
original repeatedly until `this+0xa` is cleared or the frame budget (`budget_ms`, default 50)
is used. Order and per-package work are unchanged.

Side effect: frames during enumeration take up to the budget, so the screen updates slowly
until all packages are mounted.

### Open files

A mounted package keeps its archive open until later per-frame work closes it. With hundreds
of packages mounted per frame, the game exceeded launchd's default soft limit of 256
descriptors (`launchctl limit maxfiles`), `fopen` failed and those packages were skipped (in a
test only ~330 of ~2,400 songs appeared). The constructor raises `RLIMIT_NOFILE` to 10240 (or
the hard limit). Up to ~2,000 archives were open at once during a scan.

### Path MD5 per mounted archive

A file-exists check (`0x1003c9b80`) goes through the game's file system (`0x10015a2e0`), whose
loop at `0x10015aa00` asks each mounted archive (`vtable+0x38`). Each archive (`0x100162de0`)
copies the path, optionally lowercases it, turns `\` into `/`, and computes its MD5 via
`0x100158b70` → `0x100158030`. `0x100158030` computes the MD5 and interns a refcounted path
object (string at `+0x0`, digest at `+0x18`, refcount at `+0x28`) in a global table
(`0x1018e6c00`, recursive mutex at `0x1018e6c10`). Missing files (per-song shader and graph
probes) are checked against every archive, so the work grows with the square of the number of
archives: ~32 million MD5s for ~2,400 songs.

Fix: `0x100158b70` starts with a jump to `hook_path_md5`, a 1024-slot cache keyed by the exact
path bytes. A hit returns the cached object with its refcount raised under the game's lock (the
same object the game would return, since the cache's own reference keeps it interned). A miss
calls `0x100158030` and caches the result; an evicted entry is released with `0x100158840`. If
the game creates a new path table, the cache is dropped.

### JSON number table

Parsed JSON numbers are interned (`0x1000f8f10`) in a global hash table (`0x101949588`, mutex
`0x101949590`) whose hash is `(uint64_t)value`, so every value in [0, 1) shares a bucket.

Fix: in insert (`0x1000f8f61`), remove (`0x1000f8d59`) and rehash (`0x1000f9a35`), the hash
becomes the value's bit pattern after adding +0.0 (so -0 and +0 hash equally). Bucket counts are
primes, so bit patterns spread well. Patched only while the table is empty.

### Measurements

~2,437 DLC archives, Apple Silicon, from the first DLC archive opened to the end of the
installs:

| Version | Time |
|---|---|
| original | 113–135 s |
| + number hash | 115 s |
| + path MD5 cache | 105 s |
| + several archives per frame, open-file limit raised | ~24 s |

## Keyboard

Cocoa `keyDown:` (`0x100184b20`) posts (type, keyCode, character) to a queue drained each frame
into `0x100ba1190`, which:

- maps `keyCode` through `0x101909f20` (macOS key code → Scaleform key code, filled by code
  around `0x100ba16e9`) and sends a Scaleform key event plus a char event;
- calls `0x101135520`/`0x101135560` with the character, which go through `0x101921c60`
  (character → game `KEY_*`, filled by `0x101134a10`) to the game keyboard's state array
  `0x101922060`.

The song-search text is drawn by native code from the game keyboard, not by a Flash text field.

End: `0x101909f20[0x77]` (`kVK_End`) = `0x23` (Scaleform End), added once the game has filled the
table and re-checked every 100 ms. Home (`0x73`) was mapped, End wasn't.

Game keyboard: `0x101135520` (key down) and `0x101135560` (key up), called only from
`0x100ba140e` and `0x100ba144e`, map characters 1-255 through `0x101921c60` (read nowhere else)
into the state array. That table holds only `0-9`, `A-Z`, `a-z`, and the caller passes only the
low byte of the character (Cocoa's `0xF700` for Up arrives as `0`), so space (in the song
search), Enter, Esc, Tab, Backspace, arrows, function keys, Home/End/Page keys and punctuation
never reached the game keyboard; Guitarcade menus such as Scale Racer's, which read it, ignored
them. The caller keeps the full character at `-0x28(%rbp)` (stored at
`0x100ba1332`), so both functions now start with `mov -0x28(%rbp), %edx` and a jump to
replacements that use the table as before and map the missing keys from the full character.

The game numbers its keys compactly, not as DirectInput does: `KEY_UP` is `0x79`, not `0xC8`. The
values are set together with their names (`KEY_NOKEY`, `KEY_ESCAPE`, ...) in code from
`0x100b7f700` onwards; all 145 are in name order. Patched only if both functions' original bytes
match.

Letters such as `ä ö å` can't be added this way: the search text builder accepts only some
keys (mapping them to the `'` `;` `[` keys added nothing).

## Diagnostics

Each start of the library logs one line with its version (`git describe`, from `build.sh`), the
macOS version, the CPU and whether the game runs under Rosetta (`sysctl.proc_translated`). If
the segment layout is unknown, the segments are logged; if the expected code never appears, each
site that differs is logged with its first 16 bytes found and expected. A log over 1 MB is
moved to `rs-mac-fixes.log.old` at startup. The library creates the log as soon as it loads, so a
missing log with the fixes installed means dyld didn't load it.

`rs-mac-fixes report` writes `~/Desktop/rs-mac-fixes-report.txt`: system, the executable's
architectures, `__TEXT` size, non-system libraries it loads, code signature, whether the
installed library matches the installer's, its version against the one the load command requires, quarantine attributes, settings and the last 64 KB of
the log, with `$HOME` replaced by `~`.
