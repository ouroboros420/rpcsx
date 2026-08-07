<p align="center">
  <img src=".github/icon.png" width="120" alt="RPCSX-Clanker icon">
</p>

<h1 align="center">RPCSX-Clanker — core <sub>(experimental)</sub></h1>

The **PlayStation 3** emulation core (RPCS3-derived) used by the
[RPCSX-Clanker Android app](https://github.com/Ouroboros420/rpcsx-ui-android),
built into `librpcsx-android.so` for arm64. A continuously-updated fork of
[RPCSX/rpcsx](https://github.com/RPCSX/rpcsx), kept in sync with current
**RPCS3 0.0.41** content for the emulation core and extensively adapted for
ARM64/Android. It installs as the **`net.rpcsx.clanker`** package so it runs
side-by-side with official RPCSX.

---

## Two requests if you use this build

This is **not original work**. [**RPCS3**](https://rpcs3.net/) is the PlayStation 3
emulator for desktop (PC / laptop). The unofficial **RPCSX** group ported RPCS3 to
Android — but that port fell behind and is now running an outdated RPCS3 base. What
this project does is bring the **latest RPCS3 changes into the RPCSX Android tree to
update it**, with heavy AI assistance. If this build is useful to you, please honour
these two things:

### 1. Donate to the teams who actually built the emulator
Every line of real emulation here came from them. Support them:
- **RPCS3** — <https://rpcs3.net/> · Patreon: <https://www.patreon.com/Nekotekina>
- **RPCSX** — <https://github.com/RPCSX/rpcsx>

### 2. Play Demon's Souls online — and find me

---

### How it's made
The work of merging and porting these RPCS3/RPCSX changes into this restructured
tree is done with heavy **AI assistance** (Claude). Treat it as **experimental** —
verify before relying on it, and expect rough edges.

### Why this exists
The RPCSX Android port is built on an older RPCS3 snapshot and had stopped tracking
upstream, leaving Android a generation behind the desktop emulator. This project
exists to **close that gap** — continuously porting current RPCS3 fixes and features
into the RPCSX Android core. It is not a permanent splinter fork: all credit for the
emulator belongs to the **RPCS3** and **RPCSX** developers, who are welcome to take
anything useful from here.

> Piracy is not permitted. Do not ask for games or system files.

---

## Things to be cautious about (read before relying on it)

<details open>
<summary><b>Known issues, trade-offs &amp; gotchas</b></summary>

- **Experimental & AI-assisted.** This tree is restructured and the porting is done with
  AI help. Expect bugs, regressions between builds, and behaviour that differs from
  official RPCS3/RPCSX. Do not use it as a reference for "how RPCS3 behaves."
- **Fresh, separate install.** The package is `net.rpcsx.clanker` for side-by-side use
  with official RPCSX. That means a **clean, empty install** — games list, configs,
  saves, caches and firmware are **not** carried over from another RPCSX install. Set up
  firmware and games again.
- **Frame-time jitter under Write Color Buffers.** Titles that read their own color
  buffers back every frame (e.g. Demon's Souls) hold 30fps but show uneven frame pacing.
  This is the inherent cost of the synchronous GPU→CPU readback on a mobile tile GPU,
  not a fork bug — the readback path matches upstream and the only full offload is
  Multithreaded RSX (off by default, experimental).
- **RPCN TLS / custom hosts.** RPCN server-certificate verification is off (same as
  upstream), and the derived login password uses a constant salt (an RPCN protocol
  requirement, not a fork choice). Credentials are now stored in app-internal storage
  (see below), but avoid adding untrusted custom RPCN hosts on a network you do not
  control.
- **RPCN online toggle is not fully authoritative yet.** A **per-game config** can pin a
  title online, which overrides the global RPCN enable/disable toggle at boot, and an
  already-open session is not torn down on disable. To reliably go offline, also clear
  the per-game config's net setting.
- **Battery-saver is default ON** (it cut idle CPU with no fps cost on the test device).
  It can be turned off in the app. All *other* power features (big-cluster affinity,
  low-power WFE, thermal cap, ADPF hints) are **default OFF and unproven** — enabling
  them may regress fps or stability; treat them as experiments.
- **Smooth-shaders (async interpreter) is forced OFF** — that path currently freezes
  (black screen/ANR). The synchronous interpreter is used instead (occasional first-use
  stutter, but it never hangs).
- **No persistent SPU object cache.** The on-disk SPU cache was removed (it wrote nothing
  on-device), so SPU code is recompiled each launch — expect a short warm-up every boot.
- **First-launch compilation is heavy.** PPU/shader compilation on first boot of a game is
  RAM- and CPU-intensive; a device-scaled compile-budget mitigation is in place but
  low-RAM devices may still struggle.
- **Not all games work.** Some titles still crash or stall (e.g. God of War 3 / SPURS-stall
  class). This is an emulator-wide reality, not specific to this fork.
- **arm64 only.** Developed and tested on a Snapdragon 8 Gen 2 (Adreno 740 / Turnip).
  Other arm64 SoCs and GPUs should run it too, but that is the configuration that was
  tested — behaviour on other hardware is unverified.
- **Savestates** may not be compatible across versions.
</details>

---

## Changes vs upstream RPCSX (baseline `b41e09a04`)

<details>
<summary><b>Android platform &amp; runtime</b></summary>

- **LLVM-target auto-detection** — detects the real SoC big/prime core via MIDR instead of a fixed CPU target; registers all LLVM targets; guards against in-order-core misdetection; pins LLVM features to runtime HWCAP (sha3/dotprod/sve).
- **Crash logging** — native backtrace on Android crashes; SIGABRT and SIGBUS captured.
- **Shutdown / lifecycle fixes** — fixed the Emulation Join Thread self-join deadlock; implemented `qt_events_aware_op` to fix stop sequencing; do not `ensure()`-crash on surface loss when no pad thread exists.
- **RLIMIT_MEMLOCK** raised so hot guest pages (vm/main/stack) can be pinned against long-session reclaim (ported from aps3e, credited).
- **Compile-thread management** — lower compile-thread priority (`nice`) on Android to reduce ANRs; Android compile-thread cap applied to the effective thread count.
- **Device-scaled PPU compile memory budget** — fixes OOM SIGABRT during first-launch concurrent PPU LLVM module compilation; budget derived from ActivityManager-reported usable memory (pushed via JNI), since `get_total_memory()` over-reports on Android (zRAM).
- **Log de-noising** — high-traffic channels raised for release builds; benign memory-lock failure logged once.
- **Package rename** to `net.rpcsx.clanker` with applicationId-derived DocumentsProvider authority (avoids `INSTALL_FAILED_CONFLICTING_PROVIDER` alongside official RPCSX).
- **Correct CalVer banner** — the in-app core version is the HEAD commit date+time, regenerated at build so it always matches the shipped build.
</details>

<details>
<summary><b>CPU, recompilers &amp; JIT (PPU / SPU / LLVM / ARM64)</b></summary>

- **Full SPU recompiler re-vendor** to current upstream (the "Reduced Loop" engine) plus upstream's authoritative ARM intrinsic codegen.
- **ARM64 SPU codegen**: NEON table lookups for SHUFB/VPERM; SMULL/UMULL for integer multiplies; UDOT for SUMB on dotprod CPUs; ARMv8.6 I8MM/dotprod for GBH/GBB gather-bits; TBL for ROTQBY-family shuffles; idiomatic FSM/FSMH/FSMB/FSMBI lowering; BSL-for-FCGT via inline asm (LLVM codegen workaround).
- **ARM64 SPU stability**: stopped emitting fragile NEON tbl2/tbx2 (SPU register corruption); do not advertise SVE/SVE2 to LLVM (SPU miscompile); inline SPU decrementer via `CNTVCT_EL0` (Android-correct; `readcyclecounter` traps on most devices).
- **PPU**: enabled LLVM IR optimization (EarlyCSE) on ARM (was x64-gated off); disabled the branch-folding loop that miscompiles (Asura's Wrath, RPCS3 #18287); analyser infinite-loop fix; 64-bit shift for SLDI; vector-NaN fixup default on; reservation compare size 127→128; PPU reservation priority over SPUs; fixed null dispatch for `adde.`/`subfe.` Rc-variants (latent interpreter crash).
- **SPU correctness**: `spu_channel` occupy/wait bit-collision fix (Uncharted 2 SPURS hang); mis-ported cache-line waiter fix; restored dropped GPR-barrier guard in store elimination; double-check reservation data before PUTLLC writeback; full reservation-notification reimplementation; restored `COOPERATE_WITH_SYSTEM` guards; implemented `sys_spu_image_open_by_fd`.
- **JIT**: built against LLVM 19.1.7 to match upstream; versioned compiled-code caches (PPU `v9-kusa`) so codegen fixes reach existing installs; codegen targets the big/prime ARM core; ARM `busy_wait` scaled to the hardware timer; `isb` for pause.
- **vm**: skip read-only (SPU reservation-check) range locks when acquiring an exclusive writer lock (less over-synchronization on the reservation path).
</details>

<details>
<summary><b>Graphics — Vulkan / RSX</b></summary>

- **Texture-cache cluster re-vendored to upstream 0.0.41** — adopted upstream's surface/texture-cache core (surface_utils/surface_store/texture_cache/texture_cache_helpers) plus per-surface `resolution_scaling_config`, on top of the `address_range32`, 2-arg `simple_array` and `format_ex` prerequisites. **Texture pop-in eliminated on-device.**
- **`format_ex` texel-remapping producer** — replaced the fork's narrower inline INT8 texel-conversion path with upstream's unified `format_ex` producer (SNORM/sRGB/BX2 via the proper channel-remap shuffle + the FORMAT_FEATURE bits the shader expects). The common 8-bit + identity-remap case is byte-identical; 16-bit-channel formats and non-identity remaps are now correct. **Validated on-device.**
- **Vulkan barrier/hazard correctness ports** (upstream): WAW/RAW texture-cache flush hazards; scratch-buffer barriers around the CPU detiler and texture uploads; per-source image-layout dedup in the texture copy path; occlusion-query copy barrier; cubemap detection/mip handling; WAW barrier after the black/letterbox present clear; `dma_transfer` barrier scope.
- **Host memory coherency** — ported `mm_flush(ranges)` and wired it into the nv0039 DMA and nv3089 blit paths so a CPU copy/blit reconciles the deferred-mprotect queue before touching the regions (weak-memory ARM correctness).
- **Adreno / Turnip support**: classify Adreno (Mesa Turnip + Qualcomm proprietary) as a driver vendor; fixed an Adreno compute workgroup-size regression (1→128); recognize Adreno in the remaining driver-vendor switches.
- **Mobile VRAM budget** — caps device-local cache to 2/3 of detected memory on shared-memory ARM (fixes inFamous-class OOM); only auto-picks when the user left the limit at default.
- **FIFO / transfer-engine crash hardening** — bounds-check transfer destination extents; recover instead of following FIFO jumps/calls into unmapped IO; bounds-check vertex upload source and index arrays against stale-draw wild reads/writes; skip textures with an unmapped source offset.
- **Texture / format fixes** — deswizzle of sub-4-byte and wide texel formats (full upstream co-vendor); DXT45 `u128` upload guarded against unaligned source on ARM; safe BC2/BC3 decode; fixed swapped width/height in `NV309E_SET_FORMAT`.
- **Android-specific (ahead of upstream)** — abort-safe fence/event guards (savestate-freeze fix upstream lacks); Android surface-lost swapchain recreate; ARM64 FIFO idle-wait/WFE park; async pipeline-compiler worker heuristic tuned above upstream's cap.
- **Mobile-tiler jitter tuning** (Android, idle-GPU exploiting) — lowered texture-cache predictor confidence threshold and tightened the ZCULL harvest cadence so synchronous occlusion/readback results are collected before the guest blocks on them; instrumented the throttled perf log with per-interval frame-time spread + readback-source attribution.
</details>

<details>
<summary><b>Shaders</b></summary>

- **Fragment-program decompiler ported to upstream 0.0.41** — adopted upstream's Assembler engine (CFG/IR + RegisterAnnotation/RegisterDependency lane-mask passes) wholesale, replacing the fork's legacy decompiler. Closes the one default-render-path subsystem that was a generation behind.
- **0.0.41 ROP / alpha-test shader specialization** — compile-time per-shader ROP specialization instead of runtime branches; SPIR-V 1.5 path.
- **Producer-side fixes the fork lacked** — raise `TEXTURE_FORMAT_CONVERT` so texel conversion actually emits (fixed disappearing geometry; enabled vegetation/foliage rendering); produce/consume `DISABLE_EARLY_Z` for cyclic-zeta draws; gate fragment color outputs by `mrt_buffers_count`.
- **Backend bridges** — emit the fp16 GLSL extension whenever the GPU supports it; bridge fragment constants to upstream's indexed `_fetch_constant` model (VK and GL).
- **Cast-shadow fix** (Android, validated) — suppress the alpha-test discard in depth-only / shadow-caster passes so the full character casts a shadow. Trade-off: alpha-tested geometry (foliage, chain-link) casts a solid rather than cut-out shadow.
- **ShaderInterpreter variant engine** ported (backend-agnostic).
</details>

<details>
<summary><b>Emulation core — lv2 / HLE / loader / crypto</b></summary>

- **Massive HLE (ps3fw) re-vendor to current RPCS3** — dozens of modules: cellSysutil, cellSaveData, cellGame, cellSysCache, cellFs, cellFont, cellHttp/HttpUtil, cellSsl, cellSync/Sync2, cellRtc, cellL10n, cellVideoOut, cellAudio/AudioOut, cellMsgDialog, cellOskDialog, cellUserInfo, sceNpTrophy/Tus/Sns/Commerce2/Matching, cellRec/Screenshot/Voice, sysPrxForUser, and many more.
- **FMV / media stack** — re-vendored the demuxer (`cellDmuxPamf`, SPU-accurate) for FMV freeze fixes; re-vendored cellVdec/cellAdec/cellAtracXdec; added missing vdec decoder-library modules (fixed `cellVdecQueryAttr` crash).
- **lv2 kernel fixes** — lazy shm allocation (savestate v2); release event port on free_address; `sys_cond` deferred-init raw-mutex pointer; `sys_fs` op_read fast-path + `check_addr` guard; `CELL_ENOTDIR` propagation across the fs syscalls; `sys_ppu_thread_detach` returns CELL_OK on a zombie-join; widen `_sys_ppu_thread_create` stack size to u64; SPU reservation-waiter scan + postponed-notification flush at `sys_mutex_unlock`; `sys_net` select fd-set guard + sendto addrlen + p2p socket fixes; dev_flash device-alias resolution; `open_raw` error-code fidelity.
- **Loader / crypto** — transcription-slip fixes (memcpy/memcmp, rel-exec); align range in `ppu_register_function_at`; clamp ELF/SPU segments to local-store bounds; ISO fail-open on short extent read + clamped directory reads.
- **Savestate fidelity** — serialize PPU scalar registers unconditionally (version-gate desync fix); re-`Init()` on reload so firmware VFS mounts survive; fixed savestate-stop crash in `has_flushable_data`; abort-break RSX flush waits on stop; ZSTD-compression finalize missed-wakeup fix.
- **Version** synced to 0.0.41.
</details>

<details>
<summary><b>Audio</b></summary>

- **cellAudio re-vendor** — `_mxr000` event-queue fixes; unblocked via `video_provider` const-correctness fixes.
- OOB-read guard in `cellMusicDecode::decode_read`.
- Audio uses the lower-latency resampler/clip path (deliberately pre-PR#17525) suited to a sync-bound mobile device.
</details>

<details>
<summary><b>Networking / RPCN</b></summary>

- **Full Emu/NP re-vendor to protocol 30** — swapped FlatBuffers (`fb_helpers`) for **Protobuf** (`pb_helpers` + generated `np2_structs.pb`), wiring protobuf + abseil into the 3rdparty Android arm64 cross-compile. Ported clans SDK symbols (full clans HLE deferred).
- **Android RPCN JNI bridge** — `_rpcsx_rpcn*` for config, host list, account create / resend-token / test-connection, enable/disable.
- **Client-side password derivation** (PBKDF2-HMAC-SHA3-256), since the fork has no Qt layer that previously did it; fixed retry / host-switch state caching.
- **Credential storage hardened** — `rpcn.yml` (derived password + token) is stored in **app-internal `filesDir`** (not MTP/USB/other-app readable) and **excluded from backup** (cloud + device-transfer), with a one-time migration off external storage. NPIDs/names of other players are redacted from netplay logs; the empty-password echo back to the UI keeps the stored hash out of logcat.
</details>

<details>
<summary><b>Power &amp; thermal (Android)</b></summary>

- **Battery-saver core override** (default ON) — clamps the SPU GETLLAR busy-wait so idle reservation polls take the OS sleep instead of pinning a big core (the #1 steady-state SPU drain); on-device A/B measured a meaningful absolute CPU drop with negligible fps cost. Prefers FIFO/FIFO_RELAXED present under battery-saver.
- **Experimental big-cluster affinity** (default OFF) — detects the big cluster via cpufreq and biases PPU/SPU/RSX threads; opt-in (over-subscription can cost fps).
- **Experimental low-power WFE waiting** (default OFF) — `ldaxr+wfe` park wired into the RSX semaphore-acquire and FIFO-empty idle spins, with a hot pre-spin to avoid frame-time jitter.
- **Thermal-aware frame cap** + ADPF performance-hint feed (default OFF, advisory; self-disables on devices whose power-HAL returns no session).
</details>

<details>
<summary><b>App-facing core features &amp; build system</b></summary>

- JNI for per-game configs (store only edited settings, inherit the rest) + community-config import; patch engine (list/toggle/version); game version + titles in patch JSON; power/thermal/affinity/WFE toggles; RPCN.
- In-game home-menu re-vendor (reworked overlay home/quick menu, SDF rendering wired into the VK backend) + a "Clanker Features" live-toggle tab; quick-menu toggle crash fixed (overlay button-press racing the flip thread's vertex upload — now locks the display manager).
- Calendar/commit-time **CalVer** version naming, regenerated per build.
- Build: `USE_LLVM_VERSION` overridable with `LLVM_PREBUILT_ROOT`; Protobuf + abseil wired into 3rdparty (`flatc` step dropped); per-arch variant builds.
</details>

## Tried but failed / reverted

<details>
<summary>Expand — the honest list of dead ends</summary>

- **Async shader interpreter ("smooth shaders")** — ported upstream's async interpreter; `preload()`'s block-drain parks the RSX thread (black screen / ANR). Reverted twice; **default OFF**. A non-blocking re-land is specced but unshipped.
- **Cast-shadow attempts before the working fix** — receiver-side DEPTH_FLOAT/format_class coercion; col0 lane-completion; D32_SFLOAT compat-list widening; fp32 z-clip remap. All on-device tested and reverted; the eventual fix was suppressing the depth-only alpha-test discard.
- **Dirty-flag re-specialization** — byte-matching upstream 0.0.41 here unmasks a fork-specific alpha-test discard that makes geometry vanish; kept on the fork's value. Lesson: "matches upstream" is not the same as "safe on this fork."
- **SPU native-object disk cache** — versioned/keyed disk cache; wrote zero objects on-device across three investigations; removed (also an unbounded-storage concern).
- **Eager pre-flush of all WCB targets (jitter)** — would remove the predictor's filter and risk fault churn on transient render targets; not certain to help, so not applied.
</details>

## Pending / TODO for full upstream parity

<details>
<summary>Expand</summary>

- **`format_ex` hardware texel-remapping (Half B)** — the GPU-side `VkFormat`-view path (SNORM/sRGB via `image_view::as()`) needs VK infrastructure the fork lacks (mutable-format sampler images, a restructured sampler loop); deferred to a deliberate VK-backend pass. The software path (Half A) is landed and is the correct Adreno default.
- **Async shader interpreter (non-blocking re-land)** — a boot-only, non-blocking preload adapter is specced; would let smooth-shaders default ON.
- **Multithreaded RSX** — the only real offload for the WCB readback frame-time jitter; off by default, needs on-device validation on this fork.
- **Clans HLE full re-vendor** — `clans_client`/`clans_config` compile but the large `sceNpClans.cpp` re-vendor is deferred.
- **RPCN remaining** — connection-status UI; make the offline toggle authoritative over per-game overrides + tear down live sessions; optional server-cert verification.
- **GoW3 / SPURS-stall titles** — not fixable from kernel code (upstream 0.0.41 did not rewrite `sys_event`).
- **SPU SVE/SVE2 codegen** — detection stubs exist, codegen does not; low value on current SoCs.
</details>

---

*Continuously rebased against current RPCS3/RPCSX upstream; the emulation core is at
0.0.41 content with ARM64/Android adaptation. Experimental — verify before relying on it.*
