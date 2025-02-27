// Copyright (C) 2016 Jochen Becher
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#include "modelindexer.h"

#include "modeleditor_constants.h"

#include "qmt/infrastructure/exceptions.h"
#include "qmt/infrastructure/uid.h"

#include "qmt/serializer/projectserializer.h"

#include "qmt/project/project.h"
#include "qmt/model_controller/mvoidvisitor.h"
#include "qmt/model/mpackage.h"
#include "qmt/model/mdiagram.h"

#include "qmt/tasks/findrootdiagramvisitor.h"

#include <projectexplorer/project.h>
#include <projectexplorer/projectmanager.h>
#include <projectexplorer/projectnodes.h>

#include <utils/mimeutils.h>
#include <utils/qtcassert.h>

#include <QQueue>
#include <QMutex>
#include <QMutexLocker>
#include <QThread>
#include <QDateTime>

#include <QLoggingCategory>
#include <QDebug>
#include <QPointer>

namespace ModelEditor {
namespace Internal {

class ModelIndexer::QueuedFile
{
    friend size_t qHash(const ModelIndexer::QueuedFile &queuedFile);
    friend bool operator==(const ModelIndexer::QueuedFile &lhs,
                           const ModelIndexer::QueuedFile &rhs);

public:
    QueuedFile() = default;

    QueuedFile(const QString &file, ProjectExplorer::Project *project)
        : m_file(file),
          m_project(project)
    {
    }

    QueuedFile(const QString &file, ProjectExplorer::Project *project,
               const QDateTime &lastModified)
        : m_file(file),
          m_project(project),
          m_lastModified(lastModified)
    {
    }

    bool isValid() const { return !m_file.isEmpty() && m_project; }
    QString file() const { return m_file; }
    ProjectExplorer::Project *project() const { return m_project; }
    QDateTime lastModified() const { return m_lastModified; }

private:
    QString m_file;
    ProjectExplorer::Project *m_project = nullptr;
    QDateTime m_lastModified;
};

bool operator==(const ModelIndexer::QueuedFile &lhs, const ModelIndexer::QueuedFile &rhs)
{
    return lhs.m_file == rhs.m_file && lhs.m_project == rhs.m_project;
}

size_t qHash(const ModelIndexer::QueuedFile &queuedFile)
{
    return qHash(queuedFile.m_project) + qHash(queuedFile.m_project);
}

class ModelIndexer::IndexedModel
{
public:
    IndexedModel(const QString &modelFile, const QDateTime &lastModified)
        : m_modelFile(modelFile),
          m_lastModified(lastModified)
    {
    }

    void reset(const QDateTime &lastModified)
    {
        m_lastModified = lastModified;
        m_modelUid = qmt::Uid::invalidUid();
        m_diagrams.clear();
    }

    QString file() const { return m_modelFile; }
    QDateTime lastModified() const { return m_lastModified; }
    QSet<ProjectExplorer::Project *> owningProjects() const { return m_owningProjects; }
    void addOwningProject(ProjectExplorer::Project *project) { m_owningProjects.insert(project); }
    void removeOwningProject(ProjectExplorer::Project *project)
    {
        m_owningProjects.remove(project);
    }
    qmt::Uid modelUid() const { return m_modelUid; }
    void setModelUid(const qmt::Uid &modelUid) { m_modelUid = modelUid; }
    QSet<qmt::Uid> diagrams() const { return m_diagrams; }
    void addDiagram(const qmt::Uid &diagram) { m_diagrams.insert(diagram); }

private:
    QString m_modelFile;
    QDateTime m_lastModified;
    QSet<ProjectExplorer::Project *> m_owningProjects;
    qmt::Uid m_modelUid;
    QSet<qmt::Uid> m_diagrams;
};

class ModelIndexer::IndexedDiagramReference
{
public:
    IndexedDiagramReference(const QString &file, const QDateTime &lastModified)
        : m_file(file),
          m_lastModified(lastModified)
    {
    }

    void reset(const QDateTime &lastModified)
    {
        m_lastModified = lastModified;
        m_modelUid = qmt::Uid::invalidUid();
        m_diagramUid = qmt::Uid::invalidUid();
    }

