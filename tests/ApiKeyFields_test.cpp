// SPDX-License-Identifier: GPL-3.0-only

#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTest>
#include <QWidget>

#include "BuildConfig.h"
#include "ui/pages/global/ApiKeyFields.h"

class ApiKeyFieldsTest : public QObject {
    Q_OBJECT

   private slots:
    void retranslateRestoresTheFieldTexts()
    {
        QWidget page;
        auto* flameKey = new QLineEdit(&page);
        auto* modrinthToken = new QLineEdit(&page);
        auto* testButton = new QPushButton(&page);
        auto* status = new QLabel(&page);
        auto* storageNote = new QLabel(&page);
        ApiKeyFields fields({ flameKey, modrinthToken, testButton, status, storageNote }, &page);

        const QString placeholder = flameKey->placeholderText();
        const QString note = storageNote->text();
        QVERIFY(!placeholder.isEmpty());
        QVERIFY(!note.isEmpty());
        QCOMPARE(placeholder, BuildConfig.FLAME_API_KEY.trimmed().isEmpty() ? QStringLiteral("Enter your CurseForge API key")
                                                                             : QStringLiteral("Use bundled key"));
        QCOMPARE(flameKey->echoMode(), QLineEdit::Password);
        QCOMPARE(modrinthToken->echoMode(), QLineEdit::Password);

        // What the page's retranslateUi() does on a language change: the .ui texts come back.
        flameKey->setPlaceholderText(QStringLiteral("Use Default"));
        storageNote->setText(QStringLiteral("Note: you probably don't need to set this if CurseForge already works."));
        fields.retranslate();

        QCOMPARE(flameKey->placeholderText(), placeholder);
        QCOMPARE(storageNote->text(), note);
    }
};

QTEST_MAIN(ApiKeyFieldsTest)

#include "ApiKeyFields_test.moc"
