// SPDX-License-Identifier: GPL-3.0-only

#include "ServerJvmArgs.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QSaveFile>

namespace {

struct VersionGatedOption {
    const char *name;
    int minMajor;  // 0 means no minimum
    int maxMajor;  // 0 means no maximum
};

// Runtime-aware knowledge base. Unknown options are preserved; only options
// with a known version floor/ceiling are filtered. This keeps the fix
// provider/pack independent instead of special-casing one modpack.
constexpr VersionGatedOption kVersionGatedOptions[] = {
    // Generational ZGC only exists on Java 21+ (JEP 439). On Java 17 the JVM
    // exits with "Unrecognized VM option 'ZGenerational'" before Minecraft
    // starts. Java 21+ retains the flag.
    { "ZGenerational", 21, 0 },
    { "GenerationalZGC", 21, 0 },
    // Garbage collectors introduced after Java 8.
    { "UseZGC", 11, 0 },
    { "UseShenandoahGC", 12, 0 },
    { "UseEpsilonGC", 11, 0 },
    // Concurrent mark sweep was removed in Java 14 (JEP 363).
    { "UseConcMarkSweepGC", 0, 13 },
    { "CMSIncrementalMode", 0, 13 },
    { "UseCMSInitiatingOccupancyOnly", 0, 13 },
    { "CMSInitiatingOccupancyFraction", 0, 13 },
    // PermGen was removed in Java 8.
    { "PermSize", 0, 7 },
    { "MaxPermSize", 0, 7 },
    // Removed verifier/GC variants.
    { "UseSplitVerifier", 0, 7 },
    { "UseParNewGC", 0, 9 },
};

const VersionGatedOption *findGatedOption(const QString &name)
{
    for (const auto &entry : kVersionGatedOptions) {
        if (name == QLatin1String(entry.name)) {
            return &entry;
        }
    }
    return nullptr;
}

bool isSupportedByGatedEntry(const VersionGatedOption &entry, int javaMajor)
{
    if (javaMajor <= 0) {
        // Unknown runtime: preserve rather than silently dropping options.
        return true;
    }
    if (entry.minMajor > 0 && javaMajor < entry.minMajor) {
        return false;
    }
    if (entry.maxMajor > 0 && javaMajor > entry.maxMajor) {
        return false;
    }
    return true;
}

// Accepted JVM heap-size suffixes: bare digits (bytes) or digits plus
// k/m/g/t (case-insensitive). Covers 2G, 2048M, 512k, 1024, etc.
bool heapSizeTextValid(const QString &value)
{
    if (value.isEmpty() || value.size() > 16) {
        return false;
    }
    int digits = 0;
    for (int i = 0; i < value.size(); ++i) {
        const QChar c = value.at(i);
        if (c.isDigit()) {
            ++digits;
            continue;
        }
        if (i == value.size() - 1 && digits > 0
            && (c == 'k' || c == 'K' || c == 'm' || c == 'M'
                || c == 'g' || c == 'G' || c == 't' || c == 'T')) {
            return true;
        }
        return false;
    }
    return digits > 0;
}

QString xxOptionName(const QString &token)
{
    // token is known to start with "-XX:".
    QString rest = token.mid(4);
    if (rest.startsWith('+') || rest.startsWith('-')) {
        return rest.mid(1);
    }
    const int equals = rest.indexOf('=');
    if (equals >= 0) {
        return rest.left(equals);
    }
    const int colon = rest.indexOf(':');
    if (colon >= 0) {
        return rest.left(colon);
    }
    return rest;
}

}  // namespace

QStringList ServerJvmArgs::serverLocaleJvmArgs()
{
    // FORMAT category properties pin numeric formatting to ASCII digits even
    // when the host OS locale uses Arabic-Indic or other native digits.
    return QStringList{
        QStringLiteral("-Duser.language=en"),
        QStringLiteral("-Duser.country=US"),
        QStringLiteral("-Duser.language.format=en"),
        QStringLiteral("-Duser.country.format=US"),
    };
}

