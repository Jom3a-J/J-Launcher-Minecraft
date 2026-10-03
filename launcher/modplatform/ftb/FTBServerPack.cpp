// SPDX-License-Identifier: GPL-3.0-only

#include "FTBServerPack.h"

#include "BuildConfig.h"
#include "FileSystem.h"
#include "logs/Privacy.h"
#include "modplatform/ServerPackStaging.h"
#include "modplatform/ServerSupportRequestQueue.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QTimer>

#include <iterator>
#include <utility>

#ifdef Q_OS_WIN
#include <windows.h>
#include <softpub.h>
#include <wintrust.h>
#endif

namespace FTB {

namespace {
ModPlatform::ServerSupportRequestQueue<ModPlatform::ServerSupport>& serverPackProbeQueue()
{
    static ModPlatform::ServerSupportRequestQueue<ModPlatform::ServerSupport> queue(ServerPackProbeConcurrency);
    return queue;
}

/// These messages are shown by FTB::PackInstallTask, so they keep its translation context.
QString installTaskTr(const char* text)
{
    return QCoreApplication::translate("FTB::PackInstallTask", text);
}

#ifdef Q_OS_WIN
/// The name on the certificate that signed a file WinVerifyTrust has just accepted.
QString verifiedSignerName(HANDLE stateData)
{
    CRYPT_PROVIDER_DATA* provider = WTHelperProvDataFromStateData(stateData);
    CRYPT_PROVIDER_SGNR* signer = provider ? WTHelperGetProvSignerFromChain(provider, 0, FALSE, 0) : nullptr;
    if (!signer || signer->csCertChain == 0 || !signer->pasCertChain || !signer->pasCertChain[0].pCert) {
        return {};
    }
    wchar_t name[256];
    const DWORD length = CertGetNameStringW(signer->pasCertChain[0].pCert, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr,
                                            name, static_cast<DWORD>(std::size(name)));
    return length > 1 ? QString::fromWCharArray(name, static_cast<int>(length - 1)) : QString();
}
#endif
}

bool verifyTrustedWindowsExecutable(const QString& path, const QString& expectedSigner, QString* error)
{
#ifdef Q_OS_WIN
    WINTRUST_FILE_INFO fileInfo{};
    fileInfo.cbStruct = sizeof(fileInfo);
    const std::wstring nativePath =
        QDir::toNativeSeparators(path).toStdWString();
    fileInfo.pcwszFilePath = nativePath.c_str();

    WINTRUST_DATA trustData{};
    trustData.cbStruct = sizeof(trustData);
    trustData.dwUIChoice = WTD_UI_NONE;
    trustData.fdwRevocationChecks = WTD_REVOKE_NONE;
    trustData.dwUnionChoice = WTD_CHOICE_FILE;
    trustData.pFile = &fileInfo;
    trustData.dwStateAction = WTD_STATEACTION_VERIFY;

    GUID policy = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    const LONG status = WinVerifyTrust(nullptr, &policy, &trustData);
    const QString signer = status == ERROR_SUCCESS ? verifiedSignerName(trustData.hWVTStateData) : QString();
    trustData.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(nullptr, &policy, &trustData);
    if (status != ERROR_SUCCESS) {
        if (error) {
            *error = QObject::tr(
                "The official FTB server installer did not pass Windows signature "
                "verification (error 0x%1), so it was not run.")
                         .arg(static_cast<qulonglong>(
                                  static_cast<unsigned long>(status)),
                              8, 16, QLatin1Char('0'));
        }
        return false;
    }
    // A valid signature only proves someone signed it; the installer must come from FTB itself.
    if (signer != expectedSigner) {
        if (error) {
            *error = QObject::tr(
                "The FTB server installer is signed by \"%1\" instead of \"%2\", so it was not run.")
                         .arg(signer.isEmpty() ? QObject::tr("an unknown publisher") : signer, expectedSigner);
        }
        return false;
    }
    return true;
#else
    Q_UNUSED(path)
    Q_UNUSED(expectedSigner)
    if (error) {
        *error = QObject::tr(
            "Automatic FTB server-package installation is currently supported "
            "on Windows only.");
    }
    return false;
#endif
}

ModPlatform::ServerSupport serverPackSupportFromHttpStatus(int status, bool networkError)
{
    if (status == 200) return ModPlatform::ServerSupport::Official;
    if (status == 404) return ModPlatform::ServerSupport::ClientDerived;
    Q_UNUSED(networkError)
    return ModPlatform::ServerSupport::Unknown;
}

void probeDedicatedServerPack(QNetworkAccessManager* network, int packId, int versionId, QObject* owner,
                              std::function<void(ModPlatform::ServerSupport)> callback)
{
    const auto key = QStringLiteral("%1/%2").arg(packId).arg(versionId);
    using Queue = ModPlatform::ServerSupportRequestQueue<ModPlatform::ServerSupport>;
    auto starter = [network, packId, versionId](Queue::Completion complete) -> Queue::Cancel {
#ifdef Q_OS_WIN
        QNetworkRequest request(QUrl(dedicatedServerInstallerUrl(packId, versionId)));
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
        auto* reply = network->head(request);
        QObject::connect(reply, &QNetworkReply::finished, reply, [reply, complete = std::move(complete)]() mutable {
            const auto status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const auto support = serverPackSupportFromHttpStatus(status, reply->error() != QNetworkReply::NoError);
            complete(support == ModPlatform::ServerSupport::Unknown
                         ? Queue::Result{}
                         : Queue::Result{ support });
            reply->deleteLater();
        });
        return [reply] { reply->abort(); };
#else
        Q_UNUSED(network)
        Q_UNUSED(packId)
        Q_UNUSED(versionId)
        QTimer::singleShot(0, QCoreApplication::instance(), [complete = std::move(complete)]() mutable {
            complete(ModPlatform::ServerSupport::ClientDerived);
        });
        return {};
#endif
    };
    serverPackProbeQueue().request(key, owner, std::move(starter),
        [callback = std::move(callback)](Queue::Result result) mutable {
            callback(result.value_or(ModPlatform::ServerSupport::Unknown));
        });
}

void cancelDedicatedServerPackRequests(QObject* owner)
{
    serverPackProbeQueue().cancelOwner(owner);
}

QString dedicatedServerInstallerUrl(int packId, int versionId)
{
    return QString(BuildConfig.FTB_API_BASE_URL
                   + "/modpack/%1/%2/server/windows")
        .arg(packId)
        .arg(versionId);
}

bool writeServerIncludeList(const QString& stagingPath, const QVector<VersionFile>& files, QString* error)
{
    QFile includeFile(ModPlatform::ServerPackStaging::path(stagingPath, "include.txt"));
    if (!includeFile.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
        if (error) {
            *error = installTaskTr("Could not finalize the FTB server compatibility manifest.");
        }
        return false;
    }
    for (const auto& file : files) {
        if (file.clientOnly) {
            continue;
        }
        const QString relativePath = FS::PathCombine(file.path, file.name);
        const QString downloadedPath = file.serverOnly ? ModPlatform::ServerPackStaging::serverFilesPath(stagingPath, relativePath)
                                                       : FS::PathCombine(stagingPath, ".minecraft", relativePath);
        if (!QFileInfo(downloadedPath).isFile()) {
            if (file.optional) {
                continue;
            }
            if (error) {
                *error = installTaskTr("The FTB pack download is incomplete. A required server file "
                            "was not downloaded: %1")
                             .arg(QDir::fromNativeSeparators(relativePath));
            }
            return false;
        }
        includeFile.write(QDir::fromNativeSeparators(relativePath).toUtf8());
        includeFile.write("\n");
    }
    return true;
}

ServerInstallerRun::ServerInstallerRun(QString installerPath, QString stagingPath, int packId, int versionId, QObject* parent)
    : QObject(parent)
    , m_installerPath(std::move(installerPath))
    , m_stagingPath(std::move(stagingPath))
    , m_packId(packId)
    , m_versionId(versionId)
{}

ServerInstallerRun::~ServerInstallerRun() = default;

QString ServerInstallerRun::prepare()
{
    QString verificationError;
    if (!verifyTrustedWindowsExecutable(m_installerPath, ServerInstallerSigner, &verificationError)) {
        return verificationError;
    }
    if (!QDir().mkpath(ModPlatform::ServerPackStaging::serverFilesPath(m_stagingPath))) {
        return installTaskTr("Could not create the official FTB server-pack folder.");
    }
    return {};
}

void ServerInstallerRun::start()
{
    const QString serverRoot = ModPlatform::ServerPackStaging::serverFilesPath(m_stagingPath);
    m_process = std::make_unique<QProcess>(this);
    m_process->setWorkingDirectory(serverRoot);
    m_process->setProcessChannelMode(QProcess::MergedChannels);
    connect(m_process.get(), &QProcess::readyReadStandardOutput, this, [this]() {
        const QString output = QString::fromUtf8(m_process->readAllStandardOutput()).trimmed();
        if (!output.isEmpty()) {
            qDebug() << "FTB server installer:" << Privacy::sanitizeText(output);
        }
    });
    connect(m_process.get(), &QProcess::errorOccurred, this, [this](QProcess::ProcessError processError) {
        if (processError == QProcess::FailedToStart) {
            emit failed(installTaskTr("The verified FTB server installer could not be started."));
        }
    });
    connect(m_process.get(), QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [this, serverRoot](int exitCode, QProcess::ExitStatus status) {
                const QByteArray output = m_process->readAll();
                QProcess* completedProcess = m_process.release();
                completedProcess->deleteLater();
                if (status != QProcess::NormalExit || exitCode != 0) {
                    qWarning() << "FTB server installer failed:" << Privacy::sanitizeText(QString::fromUtf8(output));
                    emit failed(installTaskTr("The official FTB server installer could not prepare the server files "
                                   "(exit code %1).")
                                    .arg(exitCode));
                    return;
                }
                QDirIterator installedFiles(serverRoot, QDir::Files | QDir::Hidden | QDir::System, QDirIterator::Subdirectories);
                if (!installedFiles.hasNext()
                    || !ModPlatform::ServerPackStaging::writeProviderMarker(
                        ModPlatform::ServerPackStaging::path(m_stagingPath, "published-server-pack.txt"), "ftb")) {
                    emit failed(installTaskTr("The FTB server installer finished without producing usable "
                                   "server files."));
                    return;
                }
                QFile::remove(m_installerPath);
                emit succeeded();
            });
    m_process->start(m_installerPath, { QStringLiteral("-pack"), QString::number(m_packId), QStringLiteral("-version"),
                                        QString::number(m_versionId), QStringLiteral("-dir"), serverRoot,
                                        QStringLiteral("-auto"), QStringLiteral("-force"), QStringLiteral("-just-files"),
                                        QStringLiteral("-validate"), QStringLiteral("-no-colours") });
}

void ServerInstallerRun::stop()
{
    if (m_process && m_process->state() != QProcess::NotRunning) {
        disconnect(m_process.get(), nullptr, this, nullptr);
        m_process->kill();
    }
}

}  // namespace FTB
