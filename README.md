# ArcSync

Archive a macOS Photos library (or a plain photo folder) to a hybrid CD/DVD/Blu-ray `.iso` with a dated media tree and a static HTML gallery.

> Win32 asm photo-CD tools, 1998; this is that program after the API grew up.

It is the 2026 terminal equivalent of those late-90s “burn my pictures to a CD with a browser index” apps — no GUI, tiny binary, man page, brew-installable.

Optical media is still a useful write-once archive: offline, readable decades later if the filesystem is boring (ISO 9660 + Joliet + UDF + HFS+). Photos.app hides originals inside a package and a Core Data database. ArcSync gives you:

1. The **real files**, named sanely, filed by capture date  
2. A **human-browsable** gallery that still looks like *your* albums/folders  
3. An **image you can burn** with Disk Utility / `hdiutil burn` / Windows Image Burner  

This tool does only that.

## Install (Homebrew)

```sh
brew tap SomerledDesign/homebrew-tap
brew install arcsync
```

Or from a checkout:

```sh
make
make install          # PREFIX=/usr/local by default
# make install PREFIX=/opt/homebrew
make uninstall
```

## Quick start

```sh
make && make test && make check-size

# Plain folder of photos
./arcsync --dir ~/Pictures/vacation --media dvd --split --out ~/Desktop/vac.iso

# See what is actually on this Mac (default --cloud fail)
./arcsync -n

# After Download Originals has landed overnight
./arcsync --media dvd --split --out ~/Desktop/family.iso

# NAS / RAID masters (skip Photos)
./arcsync --dir /Volumes/NAS/Masters --media none --out ~/Desktop/masters.iso

# Folder archive + burn to optical (drive required)
./arcsync --dir ~/Pictures/vacation --media dvd --burn --out ~/Desktop/vac.iso

# Test burn of whatever happens to be local
./arcsync --cloud skip --media none --out /tmp/test.iso
```

## Hard constraints

| Constraint | Requirement |
|---|---|
| Language | C99 + required `src/banner.s` / `src/banner_x86_64.s` souvenir |
| Binary size | **≤ 233 KiB** stripped Mach-O (`make check-size`) |
| Interface | argv + stderr progress + man page — no GUI |
| Host | macOS 12+ (best-effort older Photos schemas) |
| Runtime deps | System dylibs only + `/usr/bin/hdiutil` |
| Non-goals | No PhotoKit / ObjC / iCloud download *inside* this binary; originals copied byte-for-byte |

Rationale for calling `hdiutil` instead of shipping an ISO encoder: a correct Joliet+UDF+HFS hybrid writer would explode both size and defect surface. Forking a system tool is the Unix thing to do and keeps the 233 KiB budget.

## Media capacity

Payload budgets (thumbs + HTML + FS overhead already reserved):

| `--media` | Budget | Intent |
|---|---|---|
| `cd` | **680 MiB** | 80-min CD-R |
| `dvd` | **4300 MiB** | DVD-5 / DVD-R (default) |
| `dvd-dl` | **7900 MiB** | DVD-9 |
| `bd` | **23000 MiB** | BD-R 25 GB |
| `none` | unlimited | single ISO of whatever size |

If staged payload exceeds the budget:

- without `--split` → exit **5** (prints how many discs you’d need)
- with `--split` → several self-contained `NAME-1.iso`, `NAME-2.iso`, … each with its own `index.html`

A single file larger than the media is a hard fail even with `--split`.

## Cloud policy (`--cloud`)

ArcSync copies files that are **already on this Mac**. It does not download from iCloud.

| Mode | Behavior |
|---|---|
| `fail` (**default**) | Any selected non-original → exit **8**, nothing staged — safe for the one-liner |
| `skip` | Local originals only; list the rest in `missing.txt` (knowingly incomplete) |
| `derivative` | Local originals when present; else best local Photos preview under `resources/`; tag `quality=derivative` in `manifest.tsv`. Still-missing stay skipped. Exit 0 if anything copied. Slideshow, not archive. |

### Overnight pull, morning burn (family / Optimize Mac Storage)

