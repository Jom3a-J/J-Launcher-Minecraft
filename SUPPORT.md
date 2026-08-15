# Getting Help with J Launcher

Official stable packages are listed only on the canonical
[J Launcher Releases page](https://github.com/Jom3a-J/J-Launcher-Minecraft/releases).
A source branch, tag, workflow artifact, or draft release is not stable by
itself. Include the package version, Git commit, and SHA-256 hash in reports.

## Where to report a problem

Use the public [J Launcher issue tracker](https://github.com/Jom3a-J/J-Launcher-Minecraft/issues)
for ordinary bugs and support questions. J Launcher currently has no official
Discord, forum, support email, or social account.

Potential security, privacy, credential, signing, or release-integrity defects
must be reported privately according to [SECURITY.md](SECURITY.md).

## What to include

1. What you did, what you expected, and what happened instead.
2. The J Launcher version, release status, platform, and Git commit shown under
   **Help → About**.
3. Your Windows edition and build.
4. Whether the portable package or installer was used and its SHA-256 hash.
5. The relevant log after removing credentials, personal paths, user names,
   and private server addresses.

## Logs

The launcher log covers startup, sign-in, downloads, settings, and Server
Manager. Game and server consoles cover Minecraft or server-process failures.

Read every log before sharing it. The **Upload** action publishes the selected
log to a third-party paste service; J Launcher never uploads a log
automatically. Copying a redacted excerpt directly into an issue is also fine.

## Upstream and third-party problems

J Launcher is based on [Prism Launcher](https://prismlauncher.org/), which is
based on [MultiMC](https://multimc.org/). If a defect is reproducible in Prism
Launcher without J Launcher's server features or identity changes, reporting it
upstream reaches more maintainers.

Report mod, modpack, server-software, or provider outages to the project that
owns them unless J Launcher caused or mishandled the failure.

## Supported platform

Phase 9 qualifies Windows x64 only. Linux and macOS artifacts, builds, tests,
and support are outside the current release scope.

Automatic launcher updates, news, analytics, telemetry, advertising, automatic
crash reports, and screenshot uploads are disabled. See
[KNOWN_ISSUES.md](KNOWN_ISSUES.md) and [PRIVACY.md](PRIVACY.md).
