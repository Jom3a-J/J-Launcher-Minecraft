// SPDX-License-Identifier: GPL-3.0-only

#include "ServerProperties.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTextStream>

namespace {
QString unescapeProperty(QString value)
{
    value.replace(QStringLiteral("\\n"), QStringLiteral("\n"));
    value.replace(QStringLiteral("\\r"), QStringLiteral("\r"));
    value.replace(QStringLiteral("\\t"), QStringLiteral("\t"));
    value.replace(QStringLiteral("\\="), QStringLiteral("="));
    value.replace(QStringLiteral("\\:"), QStringLiteral(":"));
    value.replace(QStringLiteral("\\\\"), QStringLiteral("\\"));
    return value;
}

QString escapeProperty(QString value)
{
    value.replace(QStringLiteral("\\"), QStringLiteral("\\\\"));
    value.replace(QStringLiteral("\t"), QStringLiteral("\\t"));
    return value;
}

int propertySeparator(const QString &line)
{
    bool escaped = false;
    for (int index = 0; index < line.size(); ++index) {
        const QChar character = line.at(index);
        if (!escaped && (character == QLatin1Char('=') || character == QLatin1Char(':'))) {
            return index;
        }
        if (!escaped && character.isSpace()) {
            return index;
        }
        if (character == QLatin1Char('\\') && !escaped) {
            escaped = true;
        } else {
            escaped = false;
        }
    }
    return -1;
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

    QTextStream stream(&file);
    while (!stream.atEnd()) {
        const QString originalLine = stream.readLine();
        const QString line = originalLine.trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#'))
            || line.startsWith(QLatin1Char('!'))) {
            continue;
        }
        const int separator = propertySeparator(line);
        const QString key = unescapeProperty(
            separator < 0 ? line : line.left(separator)).trimmed();
        QString value = separator < 0 ? QString() : line.mid(separator + 1).trimmed();
        if (!key.isEmpty()) {
            values.insert(key, unescapeProperty(value));
        }
    }
    return values;
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
        if (iterator.value().contains(QLatin1Char('\n'))
            || iterator.value().contains(QLatin1Char('\r'))) {
            if (error) {
                *error = QObject::tr("The value for %1 contains a line break.")
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
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        if (error) {
            *error = QObject::tr("Could not write server.properties.");
        }
        return false;
    }
    QTextStream stream(&file);
    stream << "#Minecraft server properties managed by J Launcher\n";
    for (auto iterator = values.constBegin(); iterator != values.constEnd(); ++iterator) {
        stream << iterator.key() << '=' << escapeProperty(iterator.value()) << '\n';
    }
    stream.flush();
    if (stream.status() != QTextStream::Ok || !file.commit()) {
        file.cancelWriting();
        if (error) {
            *error = QObject::tr("Could not finalize server.properties.");
        }
        return false;
    }
    return true;
}
