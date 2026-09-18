# ArcSync

Archive a photo library (or a plain folder) to a hybrid CD/DVD/Blu-ray ISO with a static HTML gallery.

> Win32 asm photo-CD tools, 1998; this is that program after the API grew up.

## Install (Homebrew)

```sh
brew install SomerledDesign/tap/arcsync
# or:
brew tap SomerledDesign/tap
brew install arcsync
```

## Quick start

```sh
make
make test
make check-size

# Folder of photos
./arcsync --dir ~/Pictures/vacation --media dvd --split --out ~/Desktop/vac.iso

# After Download Originals (run from Terminal with Full Disk Access)
./arcsync -n --cloud fail
./arcsync --cloud fail --media dvd --split --out ~/Desktop/family.iso

# Burn after ISO (needs optical drive)
./arcsync --dir ./testdata/vacation --media none --burn --out /tmp/t.iso
```

## Hard constraints

- C99 + required `src/banner.s` souvenir
- Stripped binary ≤ **233 KiB**
- No PhotoKit / ObjC / iCloud download inside this binary

## License

MIT © Kevin Murphy <somerleddesign@gmail.com>

## Docs for users

```sh
man arcsync          # after make install
# or:
man ./man/arcsync.1
```

Overnight iCloud download + NAS/`--dir` recipes live in the man page
(**ICLOUD AND OPTIMIZED LIBRARIES**), not only in the developer PRD.

## Spec (developers)

See [`spec/prd.md`](spec/prd.md) and [`DECISIONS.md`](DECISIONS.md).
