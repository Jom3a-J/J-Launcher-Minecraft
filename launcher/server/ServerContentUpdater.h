// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QCryptographicHash>
#include <QObject>
#include <QString>
#include <QUrl>
#include <memory>

class QNetworkAccessManager;
class QNetworkReply;
class QSaveFile;
class ServerInstance;

enum class ServerContentUpdateFailure {
    None,
    InvalidRequest,
    Network,
    Cancelled,
    Write,
    HashMismatch,
    Commit,
    Cleanup
};

struct ServerContentUpdateRequest {
    QUrl url;
    QString installedPath;
    QString replacementName;
    QCryptographicHash::Algorithm hashAlgorithm = QCryptographicHash::Sha512;
    QByteArray expectedHash;
};

struct ServerContentUpdateResult {
    bool success = false;
    ServerContentUpdateFailure failure = ServerContentUpdateFailure::None;
    QString message;
    QString installedPath;
    QString destinationPath;
    QUrl finalUrl;
    bool workingFilePreserved = false;
    bool retryAvailable = false;
};

struct ServerContentUpdateCandidate {
    bool available = false;
    bool upToDate = false;
    QString versionId;
    QString versionNumber;
    QString providerFileId;
    QString fileName;
    QUrl url;
    QCryptographicHash::Algorithm hashAlgorithm = QCryptographicHash::Sha512;
    QByteArray expectedHash;
};

Q_DECLARE_METATYPE(ServerContentUpdateResult)

class ServerContentUpdater final : public QObject
{
    Q_OBJECT

public:
    explicit ServerContentUpdater(QObject* parent = nullptr);
    explicit ServerContentUpdater(QNetworkAccessManager* network, QObject* parent = nullptr);
    ~ServerContentUpdater() override;

    bool start(const std::shared_ptr<ServerInstance>& server,
               const ServerContentUpdateRequest& request, QString* error = nullptr);
    bool cancel();
    bool isRunning() const;
    static ServerContentUpdateCandidate parseModrinthVersionResponse(
        const QByteArray& data, const QString& installedName, QString* error = nullptr);
    static ServerContentUpdateCandidate parseCurseForgeFilesResponse(
        const QByteArray& data, const QString& installedName, const QString& loader,
        QString* error = nullptr);

signals:
    void progress(qint64 received, qint64 total);
    void finished(const ServerContentUpdateResult& result);

private:
    bool validate(const std::shared_ptr<ServerInstance>& server,
                  const ServerContentUpdateRequest& request, QString* error) const;
    bool append(const QByteArray& data);
    void finishReply();
    void complete(ServerContentUpdateFailure failure, const QString& message,
                  bool retryAvailable);
    void reset();

    QNetworkAccessManager* m_network = nullptr;
    QNetworkReply* m_reply = nullptr;
    std::unique_ptr<QSaveFile> m_output;
    std::unique_ptr<QCryptographicHash> m_hash;
    ServerContentUpdateRequest m_request;
    QString m_destinationPath;
    bool m_cancelled = false;
    bool m_writeFailed = false;
};
