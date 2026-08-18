// SPDX-License-Identifier: GPL-3.0-only
/*
 *  Prism Launcher - Minecraft Launcher
 *  Copyright (c) 2022 flowln <flowlnlnln@gmail.com>
 *  Copyright (C) 2022 Sefa Eyeoglu <contact@scrumplex.net>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, version 3.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * This file incorporates work covered by the following copyright and
 * permission notice:
 *
 *      Copyright 2013-2021 MultiMC Contributors
 *
 *      Licensed under the Apache License, Version 2.0 (the "License");
 *      you may not use this file except in compliance with the License.
 *      You may obtain a copy of the License at
 *
 *          http://www.apache.org/licenses/LICENSE-2.0
 *
 *      Unless required by applicable law or agreed to in writing, software
 *      distributed under the License is distributed on an "AS IS" BASIS,
 *      WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *      See the License for the specific language governing permissions and
 *      limitations under the License.
 */

#include "TranslationsModel.h"

#include <QDebug>
#include <QTimer>
#include <algorithm>
#include <memory>
#include <utility>

#include "BuildConfig.h"
#include "FileSystem.h"
#include "Json.h"
#include "net/ChecksumValidator.h"
#include "net/NetJob.h"

#include "POTranslator.h"
#include "TranslationMetadata.h"

#include "Application.h"
#include "settings/SettingsObject.h"

static constexpr QLatin1String g_defaultLangCode("en_US");

namespace {
enum class FileType : std::uint8_t { None, Qm, Po };

QString getSystemLocaleName()
{
    return QLocale::system().name();
}
QString getSystemLanguage()
{
    return getSystemLocaleName().split('_').front();
}

QString fileSha1(const QString& path)
{
    try {
        return QString::fromLatin1(QCryptographicHash::hash(FS::read(path), QCryptographicHash::Sha1).toHex());
    } catch ([[maybe_unused]] const Exception& e) {
        return {};
    }
}
}  // namespace

struct Language {
    Language() : updated(true) {}

    explicit Language(QString _key) : key(std::move(_key)), updated(key == g_defaultLangCode) { locale = QLocale(key); }

    QString languageName() const
    {
        QString result;
        if (key == "ja_KANJI") {
            result = locale.nativeLanguageName() + u8" (漢字)";
        } else if (key == "es_UY") {
            result = u8"Español de Latinoamérica";
        } else if (key == "en_NZ") {
            result = u8"New Zealand English";  // No idea why qt translates this to just english and not to New Zealand English
        } else if (key == "en@pirate") {
            result = u8"Tongue of the High Seas";
        } else if (key == "en@uwu") {
            result = u8"Cute Engwish";
        } else if (key == "tok") {
            result = u8"toki pona";
        } else if (key == "zh_Hant_HK") {
            result = u8"繁體中文 (香港)";
        } else if (key == "nan") {
            result = u8"閩南語";  // Using traditional Chinese script. Not sure if we should use simplified instead?
        } else {
            result = locale.nativeLanguageName();
        }

        if (result.isEmpty()) {
            result = key;
        }
        return result;
    }

    float percentTranslated() const
    {
        if (total == 0) {
            return 100.0F;
        }
        return 100.0F * static_cast<float>(translated) / static_cast<float>(total);
    }

    void setTranslationStats(const unsigned _translated, const unsigned _untranslated, const unsigned _fuzzy)
    {
        translated = _translated;
        untranslated = _untranslated;
        fuzzy = _fuzzy;
        total = translated + untranslated + fuzzy;
    }

    bool isOfSameNameAs(const Language& other) const { return key == other.key; }

    bool isIdenticalTo(const Language& other) const
    {
        return key == other.key && updated == other.updated && metadata() == other.metadata();
    }

    Translations::Metadata metadata() const
    {
        return { fileName, fileSize, fileSha1, translated, untranslated, fuzzy, total, static_cast<std::uint8_t>(localFileType) };
    }

