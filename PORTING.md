# Porting changes from upstream RPCS3

This fork tracks **RPCSX/rpcsx**, which vendors a **restructured** copy of
RPCS3. Because of that restructuring, raw RPCS3 patches rarely apply cleanly.
This is the recipe that worked for the SPU re-vendor (Reduced Loop) and the
smaller fixes.

## 1. Directory / API mapping (RPCS3 → this tree)

| Upstream RPCS3 | Here |
|---|---|
| `Utilities/` | `rpcs3/util/` |
| `rpcs3/Emu/Cell/Modules/` (HLE) | `ps3fw/` |
| `rpcs3/Emu/Cell/lv2/` (kernel) | `kernel/cellos/` (rewritten) |
| `rpcs3/Emu/Cell/SPU*`, `Emu/CPU/CPUTranslator*` | same paths |

API differences to expect inside merged-in code:
- `::offset32(&spu_thread::x)` → `OFFSET_OF(spu_thread, x)` (both compile; `offset32` still exists)
- `spu_ptr(&spu_thread::x)` (member-ptr) → `spu_ptr<T>(OFFSET_OF(spu_thread, x))`
- `spu_context_attr(...)` is an **upstream** helper (present here)
- The SPU decrementer uses inline-asm `CNTVCT_EL0`, **not** `llvm.readcyclecounter`
  (which lowers to `PMCCNTR_EL0` and traps in userspace on Android) — keep ours.

## 2. The re-vendor recipe (per file, 3-way merge)

`git merge-file` does the heavy lifting. `BASE` = the RPCS3 commit this tree
forked from; `THEIRS` = current RPCS3 `master`; `OURS` = our file.

```sh
UP=/path/to/rpcs3          # a clean RPCS3 checkout
base=$(cd $UP && git rev-parse a863e94c2^)   # SPU base; see note below
theirs=$(cd $UP && git rev-parse master)
git -C $UP show "$base:rpcs3/Emu/Cell/Foo.cpp"   > /tmp/base
git -C $UP show "$theirs:rpcs3/Emu/Cell/Foo.cpp" > /tmp/theirs
sed 's/\r$//' rpcs3/Emu/Cell/Foo.cpp > /tmp/ours    # normalize CRLF->LF (autocrlf)
git merge-file -p /tmp/ours /tmp/base /tmp/theirs > /tmp/merged   # count <<<<<<< blocks
```

Resolve conflicts: **take theirs** for new logic; **keep ours** for the API
idioms above and the CNTVCT decrementer. Then build-iterate (see §4). For badly
reordered regions, splice the function wholesale from a backup of OURS or from
THEIRS. Files are autocrlf (working tree CRLF, repo stores LF) — work in LF.

> **rpcs3 base:** the SPU recompiler forked around `a863e94c2^` (RPCS3 ~Mar 2026).
> Other subsystems may differ — find the base by diffing OURS against a few
> candidate upstream commits and picking the closest.

## 3. What grafts vs. what doesn't
- **Grafts:** `util/` (Thread, TAR, Config), `Emu/CPU` recompilers, the SPU subsystem.
- **Doesn't (diverged):** HLE modules in `ps3fw/` (older design — e.g. cellDmux/cellMusic),
  the `kernel/cellos/` lv2 rewrite, ISO loader (`dev/iso.cpp`), the RSX shader assembler.
- **N/A:** `gl/*` (Android is Vulkan), Qt/desktop, Apple-ARM-only fixes, LLVM≥21-only changes (we're on LLVM 20).

## 4. Build / strip (arm64-v8a)
```sh
cmake --build build
cp build/librpcsx-android.so build/librpcsx-android-arm64-v8a-armv8-a.so
llvm-strip build/librpcsx-android-arm64-v8a-armv8-a.so   # ~97 MB, sideload this
```
A clean build does **not** mean correct emulation — SPU/PPU miscompiles surface
as in-game glitches, not build errors. Always game-test before merging to `dev`.
