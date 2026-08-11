// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QString>

#include <cstddef>
#include <cstdint>

namespace Translations {

struct Metadata {
    QString fileName;
    std::size_t fileSize = 0;
    QString fileSha1;
    unsigned translated = 0;
    unsigned untranslated = 0;
    unsigned fuzzy = 0;
    unsigned total = 0;
    std::uint8_t localFileType = 0;

    bool operator==(const Metadata& other) const
    {
        return fileName == other.fileName && fileSize == other.fileSize && fileSha1 == other.fileSha1 &&
               translated == other.translated && untranslated == other.untranslated && fuzzy == other.fuzzy &&
               total == other.total && localFileType == other.localFileType;
    }
};

}  // namespace Translations
