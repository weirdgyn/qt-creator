// Copyright (C) 2016 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#pragma once

#include "itestparser.h"

#include <qmljs/qmljsdocument.h>

#include <utils/futuresynchronizer.h>
#include <utils/id.h>

#include <QObject>
#include <QTimer>

QT_BEGIN_NAMESPACE
class QThreadPool;
QT_END_NAMESPACE

namespace ProjectExplorer { class Project; }
namespace Utils { class TaskTree; }

namespace Autotest {
namespace Internal {

class TestCodeParser : public QObject
{
    Q_OBJECT
public:
    enum State {
        Idle,
        PartialParse,
        FullParse,
        Shutdown
    };

    TestCodeParser();
    ~TestCodeParser();

    void setState(State state);
    State state() const { return m_parserState; }
    bool isParsing() const { return m_parserState == PartialParse || m_parserState == FullParse; }
    void setDirty() { m_dirty = true; }
    void syncTestFrameworks(const QList<ITestParser *> &parsers);
#ifdef WITH_TESTS
    bool furtherParsingExpected() const
    { return m_singleShotScheduled || m_postponedUpdateType != UpdateType::NoUpdate; }
#endif

signals:
    void aboutToPerformFullParse();
    void testParseResultReady(const TestParseResultPtr result); // TODO: pass list of results?
    void parsingStarted();
    void parsingFinished();
    void parsingFailed();
    void requestRemoval(const Utils::FilePath &filePath);
    void requestRemoveAllFrameworkItems();

public:
    void emitUpdateTestTree(ITestParser *parser = nullptr);
    void updateTestTree(const QSet<ITestParser *> &parsers = {});
    void onCppDocumentUpdated(const CPlusPlus::Document::Ptr &document);
    void onQmlDocumentUpdated(const QmlJS::Document::Ptr &document);
    void onStartupProjectChanged(ProjectExplorer::Project *project);
    void onProjectPartsUpdated(ProjectExplorer::Project *project);
    void aboutToShutdown();

private:
    bool postponed(const Utils::FilePaths &fileList);
    void scanForTests(const Utils::FilePaths &fileList = Utils::FilePaths(),
                      const QList<ITestParser *> &parsers = {});

    // qml files must be handled slightly different
    void onDocumentUpdated(const Utils::FilePath &fileName, bool isQmlFile = false);
    void onTaskStarted(Utils::Id type);
    void onAllTasksFinished(Utils::Id type);
    void onFinished(bool success);
    void onPartialParsingFinished();
    void parsePostponedFiles();
    void releaseParserInternals();

    // used internally to indicate a parse that failed due to having triggered a parse for a file that
    // is not (yet) part of the CppModelManager's snapshot
    bool m_parsingHasFailed = false;

    bool m_codeModelParsing = false;
    enum class UpdateType { NoUpdate, PartialUpdate, FullUpdate };
    UpdateType m_postponedUpdateType = UpdateType::NoUpdate;
    bool m_dirty = false;
    bool m_singleShotScheduled = false;
    bool m_reparseTimerTimedOut = false;
    QSet<Utils::FilePath> m_postponedFiles;
    State m_parserState = Idle;
    QList<ITestParser *> m_testCodeParsers; // ptrs are still owned by TestFrameworkManager
    QTimer m_reparseTimer;
    QSet<ITestParser *> m_updateParsers;
    QThreadPool *m_threadPool = nullptr;
    Utils::FutureSynchronizer m_futureSynchronizer;
    std::unique_ptr<Utils::TaskTree> m_taskTree;
};

} // namespace Internal
} // namespace Autotest
