RPCSX-android core build. Update via the in-app core updater, or download the
`.so` for your device and install it manually. Pairs with a new app (APK) build.

## Highlights (fixes over the previous build)

- **RPCN sign-in works now.** The previous build sent the raw password; RPCN
  requires the client to derive it (PBKDF2-SHA3) first, which the desktop did in
  its Qt dialog that Android does not ship. That derivation is now done in the
  app. IMPORTANT: re-enter your RPCN password once in this build so it gets
  stored in the correct (derived) form.
- **Server switching + retries fixed.** Switching servers now actually
  reconnects to the new host, and a previous error (e.g. "resolve error") no
  longer sticks across retries. The bundled per-game server list was emptied of
  non-working placeholder entries; the default `np.rpcs3.net` covers most titles
  (Demon's Souls included), and custom servers can be added manually.
- **Smoothness regression fixed.** The last build was quieter but introduced
  audio stutter and frametime jitter. The SPU audio path no longer parks too
  eagerly (stutter gone), and the RSX low-power parks now spin briefly first so
  frame-boundary work is caught without wake-latency jitter. The battery/heat
  savings are kept (cores still park on sustained idle).
- **Bigger in-game menu.** The quick/home menu sidebar and sub-pages are
  enlarged and more readable.

## Privacy

- Credentials (password, token, email) are never written to logs.
- The login/account and friend/message log lines no longer print NPIDs or online
  names, so shared logs do not leak your or other players' online identities.

## Variants

- `librpcsx-android-arm64-v8a-armv8.4-a.so` — built this round; primary,
  auto-selected on capable devices (e.g. Retroid Pocket 6 / Snapdragon 8 Gen 2).
- The armv8.2-a variant (older arm64 devices) was not rebuilt this round.

## Note for testers

1. RPCN: re-enter your password, then "Test connection" should report connected;
   try signing in. Report any error text verbatim.
2. Smoothness: confirm the audio stutter and frametime jitter from the last build
   are gone, while the device still runs cool/quiet with battery saver on.
3. Menu: confirm the in-game menu is comfortably readable now.
