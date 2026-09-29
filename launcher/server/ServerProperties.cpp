// SPDX-License-Identifier: GPL-3.0-only

#include "ServerProperties.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <QJsonDocument>
#include <QJsonObject>
#include <QObject>
#include <QRegularExpression>
#include <QSaveFile>

namespace {
struct LogicalPropertyLine {
    QByteArray original;
    QString text;
};

bool hasContinuation(const QByteArray &line)
{
    int trailingBackslashes = 0;
    for (qsizetype index = line.size() - 1; index >= 0 && line.at(index) == '\\'; --index) {
        ++trailingBackslashes;
    }
    return trailingBackslashes % 2 != 0;
}

QList<LogicalPropertyLine> logicalPropertyLines(const QByteArray &contents)
{
    QList<LogicalPropertyLine> lines;
    qsizetype offset = 0;
    while (offset < contents.size()) {
        LogicalPropertyLine logical;
        bool continuing = false;
        do {
            const qsizetype newline = contents.indexOf('\n', offset);
            const qsizetype physicalEnd = newline < 0 ? contents.size() : newline;
            const qsizetype contentEnd = physicalEnd > offset && contents.at(physicalEnd - 1) == '\r'
                ? physicalEnd - 1 : physicalEnd;
            const QByteArray physical = contents.mid(offset, physicalEnd - offset);
            const QByteArray text = contents.mid(offset, contentEnd - offset);
            logical.original.append(physical);
            if (newline >= 0) {
                logical.original.append('\n');
            }
            offset = newline < 0 ? contents.size() : newline + 1;

            if (continuing) {
                qsizetype leadingWhitespace = 0;
                while (leadingWhitespace < text.size()
                       && (text.at(leadingWhitespace) == ' ' || text.at(leadingWhitespace) == '\t'
                           || text.at(leadingWhitespace) == '\f')) {
                    ++leadingWhitespace;
                }
                logical.text.append(QString::fromUtf8(text.mid(leadingWhitespace)));
            } else {
                logical.text = QString::fromUtf8(text);
            }
            continuing = hasContinuation(text);
            if (continuing) {
                logical.text.chop(1);
            }
        } while (continuing && offset < contents.size());
        lines.append(logical);
    }
    return lines;
}

QString unescapeProperty(const QString &value)
{
    QString result;
    result.reserve(value.size());
    for (qsizetype index = 0; index < value.size(); ++index) {
        const QChar character = value.at(index);
        if (character != QLatin1Char('\\') || index + 1 >= value.size()) {
            result.append(character);
            continue;
        }
        const QChar escaped = value.at(index + 1);
        if (escaped == QLatin1Char('u') && index + 5 < value.size()) {
            ushort codeUnit = 0;
            bool validEscape = true;
            for (qsizetype digitIndex = index + 2; digitIndex <= index + 5; ++digitIndex) {
                const ushort hexCharacter = value.at(digitIndex).unicode();
                const int digit = hexCharacter >= '0' && hexCharacter <= '9'
                    ? hexCharacter - '0'
                    : hexCharacter >= 'a' && hexCharacter <= 'f'
                    ? hexCharacter - 'a' + 10
                    : hexCharacter >= 'A' && hexCharacter <= 'F'
                    ? hexCharacter - 'A' + 10
                    : -1;
                if (digit < 0 || digit > 15) {
                    validEscape = false;
                    break;
                }
                codeUnit = static_cast<ushort>((codeUnit << 4) | digit);
            }
            if (validEscape) {
                result.append(QChar(codeUnit));
                index += 5;
                continue;
            }
        }
        if (escaped == QLatin1Char('u')) {
            result.append(character);
            continue;
        }
        switch (escaped.unicode()) {
            case 't': result.append(QLatin1Char('\t')); break;
            case 'n': result.append(QLatin1Char('\n')); break;
            case 'r': result.append(QLatin1Char('\r')); break;
            case 'f': result.append(QLatin1Char('\f')); break;
            default: result.append(escaped); break;
        }
        ++index;
    }
    return result;
}

QString escapeProperty(const QString &value)
{
    QString escaped;
    escaped.reserve(value.size());
    for (qsizetype index = 0; index < value.size(); ++index) {
        const QChar character = value.at(index);
        switch (character.unicode()) {
            case '\\': escaped.append(QStringLiteral("\\\\")); continue;
            case '\t': escaped.append(QStringLiteral("\\t")); continue;
            case '\n': escaped.append(QStringLiteral("\\n")); continue;
            case '\r': escaped.append(QStringLiteral("\\r")); continue;
            case '\f': escaped.append(QStringLiteral("\\f")); continue;
            case ' ':
                if (index == 0) {
                    escaped.append(QStringLiteral("\\ "));
                } else {
                    escaped.append(character);
                }
                continue;
            default: break;
        }
        if (character.unicode() < 0x20 || character.unicode() > 0x7e) {
            escaped.append(QStringLiteral("\\u"));
            escaped.append(QString::number(character.unicode(), 16).rightJustified(4, QLatin1Char('0')).toUpper());
        } else {
            escaped.append(character);
        }
    }
    return escaped;
}

bool parseProperty(const QString &line, QString *key, QString *value)
{
    qsizetype first = 0;
    while (first < line.size() && line.at(first).isSpace()) {
        ++first;
    }
    if (first == line.size() || line.at(first) == QLatin1Char('#')
        || line.at(first) == QLatin1Char('!')) {
        return false;
    }

    qsizetype separator = -1;
    bool escaped = false;
    for (qsizetype index = first; index < line.size(); ++index) {
        const QChar character = line.at(index);
        if (!escaped && (character == QLatin1Char('=') || character == QLatin1Char(':')
                         || character.isSpace())) {
            separator = index;
            break;
        }
        if (character == QLatin1Char('\\') && !escaped) {
            escaped = true;
        } else {
            escaped = false;
        }
    }

    const QString parsedKey = unescapeProperty(
        separator < 0 ? line.mid(first) : line.mid(first, separator - first));
    if (parsedKey.isEmpty()) {
        return false;
    }
    qsizetype valueStart = separator < 0 ? line.size() : separator;
    if (separator >= 0 && line.at(separator).isSpace()) {
        while (valueStart < line.size() && line.at(valueStart).isSpace()) {
            ++valueStart;
        }
        if (valueStart < line.size()
            && (line.at(valueStart) == QLatin1Char('=') || line.at(valueStart) == QLatin1Char(':'))) {
            ++valueStart;
        }
    } else if (separator >= 0) {
        ++valueStart;
    }
    while (valueStart < line.size() && line.at(valueStart).isSpace()) {
        ++valueStart;
    }
    *key = parsedKey;
    *value = unescapeProperty(line.mid(valueStart));
    return true;
}
}