    Language& apply(const Language& other)
    {
        if (!isOfSameNameAs(other)) {
            return *this;
        }
        fileName = other.fileName;
        fileSize = other.fileSize;
        fileSha1 = other.fileSha1;
        translated = other.translated;
        untranslated = other.untranslated;
        fuzzy = other.fuzzy;
        total = other.total;
        localFileType = other.localFileType;
        updated = other.updated;
        return *this;
    }

    QString key;
    QLocale locale;
    bool updated;

    QString fileName = QString();
    std::size_t fileSize = 0;
    QString fileSha1 = QString();

    unsigned translated = 0;
    unsigned untranslated = 0;
    unsigned fuzzy = 0;
    unsigned total = 0;

    FileType localFileType = FileType::None;
};

struct TranslationsModel::Private {
    QDir m_dir;

    // initial state is just english
    QList<Language> m_languages = { Language(g_defaultLangCode) };

    QString m_selectedLanguage = g_defaultLangCode;
    std::unique_ptr<QTranslator> m_qtTranslator;
    std::unique_ptr<QTranslator> m_appTranslator;
    QString m_appTranslationSignature;
    bool m_languageSelectionApplied = false;
    bool m_lastLanguageSelectionSuccessful = false;

    Net::Download* m_indexTask = nullptr;
    QString m_downloadingTranslation;
    NetJob::Ptr m_downloadJob;
    NetJob::Ptr m_indexJob;
    QString m_nextDownload;

    QFileSystemWatcher* watcher = nullptr;
    QTimer* translationReloadTimer = nullptr;

    bool m_noLanguageSet = false;
    bool m_useSystemLocale = false;
};

TranslationsModel::TranslationsModel(const QString& path, QString selectedLanguage, bool useSystemLocale, QObject* parent)
    : QAbstractListModel(parent)
{
    d = std::make_unique<Private>();
    d->m_dir.setPath(path);
    d->m_selectedLanguage = std::move(selectedLanguage);
    d->m_useSystemLocale = useSystemLocale;
    FS::ensureFolderPathExists(path);
    reloadLocalFiles();

    d->watcher = new QFileSystemWatcher(this);
    connect(d->watcher, &QFileSystemWatcher::directoryChanged, this, &TranslationsModel::translationDirChanged);
    d->watcher->addPath(d->m_dir.canonicalPath());

    d->translationReloadTimer = new QTimer(this);
    d->translationReloadTimer->setSingleShot(true);
    d->translationReloadTimer->setInterval(200);
    connect(d->translationReloadTimer, &QTimer::timeout, this, &TranslationsModel::applyPendingTranslationChange);

    selectLanguage(d->m_selectedLanguage);
}

TranslationsModel::~TranslationsModel() = default;

void TranslationsModel::translationDirChanged(const QString& path)
{
    qDebug() << "Dir changed:" << path;
    d->translationReloadTimer->start();
}

void TranslationsModel::applyPendingTranslationChange()
{
    if (!d->m_noLanguageSet) {
        reloadLocalFiles();
    }
    selectLanguage(selectedLanguage());
}

void TranslationsModel::indexReceived()
{
    qDebug() << "Got translations index!";
    d->m_indexJob.reset();
    reloadLocalFiles();

    if (d->m_noLanguageSet) {
        auto language = getSystemLocaleName();
        if (!findLanguageAsOptional(language).has_value()) {
            language = getSystemLanguage();
        }
        selectLanguage(language);
        APPLICATION->settings()->set("Language", selectedLanguage());
        d->m_noLanguageSet = false;
    }

    if (selectedLanguage() != g_defaultLangCode) {
        updateLanguage(selectedLanguage());
    }
}

