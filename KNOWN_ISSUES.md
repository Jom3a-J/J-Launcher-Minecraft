# Known Issues

**Applies to:** J Launcher 0.1.1

**Last reviewed:** 14 August 2026

Only packages explicitly published on the canonical
[Releases page](https://github.com/Jom3a-J/J-Launcher-Minecraft/releases) are
stable. A tag, draft release, or workflow artifact is not a stable package by
itself.

## Unsigned package and Windows security

The first stable J Launcher packages will intentionally be unsigned because
trusted signing is not currently affordable. Windows SmartScreen may warn, and
Smart App Control, Windows in S mode, or an organization-managed
application-control policy may block them completely.

Do not disable an operating-system security feature merely to run J Launcher.
This limitation is accepted for the first stable release and must remain clear
on its release page. Signing may be added later if financial or community
support makes it practical.

## Qualification notes

The public workflow provenance, exact artifact checksums, and recorded
qualification evidence linked from the release page apply to the published
files. Local builds and historical preflight results are not substitutes for
that evidence.

## Platform support

Only Windows x64 is in J Launcher's build, package, test, and release scope.
The inherited Linux and macOS source is not qualified or supported by this
release plan. Windows account tokens are protected with a key stored in Windows
Credential Manager; no J Launcher release is being made for other platforms.

## Updates and online services

- Automatic launcher updates remain disabled pending separate approval of
  signing, integrity, rollback, recovery, and user experience.
- CurseForge appears only in builds configured with a reviewed J Launcher-owned
  key. Development and source builds without one keep the provider unavailable.
- Live provider and Minecraft downloads depend on third-party service
  availability and remain release smoke tests in addition to deterministic
  automated coverage.

Use [SUPPORT.md](SUPPORT.md) for ordinary reports and [SECURITY.md](SECURITY.md)
for private vulnerability reporting. Do not report J Launcher-specific defects
to Prism Launcher.