QMap<QString, QString> ServerProperties::load(const QString &path, QString *error)
{
    QMap<QString, QString> values;
    QFile file(path);
    if (!file.exists()) {
        return values;
    }
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (error) {
            *error = QObject::tr("Could not read server.properties.");
        }
        return values;
    }

    const QList<LogicalPropertyLine> lines = logicalPropertyLines(file.readAll());
    for (const LogicalPropertyLine &line : lines) {
        QString key;
        QString value;
        if (parseProperty(line.text, &key, &value)) {
            values.insert(key, value);
        }
    }
    return values;
}

QString ServerProperties::worldSetupIssue(const QString &serverRoot)
{
    const QDir root(serverRoot);
    QString error;
    const auto properties = load(root.filePath("server.properties"), &error);
    if (!error.isEmpty()) {
        return error;
    }
    QFile requiredFiles(root.filePath(
        QStringLiteral("jlauncher_required_server_files.txt")));
    if (requiredFiles.exists()) {
        if (!requiredFiles.open(QIODevice::ReadOnly | QIODevice::Text)) {
            return QObject::tr("Setup required: could not read the missing server-file list.");
        }
        QStringList missing;
        while (!requiredFiles.atEnd()) {
            QString path = QDir::fromNativeSeparators(
                QDir::cleanPath(QString::fromUtf8(requiredFiles.readLine()).trimmed()));
            while (path.startsWith(QStringLiteral("./"))) path.remove(0, 2);
            if (path.isEmpty() || path == QStringLiteral("..")
                || path.startsWith(QStringLiteral("../"))
                || QDir::isAbsolutePath(path)) {
                return QObject::tr("Setup required: the missing server-file list contains an unsafe path.");
            }
            const QFileInfo file(root.filePath(path));
            if (!file.isFile() || file.isSymLink()) missing.append(path);
        }
        if (!missing.isEmpty()) {
            missing.removeDuplicates();
            return QObject::tr(
                "Setup required: add these missing server files before starting: %1")
                .arg(missing.mid(0, 10).join(QStringLiteral(", ")));
        }
    }
    // Existing worlds own their generation settings. Never regenerate them.
    const QString world = properties.value("level-name", "world");
    if (QFileInfo(root.filePath(world + "/level.dat")).isFile()) {
        return {};
    }
    // Provider adapters can declare arbitrary required properties without
    // teaching the launcher about each individual modpack or generator.
    QFile requirements(root.filePath("server-setup-required.txt"));
    if (requirements.exists()) {
        if (!requirements.open(QIODevice::ReadOnly | QIODevice::Text)) {
            return QObject::tr("Setup required: could not read the pack's required server settings.");
        }
        QStringList missing;
        while (!requirements.atEnd()) {
            const QString key = QString::fromUtf8(requirements.readLine()).trimmed();
            if (key.isEmpty() || key.startsWith('#')) {
                continue;
            }
            if (properties.value(key).trimmed().isEmpty()) {
                missing.append(key);
            }
        }
        if (!missing.isEmpty()) {
            missing.removeDuplicates();
            return QObject::tr("Setup required: set %1 in Server Settings using the pack's "
                               "instructions before generating a world.").arg(missing.join(", "));
        }
    }
    if (QFileInfo(root.filePath("config/topography/Topography.js")).isFile()
        && properties.value("topography-preset").trimmed().isEmpty()) {
        return QObject::tr("Setup required: this pack uses Topography. Choose the pack's world preset "
                           "and add topography-preset in Server Settings before the first start. "
                           "Consult the pack's server instructions; no world has been generated.");
    }
    if (QFileInfo(root.filePath("config/topography/Topography.txt")).isFile()) {
        const auto generator = QJsonDocument::fromJson(
            properties.value("generator-settings").toUtf8()).object();
        if (generator.value("Topography-Preset").toString().trimmed().isEmpty()) {
            return QObject::tr("Setup required: this pack uses legacy Topography. Set the "
                               "Topography-Preset in generator-settings using the pack's server "
                               "instructions before the first start. No world has been generated.");
        }
    }
    return {};
}

