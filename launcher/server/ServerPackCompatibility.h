/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once

#include <QList>
#include <QString>
#include <QStringList>

enum class ServerPackCompatibilityState {
    KnownCompatible,
    Unknown,
    Incompatible,
};

enum class ServerPackFileSide {
    Unknown,
    ClientOnly,
    ServerOnly,
    Universal,
};

enum class ServerPackIntegrityState {
    Unverified,
    ProviderHashAvailable,
};

// Explicit severity for compatibility diagnostics. Blockers mean continuing
// cannot safely or meaningfully create a server (missing instance, unsafe
// paths). Everything derived from optional provider metadata is Advisory:
// J Launcher can safely fall back to the installed instance profile and
// local server content. Callers must branch on severity/kind, never by
// matching message strings.
enum class ServerPackIssueSeverity {
    Blocker,
    Advisory,
};

// Typed classification for every compatibility diagnostic. Used for logging,
// warning summaries, and programmatic checks without string matching.
enum class ServerPackIssueKind {
    MissingInstance,
    UnsafePath,
    UnreadableMetadata,
    MalformedMetadata,
    ConflictingProviders,
    UnsupportedGame,
    MissingFileList,
    InvalidFileEntry,
    UnknownLoader,
    MultipleLoaders,
    VersionMismatch,
    UnverifiedIntegrity,
    CompatibilityUncertainty,
};

// Trust model for server creation:
// - AuthoritativeInstalledProfile: mmc-pack.json / PackProfile versions and
//   local game content. Always wins; its absence/invalidity blocks creation.
// - AdvisoryProviderMetadata: optional provider documents (mrpack index,
//   CurseForge manifest, FTB/Technic metadata, side lists). Never overrides
//   the installed profile or safety checks; bad values are ignored with a
//   warning and a conservative deterministic fallback.
// - TrustedDownloadedContent: hashes verified during download. Mismatches
//   there remain fatal and are enforced by the download layer, not here.
enum class ServerPackTrust {
    AuthoritativeInstalledProfile,
    AdvisoryProviderMetadata,
    TrustedDownloadedContent,
};

struct ServerPackIssue {
    ServerPackIssueKind kind = ServerPackIssueKind::CompatibilityUncertainty;
    ServerPackIssueSeverity severity = ServerPackIssueSeverity::Advisory;
    ServerPackTrust trust = ServerPackTrust::AdvisoryProviderMetadata;
    QString message;
};

struct ServerPackFileDecision {
    QString path;
    ServerPackFileSide side = ServerPackFileSide::Unknown;
    ServerPackIntegrityState integrity = ServerPackIntegrityState::Unverified;
    QString hashAlgorithm;
    QString hashValue;
    QString hashSource;
};

struct ServerPackCompatibilityReport {
    QString provider;
    QString minecraftVersion;
    QString loaderType;
    QString loaderVersion;
    bool providerMetadataPresent = false;
    bool minecraftVersionMetadataPresent = false;
    bool loaderMetadataPresent = false;
    bool sideMetadataPresent = false;
    bool hasClientOnlyFileMetadata = false;
    bool hasDedicatedServerPack = false;
    ServerPackCompatibilityState state = ServerPackCompatibilityState::Unknown;
    QList<ServerPackFileDecision> files;
    QStringList reasons;
    QStringList warnings;
    // Sparse, user-facing projection notices. Keep diagnostic compatibility
    // warnings separate because provider indexes can contain one per file.
    QStringList projectionWarnings;
    // Typed issue log. Blockers also appear in reasons; advisories appear in
    // warnings/projectionWarnings. Prefer kind/severity checks over strings.
    QList<ServerPackIssue> issues;
    // Aggregate integrity state (no string matching). Counts files whose
    // provider hash is missing/invalid and are therefore unverified.
    // Individual UnverifiedIntegrity issues remain per-file in issues/warnings;
    // installers must use this count for a single bounded user warning.
    int unverifiedFileCount = 0;

    bool isCompatible() const
    {
        return state == ServerPackCompatibilityState::KnownCompatible;
    }
    bool isIncompatible() const
    {
        return state == ServerPackCompatibilityState::Incompatible;
    }
    bool hasBlocker(ServerPackIssueKind kind) const
    {
        for (const auto &issue : issues) {
            if (issue.kind == kind
                && issue.severity == ServerPackIssueSeverity::Blocker) {
                return true;
            }
        }
        return false;
    }
    bool hasAdvisory(ServerPackIssueKind kind) const
    {
        for (const auto &issue : issues) {
            if (issue.kind == kind
                && issue.severity == ServerPackIssueSeverity::Advisory) {
                return true;
            }
        }
        return false;
    }
    bool hasAnyAdvisory() const
    {
        for (const auto &issue : issues) {
            if (issue.severity == ServerPackIssueSeverity::Advisory) {
                return true;
            }
        }
        return false;
    }
};

ServerPackCompatibilityReport inspectServerPack(const QString &instanceRoot);
ServerPackCompatibilityReport evaluateServerPack(const QString &instanceRoot,
                                                  const QString &minecraftVersion,
                                                  const QString &loaderType,
                                                  const QString &loaderVersion);

QString serverPackCompatibilityDescription(const ServerPackCompatibilityReport &report);
QString serverPackIssueKindName(ServerPackIssueKind kind);
