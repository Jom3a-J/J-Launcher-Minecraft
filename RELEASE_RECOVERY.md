# Release Withdrawal and Recovery

This policy applies to J Launcher's Windows x64 stable releases. It covers both
ordinary defects and incidents involving security, credentials, destructive
filesystem behavior, source provenance, or artifact integrity.

## Before publication

If a candidate fails any release gate, leave its GitHub release as a draft and
do not distribute its assets as stable. Preserve the failed workflow run,
checksums, logs with secrets removed, and the reason for rejection.

An annotated candidate tag is immutable release evidence. Do not move, delete,
or reuse it for different source. Correct the defect and use a new version tag.

## Withdrawing a published release

If a published release may harm users or cannot be trusted:

1. Stop recommending and distributing it immediately.
2. Preserve the original tag, commit, workflow provenance, artifact hashes,
   and incident evidence. Never silently replace an asset under the same name.
3. Mark the release as withdrawn at the top of its release notes and remove
   affected binaries from normal download paths when continued availability
   creates unacceptable risk.
4. Publish a security advisory when confidentiality or coordinated disclosure
   permits; otherwise publish a clear non-sensitive notice.
5. Identify the last independently verified stable release and link only to
   its immutable tag, artifacts, and checksums.
6. Prepare any corrected build as a new version through the complete release
   checklist. Do not reuse the withdrawn version or its filenames.

## First stable release

J Launcher 0.1.1 has no earlier stable release in the canonical
`Jom3a-J/J-Launcher-Minecraft` repository. If it must be withdrawn before a
newer verified release exists, state plainly that **no supported stable J
Launcher download is currently available**. Do not present an old beta, local
build, artifact from the legacy repository, or upstream Prism Launcher package
as a J Launcher rollback.

Users may uninstall J Launcher or stop using the portable package while keeping
backups of their instances, worlds, servers, and launcher data. A launcher
downgrade must never be recommended for an existing profile unless data-format
compatibility has been explicitly tested on a copy of that profile.

## Recovery validation

A replacement release must complete the same exact-tag build, tests, package
audit, checksum verification, post-reboot cold start, fresh-machine product
smoke, unsigned-status disclosure, and maintainer approval as any other stable
release. Incident urgency does not justify bypassing those gates.

Report ordinary defects through [SUPPORT.md](SUPPORT.md). Report security,
privacy, credential, signing, or release-integrity issues privately according
to [SECURITY.md](SECURITY.md).
