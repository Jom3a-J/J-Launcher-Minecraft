// SPDX-License-Identifier: GPL-3.0-only

#include "ModrinthServerFiles.h"

#include "FileSystem.h"
#include "Json.h"
#include "net/ApiRequest.h"
#include "net/ChecksumValidator.h"
#include "net/NetJob.h"
#include "server/ServerPaths.h"

#include <QCoreApplication>
#include <QDir>
#include <QJsonArray>
#include <QJsonObject>
#include <QRegularExpression>
#include <QUrl>

#include <functional>
#include <memory>

namespace Modrinth {

namespace {
/// These messages are shown by ModrinthCreationTask, so they keep its translation context.
QString tr(const char* text)
{
    return QCoreApplication::translate("ModrinthCreationTask", text);
}
}  // namespace

QString addServerOnlyDownloads(const QString& indexPath, const QString& cacheRoot, NetJob* downloads)
{
    QJsonDocument document;
    try {
        document = Json::requireDocument(indexPath, "modrinth.index.json");
    } catch (const JSONValidationError& e) {
        return tr("Could not read the Modrinth server file manifest:\n%1").arg(e.cause());
    }

    if (!downloads || !document.isObject()) {
        return tr("The Modrinth server file manifest is malformed.");
    }

    const QJsonArray files = document.object().value(QStringLiteral("files")).toArray();
    for (const QJsonValue& value : files) {
        if (!value.isObject()) {
            return tr("The Modrinth server file manifest contains an invalid entry.");
        }
        const QJsonObject file = value.toObject();
        const QJsonValue envValue = file.value(QStringLiteral("env"));
        const QString filePath = file.value(QStringLiteral("path")).toString();
        if (!envValue.isUndefined() && !envValue.isObject()) {
            return tr("The Modrinth environment metadata for %1 is malformed.").arg(filePath);
        }
        const QJsonObject environment = envValue.toObject();
        QString clientSupport = QStringLiteral("required");
        QString serverSupport = QStringLiteral("required");
        QString error;
        const auto readSupport = [&environment, &filePath, &error](const QString& key, QString& support) {
            if (!environment.contains(key)) {
                return true;
            }
            const QJsonValue value = environment.value(key);
            if (!value.isString()) {
                error = tr("The Modrinth environment value for %1 is malformed.").arg(filePath);
                return false;
            }
            support = value.toString();
            if (support != QStringLiteral("required") && support != QStringLiteral("optional")
                && support != QStringLiteral("unsupported")) {
                error = tr("The Modrinth environment value for %1 is unsupported.").arg(filePath);
                return false;
            }
            return true;
        };
        if (!readSupport(QStringLiteral("client"), clientSupport) || !readSupport(QStringLiteral("server"), serverSupport)) {
            return error;
        }
        if (clientSupport == QStringLiteral("required")) {
            continue;
        }
        if (serverSupport == QStringLiteral("unsupported")) {
            continue;
        }

        const QString relativePath = ServerPaths::normalizedRelativePath(QString(filePath).replace('\\', '/'));
        if (!ServerPaths::isSafeRelativePath(relativePath) || !ServerPaths::hasValidWindowsNames(relativePath)) {
            return tr("The Modrinth server manifest contains an unsafe path: %1").arg(relativePath);
        }

        const QJsonValue downloadsValue = file.value(QStringLiteral("downloads"));
        if (!downloadsValue.isArray()) {
            return tr("The Modrinth server file %1 has invalid download metadata.").arg(relativePath);
        }
        const QJsonArray downloadValues = downloadsValue.toArray();
        QList<QUrl> downloadUrls;
        for (const QJsonValue& downloadValue : downloadValues) {
            if (!downloadValue.isString()) {
                return tr("The Modrinth server file %1 has an invalid download URL.").arg(relativePath);
            }
            const QString urlText = downloadValue.toString().trimmed();
            const QUrl url(urlText);
            if (urlText.isEmpty() || !url.isValid() || url.scheme().compare(QStringLiteral("https"), Qt::CaseInsensitive) != 0
                || url.host().isEmpty()) {
                return tr("The Modrinth server file %1 has an invalid HTTPS download URL.").arg(relativePath);
            }
            downloadUrls.append(url);
        }
        const QString sha512 = file.value(QStringLiteral("hashes")).toObject().value(QStringLiteral("sha512")).toString().trimmed();
        if (downloadUrls.isEmpty() || sha512.size() != 128
            || !QRegularExpression(QStringLiteral("^[0-9a-fA-F]{128}$")).match(sha512).hasMatch()) {
            return tr("The required Modrinth server file %1 has incomplete download or checksum metadata.").arg(relativePath);
        }

        const QString destination = FS::PathCombine(cacheRoot, relativePath);
        FS::ensureFilePathExists(destination);
        const QByteArray hash = QByteArray::fromHex(sha512.toLatin1());
        auto enqueueDownload = [downloads, destination, hash, urls = std::move(downloadUrls)]() mutable {
            struct DownloadFallbackState {
                QList<QUrl> remaining;
                std::function<void()> enqueue;
            };

            auto state = std::make_shared<DownloadFallbackState>();
            state->remaining = std::move(urls);
            const std::weak_ptr<DownloadFallbackState> weakState = state;
            state->enqueue = [downloads, destination, hash, weakState]() {
                auto state = weakState.lock();
                if (!state || state->remaining.isEmpty()) {
                    return;
                }

                auto download = Net::ApiRequest::makeFile(state->remaining.takeFirst(), destination);
                download->addValidator(new Net::ChecksumValidator(QCryptographicHash::Sha512, hash));
                if (!state->remaining.isEmpty()) {
                    const auto previous = download.toWeakRef();
                    QObject::connect(download.get(), &Task::failed, download.get(), [state, previous] {
                        state->enqueue();
                        if (auto shared = previous.lock()) {
                            shared->succeeded();
                        }
                    });
                }
                downloads->addNetAction(download);
            };
            state->enqueue();
        };
        enqueueDownload();
    }
    return {};
}

}  // namespace Modrinth
