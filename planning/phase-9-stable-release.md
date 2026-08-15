# Phase 9 — Stable Release and Maintenance

**Status:** In progress — 14 August 2026

**Scope:** Windows x64 only

**Candidate version:** 0.1.1 (not yet tagged or approved for publication)
**Candidate tag:** `jlauncher-0.1.1`

## Outcome

Produce J Launcher's first stable Windows x64 release from the canonical public
repository. The release must be traceable to an exact public source tag,
integrity-verifiable, honest about its unsigned status, tested as a standard
user on a genuinely fresh Windows machine, and recoverable if publication goes
wrong.

Phase 9 is release qualification, not another feature phase. Code changes are
limited to defects found by the gates below.

## Current position

- The canonical repository is `Jom3a-J/J-Launcher-Minecraft`; its default branch
  is `develop` and it currently has no tags, releases, or workflow runs.
- The active feature branch is merged into `official/develop`, but the local
  worktree still contains intentional, uncommitted Phase 11 and Server Manager
  changes. No candidate commit can be selected while that state remains dirty.
- Phase 11's optimized Release package passed local empty and populated
  performance, dependency-isolation, visual, and data-safety checks.
- A Phase 9 stable-channel MSVC Release build with LTO completed locally on
  14 August. All 34 deterministic Release tests passed in 17.64 seconds. The
  local preflight then produced and audited an x64 portable ZIP and NSIS
  installer with matching independently recomputed SHA-256 files.
- Dependency-isolated one-sample package smokes passed every budget: empty
  profile main/Settings/Servers were 729/118.9/56.2 ms; populated were
  900.2/126.9/103.1 ms, with 0% sampled idle CPU in both runs. These are
  observational first runs, not the certified post-reboot sample.
- The local artifacts are unsigned and came from the intentional dirty
  worktree. Their preflight report correctly marks `publicationReady` false.
  That generated report predates the maintainer's unsigned-release decision and
  still lists trusted signing as open; the updated script no longer does so.
- Those Phase 11 results are supporting evidence only. Candidate qualification
  must be rerun against the exact clean commit and artifact that would ship.
- A manual-only Windows x64 workflow is prepared locally. It validates an
  existing annotated `jlauncher-X.Y.Z` tag on canonical `develop`,
  builds/tests/packages it, produces an exact tracked-source archive with
  populated submodules, preserved Git link/executable metadata, and checksums,
  and can create only a GitHub draft release. It has not been committed or run
  publicly yet.
- The inherited history already contains an annotated `0.1.1` tag for MultiMC
  from January 2014. J Launcher must not move or reuse that historical tag, so
  the collision-safe candidate tag is `jlauncher-0.1.1`; the application
  version and public artifact filenames remain `0.1.1`.
- A public release-page template and a first-release withdrawal/recovery policy
  are prepared for exact-tag review. Because 0.1.1 has no prior canonical
  stable release, withdrawal must say no supported stable download is available
  rather than directing users to an old beta or legacy artifact.
- On 15 August 2026, the canonical repository enabled GitHub Issues and private
  vulnerability reporting and replaced the inherited Prism Launcher homepage
  with `https://jom3a-j.github.io/J-Launcher-web/`. API verification reports
  both features enabled; the homepage and public new-issue route return HTTP
  200, and the signed-out private-report route reaches GitHub's authentication
  handoff.
- A certified post-reboot cold-start sample and execution on a genuinely fresh
  Windows machine have not yet been completed.
- On 14 August 2026, the maintainer decided that the first stable release may be
  unsigned because trusted signing is not currently affordable. This is an
  accepted release limitation, not an open gate. Signing may be added to a
  later release if financial or community support makes it practical.

## Release gates

