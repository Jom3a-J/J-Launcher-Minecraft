# Code Signing Policy

**Status:** deferred. The first stable Windows x64 release may be unsigned.

Trusted Windows code signing is not currently affordable for this project. On
14 August 2026, the maintainer decided that this cost will not block the first
stable release. Signing may be introduced in a later release if financial or
community support makes it practical.

## Current unsigned-release policy

Every unsigned release must:

- Say **unsigned** plainly on the release page, in the release notes, README,
  and known issues.
- Never claim a verified publisher or show signing attribution that does not
  apply to the published files.
- Explain that Windows SmartScreen may warn and that Smart App Control,
  Windows in S mode, or managed application-control policy may block execution.
- Never instruct users to disable or weaken an operating-system security
  feature.
- Publish the exact source archive and matching SHA-256 files alongside the
  Windows x64 portable ZIP and installer.
- Preserve hosted build provenance, clean-machine qualification, manual release
  review, and rollback requirements. Being unsigned does not relax any other
  release gate.

## Future signing scope

If trusted signing becomes available, the J Launcher signing identity may be
used only for Windows x64 binaries built from the canonical
`Jom3a-J/J-Launcher-Minecraft` repository:

- `jlauncher.exe`
- `jlauncher_filelink.exe`
- J Launcher helper executables produced by the tagged source
- the J Launcher NSIS installer containing those files

It must not sign another project, a maintainer's unrelated software, a locally
modified binary, or an unofficial build. Bundled third-party runtime DLLs are
not represented as J Launcher-owned binaries.

## Future signing provenance and approval

Any artifact submitted for signing must:

1. Be built by a public, manual GitHub Actions release workflow from an existing
   annotated version tag in the canonical repository.
2. Check out all submodules at the revisions recorded by that tag.
3. Pass the Windows x64 Release build, deterministic tests, package audit, and
   integrity gates.
4. Carry the same product name, version, release channel, and Git commit as the
   tagged source.
5. Remain in a draft release while its contents, checksums, provenance, and
   test evidence are reviewed.
6. Receive a separate manual signing approval.
7. Be downloaded and independently verified for signature and checksum before
   the maintainer makes a separate publication decision.

Locally built or manually uploaded binaries are never eligible for a trusted
release-signing request. A tag push alone must not start signing or publication.

## Product identity and account security

J Launcher is a visible GPL-3.0 fork of
[Prism Launcher](https://github.com/PrismLauncher/PrismLauncher), which is based
on MultiMC. Required upstream history, copyright, licence, and attribution must
remain present. Any future signed executable must identify the product as
`J Launcher` and may not imply endorsement by Prism Launcher, MultiMC, Mojang,
or Microsoft.

Every account with repository, workflow, signing, reviewer, or approver access
must use multi-factor authentication and the least privilege needed for its
role. Future signing secrets must be stored only in the signing service or a
protected repository environment, never in source, artifacts, workflow logs,
caches, or local handoff files.

## Incident response

If provenance, a future signing account, a workflow, or a published artifact
may be compromised, pause publication, withdraw affected downloads, preserve
evidence, publish a clear advisory, and direct users to the previous known-good
release. Do not silently replace an asset under an existing name.

Report potential signing or release-integrity vulnerabilities privately under
[SECURITY.md](SECURITY.md).
