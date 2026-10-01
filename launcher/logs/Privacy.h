/* SPDX-License-Identifier: GPL-3.0-only */

#pragma once

#include <QList>
#include <QPair>
#include <QProcessEnvironment>
#include <QString>
#include <QStringList>
#include <QUrl>

class QNetworkRequest;

namespace Privacy {

constexpr int DefaultLogLimit = 4096;

QString sanitizeText(const QString& text, int maxLength = DefaultLogLimit);
/*! Redacts secrets and user paths in text of any length without truncating it.
 *
 *  For whole logs (uploads, clipboard copies) and individual game log lines, where cutting
 *  the text would lose the part the user needs (usually the crash at the end).
 */
QString sanitizeLongText(const QString& text);
QString sanitizeCommandForDisplay(const QString& command,
                                  int maxLength = DefaultLogLimit);
QString sanitizeJson(const QByteArray& json, int maxLength = DefaultLogLimit);
QString sanitizeJson(const QString& json, int maxLength = DefaultLogLimit);
QString sanitizeResponseBody(const QByteArray& body,
                             int maxLength = DefaultLogLimit);

QString sanitizeUrl(const QUrl& url, int maxLength = DefaultLogLimit);
QString sanitizeUrl(const QString& url, int maxLength = DefaultLogLimit);
QString sanitizePath(const QString& path, int maxLength = DefaultLogLimit);

QString sanitizeEnvironmentValue(const QString& name, const QString& value,
                                 int maxLength = DefaultLogLimit);
QStringList formatEnvironmentForDiagnostics(
    const QProcessEnvironment& environment,
    const QStringList& allowlist = {});

QStringList sanitizeHeaders(
    const QList<QPair<QByteArray, QByteArray>>& headers,
    int maxValueLength = 1024);
QString formatNetworkRequest(const QNetworkRequest& request);

}  // namespace Privacy