namespace {
void readIndex(const QString& path, QMap<QString, Language>& languages)
{
    QByteArray data;
    try {
        data = FS::read(path);
    } catch ([[maybe_unused]] const Exception& e) {
        qCritical() << "Translations Download Failed: index file not readable";
        return;
    }

    try {
        auto toplevelDoc = Json::requireDocument(data);
        auto doc = Json::requireObject(toplevelDoc);
        auto fileType = Json::requireString(doc, "file_type");
        if (fileType != "MMC-TRANSLATION-INDEX") {
            qCritical() << "Translations Download Failed: index file is of unknown file type" << fileType;
            return;
        }
        auto version = Json::requireInteger(doc, "version");
        if (version > 2) {
            qCritical() << "Translations Download Failed: index file is of unknown format version" << fileType;
            return;
        }
        auto langObjs = Json::requireObject(doc, "languages");
        for (auto iter = langObjs.begin(); iter != langObjs.end(); ++iter) {
            Language lang(iter.key());

            auto langObj = Json::requireObject(iter.value());
            lang.setTranslationStats(langObj["translated"].toInt(), langObj["untranslated"].toInt(), langObj["fuzzy"].toInt());
            lang.fileName = Json::requireString(langObj, "file");
            lang.fileSha1 = Json::requireString(langObj, "sha1");
            lang.fileSize = Json::requireInteger(langObj, "size");

            languages.insert(lang.key, lang);
        }
    } catch ([[maybe_unused]] Json::JsonException& e) {
        qCritical() << "Translations Download Failed: index file could not be parsed as json";
    }
}
}  // namespace

void TranslationsModel::reloadLocalFiles()
{
    QMap<QString, Language> languages = { { g_defaultLangCode, Language(g_defaultLangCode) } };

    const auto indexPath = d->m_dir.absoluteFilePath("index_v2.json");
    if (QFileInfo::exists(indexPath)) {
        readIndex(indexPath, languages);
    }
    auto entries = d->m_dir.entryInfoList({ "mmc_*.qm", "*.po" }, QDir::Files | QDir::NoDotAndDotDot);
    for (auto& entry : entries) {
        auto completeSuffix = entry.completeSuffix();
        QString langCode;
        FileType fileType = FileType::None;
        if (completeSuffix == "qm") {
            langCode = entry.baseName().remove(0, 4);
            fileType = FileType::Qm;
        } else if (completeSuffix == "po") {
            langCode = entry.baseName();
            fileType = FileType::Po;
        } else {
            continue;
        }

        auto langIter = languages.find(langCode);
        if (langIter != languages.end()) {
            auto& language = *langIter;
            // TODO: use std::to_underlying in C++23
            if (static_cast<int>(fileType) > static_cast<int>(language.localFileType)) {
                language.localFileType = fileType;
            }
            if (fileType == FileType::Po) {
                // A local PO file intentionally overrides the downloaded catalog.
                language.updated = true;
            } else if (language.localFileType != FileType::Po
                       && entry.size() == static_cast<qint64>(language.fileSize)) {
                language.updated =
                    fileSha1(entry.absoluteFilePath()).compare(language.fileSha1, Qt::CaseInsensitive) == 0;
            }
        } else {
            // Local catalogs remain usable without a downloaded index. This is
            // what lets first startup stay offline and still retain cached or
            // bundled translations.
            Language localFound(langCode);
            localFound.localFileType = fileType;
            localFound.updated = true;
            languages.insert(langCode, localFound);
        }
    }

    const auto comp = [systemLocale = getSystemLocaleName(), systemLanguage = getSystemLanguage()](const Language& a, const Language& b) {
        if (a.key != b.key) {
            if (a.key == systemLocale || a.key == systemLanguage) {
                return true;
            }
            if (b.key == systemLocale || b.key == systemLanguage) {
                return false;
            }
        }
        return a.languageName().toLower() < b.languageName().toLower();
    };

    QList<Language> refreshedLanguages;
    refreshedLanguages.reserve(languages.size());
    for (const auto& language : languages) {
        refreshedLanguages.append(language);
    }
    std::ranges::sort(refreshedLanguages, comp);

    const bool unchanged = d->m_languages.size() == refreshedLanguages.size()
        && std::equal(d->m_languages.cbegin(), d->m_languages.cend(),
                      refreshedLanguages.cbegin(),
                      [](const Language& current, const Language& refreshed) {
                          return current.isIdenticalTo(refreshed);
                      });
    if (unchanged) {
        return;
    }

    // The sort order can change whenever the system locale or catalog set
    // changes. A reset accurately describes that transition; inserting rows
    // and then sorting the entire backing list would invalidate persistent
    // indexes without emitting the required layout signals.
    beginResetModel();
    d->m_languages = std::move(refreshedLanguages);
    endResetModel();
}

