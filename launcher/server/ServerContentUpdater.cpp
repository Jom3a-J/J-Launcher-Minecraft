// SPDX-License-Identifier: GPL-3.0-only

#include "ServerContentUpdater.h"

#include "ServerInstance.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QObject>
#include <QSaveFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>

namespace {
int expectedDigestSize(QCryptographicHash::Algorithm algorithm)
{
    switch (algorithm) {
        case QCryptographicHash::Sha1: return 20;
        case QCryptographicHash::Sha256: return 32;
        case QCryptographicHash::Sha512: return 64;
        default: return 0;
    }
}

QNetworkRequest updateRequest(const QUrl& url)
{
    QNetworkRequest request(url);
    request.setRawHeader("User-Agent", "JLauncher/1.0");
    request.setRawHeader("Accept", "application/java-archive, application/octet-stream");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    return request;
}
}

ServerContentUpdateCandidate ServerContentUpdater::parseModrinthVersionResponse(
    const QByteArray& data, const QString& installedName, QString* error)
{
    ServerContentUpdateCandidate candidate;
    if (error) error->clear();
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(data, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isArray()) {
        if (error) *error = QObject::tr("The provider returned invalid update metadata.");
        return candidate;
    }
    const QJsonArray versions = document.array();
    if (versions.isEmpty()) {
        return candidate;
    }
    const QJsonObject version = versions.first().toObject();
    if (version.isEmpty()) {
        if (error) *error = QObject::tr("The provider returned an invalid update version.");
        return candidate;
    }
    const QJsonArray files = version.value("files").toArray();
    QJsonObject file;
    for (const QJsonValue& value : files) {
        if (value.toObject().value("primary").toBool()) {
            file = value.toObject();
            break;
        }
    }
    if (file.isEmpty() && !files.isEmpty()) {
        file = files.first().toObject();
    }
    if (file.isEmpty()) {
        if (error) *error = QObject::tr("The provider did not include a downloadable update file.");
        return candidate;
    }

    const QString providerName = file.value("filename").toString().trimmed();
    candidate.fileName = QFileInfo(providerName).fileName();
    candidate.url = QUrl(file.value("url").toString());
    if (providerName.isEmpty() || candidate.fileName != providerName
        || !candidate.fileName.endsWith(QStringLiteral(".jar"), Qt::CaseInsensitive)
        || !candidate.url.isValid()
        || (candidate.url.scheme() != QStringLiteral("https")
            && candidate.url.scheme() != QStringLiteral("http"))) {
        if (error) *error = QObject::tr("The provider returned unsafe or incomplete update file metadata.");
        return {};
    }

    const QJsonObject hashes = file.value("hashes").toObject();
    QString hashText;
    if (!hashes.value("sha512").toString().isEmpty()) {
        candidate.hashAlgorithm = QCryptographicHash::Sha512;
        hashText = hashes.value("sha512").toString();
    } else if (!hashes.value("sha256").toString().isEmpty()) {
        candidate.hashAlgorithm = QCryptographicHash::Sha256;
        hashText = hashes.value("sha256").toString();
    } else if (!hashes.value("sha1").toString().isEmpty()) {
        candidate.hashAlgorithm = QCryptographicHash::Sha1;
        hashText = hashes.value("sha1").toString();
    }
    candidate.expectedHash = QByteArray::fromHex(hashText.toLatin1());
    if (hashText.size() != expectedDigestSize(candidate.hashAlgorithm) * 2
        || candidate.expectedHash.size() != expectedDigestSize(candidate.hashAlgorithm)) {
        if (error) *error = QObject::tr("The provider did not supply a valid supported update hash.");
        return {};
    }

    candidate.versionId = version.value("id").toString();
    candidate.versionNumber = version.value("version_number").toString();
    candidate.upToDate = candidate.fileName == QFileInfo(installedName).fileName();
    candidate.available = !candidate.upToDate;
    return candidate;
}

ServerContentUpdater::ServerContentUpdater(QObject* parent)
    : QObject(parent)
    , m_network(new QNetworkAccessManager(this))
{
    qRegisterMetaType<ServerContentUpdateResult>();
}

ServerContentUpdater::ServerContentUpdater(QNetworkAccessManager* network, QObject* parent)
    : QObject(parent)
    , m_network(network)
{
    qRegisterMetaType<ServerContentUpdateResult>();
}

ServerContentUpdater::~ServerContentUpdater()
{
    if (m_reply) {
        m_reply->disconnect(this);
        m_reply->abort();
    }
    if (m_output) {
        m_output->cancelWriting();
    }
}

bool ServerContentUpdater::isRunning() const
{
    return m_reply != nullptr;
}

bool ServerContentUpdater::validate(const std::shared_ptr<ServerInstance>& server,
                                    const ServerContentUpdateRequest& request,
                                    QString* error) const
{
    const auto fail = [error](const QString& message) {
        if (error) *error = message;
        return false;
    };
    if (!server || (server->status() != ServerStatus::Stopped
                    && server->status() != ServerStatus::Error)) {
        return fail(QObject::tr("Stop the server before updating installed content."));
    }
    if (!request.url.isValid()
        || (request.url.scheme() != QStringLiteral("https")
            && request.url.scheme() != QStringLiteral("http"))) {
        return fail(QObject::tr("The update download URL is invalid."));
    }
    const QFileInfo installed(request.installedPath);
    if (!installed.isFile()) {
        return fail(QObject::tr("The installed file to update could not be found."));
    }
    const QString replacement = request.replacementName.trimmed();
    if (replacement.isEmpty() || replacement != QFileInfo(replacement).fileName()
        || !replacement.endsWith(QStringLiteral(".jar"), Qt::CaseInsensitive)) {
        return fail(QObject::tr("The provider returned an unsafe replacement filename."));
    }
    const int digestSize = expectedDigestSize(request.hashAlgorithm);
    if (digestSize == 0 || request.expectedHash.size() != digestSize) {
        return fail(QObject::tr("The provider did not supply a valid supported file hash."));
    }
    const QString destination = installed.dir().filePath(replacement);
    if (QFileInfo(destination).absoluteFilePath() != installed.absoluteFilePath()
        && QFileInfo::exists(destination)) {
        return fail(QObject::tr("A different installed file already uses the replacement filename."));
    }
    return true;
}

