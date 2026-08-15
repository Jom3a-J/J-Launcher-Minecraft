# Stable Release Checklist

This checklist applies to J Launcher's Windows x64 stable release. A checkbox
may be marked only with evidence from the exact source tag and exact artifacts
being considered for publication.

## Source and identity

- [ ] Confirm the version, stable channel, annotated tag, target commit, and
      canonical repository.
- [ ] Confirm the product-scoped tag is `jlauncher-0.1.1`; do not move or reuse
      the inherited MultiMC `0.1.1` tag from 2014.
- [ ] Confirm the tag is reachable from the reviewed release branch and the
      source tree is clean.
- [ ] Confirm the binary reports the exact version, stable status, official
      platform, and tagged Git commit.
- [ ] Review release notes, known issues, privacy, integrations, support,
      security, code-signing policy, licences, and upstream attribution at the
      tag.
- [x] Confirm GitHub Issues is enabled and a non-maintainer can open the linked
      public issue form.
- [x] Confirm GitHub private vulnerability reporting is enabled and a
      non-maintainer can open the private reporting form linked from
      `SECURITY.md`.
- [x] Replace the canonical repository's inherited Prism Launcher homepage
      with the reviewed J Launcher website or canonical repository URL.
- [ ] Confirm production Microsoft and provider configuration belongs to
      J Launcher and no private test data or credential file is present.
- [ ] Confirm automatic updates, analytics, advertising, news, and screenshot
      uploading remain disabled unless separately reviewed and approved.

## Build and test evidence

- [ ] A public, manual workflow checks out the exact tag and builds only
      Windows x64 with MSVC in Release mode.
- [ ] The exact tag passes all 34 deterministic CTest targets; record any
      additional opt-in or live test that was deliberately skipped.
- [ ] `git diff --check` passes before tagging.
- [ ] Workflow syntax and pinned third-party actions are reviewed.
- [ ] Empty and representative populated profiles pass the documented Phase 11
      startup, Settings, Servers, server-destination, idle CPU, and memory
      budgets from the packaged artifact.
- [ ] A first launch after an actual reboot is recorded before any warm-up and
      explicitly identified as the certified cold-start sample.

## Artifact integrity and contents

- [ ] Portable ZIP, standard-user installer, exact-source archive, and matching
      SHA-256 files are present, non-empty, and produced from the tag.
- [ ] Every checksum is independently recomputed after downloading the draft
      assets.
- [ ] The source archive matches every tracked file and populated submodule at
      the tag, preserves Git symlinks and executable metadata, and contains
      nothing extra.
- [ ] Portable and installer payloads contain the launcher/helper binaries,
      Java helpers, required Qt/MSVC runtime, plugins, `portable.txt` where
      appropriate, manifests, GPL licence, upstream notices, and third-party
      notices.
- [ ] Payloads and build logs contain no profile state, tokens, credentials,
      private keys, personal paths, caches, debug runtimes, or development-only
      configuration.
- [ ] Portable startup succeeds with `PATH` limited to Windows system
      directories, proving no development Qt/vcpkg dependency.
- [ ] Record the Authenticode status of the installer and every J Launcher
      executable. For 0.1.1, the expected status is `NotSigned`.
- [ ] Confirm the release page, README, notes, and known issues call the build
      unsigned, explain Windows warnings/blocking, and do not claim a verified
      publisher.
- [ ] Record artifact hashes and the public workflow provenance URL.

## Fresh Windows x64 standard-user test

Use the exact candidate on a genuinely fresh, fully updated Windows machine,
not the development account or a profile merely emptied on the same machine.

- [ ] Record Windows edition/build, hardware/VM description, standard-user
      status, artifact hashes, and the expected unsigned signature state.
- [ ] Launch the portable package and confirm data remains under its portable
      root.
- [ ] Install without elevation, verify Start/Desktop integration, launch, and
      uninstall without deleting unrelated data.
- [ ] Complete Microsoft/Minecraft sign-in, sign-out, and account removal.
- [ ] Detect or download the compatible Java runtime and launch a Vanilla
      instance.
