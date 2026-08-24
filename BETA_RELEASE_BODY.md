# J Launcher {{VERSION}} Beta

J Launcher {{VERSION}} is a **beta prerelease** for controlled testing on
**Windows x64 only**. It is not a stable release and may contain defects that
affect launcher profiles, downloads, modpacks, or local servers. Back up
important data before testing and report failures with secrets removed.

## Important: unsigned Windows application

The portable package, installer, and J Launcher executables are intentionally
**unsigned** because trusted code signing is not currently affordable.
Windows SmartScreen may warn, while Smart App Control, Windows in S mode, or an
organization-managed application-control policy may block the application.
Do not disable or weaken Windows security to run J Launcher.

## Downloads

- `JLauncher-Windows-x64-Portable-{{VERSION}}.zip` keeps beta launcher data
  under its extracted portable directory.
- `JLauncher-Windows-x64-Setup-{{VERSION}}.exe` installs J Launcher for the
  current Windows user. Windows may request administrator approval to install
  the Microsoft Visual C++ runtime if that prerequisite is missing.
- `JLauncher-{{VERSION}}-Source.tar.gz` contains the tracked source and
  populated submodules from the exact beta tag.
- The matching `.sha256` files and `SHA256SUMS.txt` allow every download to be
  checked independently before it is run.

Automatic launcher updates are disabled. Install later beta or stable builds
manually from this repository's Releases page. Do not assume beta data can be
downgraded safely without restoring a backup.

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
- Release classification: GitHub draft prerelease
- Expected Authenticode status: `NotSigned`
- Bundled Microsoft Visual C++ runtime: valid Authenticode signature required

The workflow checks out the annotated beta tag and its recorded submodules,
builds with MSVC in Release mode with LTO and beta channel metadata, runs the
deterministic native test suite, audits the portable and installer payloads,
and creates the release as a draft prerelease only. Publication requires a
separate manual review, clean-machine testing, and maintainer decision.

## Beta focus

- Exercise Vanilla, Fabric, Forge, NeoForge, Paper, and Purpur local servers.
- Verify server diagnostics, backups, guarded updates, and content management.
- Verify Modrinth and user-keyed CurseForge discovery and transactional updates.
- Confirm first launch, Microsoft sign-in, Minecraft launch/restart, and profile
  persistence on a standard-user Windows x64 installation.
- Confirm installer bootstrap, shortcuts, reboot launch, and uninstall behavior.

Read the exact-tag [release notes](https://github.com/Jom3a-J/J-Launcher-Minecraft/blob/{{TAG}}/RELEASE_NOTES.md),
[known issues](https://github.com/Jom3a-J/J-Launcher-Minecraft/blob/{{TAG}}/KNOWN_ISSUES.md),
[privacy policy](https://github.com/Jom3a-J/J-Launcher-Minecraft/blob/{{TAG}}/PRIVACY.md),
[support guide](https://github.com/Jom3a-J/J-Launcher-Minecraft/blob/{{TAG}}/SUPPORT.md),
and [release recovery policy](https://github.com/Jom3a-J/J-Launcher-Minecraft/blob/{{TAG}}/RELEASE_RECOVERY.md)
before installing.

J Launcher is an independent GPL-3.0-only fork of Prism Launcher, which is
derived from MultiMC. It is not affiliated with or endorsed by Prism Launcher,
MultiMC, Mojang, or Microsoft.
