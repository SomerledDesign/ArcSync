# Product Requirements Document

**Product:** ArcSync (`arcsync`)  
**Type:** macOS command-line utility  
**Version of this PRD:** 1.2 (ArcSync; Win32-asm nod in `banner.s`)  
**Date:** 2026-09-17  
**Status:** Ready for implementation  
**Primary deliverable of this document:** specify a shippable v1 that a coding agent can implement on a Mac without inventing product decisions.

---

## 1. One-sentence pitch

`arcsync` reads a macOS Photos library (or a plain photo folder), copies the media onto a dated filesystem tree, builds a static HTML gallery that preserves the user's album/folder organization, and wraps the result in a hybrid CD/DVD/Blu-ray `.iso` that opens an index when the disc is mounted.

It is the 2026 terminal equivalent of those late-90s "burn my pictures to a CD with a browser index" apps — no GUI, tiny binary, man page, brew-installable.

---

## 2. Why this exists

Optical media is still a useful write-once archive: it is offline, bit-rot-resistant relative to a single HDD, and readable on other machines decades later if the filesystem is boring (ISO 9660 + Joliet + UDF + HFS+). Photos.app hides originals inside a package and a Core Data database. Users who want a disc they can hand to a relative, drop in a fire safe, or mount on Windows/Linux need:

1. The real files, named sanely, filed by capture date.
2. A human-browsable view that still looks like *their* albums.
3. An image they can burn with Disk Utility / `wodim` / Windows Image Burner.

This tool does only that.

---

## 3. Non-goals (v1)

- No GUI. No SwiftUI. No AppKit window. A `--help` banner and a progress line on stderr is the entire interface.
- No iCloud download / PhotoKit cloud fetch. If the original is not on disk, skip it and record the miss.
- No face recognition, maps, Memories, or Shared Albums as first-class browsers (Shared Albums may appear if they exist locally).
- No video transcode, no HEIC→JPEG conversion of originals. Originals are copied byte-for-byte.
- No incremental/smart "only new photos since last disc" in v1 (a later flag is reserved).
- No writing the physical burner itself (`hdiutil burn` / `drutil` can be a v1.1 flag; v1 stops at `.iso`).
- No Windows/Linux build of the *tool*. The *ISO* must be readable on those OSes.
- No embedding of third-party libraries that bloat the binary (no libjpeg-turbo, no ffmpeg, no sqlite amalgamation unless system sqlite is unavailable — it is available).

---

## 4. Hard constraints

| Constraint | Requirement |
|---|---|
| Language | C99 (ISO C) for the product. One required assembly souvenir: `src/banner.s` (see §4.1). No further asm unless a profile demands it. No C++, no ObjC *source*. Linking Apple *C* frameworks (CoreFoundation, ImageIO, CoreGraphics) is allowed. |
| Binary size | **≤ 233 KiB** stripped, uncompressed Mach-O for the `arcsync` executable on arm64 *or* x86_64 (each arch counted separately if universal; a universal binary may be 2×). Measured with `ls -l` after `strip`. |
| Interface | argv + stdin/stdout/stderr + a man page. Exit codes documented. |
| Host | macOS 12 Monterey and newer (Photos 7+ / `ZASSET` schema). Best-effort on Catalina Photos 5 (`ZGENERICASSET`). |
| Privileges | No root. No setuid. User must be able to read the library. |
| Dependencies at runtime | System dylibs only: `libSystem`, `libsqlite3`, `CoreFoundation`, `ImageIO`, `CoreGraphics`. Plus the already-installed `/usr/bin/hdiutil`. |
| Dependencies at build | `cc` (Apple clang), `make`, optionally `mandoc`. |
| Install | Homebrew formula + a POSIX `make install` (`PREFIX=/usr/local`). Optional MacPorts Portfile. |
| Disc image | Hybrid ISO produced by `hdiutil makehybrid`. Do **not** write an ISO 9660 encoder in-tree in v1. |

### 4.1 Assembly souvenir (required)

This is nostalgia with a job, not assembler for assembler's sake.

The Bot must not fold this back into `printf`. Two files:

**`src/main.c` — first lines of the file, before includes:**

```
/*
 * ArcSync — archive a photo library to a hybrid CD/DVD ISO.
 *
 * Win32 asm photo-CD tools, 1998; this is that program after the API grew up.
 */
```

