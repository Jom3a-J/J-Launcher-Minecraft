# J Launcher 0.1.1 — Stable Release Notes

**Publication status:** the canonical
[Releases page](https://github.com/Jom3a-J/J-Launcher-Minecraft/releases) is
authoritative. A source branch, tag, workflow artifact, or draft release alone
is not an approved stable package.

J Launcher is an independent, GPL-3.0 Minecraft launcher derived from Prism
Launcher and MultiMC. This candidate is for Windows x64 only.

## What's new in 0.1.1-beta.5

Changes since 0.1.1-beta.4.

### Prism Launcher update

- J Launcher now includes Prism Launcher's changes up to 5 October 2026,
  with their fixes and improvements.
- The installer has an optional **Open Modrinth website links** component. It
  is off by default, so it does not take over links from the Modrinth App, and
  uninstalling removes it only while it still points to this installation.

### Servers

- Closing J Launcher while servers are running asks first and stops them
  cleanly. Restart timers, the crash-restart limit, and restore rollback were
  fixed.
- Purpur, Forge, and NeoForge server downloads are checked against their
  published checksums, and Fabric server jars are validated.
- Preparing a server from a modpack and importing a server pack no longer
  freeze the window. Server packs wrapped in a single folder import correctly.
- Removing a mod or plugin moves it to the Recycle Bin. Mods and plugins cannot
  be added or removed while a server pack is being imported.
- **View Latest Crash Report** works again.
- The FTB server installer runs only when it is signed by Feed The Beast Ltd,
  and it cannot be changed between that check and running it.

### Instances and downloads

- A freshly installed modpack could lose some of its components, such as LWJGL
  or Fabric's intermediary mappings. It then showed "unresolved dependencies"
  and could not launch offline until it had been launched once online. Fixed.
- A game launch and a server that need the same Java version no longer
  download it into the same folder at the same time.
- Legacy FTB and ATLauncher installs can be cancelled.
- A failed download no longer leaves a hidden temporary file in the target
  folder.
- Launcher logs are no longer cut off at 256 KB.

### Security

- Your CurseForge API key and Modrinth token are only ever sent to the site
  they belong to, even when a download is redirected elsewhere.
- Windows helper programs (`cmd.exe`, `taskkill.exe`) are started from the
  Windows system folder by full path.

## Highlights

- Manage local Vanilla, Fabric, Forge, NeoForge, Paper, and Purpur servers from
  a dedicated workspace.
- Use live console and player administration, health/crash diagnostics,
  compatible mod/plugin installation, backups and guarded restore/update flows.
- Keep the main launcher usable while Settings and other secondary windows are
  open.
- See persistent server status from the main launcher and use the redesigned
  Server Manager for both empty and populated profiles.
- Sign in through J Launcher's own Microsoft application and J Launcher-owned
  completion page; the page receives no authentication code, token, or account
  identifier.
- Protect account tokens at rest on Windows with an encryption key stored in
  Windows Credential Manager.

## Reliability and privacy

- Hardened authentication cancellation, callback handling, token-safe logging,
  API headers, filesystem operations, server downloads, Java/runtime handling,
  and release packaging.
- Translation downloads are explicit by default; automatic checks require user
  opt-in.
- The launcher has no advertising, analytics, telemetry, or automatic crash
  reporting.
- Automatic launcher updates remain disabled until their signing, integrity,
  rollback, and recovery design is separately approved.

## Performance

Optimized Windows Release builds passed the documented Phase 11 budgets for
empty and representative populated profiles, including startup, Settings,
Servers, server tabs, idle CPU, and memory. The final numbers must be rerun and
recorded from the exact tagged package before these notes are published.

## Distribution

The stable release will provide a portable ZIP, standard-user installer,
exact-source archive, and matching SHA-256 files. The first stable release will
be unsigned because trusted signing is not currently affordable. Windows
SmartScreen may warn, while Smart App Control, Windows in S mode, or managed
application-control policy may block it. Do not disable Windows security to run
J Launcher. Signing may be added to a later release if support makes it
practical.

Publication requires exact-tag provenance, checksum verification, post-reboot
cold-start evidence, genuinely fresh-machine Windows x64 qualification, and a
separate maintainer decision.

See [KNOWN_ISSUES.md](KNOWN_ISSUES.md), [PRIVACY.md](PRIVACY.md),
[SUPPORT.md](SUPPORT.md), [RELEASE_RECOVERY.md](RELEASE_RECOVERY.md), and
[RELEASE_CHECKLIST.md](RELEASE_CHECKLIST.md).