| Gate | Current state | Evidence required to close |
| --- | --- | --- |
| Candidate source | Blocked | Clean commit on the canonical repository, reviewed version/channel change, annotated tag, and no untracked release inputs |
| Native regression | Local preflight passed; exact-candidate rerun needed | Windows x64 MSVC Release build and all 34 deterministic CTest targets passing |
| Performance | Local one-sample smokes passed; exact-candidate rerun needed | Empty and populated package reports within the Phase 11 budgets |
| Packaging | Local portable/installer preflight passed | Portable ZIP, standard-user installer, exact-source archive, licences/notices, and matching SHA-256 files from the tag |
| Hosted provenance | Workflow prepared locally; public run missing | Public manual workflow run that checks out the tag, builds/tests it, and creates only a draft release |
| Signature disclosure | Decided | Exact artifacts report `NotSigned`; release page, README, notes, and known issues clearly explain Windows warnings/blocking and never imply a publisher signature |
| Post-reboot cold start | Missing | First launch after an actual reboot, before any J Launcher warm-up, recorded with the test machine and package hash |
| Fresh-machine smoke | Missing | Fully updated, genuinely fresh Windows x64 machine using a standard-user account and the exact candidate artifacts |
| Product smoke | Missing | Sign-in/out, Java acquisition, Vanilla and modded client launch, Vanilla and modpack server lifecycle, backup/restore, safe deletion, and log review |
| Public material | Draft set prepared | Release notes, release-page template, known issues, privacy, integrations, support, security, signing policy, source/build instructions, and withdrawal/recovery procedure reviewed at the tag |
| Repository public configuration | Passed — 15 August 2026 | GitHub Issues and private vulnerability reporting enabled; J Launcher homepage and intake routes verified |
| Maintainer decision | Missing | Explicit review of the draft artifacts and evidence followed by explicit approval to publish |

## Qualification order

1. Review and preserve the current Phase 11 changes, then place the complete
   product state on `official/develop` through the maintainer's normal Git
   process.
2. Review the Phase 9 public documents and the Windows x64-only scope.
3. Reconfirm the canonical J Launcher homepage, GitHub Issues, and private
   vulnerability reporting remain enabled when the exact tag is reviewed.
4. Review and commit the prepared manual, draft-only hosted release workflow.
   It must never publish on a push or tag event and must never consume a local
   binary for release.
5. Select the candidate commit, set the stable channel, create the annotated
   version tag, and stop feature changes for that candidate.
6. Run the exact-tag Release build, 34-test suite, package audit, performance
   checks, source/archive comparison, and payload secret scan.
7. Complete the post-reboot cold-start sample with the exact portable package.
8. Complete portable and installer qualification on a genuinely fresh Windows
   x64 standard-user machine.
9. Confirm the candidate is unsigned and that all public release material
   accurately explains SmartScreen, Smart App Control, S mode, and managed
   application-control limitations without asking users to weaken security.
10. Review the draft release, record known issues and rollback instructions, and
   publish only after explicit maintainer approval.

## Fresh-machine smoke

The fresh machine must not contain the development Qt installation, vcpkg
tree, J Launcher profile, Credential Manager entries, or prior J Launcher
installation. Record the Windows edition/build, account type, artifact hashes,
the expected `NotSigned` result, start/end time, screenshots, and the result of
each step.

- Launch the portable package and confirm all profile data remains portable.
- Install without elevation, launch from Start/Desktop shortcuts, and uninstall
  without removing unrelated user data.
- Sign in through J Launcher's Microsoft application, then sign out and remove
  the account.
- Detect or download Java, launch a Vanilla instance, and launch one supported
  modded instance or modpack.
- Create, start, use, stop, back up, restore, and safely delete a Vanilla
  server.
- Create a compatible modpack client/server pair and verify version and loader
  agreement.
- Review launcher, game, and server logs for actionable failures and absence of
  secrets.

## Publication and maintenance

- Keep the previous usable stable release and all checksum files available. For
  0.1.1, no such canonical stable release exists.
