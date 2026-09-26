#pragma once
#include "InstanceTask.h"
#include "PackHelpers.h"
#include "minecraft/MinecraftInstance.h"
#include "net/NetJob.h"

#include <memory>
#include <optional>
#include <QUrl>

namespace LegacyFTB {

class PackInstallTask : public InstanceTask {
    Q_OBJECT

   public:
    explicit PackInstallTask(QNetworkAccessManager* network, Modpack pack, QString version);
    ~PackInstallTask() override = default;

    bool canAbort() const override { return true; }
    bool abort() override;

   protected:
    //! Entry point for tasks.
    void executeTask() override;

  private:
    struct ExtractionResult {
        std::optional<QStringList> clientFiles;
        bool publishedServerPackExtracted = false;
    };

    void downloadPack();
    void onClientDownloadSucceeded();
    void onServerPackDownloadSucceeded();
    void onServerPackDownloadFailed(QString reason);
    void onServerPackDownloadAborted();
    void unzip();
    void install();

   private slots:

    void onUnzipFinished();
    void onUnzipCanceled();

   private: /* data */
    QNetworkAccessManager* m_network;
    bool m_abortable = false;
    QFuture<ExtractionResult> m_extractFuture;
    QFutureWatcher<ExtractionResult> m_extractFutureWatcher;
    NetJob::Ptr m_netJobContainer;
    QString m_archivePath;
    QString m_serverArchivePath;
    QUrl m_serverPackUrl;
    bool m_serverPackDownloaded = false;
    bool m_serverPackExtracted = false;

    std::unique_ptr<MinecraftInstance> m_instance;

    Modpack m_pack;
    QString m_version;
};

}  // namespace LegacyFTB
