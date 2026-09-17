# ArcSync

Archive a photo library (or a plain folder) to a hybrid CD/DVD/Blu-ray ISO with a static HTML gallery.

> Win32 asm photo-CD tools, 1998; this is that program after the API grew up.

## Status

Scaffold (PRD §14 steps 1–2): compiling `arcsync` with `--help` / `--version` (banner from `src/banner.s`) and a full argument parser. Pipeline (`--dir` → stage → thumbs → HTML → `hdiutil` → optional `--burn`) is next.

Hard constraints (do not relax):

- C99 only (+ required `banner.s` souvenir)
- Stripped binary ≤ **233 KiB**
- No PhotoKit / ObjC / iCloud download inside this binary

## Build

```sh
make
make test
make check-size
```

## License

MIT © Kevin Murphy <somerleddesign@gmail.com>

## Spec

See [`spec/prd.md`](spec/prd.md).