1. Dry-run to see the gap: `arcsync -n` (default `--cloud fail`)  
2. Photos → Settings → iCloud → **Download Originals to this Mac**  
3. Leave the Mac awake on power overnight  
4. Morning dry-run again until cloud-only is as close to zero as Photos allows  
5. Burn: `arcsync --media dvd --split --out ~/Desktop/family.iso`  
6. Spot-check the mounted disc (`index.html` + a few full images), then you may switch Photos back to Optimize  

Default `--cloud fail` is deliberate: a bare `arcsync --out …` will not silently omit iCloud originals.

### Working photographers (NAS / folder)

If the camera files already live on disk:

```sh
arcsync --dir /Volumes/NAS/Jobs/2026 --media none --out job.iso
```

That is the 2000-era workflow: the files were next to the machine because there was no cloud to hide them in.

`--dir` names and sorts into `media/YYYY/MM/DD/` using capture time from EXIF (images) or QuickTime/movie metadata (videos) when ImageIO can read it; otherwise birthtime/mtime.

**Full Disk Access:** Terminal (or iTerm) may need FDA to read `~/Pictures/Photos Library.photoslibrary`. The tool cannot grant that for you. Close Photos.app if the database copy fails on a lock.

## Disc layout

```
index.html                 landing page
open-me.html               alias of index.html
autorun.inf                Windows hint
README.txt                 how to open this disc
css/style.css
thumbs/                    generated JPEG thumbnails
albums/                    album / folder pages
media/YYYY/MM/DD/          real files (byte-for-byte)
by-date/index.html
missing.txt
manifest.tsv
```

## Exit status

| Code | Meaning |
|---|---|
| 0 | Success (including dry-run) |
| 1 | Usage / argument error |
| 2 | Library unreadable or schema unrecognized |
| 3 | No assets matched filters |
| 4 | Staging I/O error |
| 5 | Over capacity without `--split` |
| 6 | `hdiutil` failed |
| 7 | Interrupted |
| 8 | `--cloud fail` and a non-local original |

## Build

```sh
make              # C99 -Os + banner.s matching uname -m
make check-size   # fail if stripped > 233 KiB
make test
make install PREFIX=/usr/local
make uninstall
```

## Homebrew formula

Shipped under [`homebrew/arcsync.rb`](homebrew/arcsync.rb) and published to
[`SomerledDesign/homebrew-tap`](https://github.com/SomerledDesign/homebrew-tap):

```ruby
class Arcsync < Formula
  desc "Archive a Photos library to a hybrid CD/DVD ISO"
  homepage "https://github.com/SomerledDesign/ArcSync"
  url "https://github.com/SomerledDesign/ArcSync/archive/refs/tags/v1.0.0.tar.gz"
  sha256 "…"  # release tarball
  license "MIT"
  depends_on :macos

  def install
    system "make", "PREFIX=#{prefix}", "install"
  end

  test do
    assert_match "arcsync 1.0.0", shell_output("#{bin}/arcsync --version 2>&1")
    assert_path_exists man1/"arcsync.1"
  end
end
```

No bottle required for v1; build-from-source is a handful of C files.


## Windows (x86_64)

Experimental MinGW port. Primary workflow is `--dir` (Photos.app is macOS-only).
ISO images are written by an **in-tree ISO 9660 + Joliet** writer (`src/iso9660.c`);
`--burn` is not wired yet. Thumbnails are placeholders until WIC lands.

```sh
# On Windows with MinGW-w64, or cross from macOS/Linux:
make -f Makefile.mingw
# produces arcsync.exe

./arcsync.exe --dir D:\Photos\vacation --media dvd --out family.iso
```

The classic 233 KiB stripped limit applies to the Darwin build; Windows may be larger.

## Docs

```sh
man arcsync              # after make install / brew install
man ./man/arcsync.1      # from a checkout
```

Overnight iCloud + NAS recipes live in the man page section **ICLOUD AND OPTIMIZED LIBRARIES**. Developer product decisions: [`spec/prd.md`](spec/prd.md), [`DECISIONS.md`](DECISIONS.md).

## License

MIT © Kevin Murphy \<somerleddesign@gmail.com\>

Win32 asm photo-CD tools, 1998; this is that program after the API grew up.
