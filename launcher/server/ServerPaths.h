// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QString>

/*! Checks for paths that come from outside the launcher (modpack files, provider metadata,
 *  server-pack archives) before they are joined to a server or instance folder.
 */
namespace ServerPaths {

/// Trimmed, cleaned, with forward slashes and no leading "./".
QString normalizedRelativePath(QString path);

/// After normalizing: not empty, not absolute, and not climbing out with "..".
bool isSafeRelativePath(const QString &path);

/*! Whether every component of a '/'-separated path is a usable Windows file name: no control
 *  or reserved characters (<>:"|?*), no trailing dot or space, not "." and not a device name
 *  such as CON or LPT1. Empty components are ignored. Reports the first bad component.
 */
bool hasValidWindowsNames(const QString &path, QString *unsafeComponent = nullptr);

}  // namespace ServerPaths
