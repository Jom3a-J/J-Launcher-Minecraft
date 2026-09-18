// SPDX-License-Identifier: GPL-3.0-only
/*
 *  J Launcher - Minecraft Launcher
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
 */

#include <QTest>
#include <functional>

#include "tasks/ConcurrentTask.h"
#include "tasks/SequentialTask.h"
#include "tasks/Task.h"

/*! Covers the shape MinecraftInstance::createUpdateTask() builds:
 *
 *      FoldersTask -> ConcurrentTask{ LibrariesTask, AssetUpdateTask } -> LegacyFMLLibrariesTask
 *
 *  wrapped in a SequentialTask by every caller (InstanceTask, VersionPage) or appended as ordered
 *  launch steps (MinecraftInstance::createLaunchTask).
 *
 *  The production steps need a fully initialised Application, a real instance directory and a
 *  resolved pack profile, so these tests drive stand-ins wired into exactly the same graph and
 *  assert the ordering relationships that matter: nothing downloads before the folders exist,
 *  libraries and assets run side by side, and the legacy FML step still runs after both - and
 *  only if they succeeded.
 */
namespace {

/*! Finishes only when the test tells it to. */
class ManualTask : public Task {
    Q_OBJECT

   public:
    using Ptr = shared_qobject_ptr<ManualTask>;

    explicit ManualTask(QString name) : Task(false) { setObjectName(std::move(name)); }

    bool started() const { return m_started; }
    bool canAbort() const override { return true; }

    void succeed()
    {
        if (isRunning())
            emitSucceeded();
    }

    void fail()
    {
        if (isRunning())
            emitFailed(objectName() + QStringLiteral(" failed"));
    }

    bool abort() override
    {
        if (isRunning())
            emitAborted();
        return true;
    }

   private:
    void executeTask() override { m_started = true; }

    bool m_started = false;
};

struct Graph {
    SequentialTask::Ptr root;
    ManualTask::Ptr folders;
    ManualTask::Ptr legacyFml;
    ManualTask::Ptr libraries;
    ManualTask::Ptr assets;
};

/*! Mirrors MinecraftInstance::createUpdateTask(): folders, then libraries and assets together,
 *  then the still sequential legacy FML step. */
Graph makeGraph()
{
    Graph graph;
    graph.folders = makeShared<ManualTask>(QStringLiteral("folders"));
    graph.libraries = makeShared<ManualTask>(QStringLiteral("libraries"));
    graph.assets = makeShared<ManualTask>(QStringLiteral("assets"));
    graph.legacyFml = makeShared<ManualTask>(QStringLiteral("legacyFml"));

    auto gameFiles = makeShared<ConcurrentTask>(QStringLiteral("gameFiles"), 2);
    gameFiles->addTask(graph.libraries);
    gameFiles->addTask(graph.assets);

    graph.root = makeShared<SequentialTask>(QStringLiteral("update"));
    graph.root->addTask(graph.folders);
    graph.root->addTask(gameFiles);
    graph.root->addTask(graph.legacyFml);
    return graph;
}

bool waitFor(std::function<bool()> predicate, int timeoutMs = 3000)
{
    return QTest::qWaitFor(std::move(predicate), timeoutMs);
}

}  // namespace

class GameUpdateTaskGraphTest : public QObject {
    Q_OBJECT

   private slots:
    /*! Nothing downloads before the required setup steps are done, and then both run together. */
    void test_downloadsWaitForSetupAndThenRunConcurrently()
    {
        auto graph = makeGraph();

        graph.root->start();
        QVERIFY(waitFor([&] { return graph.folders->started(); }));
        QVERIFY2(!graph.legacyFml->started(), "the sequential step started before folders finished");
        QVERIFY2(!graph.libraries->started(), "libraries started before folders finished");
        QVERIFY2(!graph.assets->started(), "assets started before folders finished");

        graph.folders->succeed();
        QVERIFY2(waitFor([&] { return graph.libraries->started() && graph.assets->started(); }),
                 "libraries and assets did not run at the same time");
        QVERIFY2(!graph.legacyFml->started(), "legacy FML started before the concurrent downloads finished");

        graph.libraries->succeed();
        graph.assets->succeed();
        QVERIFY(waitFor([&] { return graph.legacyFml->started(); }));
        graph.legacyFml->succeed();
        QVERIFY(waitFor([&] { return graph.root->isFinished(); }));
        QVERIFY(graph.root->wasSuccessful());
    }

    /*! The group only completes once both of its members have. */
    void test_groupWaitsForBothMembers()
    {
        auto graph = makeGraph();

        graph.root->start();
        QVERIFY(waitFor([&] { return graph.folders->started(); }));
        graph.folders->succeed();
        QVERIFY(waitFor([&] { return graph.libraries->started() && graph.assets->started(); }));

        graph.libraries->succeed();
        QTest::qWait(50);
        QVERIFY2(!graph.root->isFinished(), "the update finished while assets were still running");
        QVERIFY2(!graph.legacyFml->started(), "legacy FML started while assets were still running");

        graph.assets->succeed();
        QVERIFY(waitFor([&] { return graph.legacyFml->started(); }));
        QVERIFY2(!graph.root->isFinished(), "the update finished before legacy FML completed");
        graph.legacyFml->succeed();
        QVERIFY(waitFor([&] { return graph.root->isFinished(); }));
        QVERIFY(graph.root->wasSuccessful());
    }

    /*! A failure inside the group fails the whole update. */
    void test_failureInTheGroupFailsTheUpdate()
    {
        auto graph = makeGraph();

        graph.root->start();
        QVERIFY(waitFor([&] { return graph.folders->started(); }));
        graph.folders->succeed();
        QVERIFY(waitFor([&] { return graph.libraries->started() && graph.assets->started(); }));

        graph.libraries->fail();
        graph.assets->succeed();

        QVERIFY(waitFor([&] { return graph.root->isFinished(); }));
        QVERIFY(!graph.root->wasSuccessful());
        QVERIFY(graph.root->failReason().contains(QStringLiteral("libraries")));
        QVERIFY2(!graph.legacyFml->started(), "legacy FML started after the download group failed");
    }

    /*! A failure in a setup step never lets the download group start. */
    void test_setupFailureSkipsTheGroup()
    {
        auto graph = makeGraph();

        graph.root->start();
        QVERIFY(waitFor([&] { return graph.folders->started(); }));
        graph.folders->fail();

        QVERIFY(waitFor([&] { return graph.root->isFinished(); }));
        QVERIFY(!graph.root->wasSuccessful());
        QVERIFY(!graph.legacyFml->started());
        QVERIFY(!graph.libraries->started());
        QVERIFY(!graph.assets->started());
    }

    /*! Cancelling the update stops both members of the group. */
    void test_cancellationStopsTheWholeGroup()
    {
        auto graph = makeGraph();

        graph.root->start();
        QVERIFY(waitFor([&] { return graph.folders->started(); }));
        graph.folders->succeed();
        QVERIFY(waitFor([&] { return graph.libraries->started() && graph.assets->started(); }));

        QVERIFY(graph.root->abort());
        QVERIFY(waitFor([&] { return graph.root->isFinished(); }));
        QVERIFY(!graph.root->wasSuccessful());
        QCOMPARE(graph.libraries->getState(), Task::State::AbortedByUser);
        QCOMPARE(graph.assets->getState(), Task::State::AbortedByUser);
    }
};

QTEST_GUILESS_MAIN(GameUpdateTaskGraphTest)

#include "GameUpdateTaskGraph_test.moc"