bool ServerProperties::validate(const QMap<QString, QString> &values, QString *error)
{
    static const QRegularExpression validKey(QStringLiteral("^[A-Za-z0-9_.-]+$"));
    for (auto iterator = values.constBegin(); iterator != values.constEnd(); ++iterator) {
        if (!validKey.match(iterator.key()).hasMatch()) {
            if (error) {
                *error = QObject::tr("Invalid server.properties option name: %1")
                             .arg(iterator.key());
            }
            return false;
        }
    }

    if (values.contains(QStringLiteral("server-port"))) {
        bool validPort = false;
        const int port = values.value(QStringLiteral("server-port")).toInt(&validPort);
        if (!validPort || port < 1 || port > 65535) {
            if (error) {
                *error = QObject::tr("server-port must be between 1 and 65535.");
            }
            return false;
        }
    }
    return true;
}

bool ServerProperties::save(const QString &path, const QMap<QString, QString> &values,
                            QString *error)
{
    if (!validate(values, error)) {
        return false;
    }
    if (!QDir().mkpath(QFileInfo(path).dir().absolutePath())) {
        if (error) {
            *error = QObject::tr("Could not create the server configuration folder.");
        }
        return false;
    }
    const bool exists = QFileInfo::exists(path);
    QByteArray existingContents;
    if (exists) {
        QFile existingFile(path);
        if (!existingFile.open(QIODevice::ReadOnly)) {
            if (error) {
                *error = QObject::tr("Could not read server.properties.");
            }
            return false;
        }
        existingContents = existingFile.readAll();
    }

    QByteArray output;
    if (!exists) {
        output.append("#Minecraft server properties managed by J Launcher\n");
    }
    QSet<QString> writtenKeys;
    const QList<LogicalPropertyLine> lines = logicalPropertyLines(existingContents);
    for (const LogicalPropertyLine &line : lines) {
        QString key;
        QString value;
        if (!parseProperty(line.text, &key, &value)) {
            output.append(line.original);
            continue;
        }
        if (!values.contains(key) || writtenKeys.contains(key)) {
            continue;
        }
        writtenKeys.insert(key);
        if (value == values.value(key)) {
            output.append(line.original);
        } else {
            output.append(key.toUtf8());
            output.append('=');
            output.append(escapeProperty(values.value(key)).toUtf8());
            output.append('\n');
        }
    }
    for (auto iterator = values.constBegin(); iterator != values.constEnd(); ++iterator) {
        if (!writtenKeys.contains(iterator.key())) {
            output.append(iterator.key().toUtf8());
            output.append('=');
            output.append(escapeProperty(iterator.value()).toUtf8());
            output.append('\n');
        }
    }

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error) {
            *error = QObject::tr("Could not write server.properties.");
        }
        return false;
    }
    if (file.write(output) != output.size() || !file.commit()) {
        file.cancelWriting();
        if (error) {
            *error = QObject::tr("Could not finalize server.properties.");
        }
        return false;
    }
    return true;
}