**`src/banner.s` — the `--version` / verbose banner writer.**

- Writes exactly one line to stderr (fd 2), no libc:
  `arcsync 1.0.0 — Win32 asm photo-CD tools, 1998; this is that program after the API grew up.`
  plus a trailing newline.
- arm64 Darwin lives in `src/banner.s` (the machine this will actually run on).
- x86_64 Darwin lives in `src/banner_x86_64.s` so an Intel or universal build still has a native nod.
- Export a C-callable symbol: `void arcsync_banner(void);`
- Use the Darwin `write` syscall only (`0x2000004`). No libc, no macros-for-days.
- `main.c` calls `arcsync_banner()` on `--version`. Also call it at the start of a run when `-v` is set. Do **not** call it for `--json` alone (keep scripts clean).
- `Makefile` assembles the `.s` that matches `$(uname -m)` and links it with the C objects.
- These two `.s` files are the only assembly in the tree unless a later profile proves a hot helper. Do not rewrite `photos_db.c` in asm.
- Keep each `.s` readable on one screen (~40 lines).

If C also needs to print a git sha after the banner, that second line may be `fprintf`. The sentimental sentence lives in the assembler.

Rationale for calling `hdiutil` instead of writing an ISO encoder: a correct Joliet+UDF+HFS hybrid writer would explode both size and defect surface. Forking a system tool is the Unix thing to do and keeps the 233 KiB budget.

---

## 5. Users and jobs

**Primary user.** A Mac user with a Photos library who wants a dated, offline copy of the pictures on a disc, plus a clickable gallery.

**Secondary user.** Same person, but the "library" is just `~/Pictures` or a folder of vacation dumps (no Photos.app).

**Job to be done.**

```
$ arcsync --media dvd --out ~/Desktop/Family-2026.iso
```

…walks the default Photos library, stages files, writes HTML, builds a DVD-sized hybrid ISO (or multiple volumes if needed), prints a summary.

---

## 6. Command-line interface

### 6.1 Name and synopsis

```
arcsync — archive a photo library to a hybrid CD/DVD/Blu-ray ISO
```

```
arcsync [-n] [-v] [-q] [--json]
        [--library PATH | --dir PATH]
        [--out PATH]
        [--media cd|dvd|dvd-dl|bd|none]
        [--split]
        [--volume-name NAME]
        [--from YYYY-MM-DD] [--to YYYY-MM-DD]
        [--include-hidden] [--include-trashed]
        [--edited | --originals]
        [--thumb-size PX]
        [--title TEXT]
        [--dry-run]
        [--force]
```

Default library:  
`~/Pictures/Photos Library.photoslibrary`  
If that does not exist, error with a hint to pass `--library` or `--dir`.

Default `--out`: `./arcsync-YYYYMMDD.iso` in the cwd.

### 6.2 Flags

| Flag | Default | Meaning |
|---|---|---|
| `--library PATH` | default Photos library | Photos library bundle (`.photoslibrary`) |
| `--dir PATH` | unset | Treat PATH as a plain directory tree of images/videos. Album view = directory tree. Mutually exclusive with `--library`. |
| `--out PATH` | `./arcsync-YYYYMMDD.iso` | Output ISO path. If splitting, this is the stem: `NAME-1.iso`, `NAME-2.iso`. |
| `--media` | `dvd` | Capacity profile. See §8. |
| `--split` | off | If staged payload exceeds media capacity, emit multiple ISOs instead of failing. |
| `--volume-name NAME` | `ARCSYNC_YYYYMMDD` | Disc volume label (ISO + Joliet + HFS). Max 16 chars recommended; truncate with warning. |
| `--from` / `--to` | unbounded | Inclusive filter on capture date (local civil date). |
| `--include-hidden` | off | Include Photos "Hidden" album assets. |
| `--include-trashed` | off | Include Recently Deleted. Off by default. |
| `--edited` | off | Prefer the most recent rendered/edited derivative if present on disk. Default is the *original* file. |
| `--originals` | on | Explicit default. |
| `--thumb-size PX` | `240` | Long-edge of generated JPEG thumbs. Allowed 96–640. |
| `--title TEXT` | volume name | `<title>` and H1 of `index.html`. |
| `-n` / `--dry-run` | off | Scan, print plan and byte counts, write nothing. |
| `-v` | off | Verbose: one line per copied asset on stderr. |
| `-q` | off | Only errors + final summary. |
| `--json` | off | Final summary as one JSON object on stdout (human text still on stderr unless `-q`). |
| `--force` | off | Overwrite existing `--out`. |
| `-h` / `--help` | | Print usage, exit 0. |
| `--version` | | Print `arcsync 1.x.x` and git/sha if baked in. |

