// SPDX-License-Identifier: GPL-3.0-only
#include <QtTest>

#include <QStandardItemModel>

#include "ui/pages/modplatform/flame/FlameModel.h"
#include "ui/pages/modplatform/flame/FlameServerReadyFilter.h"

class FlameServerReadyFilterTest final : public QObject {
    Q_OBJECT

private slots:
    void filtersLoadedRows()
    {
        QVERIFY(!Flame::isVisibleWithServerReadyFilter(false, true));
        QVERIFY(Flame::isVisibleWithServerReadyFilter(false, false));
        QVERIFY(Flame::isVisibleWithServerReadyFilter(true, true));
    }

    void automaticallyFetchesOnlyWhenNeededAndWithinLimit()
    {
        QVERIFY(Flame::shouldAutomaticallyFetchServerReadyPage(true, 3, 10, 0, true));
        QVERIFY(!Flame::shouldAutomaticallyFetchServerReadyPage(true, 10, 10, 0, true));
        QVERIFY(!Flame::shouldAutomaticallyFetchServerReadyPage(true, 3, 10,
                                                               Flame::MaximumAutomaticSearchPages, true));
        QVERIFY(!Flame::shouldAutomaticallyFetchServerReadyPage(true, 3, 10, 0, false));
        QVERIFY(!Flame::shouldAutomaticallyFetchServerReadyPage(false, 3, 10, 0, true));
    }

    void proxyHidesPacksWithoutServerPacksAndShowsThemAgain()
    {
        QStandardItemModel source;
        const QList<bool> ready{ true, false, true, false };
        for (const bool hasServerPack : ready) {
            auto* item = new QStandardItem;
            item->setData(hasServerPack, Flame::ServerReadyRole);
            source.appendRow(item);
        }
        Flame::ServerReadyFilterModel proxy;
        proxy.setSourceModel(&source);
        QCOMPARE(proxy.rowCount(), 4);

        proxy.setServerReadyOnly(true);
        QCOMPARE(proxy.rowCount(), 2);
        for (int row = 0; row < proxy.rowCount(); ++row)
            QVERIFY(proxy.index(row, 0).data(Flame::ServerReadyRole).toBool());
        QCOMPARE(proxy.mapToSource(proxy.index(1, 0)).row(), 2);

        // Rows that arrive later are filtered too.
        auto* late = new QStandardItem;
        late->setData(true, Flame::ServerReadyRole);
        source.appendRow(late);
        QCOMPARE(proxy.rowCount(), 3);

        proxy.setServerReadyOnly(false);
        QCOMPARE(proxy.rowCount(), 5);
    }
};

QTEST_MAIN(FlameServerReadyFilterTest)
#include "FlameServerReadyFilter_test.moc"
