// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QString>
#include <QtGlobal>

/// A mod or plugin file as the Server Manager lists it.
struct ServerContentFileDetails {
    /// Readable name: the file name without its version, extension and separators.
    QString name;
    /// The version part of the file name, or empty when it has none.
    QString version;
    /// False for files renamed to ".disabled".
    bool enabled = true;
};

/// Small file helpers shared by the server code and the Server Manager page.
namespace ServerFiles {

/// Total size of the files under a folder, including hidden ones.
qint64 directorySize(const QString &directoryPath);

/// "512 B", "1.5 KB", "3.2 MB" or "1.1 GB".
QString formatByteSize(qint64 bytes);

/// Splits a content file name such as "sodium-fabric-0.5.8.jar.disabled".
ServerContentFileDetails describeContentFile(const QString &fileName);

}  // namespace ServerFiles
