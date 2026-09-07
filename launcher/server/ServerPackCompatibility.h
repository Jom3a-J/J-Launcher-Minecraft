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

    bool isCompatible() const
    {
        return state == ServerPackCompatibilityState::KnownCompatible;
    }
    bool isIncompatible() const
    {
        return state == ServerPackCompatibilityState::Incompatible;
    }
};

ServerPackCompatibilityReport inspectServerPack(const QString &instanceRoot);
ServerPackCompatibilityReport evaluateServerPack(const QString &instanceRoot,
                                                  const QString &minecraftVersion,
                                                  const QString &loaderType,
                                                  const QString &loaderVersion);

QString serverPackCompatibilityDescription(const ServerPackCompatibilityReport &report);