No subcommands in v1. One verb: archive.

### 6.3 Exit codes

| Code | Meaning |
|---|---|
| 0 | Success (including dry-run). |
| 1 | Usage / argument error. |
| 2 | Library unreadable or schema unrecognized. |
| 3 | No assets matched filters. |
| 4 | Staging I/O error (disk full, permission). |
| 5 | Payload exceeds media capacity and `--split` was not set. |
| 6 | `hdiutil` failed. |
| 7 | Interrupted (SIGINT/SIGTERM); staging dir left in place with a note). |
| 130 | POSIX convention if we choose to raise SIGINT rather than catch it. Prefer 7. |

---

## 7. Functional requirements

### F1. Discover assets

**Photos library mode**

1. Resolve the `.photoslibrary` bundle.
2. Open a *copy* of `database/Photos.sqlite` (and `-wal`/`-shm` if present) in a temp file. Never write to the live DB. The live DB is often locked by Photos.app; a `VACUUM INTO` or file copy is fine. If copy fails because of lock, instruct the user to close Photos and retry; do not try to be clever with PhotoKit.
3. Detect schema generation:
   - Photos 5 (Catalina): asset table `ZGENERICASSET`.
   - Photos 6+ (Big Sur onward): asset table `ZASSET`.
4. Discover the album↔asset junction table name at runtime from `Z_PRIMARYKEY` (historically `Z_26ASSETS`, `Z_27ASSETS`, …). Do not hard-code the number.
5. For each non-trashed (unless opted in), non-hidden (unless opted in) asset with an on-disk original:
   - UUID
   - capture timestamp (`ZDATECREATED`, Core Data epoch = Unix + 978307200)
   - `ZDIRECTORY` + `ZFILENAME` → path under `originals/`
   - original filename from `ZADDITIONALASSETATTRIBUTES.ZORIGINALFILENAME`
   - title if any
   - kind (photo / video / other) from `ZKIND`
   - favorite flag
6. Walk `ZGENERICALBUM`:
   - `ZKIND` distinguishes Folder vs Album vs smart/system albums. Include user Albums and user Folders. Skip trash, hidden, recents, imports-as-albums unless they are real user albums.
   - `ZPARENTFOLDER` rebuilds the folder tree.
   - Junction table supplies membership + sort order.
7. An asset may belong to many albums. The file is stored **once** (date tree). HTML pages in every album link to that one file.

**Directory mode (`--dir`)**

- Recurse. Treat each subdirectory as an album. Top-level files go in an album named "Unsorted" (or the directory basename).
- Capture date: EXIF/QuickTime `DateTimeOriginal` via ImageIO metadata if present, else `st_birthtime`, else `st_mtime`.
- Organization for HTML = the directory tree as the user sees it.

Supported extensions (case-insensitive):

```
jpg jpeg png gif tif tiff heic heif raw dng cr2 cr3 nef arw orf rw2
webp bmp
mov mp4 m4v mts m2ts avi mkv 3gp hevc
```

Unknown extensions: skip, count as skipped.

### F2. Physical layout on the disc (date tree)

All media files live under:

```
/media/YYYY/MM/DD/<filename>
```

Rules:

- `YYYY/MM/DD` from capture date in local time.
- `<filename>` is the *original* filename when available and unique in that day folder.
- Collision: `name-2.ext`, `name-3.ext`, …
- Never use the Photos UUID as the visible name unless the original name is missing.
- Preserve file mode 0644, copy data with `copyfile(3)` or `fcopyfile` so resource forks / Finder info are not required (ISO will not keep them usefully anyway).
- Do not rewrite EXIF. Byte-for-byte copy of the chosen file (original or edited derivative).

