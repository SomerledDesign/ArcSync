# ArcSync handoff — 2026-09-17

- Owner: Jarvis
- Repo: https://github.com/SomerledDesign/ArcSync (private)
- Local: Dropbox/Penus/iWasCoding/ArcSync
- Status: **v1 --dir pipeline complete**; Photos.sqlite reader present (Library album; user-album junction deferred)
- Binary ~72 KiB stripped (≤233 KiB)
- `make test` mounts hybrid ISO from testdata/vacation
- Next polish: richer Photos album tree via junction; `--cloud derivative` file hunt; EXIF capture dates

- 2026-09-17: Full ICLOUD/NAS recipes + OPTIONS/EXAMPLES shipped in `man/arcsync.1` (users need not read prd.md).
- 2026-11-09: Release v1.1.0. `--cloud derivative` local Photos fallback, EXIF/QuickTime capture dates for folder archives, experimental Windows port, terminal `--demo`, and `make uninstall`. `--cloud` default is `fail`. Stripped arm64 binary 91,672 bytes (limit 238,592).