QString ServerJvmArgs::wrapperEnvironmentArgs(int minMemoryMiB, int maxMemoryMiB,
                                              const QString &extraJvmArguments)
{
    QStringList parts;
    parts << QStringLiteral("-Xmx%1M").arg(maxMemoryMiB);
    parts << QStringLiteral("-Xms%1M").arg(minMemoryMiB);
    if (!extraJvmArguments.trimmed().isEmpty()) {
        parts << QProcess::splitCommand(extraJvmArguments.trimmed());
    }
    parts << serverLocaleJvmArgs();
    // Narrowly justified fallback for opaque wrappers only: tolerate unknown
    // pack options instead of exiting before Minecraft starts. This does not
    // fix conflicting memory flags (last option still wins) and is never used
    // on the direct-jar or filtered-argfile paths where precise filtering
    // applies.
    parts << QStringLiteral("-XX:+IgnoreUnrecognizedVMOptions");
    QStringList quoted;
    quoted.reserve(parts.size());
    for (const QString &part : parts) {
        quoted << quoteToken(part);
    }
    return quoted.join(' ');
}

QString ServerJvmArgs::effectiveFileName()
{
    return QStringLiteral("jlauncher_effective_jvm_args.txt");
}

int ServerJvmArgs::maxArgfileBytes()
{
    return 256 * 1024;
}

bool ServerJvmArgs::isBareMemoryFlag(const QString &token)
{
    return token == QLatin1String("-Xms") || token == QLatin1String("-Xmx");
}

bool ServerJvmArgs::isHeapSizeValue(const QString &value)
{
    return heapSizeTextValid(value);
}

bool ServerJvmArgs::isMemoryOption(const QString &token)
{
    if (token.startsWith(QLatin1String("-Xms")) || token.startsWith(QLatin1String("-Xmx"))) {
        return true;
    }
    if (token.startsWith(QLatin1String("-XX:InitialHeapSize")) ||
        token.startsWith(QLatin1String("-XX:MaxHeapSize"))) {
        return true;
    }
    return false;
}

QString ServerJvmArgs::unsupportedOptionName(const QString &token, int javaMajor)
{
    if (!token.startsWith(QLatin1String("-XX:")) || javaMajor <= 0) {
        return QString();
    }
    const QString name = xxOptionName(token);
    if (name.isEmpty()) {
        return QString();
    }
    if (const auto *entry = findGatedOption(name)) {
        if (!isSupportedByGatedEntry(*entry, javaMajor)) {
            return name;
        }
        return QString();
    }
    // Cover the CMS family without enumerating every CMS* flag: all CMS
    // options were removed together with the collector in Java 14.
    if (name.startsWith(QLatin1String("CMS")) && javaMajor >= 14) {
        return name;
    }
    return QString();
}