namespace {
enum class Column : std::uint8_t { Language, Completeness };
}

QVariant TranslationsModel::data(const QModelIndex& index, const int role) const
{
    if (!index.isValid()) {
        return {};
    }

    const int row = index.row();
    const auto column = static_cast<Column>(index.column());

    if (row < 0 || row >= d->m_languages.size()) {
        return {};
    }

    auto& lang = d->m_languages[row];
    switch (role) {
        case Qt::DisplayRole: {
            switch (column) {
                case Column::Language: {
                    return lang.languageName();
                }
                case Column::Completeness: {
                    return QString("%1%").arg(lang.percentTranslated(), 3, 'f', 1);
                }
            }
            qWarning("TranslationModel::data not implemented when role is DisplayRole");
        }
        case Qt::ToolTipRole: {
            return tr("%1:\n%2 translated\n%3 fuzzy\n%4 total")
                .arg(lang.key, QString::number(lang.translated), QString::number(lang.fuzzy), QString::number(lang.total));
        }
        case Qt::UserRole:
            return lang.key;
        default:
            return {};
    }
}

QVariant TranslationsModel::headerData(int section, const Qt::Orientation orientation, const int role) const
{
    auto column = static_cast<Column>(section);
    if (role == Qt::DisplayRole) {
        switch (column) {
            case Column::Language: {
                return tr("Language");
            }
            case Column::Completeness: {
                return tr("Completeness");
            }
        }
    } else if (role == Qt::ToolTipRole) {
        switch (column) {
            case Column::Language: {
                return tr("The native language name.");
            }
            case Column::Completeness: {
                return tr("Completeness is the percentage of fully translated strings, not counting automatically guessed ones.");
            }
        }
    }
    return QAbstractListModel::headerData(section, orientation, role);
}

int TranslationsModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : d->m_languages.size();
}

int TranslationsModel::columnCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : 2;
}

QList<Language>::Iterator TranslationsModel::findLanguage(const QString& key) const
{
    return std::ranges::find_if(d->m_languages, [key](const Language& lang) { return lang.key == key; });
}

std::optional<Language> TranslationsModel::findLanguageAsOptional(const QString& key) const
{
    auto found = findLanguage(key);
    if (found != d->m_languages.end()) {
        return *found;
    }
    return {};
}

void TranslationsModel::setUseSystemLocale(const bool useSystemLocale) const
{
    d->m_useSystemLocale = useSystemLocale;
    APPLICATION->settings()->set("UseSystemLocale", useSystemLocale);
    QLocale::setDefault(useSystemLocale ? QLocale::system() : QLocale(selectedLanguage()));
}

