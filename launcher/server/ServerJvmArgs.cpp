// SPDX-License-Identifier: GPL-3.0-only

#include "ServerJvmArgs.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QSaveFile>

namespace {

bool isArgfileWhitespace(QChar c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

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
    QStringList encoded;
    encoded.reserve(parts.size());
    for (const QString &part : parts) {
        encoded << quoteEnvToken(part);
    }
    return encoded.join(' ');
}

QString ServerJvmArgs::quoteEnvToken(const QString &token)
{
    // Concatenate double-quoted segments with single-quoted literal double
    // quotes. Neither parser consumes backslashes in these environment values.
    QString encoded = token;
    encoded.replace(QLatin1Char('"'), QStringLiteral("\"'\"'\""));
    return QLatin1Char('"') + encoded + QLatin1Char('"');
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
    const auto handleQuotedChar = [&](QChar c, int &index, QChar quote, bool &inQuote) {
        if (c == quote) {
            inQuote = false;
            return;
        }
        if (c == '\\' && index + 1 < size) {
            const QChar nxt = content.at(index + 1);
            if (nxt == '\n' || nxt == '\r') {
                // Java also discards indentation and blank lines following
                // a quoted line continuation.
                ++index;
                while (index + 1 < size && isArgfileWhitespace(content.at(index + 1))) {
                    ++index;
                }
                return;
            }
            switch (nxt.unicode()) {
                case 'n': current += '\n'; break;
                case 'r': current += '\r'; break;
                case 't': current += '\t'; break;
                case 'f': current += '\f'; break;
                default: current += nxt; break;
            }
            tokenStarted = true;
            index += 1;
            return;
        }
        current += c;
        tokenStarted = true;
    };
    for (int i = 0; i < size; ++i) {
        const QChar c = content.at(i);
        if ((inSingle || inDouble) && (c == '\n' || c == '\r')) {
            if (error) {
                *error = QStringLiteral("Unclosed quote in supplied JVM arguments file.");
            }
            return false;
        }
        if (inSingle) {
            handleQuotedChar(c, i, QChar('\''), inSingle);
            continue;
        }
        if (inDouble) {
            handleQuotedChar(c, i, QChar('"'), inDouble);
            continue;
        }
        if (c == '\'') {
            inSingle = true;
            tokenStarted = true;
        } else if (c == '"') {
            inDouble = true;
            tokenStarted = true;
        } else if (c == '#') {
            // Verified on a real JVM: an unquoted '#' abandons the
            // in-progress token (if any) and comments out the rest of the
            // line, while '#' inside quotes stays literal. Quoting the
            // token on write preserves an intended literal '#'.
            current.clear();
            tokenStarted = false;
            while (i < size && content.at(i) != '\n' && content.at(i) != '\r') {
                ++i;
            }
            if (i >= size) {
                break;
            }
            // The newline itself is whitespace; the for-loop increment
            // moves past it.
        } else if (isArgfileWhitespace(c)) {
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
        switch (c.unicode()) {
            case '\n': quoted += QStringLiteral("\\n"); continue;
            case '\r': quoted += QStringLiteral("\\r"); continue;
            case '\t': quoted += QStringLiteral("\\t"); continue;
            case '\f': quoted += QStringLiteral("\\f"); continue;
            default: break;
        }
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
    // Java's native launcher decodes @-files with the platform encoding,
    // including the Windows ANSI code page (also on Java 21/25).
    const QString content = QString::fromLocal8Bit(data);
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
        const QString quoted = quoteToken(token);
        const QByteArray encoded = quoted.toLocal8Bit();
        if (QString::fromLocal8Bit(encoded) != quoted) {
            if (error) {
                *error = QStringLiteral("A supplied JVM option cannot be represented in the Java launcher's platform encoding.");
            }
            file.cancelWriting();
            return false;
        }
        output += encoded;
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