    QString file() const { return m_file; }
    QDateTime lastModified() const { return m_lastModified; }
    QSet<ProjectExplorer::Project *> owningProjects() const { return m_owningProjects; }
    void addOwningProject(ProjectExplorer::Project *project) { m_owningProjects.insert(project); }
    void removeOwningProject(ProjectExplorer::Project *project)
    {
        m_owningProjects.remove(project);
    }
    qmt::Uid modelUid() const { return m_modelUid; }
    void setModelUid(const qmt::Uid &modelUid) { m_modelUid = modelUid; }
    qmt::Uid diagramUid() const { return m_diagramUid; }
    void setDiagramUid(const qmt::Uid &diagramUid) { m_diagramUid = diagramUid; }

private:
    QString m_file;
    QDateTime m_lastModified;
    QSet<ProjectExplorer::Project *> m_owningProjects;
    qmt::Uid m_modelUid;
    qmt::Uid m_diagramUid;
};

class ModelIndexer::IndexerThread :
        public QThread
{
public:
    IndexerThread(ModelIndexer *indexer)
        : QThread(),
          m_indexer(indexer)
    {
    }

    void onQuitIndexerThread();
    void onFilesQueued();

private:
    ModelIndexer *m_indexer;
};

class ModelIndexer::DiagramsCollectorVisitor :
        public qmt::MVoidConstVisitor
{
public:
    DiagramsCollectorVisitor(ModelIndexer::IndexedModel *indexedModel);

    void visitMObject(const qmt::MObject *object) final;
    void visitMDiagram(const qmt::MDiagram *diagram) final;

private:
    ModelIndexer::IndexedModel *m_indexedModel;
};

ModelIndexer::DiagramsCollectorVisitor::DiagramsCollectorVisitor(IndexedModel *indexedModel)
    : qmt::MVoidConstVisitor(),
      m_indexedModel(indexedModel)
{
}

void ModelIndexer::DiagramsCollectorVisitor::visitMObject(const qmt::MObject *object)
{
    for (const qmt::Handle<qmt::MObject> &child : object->children()) {
        if (child.hasTarget())
            child.target()->accept(this);
    }
    visitMElement(object);
}

void ModelIndexer::DiagramsCollectorVisitor::visitMDiagram(const qmt::MDiagram *diagram)
{
    qCDebug(logger) << "add diagram " << diagram->name() << " to index";
    m_indexedModel->addDiagram(diagram->uid());
    visitMObject(diagram);
}

class ModelIndexer::ModelIndexerPrivate
{
public:
    ~ModelIndexerPrivate()
    {
        QMT_CHECK(filesQueue.isEmpty());
        QMT_CHECK(queuedFilesSet.isEmpty());
        QMT_CHECK(indexedModels.isEmpty());
        QMT_CHECK(indexedModelsByUid.isEmpty());
        QMT_CHECK(indexedDiagramReferences.isEmpty());
        QMT_CHECK(indexedDiagramReferencesByDiagramUid.isEmpty());
        delete indexerThread;
    }

    QMutex indexerMutex;

    QQueue<ModelIndexer::QueuedFile> filesQueue;
    QSet<ModelIndexer::QueuedFile> queuedFilesSet;
    QSet<ModelIndexer::QueuedFile> defaultModelFiles;

    QHash<QString, ModelIndexer::IndexedModel *> indexedModels;
    QHash<qmt::Uid, QSet<ModelIndexer::IndexedModel *> > indexedModelsByUid;

    QHash<QString, ModelIndexer::IndexedDiagramReference *> indexedDiagramReferences;
    QHash<qmt::Uid, QSet<ModelIndexer::IndexedDiagramReference *> > indexedDiagramReferencesByDiagramUid;

