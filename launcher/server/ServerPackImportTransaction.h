// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QString>
#include <memory>

class ServerPackImportTransaction final
{
  public:
    explicit ServerPackImportTransaction(QString serverRoot);
    ~ServerPackImportTransaction();

    ServerPackImportTransaction(const ServerPackImportTransaction&) = delete;
    ServerPackImportTransaction& operator=(const ServerPackImportTransaction&) = delete;

    bool stage(const QString& archivePath, QString* error);
    bool publish(QString* error);

  private:
    class Private;
    std::unique_ptr<Private> m_private;
};