Edited derivatives, when `--edited` is on, are typically under `resources/` or the internally rendered JPEG next to the asset. If the derivative cannot be located, fall back to the original and warn.

### F3. Logical layout (HTML = user's organization)

Root of the ISO:

```
index.html                 # landing page
autorun.inf                # Windows hint
README.txt                 # how to open this disc
open-me.html               # alias of index.html (some autorun tools look for this)
css/style.css              # one small hand-written stylesheet, no webfonts
thumbs/<uuid_or_hash>.jpg  # generated thumbnails
albums/index.html          # album tree
albums/<slug>/index.html   # one page per album / folder
media/YYYY/MM/DD/...       # the real files
by-date/index.html         # calendar browse (year → month → day)
missing.txt                # assets referenced in the library but not on disk
manifest.tsv               # uuid, date, albums, dest path, bytes, kind
```

HTML requirements:

- Static files only. No JS framework. A *tiny* amount of vanilla JS is allowed for a lightbox (click thumb → full image) **if and only if** it stays under ~8 KB total and works with `file://`. Prefer CSS-only `:target` lightbox if feasible.
- Works offline from `file://` on Safari, Chrome, Firefox, Edge. Relative paths only.
- Album pages show thumbs in Photos sort order.
- Each thumb is a link to the full media file in `/media/...`.
- Videos: thumb is a generated still if possible (see F4); otherwise a labeled placeholder PNG shipped as a compiled-in 1–2 KB resource. The link is still the video file. Caption includes duration if known.
- Folder pages list child folders then child albums then loose assets.
- Landing page contains:
  - title
  - generated date
  - asset counts (photos / videos / missing / bytes)
  - links: Albums, By date, README
  - first 40 thumbs as a "sampler"
- Pages must be readable on a 1024×768 display. No external CDN.

Slug rules: lowercase, ASCII, `[a-z0-9-]`. Unicode album names get a NFKD-ish ASCII fallback plus a numeric suffix on collision. The visible heading keeps the real Unicode name (UTF-8 HTML).

### F4. Thumbnails

- Still images: ImageIO. Decode, draw into a CGBitmap scaled to `--thumb-size` on the long edge, encode JPEG quality ~0.72.
- HEIC/HEIF: ImageIO handles these on macOS. Good.
- RAW: ImageIO often has an embedded preview; use it. If decode fails, placeholder.
- Video: v1 tries, in order:
  1. An already-present Photos poster/thumb under `resources/derivatives` if we can find it cheaply.
  2. Else a compiled-in "video" placeholder.
  - Do **not** link AVFoundation from v1 if it blows the size budget or forces ObjC. Placeholder is acceptable.
- Thumbnails named by a stable id (asset UUID or sha256-12 of the source path in `--dir` mode) so album pages can share one thumb file.

### F5. ISO production

Staging directory: `$TMPDIR/arcsync-<pid>/stage/` (or `--work DIR` later). Always `rm -rf` on success. On failure leave it and print the path.

After staging:

```
hdiutil makehybrid \
  -iso -joliet -udf -hfs \
  -default-volume-name "$VOL" \
  -hfs-openfolder "$STAGE" \
  -o "$OUT" \
  "$STAGE"
```

Notes:

- `-hfs-openfolder` is the macOS equivalent of "open this when the disc is mounted." It opens the volume root in Finder, where `index.html` is the first obvious document. True autorun of HTML is **not possible on modern macOS** (and has not been since OS X). Document this honestly in the man page and README.
- Windows: write `autorun.inf`:

  ```
  [autorun]
  label=ARCSYNC_YYYYMMDD
  shellexecute=index.html
  icon=arcsync.ico
  action=Open photo archive
  ```

  `shellexecute` is the portable way to open HTML. Many modern Windows versions will still prompt rather than auto-open. That is expected. Also ship `index.html` at root so double-clicking works everywhere.
- Include a 16×16 / 32×32 ICO only if it fits the size budget; otherwise omit `icon=`.
- Do not bless a boot folder. This is a data disc.

### F6. Capacity and splitting

| `--media` | Budget (payload, not raw disc) | Intent |
|---|---|---|
| `cd` | 680 MiB | 80-min CD-R minus FS overhead |
| `dvd` | 4300 MiB | DVD-5 / DVD-R |
| `dvd-dl` | 7900 MiB | DVD-9 |
| `bd` | 23000 MiB | BD-R 25 GB |
| `none` | unlimited | single ISO of whatever size |