    ModelIndexer::IndexerThread *indexerThread = nullptr;
};

void ModelIndexer::IndexerThread::onQuitIndexerThread()
{
    QThread::exit(0);
}

void ModelIndexer::IndexerThread::onFilesQueued()
{
    QMutexLocker locker(&m_indexer->d->indexerMutex);

    while (!m_indexer->d->filesQueue.isEmpty()) {
        ModelIndexer::QueuedFile queuedFile = m_indexer->d->filesQueue.takeFirst();
        m_indexer->d->queuedFilesSet.remove(queuedFile);
        qCDebug(logger) << "handle queued file " << queuedFile.file()
                        << "from project " << queuedFile.project()->displayName();

        bool scanModel = false;
        IndexedModel *indexedModel = m_indexer->d->indexedModels.value(queuedFile.file());
        if (!indexedModel) {
            qCDebug(logger) << "create new indexed model";
            indexedModel = new IndexedModel(queuedFile.file(),
                                            queuedFile.lastModified());
            indexedModel->addOwningProject(queuedFile.project());
            m_indexer->d->indexedModels.insert(queuedFile.file(), indexedModel);
            scanModel = true;
        } else if (queuedFile.lastModified() > indexedModel->lastModified()) {
            qCDebug(logger) << "update indexed model";
            indexedModel->addOwningProject(queuedFile.project());
            indexedModel->reset(queuedFile.lastModified());
            scanModel = true;
        }
        if (scanModel) {
            locker.unlock();
            // load model file
            qmt::ProjectSerializer projectSerializer;
            qmt::Project project;
            try {
                projectSerializer.load(queuedFile.file(), &project);
            } catch (const qmt::Exception &e) {
                qWarning() << e.errorMessage();
                return;
            }
            locker.relock();
            indexedModel->setModelUid(project.uid());
            // add indexedModel to set of indexedModelsByUid
            QSet<IndexedModel *> indexedModels = m_indexer->d->indexedModelsByUid.value(project.uid());
            indexedModels.insert(indexedModel);
            m_indexer->d->indexedModelsByUid.insert(project.uid(), indexedModels);
            // collect all diagrams of model
            DiagramsCollectorVisitor visitor(indexedModel);
            project.rootPackage()->accept(&visitor);
            if (m_indexer->d->defaultModelFiles.contains(queuedFile)) {
                m_indexer->d->defaultModelFiles.remove(queuedFile);
                // check if model has a diagram which could be opened
                qmt::FindRootDiagramVisitor diagramVisitor;
                project.rootPackage()->accept(&diagramVisitor);
                if (diagramVisitor.diagram())
                    emit m_indexer->openDefaultModel(project.uid());
            }
        }
    }
}

ModelIndexer::ModelIndexer(QObject *parent)
    : QObject(parent),
      d(new ModelIndexerPrivate())
{
    d->indexerThread = new IndexerThread(this);
    connect(this, &ModelIndexer::quitIndexerThread,
            d->indexerThread, &ModelIndexer::IndexerThread::onQuitIndexerThread);
    connect(this, &ModelIndexer::filesQueued,
            d->indexerThread, &ModelIndexer::IndexerThread::onFilesQueued);
    d->indexerThread->start();
    connect(ProjectExplorer::ProjectManager::instance(), &ProjectExplorer::ProjectManager::projectAdded,
            this, &ModelIndexer::onProjectAdded);
    connect(ProjectExplorer::ProjectManager::instance(), &ProjectExplorer::ProjectManager::aboutToRemoveProject,
            this, &ModelIndexer::onAboutToRemoveProject);
}

ModelIndexer::~ModelIndexer()
{
    emit quitIndexerThread();
    d->indexerThread->wait();
    delete d;
}

QString ModelIndexer::findModel(const qmt::Uid &modelUid)
{
    QMutexLocker locker(&d->indexerMutex);
    QSet<IndexedModel *> indexedModels = d->indexedModelsByUid.value(modelUid);
    if (indexedModels.isEmpty())
        return QString();
    IndexedModel *indexedModel = *indexedModels.cbegin();
    QMT_ASSERT(indexedModel, return QString());
    return indexedModel->file();
}

QString ModelIndexer::findDiagram(const qmt::Uid &modelUid, const qmt::Uid &diagramUid)
{
    Q_UNUSED(modelUid) // avoid warning in release mode

    QMutexLocker locker(&d->indexerMutex);
    QSet<IndexedDiagramReference *> indexedDiagramReferences = d->indexedDiagramReferencesByDiagramUid.value(diagramUid);
    if (indexedDiagramReferences.isEmpty())
        return QString();
    IndexedDiagramReference *indexedDiagramReference = *indexedDiagramReferences.cbegin();
    QMT_ASSERT(indexedDiagramReference, return QString());
    QMT_ASSERT(indexedDiagramReference->modelUid() == modelUid, return QString());
    return indexedDiagramReference->file();
}

void ModelIndexer::onProjectAdded(ProjectExplorer::Project *project)
{
    connect(project,
            &ProjectExplorer::Project::fileListChanged,
            this,
            [this, p = QPointer(project)] { if (p) onProjectFileListChanged(p.data()); },
            Qt::QueuedConnection);
    scanProject(project);
}

void ModelIndexer::onAboutToRemoveProject(ProjectExplorer::Project *project)
{
    disconnect(project, &ProjectExplorer::Project::fileListChanged, this, nullptr);
    forgetProject(project);
}

void ModelIndexer::onProjectFileListChanged(ProjectExplorer::Project *project)
{
    scanProject(project);
}

void ModelIndexer::scanProject(ProjectExplorer::Project *project)
{
    if (!project->rootProjectNode())
        return;

    // TODO harmonize following code with findFirstModel()?
    const Utils::FilePaths files = project->files(ProjectExplorer::Project::SourceFiles);
    QQueue<QueuedFile> filesQueue;
    QSet<QueuedFile> filesSet;

    const Utils::MimeType modelMimeType = Utils::mimeTypeForName(Constants::MIME_TYPE_MODEL);
    if (modelMimeType.isValid()) {
        for (const Utils::FilePath &file : files) {
            if (modelMimeType.suffixes().contains(file.completeSuffix())) {
                QueuedFile queuedFile(file.toString(), project, file.lastModified());
                filesQueue.append(queuedFile);
                filesSet.insert(queuedFile);
            }
        }
    }

    // FIXME: This potentially iterates over all files again.
    QString defaultModelFile = findFirstModel(project->rootProjectNode(), modelMimeType);

    bool filesAreQueued = false;
    {
        QMutexLocker locker(&d->indexerMutex);

        // remove deleted files from queue
        for (int i = 0; i < d->filesQueue.size();) {
            if (d->filesQueue.at(i).project() == project) {
                if (filesSet.contains(d->filesQueue.at(i))) {
                    ++i;
                } else {
                    d->queuedFilesSet.remove(d->filesQueue.at(i));
                    d->filesQueue.removeAt(i);
                }
            }
        }

        // remove deleted files from indexed models
        const QStringList files = d->indexedModels.keys();
        for (const QString &file : files) {
            if (!filesSet.contains(QueuedFile(file, project)))
                removeModelFile(file, project);
        }

        // remove deleted files from indexed diagrams
        const QStringList deletedFiles = d->indexedDiagramReferences.keys();
        for (const QString &file : deletedFiles) {
            if (!filesSet.contains(QueuedFile(file, project)))
                removeDiagramReferenceFile(file, project);
        }

        // queue files
        while (!filesQueue.isEmpty()) {
            QueuedFile queuedFile = filesQueue.takeFirst();
            if (!d->queuedFilesSet.contains(queuedFile)) {
                QMT_CHECK(!d->filesQueue.contains(queuedFile));
                d->filesQueue.append(queuedFile);
                d->queuedFilesSet.insert(queuedFile);
                filesAreQueued = true;
            }
        }

        // auto-open model file only if project is already configured
        if (!defaultModelFile.isEmpty() && !project->targets().isEmpty()) {
            d->defaultModelFiles.insert(QueuedFile(defaultModelFile, project, QDateTime()));
        }
    }

    if (filesAreQueued)
        emit filesQueued();
}

QString ModelIndexer::findFirstModel(ProjectExplorer::FolderNode *folderNode,
                                     const Utils::MimeType &mimeType)
{
    if (!mimeType.isValid())
        return QString();
    const QList<ProjectExplorer::FileNode *> fileNodes = folderNode->fileNodes();
    for (const ProjectExplorer::FileNode *fileNode : fileNodes) {
        if (mimeType.suffixes().contains(fileNode->filePath().completeSuffix()))
            return fileNode->filePath().toString();
    }
    const QList<ProjectExplorer::FolderNode *> subFolderNodes = folderNode->folderNodes();
    for (ProjectExplorer::FolderNode *subFolderNode : subFolderNodes) {
        QString modelFileName = findFirstModel(subFolderNode, mimeType);
        if (!modelFileName.isEmpty())
            return modelFileName;
    }
    return QString();
}

void ModelIndexer::forgetProject(ProjectExplorer::Project *project)
{
    const Utils::FilePaths files = project->files(ProjectExplorer::Project::SourceFiles);

    QMutexLocker locker(&d->indexerMutex);
    for (const Utils::FilePath &file : files) {
        const QString fileString = file.toString();
        // remove file from queue
        QueuedFile queuedFile(fileString, project);
        if (d->queuedFilesSet.contains(queuedFile)) {
            QMT_CHECK(d->filesQueue.contains(queuedFile));
            d->filesQueue.removeOne(queuedFile);
            QMT_CHECK(!d->filesQueue.contains(queuedFile));
            d->queuedFilesSet.remove(queuedFile);
        }
        removeModelFile(fileString, project);
        removeDiagramReferenceFile(fileString, project);
    }
}

void ModelIndexer::removeModelFile(const QString &file, ProjectExplorer::Project *project)
{
    IndexedModel *indexedModel = d->indexedModels.value(file);
    if (indexedModel && indexedModel->owningProjects().contains(project)) {
        qCDebug(logger) << "remove model file " << file
                        << " from project " << project->displayName();
        indexedModel->removeOwningProject(project);
        if (indexedModel->owningProjects().isEmpty()) {
            qCDebug(logger) << "delete indexed model " << project->displayName();
            d->indexedModels.remove(file);

            // remove indexedModel from set of indexedModelsByUid
            QMT_CHECK(d->indexedModelsByUid.contains(indexedModel->modelUid()));
            QSet<IndexedModel *> indexedModels = d->indexedModelsByUid.value(indexedModel->modelUid());
            QMT_CHECK(indexedModels.contains(indexedModel));
            indexedModels.remove(indexedModel);
            if (indexedModels.isEmpty())
                d->indexedModelsByUid.remove(indexedModel->modelUid());
            else
                d->indexedModelsByUid.insert(indexedModel->modelUid(), indexedModels);

            delete indexedModel;
        }
    }
}

void ModelIndexer::removeDiagramReferenceFile(const QString &file,
                                              ProjectExplorer::Project *project)
{
    IndexedDiagramReference *indexedDiagramReference = d->indexedDiagramReferences.value(file);
    if (indexedDiagramReference) {
        QMT_CHECK(indexedDiagramReference->owningProjects().contains(project));
        qCDebug(logger) << "remove diagram reference file "
                        << file << " from project " << project->displayName();
        indexedDiagramReference->removeOwningProject(project);
        if (indexedDiagramReference->owningProjects().isEmpty()) {
            qCDebug(logger) << "delete indexed diagram reference from " << file;
            d->indexedDiagramReferences.remove(file);

            // remove indexedDiagramReference from set of indexedDiagramReferecesByDiagramUid
            QMT_CHECK(d->indexedDiagramReferencesByDiagramUid.contains(indexedDiagramReference->diagramUid()));
            QSet<IndexedDiagramReference *> indexedDiagramReferences = d->indexedDiagramReferencesByDiagramUid.value(indexedDiagramReference->diagramUid());
            QMT_CHECK(indexedDiagramReferences.contains(indexedDiagramReference));
            indexedDiagramReferences.remove(indexedDiagramReference);
            if (indexedDiagramReferences.isEmpty()) {
                d->indexedDiagramReferencesByDiagramUid.remove(
                            indexedDiagramReference->diagramUid());
            } else {
                d->indexedDiagramReferencesByDiagramUid.insert(
                            indexedDiagramReference->diagramUid(), indexedDiagramReferences);
            }

            delete indexedDiagramReference;
        }
    }
}

const QLoggingCategory &ModelIndexer::logger()
{
    static const QLoggingCategory category("qtc.modeleditor.modelindexer", QtWarningMsg);
    return category;
}

} // namespace Internal
} // namespace ModelEditor
