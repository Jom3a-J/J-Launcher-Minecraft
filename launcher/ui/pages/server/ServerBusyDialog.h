// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QEventLoop>
#include <QFutureWatcher>
#include <QProgressDialog>
#include <QtConcurrent/QtConcurrentRun>

#include <type_traits>
#include <utility>

/*! A modal "please wait" window for slow file work in the Server Manager.
 *
 *  run() does the work on a worker thread and waits for it while the window keeps painting,
 *  then returns the work's result. The window stays up until close() or destruction, so a
 *  caller can finish quick steps on the UI thread before it disappears.
 *
 *  The work must not touch widgets or other UI-thread objects.
 */
class ServerBusyDialog
{
public:
    ServerBusyDialog(QWidget *parent, const QString &title, const QString &label)
        : m_dialog(label, QString(), 0, 0, parent)
    {
        m_dialog.setWindowTitle(title);
        m_dialog.setCancelButton(nullptr);
        m_dialog.setWindowModality(Qt::ApplicationModal);
        m_dialog.setMinimumDuration(0);
        m_dialog.show();
    }

    template <typename Work>
    std::invoke_result_t<Work> run(Work &&work)
    {
        using Result = std::invoke_result_t<Work>;
        QFutureWatcher<Result> watcher;
        QEventLoop waitLoop;
        QObject::connect(&watcher, &QFutureWatcher<Result>::finished, &waitLoop, &QEventLoop::quit);
        watcher.setFuture(QtConcurrent::run(std::forward<Work>(work)));
        if (!watcher.isFinished()) {
            waitLoop.exec();
        }
        return watcher.result();
    }

    void close() { m_dialog.close(); }

private:
    QProgressDialog m_dialog;
};
