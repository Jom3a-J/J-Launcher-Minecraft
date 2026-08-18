// SPDX-License-Identifier: GPL-3.0-only

#include <QFileInfo>
#include <QTemporaryDir>
#include <QTest>

#include "minecraft/auth/AccountList.h"

class AccountListTest : public QObject {
    Q_OBJECT

   private slots:
    void missingListStartsEmpty()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());

        const QString path = directory.filePath(QStringLiteral("accounts.json"));
        AccountList accounts;
        accounts.setListFilePath(path, true);

        QVERIFY(accounts.loadList());
        QCOMPARE(accounts.count(), 0);
        QVERIFY(!QFileInfo::exists(path));
    }
};

QTEST_GUILESS_MAIN(AccountListTest)

#include "AccountList_test.moc"
