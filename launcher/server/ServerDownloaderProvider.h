// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QByteArray>
#include <QCryptographicHash>
#include <QString>
#include <QUrl>
#include <memory>

class ServerDownloader;
struct ServerProviderEndpoints;

/*! One server type's part of ServerDownloader: where it lists versions and builds, and the steps
 *  that install its server software.
 *
 *  The downloader owns everything that has to be stopped on cancel — the network request, file
 *  downloads and the loader installer process — and reports the result. A provider only decides
 *  what to fetch next and keeps its own progress through those steps. Not for use outside
 *  ServerDownloader.
 */
class ServerDownloaderProvider
{
public:
    explicit ServerDownloaderProvider(ServerDownloader &downloader) : m_downloader(downloader) {}
    virtual ~ServerDownloaderProvider() = default;
    ServerDownloaderProvider(const ServerDownloaderProvider &) = delete;
    ServerDownloaderProvider &operator=(const ServerDownloaderProvider &) = delete;

    /// Where the Minecraft versions this provider serves are listed.
    virtual QUrl versionListUrl() const = 0;
    /// Where its builds for one Minecraft version are listed; empty when it has no builds to pick.
    virtual QUrl buildListUrl(const QString &minecraftVersion) const = 0;

    /// Begins installing the requested version.
    virtual void start() = 0;
    /// The answer to the provider's last request().
    virtual void onReply(const QByteArray &data) = 0;
    /// The last request() failed. Returns false to let the downloader report the error itself.
    virtual bool onRequestFailed(int httpStatus, const QString &error);
    /// A file started with downloadFile() arrived and passed its checksum.
    virtual void onFileDownloaded() {}
    /// A last look at the finished server jar before the install is reported as done.
    virtual bool checkServerJar(QString *error);
    /// What the provider is doing now, for error messages, such as "fetching provider builds".
    virtual QString currentStep() const = 0;

protected:
    static QString tr(const char *text);

    const QString &version() const;
    const QString &requestedLoaderVersion() const;
    const QString &destinationDir() const;
    const QString &targetJarPath() const;
    const ServerProviderEndpoints &endpoints() const;
    void setResolvedLoaderVersion(const QString &loaderVersion);

    void status(const QString &message);
    void progress(int percentage);
    /// Fetches url; the answer arrives in onReply() or onRequestFailed().
    void request(const QUrl &url);
    /// Downloads the finished server jar to targetJarPath(); the install ends when it arrives.
    void downloadServerJar(const QUrl &url, const QByteArray &expectedHash,
                           QCryptographicHash::Algorithm hashAlgorithm);
    /// Downloads another file the install needs; onFileDownloaded() follows. A large server jar
    /// is fetched in ranged pieces, counts for the whole progress bar, and is cleaned up on cancel.
    void downloadFile(const QUrl &url, const QString &path, const QByteArray &expectedHash,
                      QCryptographicHash::Algorithm hashAlgorithm, bool largeServerJar);
    /// Runs a Forge-style installer in the server folder; the downloader reports the result.
    void runLoaderInstaller(const QString &loaderName, const QString &installerPath);
    /// Stops any request or download still running.
    void stopActiveWork();
    void fail(const QString &message);

private:
    ServerDownloader &m_downloader;
};

/// The provider for a server type such as "paper", or nullptr when the type is not supported.
std::unique_ptr<ServerDownloaderProvider> createServerDownloaderProvider(const QString &type,
                                                                         ServerDownloader &downloader);

std::unique_ptr<ServerDownloaderProvider> makeVanillaServerProvider(ServerDownloader &downloader);
std::unique_ptr<ServerDownloaderProvider> makePaperServerProvider(ServerDownloader &downloader);
std::unique_ptr<ServerDownloaderProvider> makePurpurServerProvider(ServerDownloader &downloader);
std::unique_ptr<ServerDownloaderProvider> makeFabricServerProvider(ServerDownloader &downloader);
std::unique_ptr<ServerDownloaderProvider> makeForgeServerProvider(ServerDownloader &downloader);
std::unique_ptr<ServerDownloaderProvider> makeNeoForgeServerProvider(ServerDownloader &downloader);
