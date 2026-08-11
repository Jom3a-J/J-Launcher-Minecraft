// SPDX-License-Identifier: GPL-3.0-only

#include <QDialog>
#include <QMainWindow>
#include <QPointer>
#include <QTest>

#include "ui/ModelessWindow.h"

class ModelessWindowTest final : public QObject
{
    Q_OBJECT

   private slots:
    void modelessDialogKeepsOwnerUsable()
    {
        QMainWindow mainWindow;
        mainWindow.show();

        QDialog dialog(&mainWindow);
        UI::Modeless::show(&dialog);

        QVERIFY(mainWindow.isEnabled());
        QVERIFY(dialog.isVisible());
        QVERIFY(!dialog.isModal());
        QCOMPARE(dialog.windowModality(), Qt::NonModal);

        dialog.close();
    }

    void repeatedActivationReusesAndRaisesWindow()
    {
        QMainWindow mainWindow;
        mainWindow.show();

        QPointer<QDialog> window;
        int creations = 0;
        const auto openWindow = [&] {
            return UI::Modeless::showOrActivate(window, [&] {
                ++creations;
                auto* dialog = new QDialog(&mainWindow);
                dialog->setAttribute(Qt::WA_DeleteOnClose);
                return dialog;
            });
        };

        QDialog* first = openWindow();
        QVERIFY(first);
        QVERIFY(mainWindow.isEnabled());
        QCOMPARE(creations, 1);

        first->showMinimized();
        QDialog* second = openWindow();
        QCOMPARE(second, first);
        QCOMPARE(creations, 1);
        QVERIFY(!second->isMinimized());
        QVERIFY(mainWindow.isEnabled());

        first->accept();
        QTRY_VERIFY(window.isNull());

        QDialog* recreated = openWindow();
        QVERIFY(recreated);
        QCOMPARE(creations, 2);
        QVERIFY(mainWindow.isEnabled());
        recreated->accept();
        QTRY_VERIFY(window.isNull());
    }

    void multipleModelessDialogsKeepOwnerUsable()
    {
        QMainWindow mainWindow;
        mainWindow.show();

        QDialog first(&mainWindow);
        QDialog second(&mainWindow);
        UI::Modeless::show(&first);
        UI::Modeless::show(&second);

        QVERIFY(first.isVisible());
        QVERIFY(second.isVisible());
        QVERIFY(mainWindow.isEnabled());

        first.close();
        second.close();
    }
};

QTEST_MAIN(ModelessWindowTest)

#include "ModelessWindow_test.moc"
