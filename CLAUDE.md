# CLAUDE.md

## IDA reverse-engineering reference

`patch\terranx_mod.exe.i64` is an IDA Pro database reverse-engineering the original
Alpha Centauri / SMAX binary that this repo's C++ source calls into directly via
hardcoded addresses (see `src/wtp_terranx.cpp`, `tools/idapatch.py`, imagebase 0x400000).

It has been exported to plain text for lookup without needing IDA open:

- `C:\Users\Tim\.claude\ida_exports\terranx_mod\names.txt` — address -> symbol name for
  all 11,002 user-named locations (functions and data).
- `C:\Users\Tim\.claude\ida_exports\terranx_mod\comments.txt` — address -> comment
  (mostly auto-generated parameter names; real analysis notes are sparse but present).
- `C:\Users\Tim\.claude\ida_exports\terranx_mod\terranx_mod.asm` — full disassembly
  listing (~134MB) with names/comments/xrefs inlined. Grep this for a specific address
  or function name to see what the original game code actually does.

These were generated via IDC scripts run interactively in IDA (this machine's IDA Free
install has no working headless batch mode or IDAPython — only interactive IDC via
File > Script file...). If the .i64 is updated, these exports should be regenerated the
same way rather than trusted as current.