- Follow `RELEASE_RECOVERY.md` when withdrawing a bad release. Publish a clear
  advisory and direct users only to a previous independently verified stable
  release when one exists; never silently replace an asset under an existing
  filename.
- Security, credential, data-loss, authentication, client-launch, and
  server-start regressions block publication.
- Automatic updates remain disabled until signed update integrity, rollback,
  recovery, and user experience have a separate approval.
- After release, triage security and data loss first, authentication second,
  core client/server launch third, then usability and enhancements.

## Completion decision

Phase 9 closes only when the exact artifacts have passed every applicable gate,
their unsigned status is disclosed clearly, and the maintainer explicitly
approves the public stable release. Preparing a candidate, creating a draft, or
passing local tests is not completion.

## Local preflight evidence — 14 August 2026

Evidence root:

```text
build-migration-build2/phase9-preflight-20260814-003829
```

- Stable channel, official platform, Windows x64, version 0.1.1, LTO enabled.
- All 34 deterministic Release tests passed in 17.64 seconds.
- Portable payload: 35 files; required runtime, Java helpers, `portable.txt`,
  GPL licence, upstream notices, and VC++ redistributable present; no profile
  files or debug runtime detected.
- Portable ZIP SHA-256:
  `477D7AFCD4FCFAA10C6D7AB72265BB9BC32F936B2F3019F255CF9C7A83B420D6`.
- Installer SHA-256:
  `8E6B22379D6357DED8343529CC0F03BB5807DFC0431848B63223AD32A7735F78`.
- Both checksum sidecars matched independent recomputation. The installer and
  launcher are unsigned, matching the maintainer's release policy.
- The saved report in this historical evidence directory was generated before
  the unsigned stable-release decision. The next exact-candidate preflight will
  record `expectedReleaseSignatureStatus: NotSigned` and will not list signing
  as an external gate.
- Static NSIS inspection initially found that the installer carried
  `COPYING.md` but not the full `LICENSE`. The installer and uninstall rules now
  include both, and the preflight uses 7-Zip to require them and the rest of the
  core installer payload without modifying the maintainer's live installation.
- `phase9-package-smoke-empty.json` and
  `phase9-package-smoke-populated.json` record the dependency-isolated package
  observations. Neither is certified cold-start evidence.
- The first LTO build used Ninja's default parallelism and briefly ran 12 link
  processes using about 23.4 GiB of aggregate working set. The preflight script
  now defaults to two parallel jobs for predictable local and hosted resource
  use.

## Release-infrastructure audit — 14 August 2026

- The draft workflow now fetches the complete canonical `develop` ref before
  proving tag ancestry. This remains valid after `develop` advances beyond an
  older release tag.
- Workflow and script clean-source checks explicitly include every untracked
  file and dirty submodule instead of depending on local Git status settings.
- The exact-source exporter now writes Git object bytes directly into a
  deterministic archive while preserving symlinks and executable metadata.
  A disposable tagged repository with a populated submodule, ordinary files,
  executable files, and symbolic links passed two exports with identical
  archive hashes and all nine entries verified; the self-test repository and
  outputs were removed afterward.
- Hosted assembly validates the source provenance fields and hashes, requires
  the exact ten release assets, and writes `SHA256SUMS.txt` over every other
  asset. The draft job rejects missing, extra, duplicate, unsafe, or mismatched
  checksum targets before creating a release.
- Both Phase 9 scripts, all ten embedded workflow PowerShell blocks, workflow
  YAML/manual-trigger/permission shape, full action pins, local Markdown
  links, release placeholders, unsigned disclosures, changed-file secret
  patterns, canonical-branch ancestry, and whitespace checks pass locally.
- The canonical repository was rechecked and still has no tag, release, or
  workflow run. On 15 August, after maintainer approval, GitHub Issues and private
  vulnerability reporting were enabled and the repository homepage was changed
  from Prism Launcher to the reviewed J Launcher website; API and public-route
  verification passed.
