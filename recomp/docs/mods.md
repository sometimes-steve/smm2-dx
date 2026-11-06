# SMM2 recomp mods

Mods change the recompiled game at run time. Their code is ours (`games/smm2/mods/`, `runtime/net/`); they ship no
game code or data. Anything they need from a game is made on the player's machine from the player's own dumps, into
`private/`, which git ignores.

## Online: OpenCourse World only

The game's online features all go through [OpenCourse World](#opencourse-world), the only backend. The
course-server mods (Course World answered from our own server or TGRCode, the 100 Mario Challenge, SMM2's Endless
Challenge against our server, Super Worlds, the built-in course server), the local NEX test server and the Nextendo
Network backend were removed, and so were all the built-in mods (SMM1 extras, the SMW goal wipe, the Muncher hit box,
quick level return, costumes, Lanky Mario, Try it now, relaxed checks, Edit any course): the Mods tab only lists the mods
you install, and the ones on GameBanana. The setup screen asks for the `ocw-config.json` exported from opencourse.world.

## Course Maker PC controls (`mods/pc_editor.c`)

A PC's mouse and keyboard in the Course Maker (GitHub #26), on top of the mouse as a finger: the **right mouse
button** erases (the Course Maker's own "[L] / [R] Erase": L held, then a finger at the pointer; drag to erase along
the way), the **middle button** drags the course and the **wheel** scrolls it (the right stick; Shift+wheel: up /
down), **Ctrl+Z** (Cmd+Z on macOS) is the Undodog (B), and **1 ... 9, 0, -, =** pick the palette's twelve slots (a tap
on the slot). Only while the Course Maker is up (the play info's kind word, 0 = edit) and only from a PC's keyboard and
mouse (`runtime/hle/input.c` `game_pc_key` / `game_pc_mouse` / `game_pc_wheel`). There, 1 and 3 pick slots instead of
ZL / ZR (multigrab). Copy and paste aren't mapped: SMM2 copies through a held part's own menu.

## OpenCourse World

`games/smm2/mods/ocw.c`. **OpenCourse World** (<https://opencourse.world/>) is a replacement online service for
SMM2 v3.0.2 / 3.0.3. On a console you install its mod (Atmosphère / Ryujinx) and the game's own online code talks
to OCW instead of Nintendo. The recomp does the same thing, with the player's own OCW account.

The console mod is `_vendor/ocw-client-mod` (exlaunch, a `subsdk9`). It cannot be loaded here — the recomp loads
only `main` and `sdk` (`runtime/loader.c`) and runs recompiled C, so there are no instructions to patch at run
time — so the two patches that matter were ported. Its other eight stub `nn::account` calls that reach real
Nintendo services on a console; here those reach `runtime/services/acc.c`, which already reports a network service
account and hands out a local ID token whenever the game's own sockets are on.

| What | Where |
|---|---|
| the domain, written over the game's own host string at `main+0x22cc307` | `ocw.c` `game_modules_loaded()`, once the modules are loaded (the whole mapping is read/write) |
| the WebSocket handshake, built by the host so the token rides in a header | `ocw.c` `smm2_ocw_header()`, from the one patch at `main+0xAF560` (`mods/patches.py`) |

### Turning it on

Sign in at opencourse.world, accept its terms, and take the **domain** and your **personal token** from the package
it gives you (on a console those go on the SD card as `ocw-config.json`).

**In the game:** the overlay menu (F1) → **Online**. Pick **OpenCourse World** under "Online backend" in the Online
section, and fill in the **OpenCourse World** rows just below it: `Domain`, `Address` and `Token`. Each opens a
field when activated — typed directly with a keyboard, or through the on-screen keyboard with a controller. The
rows are always shown, even with every mod off, because they have to be set *before* anything can work. Changing
any of them writes `ocw-config.json`. It all takes effect at the next start: the domain is written into the game's
code while the modules load.

`Token` says only "set" or "not set" until you activate it; the field appears then, and is hidden again when the
menu closes. The token never reaches a log either — `net_log_secret` registers it, and `net_log_redact` takes it out
of a logged line along with the player id.

**Or by hand**, which is the same thing — the file is where the settings live either way:

1. (as above: domain and token from opencourse.world)
2. `ocw-config.json` next to `smm2mods.ini` in the data folder (macOS source build:
   `recomp/private/smm2/ocw-config.json`):

   ```json
   { "domain": "server.opencourse.world", "token": "<22 characters>" }
   ```

   | key | meaning |
   |---|---|
   | `domain` | the host written into the game, so the game looks it up, sends it as `Host:` and names it for SNI. At most **29 characters**: the size of the string slot in the game's own binary. May itself be an address |
   | `token` | your account's token, exactly **22 characters**. A secret: keep it out of git |
   | `ip` | optional. The address `domain` resolves to, instead of asking the host resolver — this is how a self-hosted or local OCW instance is reached, and how an IP-only set-up keeps a real name for SNI and `Host:` |

3. The setup screen's OpenCourse World row takes the file you export from opencourse.world and reads it where it is
   (the launcher hands its path to the game as `RECOMP_OCW_CONFIG`); without that, `ocw-config.json` in the data
   folder is read. With neither, the log says what is missing and the game stays offline.

With `net_log=1` and `log=1` the log shows the domain being written, the name resolving, the socket being allowed
and the TLS handshake. Add `net_log_data=1` to see the plaintext of the WebSocket upgrade itself; the token is
overwritten there, so the log stays safe to attach to a bug report.

### What it is allowed to reach

The runtime's network policy is deny-by-default (`runtime/net/netcfg.h`, `../../DECISIONS.md`) and the `ocw`
backend is the one narrow opening in it:

- **exactly one name resolves** — the one `domain` names, from `ip` if given, else the host resolver. Every other
  name still fails; the game's names never reach the host resolver.
- **only its addresses are reachable** — `net_dest_allowed()` allows loopback and the addresses that name has
  answered with lately, nothing else. The name is re-resolved every five minutes (OCW's address is in AWS and
  moves), and **the newest answer is what the game is told**; the previous few stay reachable so a session already
  connected to one keeps working. They are kept in a ring of 8, so a new address always gets in. A destination that
  is refused marks the name for a fresh lookup instead of waiting out the five minutes, so a dropped connection can
  be remade at the new address.
- **its certificate is checked.** OCW serves an ordinary publicly-signed certificate, so `tls_trust` defaults to
  `roots` under this backend: the chain must end in a public root the runtime carries (`runtime/net/roots.c`, which
  already holds Let's Encrypt's **ISRG Root X1 and X2** for the https client) or one the system bundle has, *and*
  match the host name. The roots are parsed **once** for the process and shared by every connection (a parsed chain
  is only read during a handshake), so a reconnect costs nothing — the system bundle is several hundred
  certificates. Verification needs a name, so a connection made by address alone with no name to check
  against is refused rather than quietly weakened.

  **A server of your own is the exception.** OpenCourse World is to be open source and self-hostable, and a LAN
  instance — or OCW itself while being developed against locally — has no certificate a public root can vouch for.
  `tls_trust=any` turns the check off for the one host `ocw-config.json` names, so that stays possible. It is never
  a default and has to be written down: with it, anyone on the path can read or alter the session, including the
  token in the handshake. If your own server has a private CA, prefer `tls_trust=local-ca` with `tls_ca=` — that
  verifies properly instead of not verifying at all.

### What the mod answers under `ocw`

Nothing: `hy_mode` (`games/smm2/mods/hybrid.h`) gives the `ocw` backend `{world 0, login 0, hybrid 0}`, so Course
World, the Endless Challenge and 100 Mario are all OCW's own, as `course_world=backend` does for Nextendo. Because
it is not hybrid, the mid-session fall-back to our server on a failed login (`net_backend_fall_back`, which Nextendo
uses) does not apply here.

### The account

OCW identifies the player by the **token in the handshake**, not by the console's account. The console mod makes
that plain: it stubs `GetNetworkServiceAccountId` to a flat `0xCAFE`, the same for every OCW player, and stubs
`LoadNetworkServiceAccountIdTokenCache` so no ID token is sent at all.

The recomp matches that exactly (`net_account_shared()`, `services/acc.c`): account ID `0xCAFE`
and an empty ID token. It does *not* use the local account (`runtime/net/token.h`), whose ID is random per install
and whose JWT is signed with a local key OCW could not check — a changing identity and an unverifiable token are
both good ways for a server not to find the player's profile again.

### Limits

TLS is a **1.2 client only** (`runtime/net/mbedtls_user_config.h`); `ssl.c` pins both the minimum and the maximum,
so a server that required 1.3 would fail the handshake whatever `tls_trust` says. OCW accepts 1.2 (checked
2026-10-22, Let's Encrypt ISRG Root X2) — re-check here if its TLS configuration is ever tightened.

How far the game's own Course World code gets against a real service is **not yet known**: `local-nex` has only
ever been run against `tools/nex_test_server`.
