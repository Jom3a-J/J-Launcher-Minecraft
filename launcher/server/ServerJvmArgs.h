// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QString>
#include <QStringList>

// Launcher-owned preparation for supplied Forge/NeoForge user_jvm_args.txt.
//
// The pack file is never modified. Filtering is runtime-aware: memory flags
// are always removed so ServerInstance memory stays authoritative, and JVM
// options known to require a different Java major are removed with a warning.
// Unknown options are preserved so packs keep working when the knowledge
// base does not cover them.
struct ServerJvmFilterResult {
    bool ok = false;
    QStringList keptTokens;
    QStringList removedMemoryOptions;
    QStringList removedUnsupportedOptions;
    QString errorMessage;
};

class ServerJvmArgs final {
   public:
    // Stable ASCII locale for every dedicated-server Java launch. The FORMAT
    // category is included so numeric formatting (and therefore generated
    // resource identifiers) does not inherit Arabic-Indic or other
    // locale-specific digits from the host.
    static QStringList serverLocaleJvmArgs();

    // JAVA_TOOL_OPTIONS fallback for opaque custom wrapper scripts whose
    // hardcoded argfile cannot be filtered safely. Memory and locale are
    // included, plus IgnoreUnrecognizedVMOptions so an unknown pack option
    // (for example a newer-Java-only -XX flag) does not abort the JVM before
    // Minecraft starts. Pack memory flags inside the wrapper still win over
    // the environment because JVM command-line options override
    // JAVA_TOOL_OPTIONS; that limit is logged at launch time.
    static QString wrapperEnvironmentArgs(int minMemoryMiB, int maxMemoryMiB,
                                          const QString &extraJvmArguments);

    static QString effectiveFileName();
    static int maxArgfileBytes();

    static bool isMemoryOption(const QString &token);
    static bool isBareMemoryFlag(const QString &token);
    static bool isHeapSizeValue(const QString &value);
    static QString unsupportedOptionName(const QString &token, int javaMajor);

    // Tokenize Java argfile content without executing it. Handles '#'
    // comments, blank lines, single/double quotes, and backslash escapes.
    // Returns false with an error for unclosed quotes.
    static bool tokenizeArgfile(const QString &content, QStringList *tokens, QString *error);
    static QString quoteToken(const QString &token);

    static ServerJvmFilterResult filterTokens(const QStringList &tokens, int javaMajor);
    static ServerJvmFilterResult prepareContent(const QString &content, int javaMajor);

    // Read, validate, and filter a supplied argfile. Never modifies the
    // source file.
    static ServerJvmFilterResult prepareFile(const QString &sourcePath, int javaMajor);
    static bool writeEffectiveArgfile(const QString &path, const QStringList &tokens, QString *error);
    static bool writeEffectiveFileForSource(const QString &sourcePath, const QString &destPath,
                                            int javaMajor, ServerJvmFilterResult *result);
};
