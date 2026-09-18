# ArcSync decisions (vs PRD v1.4)

Recorded 2026-09-17 with Kevin:

| Topic | PRD | Decision |
|-------|-----|----------|
| License | ISC | **MIT** — Kevin Murphy <somerleddesign@gmail.com> |
| First target | Photos or `--dir` | **`--dir` first**; Photos on the big laptop later |
| Remote | example homepage | **https://github.com/SomerledDesign/ArcSync** (private) |
| Burn | v1.1 / out of scope for v1 | **In scope** — `--burn` wraps `hdiutil burn` after ISO |
| 233 KiB + no PhotoKit | hard | **Hard** |

FDA: local-tools session may lack Full Disk Access for Photos.sqlite. Grant FDA to Terminal/Cursor before Photos mode.

| Windows x86_64 | PRD: no Windows tool | **Paused** until a Windows box — MinGW scaffolding + in-tree ISO 9660+Joliet landed; --dir primary; --burn stub |