bool ServerContentUpdater::start(const std::shared_ptr<ServerInstance>& server,
                                 const ServerContentUpdateRequest& request,
                                 QString* error)
{
    if (error) error->clear();
    if (isRunning()) {
        if (error) *error = tr("A content update is already running.");
        return false;
    }
    if (!m_network) {
        if (error) *error = tr("The update network service is unavailable.");
        return false;
    }
    if (!validate(server, request, error)) {
        return false;
    }

    m_request = request;
    m_destinationPath = QFileInfo(request.installedPath).dir().filePath(
        request.replacementName.trimmed());
    m_output = std::make_unique<QSaveFile>(m_destinationPath);
    if (!m_output->open(QIODevice::WriteOnly)) {
        complete(ServerContentUpdateFailure::Write,
                 tr("Could not prepare the replacement file. The installed file was kept."),
                 true);
        return true;
    }
    m_hash = std::make_unique<QCryptographicHash>(request.hashAlgorithm);
    m_cancelled = false;
    m_writeFailed = false;
    m_reply = m_network->get(updateRequest(request.url));
    connect(m_reply, &QNetworkReply::readyRead, this, [this]() {
        if (m_reply && !append(m_reply->readAll())) {
            m_reply->abort();
        }
    });
    connect(m_reply, &QNetworkReply::downloadProgress, this,
            &ServerContentUpdater::progress);
    connect(m_reply, &QNetworkReply::finished, this, &ServerContentUpdater::finishReply);
    return true;
}

bool ServerContentUpdater::cancel()
{
    if (!m_reply) {
        return false;
    }
    m_cancelled = true;
    m_reply->abort();
    return true;
}

bool ServerContentUpdater::append(const QByteArray& data)
{
    if (data.isEmpty()) {
        return true;
    }
    if (!m_output || m_output->write(data) != data.size()) {
        m_writeFailed = true;
        return false;
    }
    m_hash->addData(data);
    return true;
}

void ServerContentUpdater::finishReply()
{
    if (!m_reply) {
        return;
    }
    QNetworkReply* reply = m_reply;
    if (!m_cancelled) {
        append(reply->readAll());
    }
    const QNetworkReply::NetworkError networkError = reply->error();
    const QString networkMessage = reply->errorString();
    const QUrl finalUrl = reply->url();
    reply->deleteLater();
    m_reply = nullptr;

    if (m_cancelled) {
        complete(ServerContentUpdateFailure::Cancelled,
                 tr("Update cancelled. The installed file was kept and the update can be retried."),
                 true);
        return;
    }
    if (m_writeFailed) {
        complete(ServerContentUpdateFailure::Write,
                 tr("The replacement could not be written. The installed file was kept."), true);
        return;
    }
    if (networkError != QNetworkReply::NoError) {
        complete(ServerContentUpdateFailure::Network,
                 tr("Download failed: %1. The installed file was kept and the update can be retried.")
                     .arg(networkMessage),
                 true);
        return;
    }
    if (!m_hash || m_hash->result() != m_request.expectedHash) {
        complete(ServerContentUpdateFailure::HashMismatch,
                 tr("Downloaded file verification failed. The installed file was kept and the update can be retried."),
                 true);
        return;
    }
    if (!m_output || !m_output->commit()) {
        complete(ServerContentUpdateFailure::Commit,
                 tr("The verified replacement could not be installed. The installed file was kept."),
                 true);
        return;
    }

    if (QFileInfo(m_request.installedPath).absoluteFilePath()
            != QFileInfo(m_destinationPath).absoluteFilePath()
        && !QFile::remove(m_request.installedPath)) {
        const bool removedReplacement = QFile::remove(m_destinationPath);
        complete(ServerContentUpdateFailure::Cleanup,
                 removedReplacement
                     ? tr("The old file could not be removed, so the replacement was rolled back.")
                     : tr("The old file could not be removed. Both files remain; remove one before starting the server."),
                 removedReplacement);
        return;
    }

    ServerContentUpdateResult result;
    result.success = true;
    result.message = tr("Updated successfully. Restart the server to load the replacement.");
    result.installedPath = m_request.installedPath;
    result.destinationPath = m_destinationPath;
    result.finalUrl = finalUrl;
    result.workingFilePreserved = true;
    emit finished(result);
    reset();
}

void ServerContentUpdater::complete(ServerContentUpdateFailure failure,
                                    const QString& message, bool retryAvailable)
{
    if (m_output) {
        m_output->cancelWriting();
    }
    ServerContentUpdateResult result;
    result.failure = failure;
    result.message = message;
    result.installedPath = m_request.installedPath;
    result.destinationPath = m_destinationPath;
    result.workingFilePreserved = QFileInfo::exists(m_request.installedPath);
    result.retryAvailable = retryAvailable;
    emit finished(result);
    reset();
}

void ServerContentUpdater::reset()
{
    m_reply = nullptr;
    m_output.reset();
    m_hash.reset();
    m_request = {};
    m_destinationPath.clear();
    m_cancelled = false;
    m_writeFailed = false;
}
