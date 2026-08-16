# J Launcher 0.1.1 — Stable Release Notes

**Publication status:** the canonical
[Releases page](https://github.com/Jom3a-J/J-Launcher-Minecraft/releases) is
authoritative. A source branch, tag, workflow artifact, or draft release alone
is not an approved stable package.

J Launcher is an independent, GPL-3.0 Minecraft launcher derived from Prism
Launcher and MultiMC. This candidate is for Windows x64 only.

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
