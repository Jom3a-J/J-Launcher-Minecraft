// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QCoreApplication>

#include "net/NetJob.h"

class QLabel;
class QLineEdit;
class QPushButton;
class QWidget;

/**
 * The personal API key fields on the Services page.
 *
 * The CurseForge key and Modrinth token are typed hidden, saved to the secure
 * credential store rather than the settings file, and the CurseForge key can
 * be tried against the API before saving.
 */
class ApiKeyFields {
    // Same translation context as the page these fields sit on.
    Q_DECLARE_TR_FUNCTIONS(APIPage)

   public:
    struct Widgets {
        QLineEdit* flameKey;
        QLineEdit* modrinthToken;
        QPushButton* testFlameKeyButton;
        QLabel* flameKeyStatus;
        QLabel* storageNote;
    };

    ApiKeyFields(const Widgets& widgets, QWidget* page);

    void load();
    /// Saves changed values; on failure tells the user and returns false.
    bool save();
    void retranslate();

   private:
    void updateTexts();
    void testFlameKey();

    Widgets m_ui;
    QWidget* m_page;
    NetJob::Ptr m_flameKeyTestJob;
};