Overhead reserve is already baked into those numbers (thumbs + HTML + FS). If staged payload > budget:

- Without `--split`: exit 5, print how many discs would be needed and the largest file (a single file larger than the media is a hard fail even with `--split`).
- With `--split`: pack assets newest-first or oldest-first (oldest-first default — archives usually want chronology). Each volume is self-contained:
  - its own `index.html`
  - only the thumbs it needs
  - a "Disc N of M" banner and links that *name* the other volumes (they cannot hyperlink across discs)

Never split a single day folder across discs if it fits; split on day boundaries when possible, then on file boundaries.

### F7. Progress and honesty

stderr, unless `-q`:

```
arcsync: library  /Users/x/Pictures/Photos Library.photoslibrary
arcsync: assets   18422 photos, 901 videos (210 missing on disk)
arcsync: staged   47.2 GiB → 11 DVD volumes (--media dvd --split)
arcsync: copying  1201/19323  media/2011/07/04/IMG_2048.JPG
arcsync: thumbs   19323
arcsync: html     84 album pages
arcsync: iso      Family-2026-1.iso  4.21 GiB
...
arcsync: done     11 volumes, 3m41s
```

Never print per-file lines unless `-v`.

`missing.txt` lists UUID, album names, expected originals path. Missing files do not fail the run unless *every* asset is missing (exit 3).

### F8. Safety

- Read-only toward the Photos library.
- Refuse to stage into the library bundle.
- Refuse to overwrite `--out` without `--force`.
- Catch SIGINT/SIGTERM, delete the incomplete ISO, keep or delete staging (delete on SIGINT after a second press; first press stops copy and prints resume hint — v1 may simply clean up; pick one and document it). Simplest v1: clean staging + partial ISO, exit 7.
- Do not follow symlinks out of `--dir`.
- Paths: handle spaces, NFC/NFD, emoji album names.

---

## 8. Man page

Ship `arcsync.1`. Written in mdoc (`mandoc`) or classic man. Sections:

- NAME, SYNOPSIS, DESCRIPTION
- OPTIONS (every flag)
- EXIT STATUS
- FILES (default library path, staging)
- DISC LAYOUT
- AUTORUN (the honest paragraph: Windows may prompt; macOS opens the folder)
- PHOTOS LIBRARY (schema is private; this tool may break on a future Photos major)
- EXAMPLES
- SEE ALSO (`hdiutil`, `osxphotos`, Disk Utility)
- AUTHORS / LICENSE (mention the 1998 Win32-asm photo-CD lineage in AUTHORS, one sentence)

`make install` puts it in `$(PREFIX)/share/man/man1`.

---

## 9. Packaging

### 9.1 Source layout (suggested)

```
arcsync/
  Makefile
  README.md
  LICENSE          # ISC or MIT. Keep it short.
  prd.md           # this file, shipped in the repo
  src/
    main.c
    args.c
    photos_db.c    # sqlite + schema discovery
    fs_dir.c       # --dir walker
    copy.c
    thumbs.c       # ImageIO
    html.c
    iso.c          # fork hdiutil
    util.c
    banner.s       # arm64 Darwin write(2) version banner (required)
    banner_x86_64.s
    arcsync.h
  man/arcsync.1
  homebrew/arcsync.rb
```

Keep the C small. Target ≤ ~4–6 kLOC. 233 KiB is the *binary*, not the source.

### 9.2 Makefile

```
CC=cc
CFLAGS=-std=c99 -Os -Wall -Wextra -Werror -fno-exceptions
LDFLAGS=-lsqlite3 -framework CoreFoundation -framework ImageIO -framework CoreGraphics
# banner.s or banner_x86_64.s selected by uname -m; linked into the binary
```

`make`, `make strip`, `make test`, `make install PREFIX=...`, `make dist`.

A `size` target must fail CI / `make check-size` if the stripped binary exceeds 233 KiB.

### 9.3 Homebrew

Formula is local/tap-friendly:

```ruby
class Arcsync < Formula
  desc "Archive a Photos library to a hybrid CD/DVD ISO"
  homepage "https://github.com/example/arcsync"
  url "..."
  sha256 "..."
  license "ISC"
  depends_on :macos
  def install
    system "make", "PREFIX=#{prefix}", "install"
  end
  test do
    system "#{bin}/arcsync", "--version"
  end
end
```

No bottle required for v1; build-from-source is fine (it is a handful of C files).

MacPorts: a thin Portfile that runs the same Makefile.

`.pkg` installer is **not** required for v1. "Installable by pkg management tools" is satisfied by Homebrew + `make install`.

---

## 10. Architecture (for the implementer)

```
argv
  → parse args
  → open source (photos sqlite copy | directory walk)
  → build in-memory catalog
       Asset { id, captured, src_path, orig_name, kind, bytes, albums[] }
       Album { id, title, parent, children[], assets[] in order }
  → filter by date / hidden / trash
  → assign dest paths  media/YYYY/MM/DD/name
  → compute packing into volumes
  → for each volume:
       copy files
       generate thumbs (skip if already generated globally — generate once)
       write HTML / CSS / README / autorun.inf / manifest.tsv
       hdiutil makehybrid
  → print summary
```

Catalog can be a few hundred thousand assets. Use growable C arrays, not a fancy graph library. Strings interned in an arena.

SQLite access: one read-only connection to the *copied* DB. Parameterized queries only.

Core Data timestamps:

```
unix = zdatecreated + 978307200.0
```

Convert with `localtime_r` for the date tree.

Schema discovery sketch:

```sql
SELECT Z_ENT, Z_NAME FROM Z_PRIMARYKEY;
-- look for ASSET / GENERICASSET / GENERICALBUM
PRAGMA table_info(ZASSET);
-- find ZDIRECTORY, ZFILENAME, ZDATECREATED, ZTRASHEDSTATE, ZHIDDEN, ZKIND
```

Junction table: the table whose columns reference both album pk and asset pk. Probe `SELECT name FROM sqlite_master WHERE name LIKE 'Z_%ASSETS'`.

### HTML generation

Write with `fprintf`. Escape `& < > "`. UTF-8 declared in `<meta charset="utf-8">`. No templates engine.

CSS: system UI font stack, dark-on-light, thumbs in a responsive grid (`grid-template-columns: repeat(auto-fill, minmax(160px, 1fr))`). Keep `style.css` < 4 KB.

### Process hygiene

- `posix_spawn` / `fork+execv` `hdiutil` with an argument vector (never `system()`).
- Capture `hdiutil` stderr on failure and reprint it.
- All temp files under one root so cleanup is one `nftw`/`rm -rf`.

---

## 11. Testing

`make test` should run without a real Photos library:

1. Fixture directory of 8 small JPEGs in two subfolders + one tiny `.mp4` (or skip video if we do not vendor a clip; a 1×1 JPEG renamed is not a video test).
2. `arcsync --dir testdata --media none --out /tmp/t.iso`
3. Mount via `hdiutil attach -nobrowse` and assert:
   - `index.html` exists
   - `media/YYYY/MM/DD/` contains the files
   - album pages exist for both subfolders
   - `hdiutil detach`
4. `--dry-run` exits 0 and creates nothing.
5. Argument errors exit 1.
6. `make check-size` on the built binary.

A live-library test is manual and documented in README:

```
arcsync -n -v
arcsync --media none --out /tmp/mylib.iso
```

---

## 12. Documentation the tool itself ships

- `README.txt` on the disc (plain ASCII/UTF-8): what this disc is, how to open `index.html`, that originals are under `media/`, that HEIC may need a codec on old Windows.
- `README.md` in the repo: build, brew, TCC/Full Disk Access note, "close Photos.app if the DB copy fails." One short paragraph on the Win32-asm banner (`banner.s`) so the Bot does not treat it as dead code.
- Man page as specified.

TCC note to include: Terminal (or iTerm) may need Full Disk Access to read `~/Pictures/Photos Library.photoslibrary`. The tool cannot grant this for the user.

---

## 13. License, identity, versioning

- License: ISC (short, matches tiny C tools).
- Version: semver. v1.0.0 is the first ISO-producing release that meets this PRD.
- `--version` format (from `arcsync_banner()`): `arcsync 1.0.0 — Win32 asm photo-CD tools, 1998; this is that program after the API grew up.`