bool ServerJvmArgs::tokenizeArgfile(const QString &content, QStringList *tokens, QString *error)
{
    if (!tokens) {
        return false;
    }
    tokens->clear();
    QString current;
    bool inSingle = false;
    bool inDouble = false;
    bool tokenStarted = false;
    const int size = content.size();
    auto consumeLineContinuation = [&](int &index, QChar first) -> bool {
        // index points at '\\'; first is the char after it.
        if (first == '\n') {
            index += 1;
            return true;
        }
        if (first == '\r') {
            index += 1;
            if (index + 1 < size && content.at(index + 1) == '\n') {
                index += 1;
            }
            return true;
        }
        return false;
    };
    for (int i = 0; i < size; ++i) {
        const QChar c = content.at(i);
        if (inSingle) {
            if (c == '\'') {
                inSingle = false;
            } else {
                current += c;
                tokenStarted = true;
            }
            continue;
        }
        if (inDouble) {
            if (c == '"') {
                inDouble = false;
            } else if (c == '\\') {
                if (i + 1 >= size) {
                    current += '\\';
                    tokenStarted = true;
                } else {
                    const QChar nxt = content.at(i + 1);
                    if (nxt == '"') {
                        current += '"';
                        tokenStarted = true;
                        ++i;
                    } else if (nxt == '\\') {
                        current += '\\';
                        tokenStarted = true;
                        ++i;
                    } else if (nxt == '\n' || nxt == '\r') {
                        // Line continuation inside quotes: join lines.
                        consumeLineContinuation(i, nxt);
                    } else {
                        // Preserve Windows separators such as C:\mods:
                        // backslash before ordinary characters stays literal.
                        current += '\\';
                        tokenStarted = true;
                    }
                }
            } else {
                current += c;
                tokenStarted = true;
            }
            continue;
        }
        if (c == '\'') {
            inSingle = true;
            tokenStarted = true;
        } else if (c == '"') {
            inDouble = true;
            tokenStarted = true;
        } else if (c == '\\') {
            if (i + 1 >= size) {
                current += '\\';
                tokenStarted = true;
            } else {
                const QChar nxt = content.at(i + 1);
                if (nxt == '\n' || nxt == '\r') {
                    consumeLineContinuation(i, nxt);
                } else {
                    // Java-argfile-compatible: ordinary backslashes (Windows
                    // paths like C:\mods) are literal. Only CR/LF continuation
                    // consumes the backslash.
                    current += '\\';
                    tokenStarted = true;
                }
            }
        } else if (c == '#') {
            // '#' starts a comment only at a token boundary so values such
            // as -Dfoo=bar#baz survive. Inside a token it is literal.
            if (current.isEmpty() && !tokenStarted) {
                while (i < size && content.at(i) != '\n') {
                    ++i;
                }
                if (i < size) {
                    // The newline itself is whitespace; the for-loop
                    // increment moves past it.
                } else {
                    break;
                }
            } else {
                current += c;
                tokenStarted = true;
            }
        } else if (c.isSpace()) {
            if (tokenStarted) {
                tokens->append(current);
                current.clear();
                tokenStarted = false;
            }
        } else {
            current += c;
            tokenStarted = true;
        }
    }
    if (inSingle || inDouble) {
        if (error) {
            *error = QStringLiteral("Unclosed quote in supplied JVM arguments file.");
        }
        return false;
    }
    if (tokenStarted) {
        tokens->append(current);
    }
    return true;
}

QString ServerJvmArgs::quoteToken(const QString &token)
{
    if (token.isEmpty()) {
        return QStringLiteral("\"\"");
    }
    bool needsQuotes = false;
    for (const QChar c : token) {
        if (c.isSpace() || c == '"' || c == '\'' || c == '#' || c == '\\') {
            needsQuotes = true;
            break;
        }
    }
    if (!needsQuotes) {
        return token;
    }
    QString quoted = QStringLiteral("\"");
    for (const QChar c : token) {
        if (c == '"' || c == '\\') {
            quoted += '\\';
        }
        quoted += c;
    }
    quoted += '"';
    return quoted;
}

ServerJvmFilterResult ServerJvmArgs::filterTokens(const QStringList &tokens, int javaMajor)
{
    ServerJvmFilterResult result;
    result.ok = true;
    for (int i = 0; i < tokens.size(); ++i) {
        const QString token = tokens.at(i);
        if (token.isEmpty()) {
            continue;
        }
        if (isBareMemoryFlag(token)) {
            // Only consume the next token when it is a valid heap size
            // (2G, 2048M, ...). If it is another JVM option, preserve it.
            if (i + 1 < tokens.size() && isHeapSizeValue(tokens.at(i + 1))) {
                result.removedMemoryOptions << (token + ' ' + tokens.at(i + 1));
                ++i;
            } else {
                result.removedMemoryOptions << token;
            }
            continue;
        }
        if (isMemoryOption(token)) {
            result.removedMemoryOptions << token;
            continue;
        }
        const QString unsupported = unsupportedOptionName(token, javaMajor);
        if (!unsupported.isEmpty()) {
            result.removedUnsupportedOptions << token;
            continue;
        }
        result.keptTokens << token;
    }
    return result;
}