bool TranslationsModel::selectLanguage(QString key) const
{
    QString& langCode = key;
    auto langPtr = findLanguageAsOptional(key);

    if (langCode.isEmpty()) {
        d->m_noLanguageSet = true;
    } else {
        d->m_noLanguageSet = false;
    }

    if (!langPtr.has_value()) {
        if (!key.isEmpty()) {
            qWarning() << "Selected invalid language" << key << ", defaulting to" << g_defaultLangCode;
        }
        langCode = g_defaultLangCode;
    } else {
        langCode = langPtr->key;
    }

    QString appTranslationPath;
    if (langPtr.has_value()) {
        if (langPtr->localFileType == FileType::Po) {
            appTranslationPath = d->m_dir.absoluteFilePath(langCode + ".po");
        } else if (langPtr->localFileType == FileType::Qm) {
            appTranslationPath = d->m_dir.absoluteFilePath("mmc_" + langCode + ".qm");
        }
    }
    QString appTranslationSignature;
    if (!appTranslationPath.isEmpty()) {
        const QFileInfo translationFile(appTranslationPath);
        if (translationFile.exists()) {
            appTranslationSignature = QString("%1|%2|%3")
                                          .arg(translationFile.canonicalFilePath())
                                          .arg(translationFile.size())
                                          .arg(fileSha1(appTranslationPath));
        }
    }

    const bool sameAppliedLanguage = d->m_languageSelectionApplied && d->m_selectedLanguage == langCode;
    if (sameAppliedLanguage && d->m_appTranslationSignature == appTranslationSignature) {
        return d->m_lastLanguageSelectionSuccessful;
    }

    // A changed application catalog does not require reinstalling Qt's catalog.
    if (d->m_appTranslator && (!sameAppliedLanguage || d->m_appTranslationSignature != appTranslationSignature)) {
        QCoreApplication::removeTranslator(d->m_appTranslator.get());
        d->m_appTranslator.reset();
    }
    if (d->m_qtTranslator && !sameAppliedLanguage) {
        QCoreApplication::removeTranslator(d->m_qtTranslator.get());
        d->m_qtTranslator.reset();
    }

    /*
     * FIXME: potential source of crashes:
     * In a multithreaded application, the default locale should be set at application startup, before any non-GUI threads are created.
     * This function is not reentrant.
     */
    QLocale::setDefault(d->m_useSystemLocale ? QLocale::system() : QLocale(langCode));

    // if it's the default UI language, finish
    if (langCode == g_defaultLangCode) {
        d->m_selectedLanguage = langCode;
        d->m_appTranslationSignature = appTranslationSignature;
        d->m_languageSelectionApplied = true;
        d->m_lastLanguageSelectionSuccessful = true;
        return true;
    }

    // otherwise install new translations
    bool successful = sameAppliedLanguage && d->m_qtTranslator;
    // FIXME: this is likely never present. FIX IT.
    if (!sameAppliedLanguage) {
        d->m_qtTranslator = std::make_unique<QTranslator>();
        if (d->m_qtTranslator->load("qt_" + langCode, QLibraryInfo::path(QLibraryInfo::TranslationsPath))) {
            qDebug() << "Loading Qt Language File for" << langCode.toLocal8Bit().constData() << "...";
            if (!QCoreApplication::installTranslator(d->m_qtTranslator.get())) {
                qCritical() << "Loading Qt Language File failed.";
                d->m_qtTranslator.reset();
            } else {
                successful = true;
            }
        } else {
            d->m_qtTranslator.reset();
        }
    }

    if (langPtr->localFileType == FileType::Po) {
        qDebug() << "Loading Application Language File for" << langCode.toLocal8Bit().constData() << "...";
        d->m_appTranslator = std::make_unique<POTranslator>(FS::PathCombine(d->m_dir.path(), langCode + ".po"));
        if (!d->m_appTranslator->isEmpty()) {
            if (!QCoreApplication::installTranslator(d->m_appTranslator.get())) {
                qCritical() << "Installing Application Language File failed.";
                d->m_appTranslator.reset();
            } else {
                successful = true;
            }
        } else {
            qCritical() << "Loading Application Language File failed.";
            d->m_appTranslator.reset();
        }
    } else if (langPtr->localFileType == FileType::Qm) {
        d->m_appTranslator = std::make_unique<QTranslator>();
        if (d->m_appTranslator->load("mmc_" + langCode, d->m_dir.path())) {
            qDebug() << "Loading Application Language File for" << langCode.toLocal8Bit().constData() << "...";
            if (!QCoreApplication::installTranslator(d->m_appTranslator.get())) {
                qCritical() << "Installing Application Language File failed.";
                d->m_appTranslator.reset();
            } else {
                successful = true;
            }
        } else {
            d->m_appTranslator.reset();
        }
    } else {
        d->m_appTranslator.reset();
    }
    d->m_selectedLanguage = langCode;
    d->m_appTranslationSignature = appTranslationSignature;
    d->m_languageSelectionApplied = true;
    d->m_lastLanguageSelectionSuccessful = successful;
    return successful;
}