---

## 14. Implementation order for the coding agent

Do these in order. Each step should leave a compiling binary.

1. `Makefile` + `main.c` (with the Win32-asm header comment) + `banner.s` / `banner_x86_64.s`. `--version` must go through `arcsync_banner()`.
2. Argument parser. No `getopt_long` portability issue — macOS has it.
3. `--dir` walker + date-tree copy into a staging folder. Stop here and inspect.
4. Thumbnail JPEG via ImageIO.
5. HTML + CSS + README + autorun.inf for the staged tree.
6. `hdiutil makehybrid` wrapper (`--media none` first).
7. Capacity math + `--split`.
8. Photos.sqlite reader + schema discovery + album tree.
9. Merge Photos mode into the same staging/HTML/ISO path.
10. `arcsync.1`, Homebrew formula, `make install`, `make check-size`.
11. Dry-run, JSON summary, missing.txt, manifest.tsv.
12. Manual run against a real library. Fix schema mismatches.

Do not start with Photos. The directory mode is the contract; Photos is a catalog frontend on top of it.

---

## 15. Risks and explicit acceptances

| Risk | Acceptance |
|---|---|
| Apple changes Photos.sqlite | Tool may break. Fail with "unrecognized schema" and a table dump hint. No crash. |
| iCloud-optimized library | Missing originals listed, not downloaded. |
| 233 KiB budget | If ImageIO usage + strings exceed it, drop the ICO, shrink help text, compile `-Os`, do not add more frameworks. |
| macOS will not auto-open HTML | Documented. HFS open-folder + root `index.html` is the ceiling. |
| Windows AutoPlay disabled | `autorun.inf` + double-click `index.html` is the ceiling. |
| Live Photos are a still + a short MOV | Treat as two assets if both exist; HTML caption can say "Live". No pairing UI in v1. |
| Burst photos | Each file is an asset. No stacking. |
| Shared albums / My Photo Stream | Include only if files exist locally. |
| Filename charset on ISO 9660 | Joliet + UDF carry Unicode. HFS+ too. Do not rely on Level-1 8.3 names. |

---

## 16. Success criteria for v1

A reviewer can:

1. `brew install --build-from-source ./homebrew/arcsync.rb` (or `make install`).
2. `man arcsync`.
3. `arcsync --dir ~/Pictures/vacation --media dvd --out ~/Desktop/vac.iso` and get a mountable hybrid ISO whose `index.html` shows the folder structure and whose `media/` tree is dated.
4. Point it at the default Photos library and get albums in the HTML and dates on disk.
5. `ls -l $(which arcsync)` reports ≤ 233 KiB stripped.
6. `file` on the binary says Mach-O, dynamically linked against system libraries only.
7. Mount the ISO on the same Mac; Finder opens the volume. Safari can open `index.html` from the volume and click through to full images.

That is the product.

---

## 17. Out-of-scope ideas to not implement unless asked

- Resume / incremental discs (`--since-manifest`).
- `arcsync burn` wrapping `hdiutil burn`.
- GUI wrapper.
- Embedding a Windows autorun helper `.exe` (WinOpen etc.). Size + legality + "C on macOS" all argue against it.
- Writing our own ISO9660.
- Swift rewrite.
- Reading iOS backups.

---

## 18. Suggested first user-visible output of `--help`

```
arcsync 1.0.0 — archive a photo library to a hybrid CD/DVD ISO

usage: arcsync [options]

  --library PATH     Photos library (.photoslibrary)
  --dir PATH         plain photo folder instead of Photos
  --out PATH         output .iso (default ./arcsync-YYYYMMDD.iso)
  --media TYPE       cd | dvd | dvd-dl | bd | none   (default dvd)
  --split            emit multiple volumes if needed
  --volume-name N    disc label
  --from DATE        include capture dates on/after YYYY-MM-DD
  --to DATE          include capture dates on/before YYYY-MM-DD
  --thumb-size PX    thumbnail long edge (default 240)
  --title TEXT       gallery title
  --edited           copy edited derivatives when present
  --include-hidden
  --include-trashed
  -n, --dry-run
  -v, --json, -q, --force
  -h, --version

See arcsync(1).
```

End of PRD.