- [ ] Create and launch one supported modded instance or modpack.
- [ ] Create a Vanilla server, accept the EULA explicitly, start it, use the
      console/player controls, and stop it cleanly.
- [ ] Create a compatible modpack server and client and verify their Minecraft
      version and loader match.
- [ ] Exercise backup, restore, Recycle Bin deletion, and the guarded permanent
      fallback with disposable data.
- [ ] Review launcher, game, and server logs for clear failures and absence of
      secrets.
- [ ] Capture visible UI checkpoints and maintainer/tester approval.

## Publish and rollback

- [ ] Review the draft exactly as a user would download it.
- [ ] Confirm the release page states Windows x64 support, unsigned status,
      known issues, source tag, checksum instructions, privacy/support routes,
      and automatic-update status.
- [ ] Confirm the prior usable stable release and checksums remain available.
      For the first stable release, confirm that no canonical prior stable
      exists and do not represent a beta, local build, or legacy-repository
      artifact as a rollback.
- [ ] Confirm withdrawal and recovery instructions follow
      `RELEASE_RECOVERY.md`, point to immutable prior artifacts when available,
      and never replace an asset under an existing filename.
- [ ] Confirm no open security, credential, data-loss, authentication,
      client-launch, or server-start blocker exists.
- [ ] Obtain explicit maintainer approval after all evidence is recorded.
- [ ] Publish manually. A successful workflow or tag push must
      never publish by itself.

## Current 0.1.1 candidate status — 15 August 2026

- [x] Phase 11 local Release packaging, dependency isolation, empty/populated
      performance, visual, and data-safety checks passed on the current working
      source.
- [x] A current Windows Debug build passed all 34 deterministic native tests.
- [x] A stable-channel Windows x64 MSVC Release build with LTO completed, and
      all 34 Release tests passed in 17.64 seconds.
- [x] The local preflight produced an audited portable ZIP and NSIS installer;
      required runtime/licence files were present, forbidden profile/debug
      files were absent, and both checksum sidecars matched independent
      SHA-256 recomputation.
- [x] Static NSIS inspection found and fixed the installer's missing full
      `LICENSE`; the preflight now requires both `LICENSE` and `COPYING.md` in
      the installer payload.
- [x] One-sample dependency-isolated empty and populated package smokes passed
      every Phase 11 budget. They are observational and are not the certified
      post-reboot cold-start result.
- [x] A manual-only, draft-only Windows x64 workflow and exact tracked-source
      archive script are prepared locally. They still require review, commit,
      and a public run against the final annotated tag.
- [x] A focused release-infrastructure audit corrected canonical-branch
      ancestry validation, made dirty-source detection include every untracked
      file and dirty submodule, preserved Git symlinks/executable metadata in
      deterministic source archives, and made the combined checksum cover the
      exact ten-file draft asset set.
- [x] The public release-page template and first-release withdrawal/recovery
      policy are prepared locally for exact-tag review.
- [x] The inherited annotated `0.1.1` tag was found to belong to MultiMC's 2014
      history. The collision-safe candidate tag is `jlauncher-0.1.1`; the
      product version and artifact filenames remain `0.1.1`.
- [ ] The worktree is not clean and no exact candidate commit or tag has been
      selected; existing results are therefore supporting evidence only.
- [ ] The canonical repository has no hosted release run or draft artifacts.
- [x] On 15 August 2026, the canonical repository enabled Issues and private
      vulnerability reporting and replaced the inherited Prism Launcher
      homepage with `https://jom3a-j.github.io/J-Launcher-web/`. The repository
      API reports both features enabled, the homepage and public new-issue route
      return HTTP 200, and the signed-out private-report route reaches GitHub's
      authentication handoff.
- [ ] The post-reboot cold-start and genuinely fresh-machine gates remain open.
- [x] The maintainer decided on 14 August 2026 to publish the first stable
      release unsigned because trusted signing is currently too expensive.
      Signing may be added to a later release if support makes it practical.
- [ ] Explicit stable-publication approval remains open.