ServerJvmFilterResult ServerJvmArgs::prepareContent(const QString &content, int javaMajor)
{
    QStringList tokens;
    QString error;
    if (!tokenizeArgfile(content, &tokens, &error)) {
        ServerJvmFilterResult result;
        result.ok = false;
        result.errorMessage = error;
        return result;
    }
    ServerJvmFilterResult result = filterTokens(tokens, javaMajor);
    result.ok = true;
    return result;
}

ServerJvmFilterResult ServerJvmArgs::prepareFile(const QString &sourcePath, int javaMajor)
{
    ServerJvmFilterResult result;
    // Pre-read bound so oversized packs fail without buffering them.
    // The post-read check below is retained for TOCTOU races.
    if (QFileInfo(sourcePath).size() > maxArgfileBytes()) {
        result.ok = false;
        result.errorMessage = QStringLiteral("The supplied JVM arguments file '%1' exceeds the %2 KiB safety limit.")
                                  .arg(QFileInfo(sourcePath).fileName())
                                  .arg(maxArgfileBytes() / 1024);
        return result;
    }
    QFile file(sourcePath);
    if (!file.open(QIODevice::ReadOnly)) {
        result.ok = false;
        result.errorMessage = QStringLiteral("Could not read the supplied JVM arguments file '%1'.")
                                  .arg(QFileInfo(sourcePath).fileName());
        return result;
    }
    const QByteArray data = file.readAll();
    if (data.contains('\0')) {
        result.ok = false;
        result.errorMessage = QStringLiteral("The supplied JVM arguments file '%1' contains NUL bytes.")
                                  .arg(QFileInfo(sourcePath).fileName());
        return result;
    }
    if (data.size() > maxArgfileBytes()) {
        result.ok = false;
        result.errorMessage = QStringLiteral("The supplied JVM arguments file '%1' exceeds the %2 KiB safety limit.")
                                  .arg(QFileInfo(sourcePath).fileName())
                                  .arg(maxArgfileBytes() / 1024);
        return result;
    }
    const QString content = QString::fromUtf8(data);
    return prepareContent(content, javaMajor);
}

bool ServerJvmArgs::writeEffectiveArgfile(const QString &path, const QStringList &tokens, QString *error)
{
    QDir().mkpath(QFileInfo(path).dir().absolutePath());
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        if (error) {
            *error = QStringLiteral("Could not write the launcher JVM arguments file '%1'.")
                         .arg(QFileInfo(path).fileName());
        }
        return false;
    }
    QByteArray output;
    output += "# Generated by J Launcher. Do not edit; the pack file 'user_jvm_args.txt' is preserved unchanged.\n";
    for (const QString &token : tokens) {
        output += quoteToken(token).toUtf8();
        output += '\n';
    }
    if (file.write(output) != output.size() || !file.commit()) {
        file.cancelWriting();
        if (error) {
            *error = QStringLiteral("Could not write the launcher JVM arguments file '%1'.")
                         .arg(QFileInfo(path).fileName());
        }
        return false;
    }
    return true;
}

bool ServerJvmArgs::writeEffectiveFileForSource(const QString &sourcePath, const QString &destPath,
                                                int javaMajor, ServerJvmFilterResult *result)
{
    ServerJvmFilterResult prepared = prepareFile(sourcePath, javaMajor);
    if (result) {
        *result = prepared;
    }
    if (!prepared.ok) {
        return false;
    }
    QString error;
    if (!writeEffectiveArgfile(destPath, prepared.keptTokens, &error)) {
        if (result) {
            result->ok = false;
            result->errorMessage = error;
        }
        return false;
    }
    if (result) {
        result->ok = true;
    }
    return true;
}
