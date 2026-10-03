// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "modplatform/ServerSupport.h"
#include "modplatform/ftb/FTBPackManifest.h"

#include <QObject>
#include <QString>
#include <QVector>

#include <functional>
#include <memory>

class QNetworkAccessManager;
class QProcess;

/*! J Launcher's server-pack support for FTB modpack installs: finding out whether FTB publishes an
 *  official server version of a pack, and running FTB's signed installer to build it. */
namespace FTB {

inline constexpr int ServerPackProbeConcurrency = 1;
/// The publisher on the code-signing certificate of FTB's official server installer.
inline constexpr auto ServerInstallerSigner = "Feed The Beast Ltd";

/// True when Windows trusts the file's signature and it was signed by expectedSigner.
bool verifyTrustedWindowsExecutable(const QString& path, const QString& expectedSigner, QString* error);
ModPlatform::ServerSupport serverPackSupportFromHttpStatus(int status, bool networkError);
void probeDedicatedServerPack(QNetworkAccessManager* network, int packId, int versionId, QObject* owner,
                              std::function<void(ModPlatform::ServerSupport)> callback);
void cancelDedicatedServerPackRequests(QObject* owner);
/// Where FTB's official server installer for one pack version downloads from.
QString dedicatedServerInstallerUrl(int packId, int versionId);

/*! Rewrites include.txt with every file the server needs that actually arrived. Fails when a
 *  required one is missing; a missing optional file is left out. */
bool writeServerIncludeList(const QString& stagingPath, const QVector<VersionFile>& files, QString* error);

/*! Runs FTB's official server installer to fill the server staging folder, after checking that
 *  FTB signed it. */
class ServerInstallerRun : public QObject {
    Q_OBJECT

   public:
    ServerInstallerRun(QString installerPath, QString stagingPath, int packId, int versionId, QObject* parent = nullptr);
    ~ServerInstallerRun() override;

    /// Checks the installer's signature and prepares its folder. Returns an error message, or empty.
    QString prepare();
    /// Starts the installer; succeeded() or failed() follows.
    void start();
    /// Stops a running installer without reporting anything.
    void stop();

   signals:
    void succeeded();
    void failed(const QString& reason);

   private:
    QString m_installerPath;
    QString m_stagingPath;
    int m_packId;
    int m_versionId;
    std::unique_ptr<QProcess> m_process;
};

}  // namespace FTB