QModelIndex TranslationsModel::selectedIndex() const
{
    auto found = findLanguage(d->m_selectedLanguage);
    if (found != d->m_languages.end()) {
        return index(std::distance(d->m_languages.begin(), found), 0, QModelIndex());
    }
    return {};
}

QString TranslationsModel::selectedLanguage() const
{
    return d->m_selectedLanguage;
}

void TranslationsModel::downloadIndex()
{
    if (d->m_indexJob || d->m_downloadJob) {
        return;
    }
    qDebug() << "Downloading Translations Index...";
    d->m_indexJob.reset(new NetJob("Translations Index", APPLICATION->network()));
    const MetaEntryPtr entry = APPLICATION->metacache()->resolveEntry("translations", "index_v2.json");
    entry->setStale(true);
    auto task = Net::Download::makeCached(QUrl(BuildConfig.TRANSLATION_FILES_URL + "index_v2.json"), entry);
    d->m_indexTask = task.get();
    d->m_indexJob->addNetAction(task);
    d->m_indexJob->setAskRetry(false);
    connect(d->m_indexJob.get(), &NetJob::failed, this, &TranslationsModel::indexFailed);
    connect(d->m_indexJob.get(), &NetJob::succeeded, this, &TranslationsModel::indexReceived);
    d->m_indexJob->start();
}

bool TranslationsModel::isIndexDownloadInProgress() const
{
    return d->m_indexJob != nullptr;
}

void TranslationsModel::updateLanguage(const QString& key)
{
    if (key == g_defaultLangCode) {
        qWarning() << "Cannot update builtin language" << key;
        return;
    }
    auto found = findLanguageAsOptional(key);
    if (!found.has_value()) {
        qWarning() << "Cannot update invalid language" << key;
        return;
    }
    if (!found->updated) {
        downloadTranslation(key);
    }
}

void TranslationsModel::downloadTranslation(const QString& key)
{
    if (d->m_downloadJob) {
        if (key != d->m_downloadingTranslation) {
            d->m_nextDownload = key;
        }
        return;
    }
    auto lang = findLanguageAsOptional(key);
    if (!lang.has_value()) {
        qWarning() << "Will not download an unknown translation" << key;
        return;
    }

    d->m_downloadingTranslation = key;
    const MetaEntryPtr entry = APPLICATION->metacache()->resolveEntry("translations", "mmc_" + key + ".qm");
    entry->setStale(true);

    auto dl = Net::Download::makeCached(QUrl(BuildConfig.TRANSLATION_FILES_URL + lang->fileName), entry);
    dl->addValidator(new Net::ChecksumValidator(QCryptographicHash::Sha1, lang->fileSha1));
    dl->setProgress(dl->getProgress(), lang->fileSize);

    d->m_downloadJob.reset(new NetJob("Translation for " + key, APPLICATION->network()));
    d->m_downloadJob->addNetAction(dl);
    d->m_downloadJob->setAskRetry(false);

    connect(d->m_downloadJob.get(), &NetJob::succeeded, this, &TranslationsModel::dlGood);
    connect(d->m_downloadJob.get(), &NetJob::failed, this, &TranslationsModel::dlFailed);

    d->m_downloadJob->start();
}

void TranslationsModel::downloadNext()
{
    if (!d->m_nextDownload.isEmpty()) {
        downloadTranslation(d->m_nextDownload);
        d->m_nextDownload.clear();
    }
}

void TranslationsModel::dlFailed(const QString& reason)
{
    qCritical() << "Translations Download Failed:" << reason;
    d->m_downloadJob.reset();
    downloadNext();
}

void TranslationsModel::dlGood()
{
    qDebug() << "Got translation:" << d->m_downloadingTranslation;

    if (d->m_downloadingTranslation == d->m_selectedLanguage) {
        d->translationReloadTimer->start();
    }
    d->m_downloadJob.reset();
    downloadNext();
}

void TranslationsModel::indexFailed(const QString& reason) const
{
    qCritical() << "Translations Index Download Failed:" << reason;
    d->m_indexJob.reset();
}
