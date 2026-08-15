# J Launcher {{VERSION}}

J Launcher {{VERSION}} is the first stable release prepared from the canonical
public repository. This release supports **Windows x64 only**.

## Important: unsigned Windows application

The portable package, installer, and J Launcher executables are intentionally
**unsigned** because trusted code signing is not currently affordable.
Windows SmartScreen may warn, while Smart App Control, Windows in S mode, or an
organization-managed application-control policy may block the application.
Do not disable or weaken Windows security to run J Launcher.

## Downloads

- `JLauncher-Windows-x64-Portable-{{VERSION}}.zip` keeps launcher data under
  its extracted portable directory.
- `JLauncher-Windows-x64-Setup-{{VERSION}}.exe` installs for the current
  Windows user without requiring administrator access.
- `JLauncher-{{VERSION}}-Source.tar.gz` contains the tracked source and
  populated submodules from the exact release tag.
- The matching `.sha256` files and `SHA256SUMS.txt` allow every download to be
  checked independently before it is run.

Automatic launcher updates are disabled. New versions must be downloaded and
verified manually from this repository's Releases page.

## Verify a download

In PowerShell, calculate the downloaded file's SHA-256 value:

```powershell
Get-FileHash -Algorithm SHA256 .\JLauncher-Windows-x64-Portable-{{VERSION}}.zip
```

Compare the complete value with the corresponding `.sha256` file or the entry
in `SHA256SUMS.txt`. Do not run the file if the values differ.

## Source and build provenance

- Source tag: [`{{TAG}}`](https://github.com/Jom3a-J/J-Launcher-Minecraft/tree/{{TAG}})
- Source commit: [`{{COMMIT}}`](https://github.com/Jom3a-J/J-Launcher-Minecraft/commit/{{COMMIT}})
- Public workflow run: [Windows x64 build, tests, packaging, and audit]({{PROVENANCE_URL}})
- Expected Authenticode status: `NotSigned`

The workflow checks out the annotated tag and its recorded submodules, builds
with MSVC in Release mode with LTO, runs the deterministic native test suite,
audits the portable and installer payloads, and creates this release as a draft
only. Publication requires a separate manual review and maintainer decision.

## Highlights

- Manage Vanilla, Fabric, Forge, NeoForge, Paper, and Purpur local servers from
  a dedicated Server Manager workspace.
- Use server console and player controls, backups, guarded restore/update and
  deletion flows, compatible content installation, and health diagnostics.
- Keep the main launcher usable while Settings and secondary windows are open.
- Sign in through J Launcher's Microsoft application and J Launcher-owned
  presentation-only completion page.
- Protect Microsoft/Minecraft tokens at rest on Windows using authenticated
  encryption and a key stored in Windows Credential Manager.

Read the exact-tag [release notes](https://github.com/Jom3a-J/J-Launcher-Minecraft/blob/{{TAG}}/RELEASE_NOTES.md),
[known issues](https://github.com/Jom3a-J/J-Launcher-Minecraft/blob/{{TAG}}/KNOWN_ISSUES.md),
[privacy policy](https://github.com/Jom3a-J/J-Launcher-Minecraft/blob/{{TAG}}/PRIVACY.md),
[support guide](https://github.com/Jom3a-J/J-Launcher-Minecraft/blob/{{TAG}}/SUPPORT.md),
and [release recovery policy](https://github.com/Jom3a-J/J-Launcher-Minecraft/blob/{{TAG}}/RELEASE_RECOVERY.md)
before installing.

J Launcher is an independent GPL-3.0-only fork of Prism Launcher, which is
derived from MultiMC. It is not affiliated with or endorsed by Prism Launcher,
MultiMC, Mojang, or Microsoft.
