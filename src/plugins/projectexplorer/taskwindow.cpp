// Copyright (C) 2016 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#include "taskwindow.h"

#include "itaskhandler.h"
#include "projectexplorericons.h"
#include "projectexplorertr.h"
#include "session.h"
#include "task.h"
#include "taskhub.h"
#include "taskmodel.h"

#include <aggregation/aggregate.h>

#include <coreplugin/actionmanager/actionmanager.h>
#include <coreplugin/actionmanager/command.h>
#include <coreplugin/editormanager/editormanager.h>
#include <coreplugin/find/itemviewfind.h>
#include <coreplugin/icore.h>
#include <coreplugin/icontext.h>

#include <utils/algorithm.h>
#include <utils/fileinprojectfinder.h>
#include <utils/itemviews.h>
#include <utils/outputformatter.h>
#include <utils/qtcassert.h>
#include <utils/stylehelper.h>
#include <utils/theme/theme.h>
#include <utils/tooltip/tooltip.h>
#include <utils/utilsicons.h>

#include <QDir>
#include <QLabel>
#include <QMenu>
#include <QPainter>
#include <QScrollBar>
#include <QStyledItemDelegate>
#include <QToolButton>
#include <QVBoxLayout>

using namespace Utils;

namespace {
const int ELLIPSIS_GRADIENT_WIDTH = 16;
const char SESSION_FILTER_CATEGORIES[] = "TaskWindow.Categories";
const char SESSION_FILTER_WARNINGS[] = "TaskWindow.IncludeWarnings";
}

namespace ProjectExplorer {

static QList<ITaskHandler *> g_taskHandlers;

ITaskHandler::ITaskHandler(bool isMultiHandler) : m_isMultiHandler(isMultiHandler)
{
    g_taskHandlers.append(this);
}

ITaskHandler::~ITaskHandler()
{
    g_taskHandlers.removeOne(this);
}

void ITaskHandler::handle(const Task &task)
{
    QTC_ASSERT(m_isMultiHandler, return);
    handle(Tasks{task});
}

void ITaskHandler::handle(const Tasks &tasks)
{
    QTC_ASSERT(canHandle(tasks), return);
    QTC_ASSERT(!m_isMultiHandler, return);
    handle(tasks.first());
}

bool ITaskHandler::canHandle(const Tasks &tasks) const
{
    if (tasks.isEmpty())
        return false;
    if (m_isMultiHandler)
        return true;
    if (tasks.size() > 1)
        return false;
    return canHandle(tasks.first());
}

namespace Internal {

class TaskView : public ListView
{
public:
    TaskView(QWidget *parent = nullptr);
    ~TaskView() override;

private:
    void resizeEvent(QResizeEvent *e) override;
    void keyReleaseEvent(QKeyEvent *e) override;
    bool event(QEvent *e) override;

    void showToolTip(const Task &task, const QPoint &pos);
};

class TaskDelegate : public QStyledItemDelegate
{
    Q_OBJECT

    friend class TaskView; // for using Positions::minimumSize()

public:
    TaskDelegate(QObject * parent = nullptr);
    ~TaskDelegate() override;
    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override;
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override;

    // TaskView uses this method if the size of the taskview changes
    void emitSizeHintChanged(const QModelIndex &index);

private:
    void generateGradientPixmap(int width, int height, QColor color, bool selected) const;

    mutable int m_cachedHeight = 0;
    mutable QFont m_cachedFont;

    /*
      +------------------------------------------------------------------------------------------+
      | TASKICONAREA  TEXTAREA                                                 FILEAREA LINEAREA |
      +------------------------------------------------------------------------------------------+
     */
    class Positions
    {
    public:
        Positions(const QStyleOptionViewItem &options, TaskModel *model) :
            m_totalWidth(options.rect.width()),
            m_maxFileLength(model->sizeOfFile(options.font)),
            m_maxLineLength(model->sizeOfLineNumber(options.font)),
            m_realFileLength(m_maxFileLength),
            m_top(options.rect.top()),
            m_bottom(options.rect.bottom())
        {
            int flexibleArea = lineAreaLeft() - textAreaLeft() - ITEM_SPACING;
            if (m_maxFileLength > flexibleArea / 2)
                m_realFileLength = flexibleArea / 2;
            m_fontHeight = QFontMetrics(options.font).height();
        }

        int top() const { return m_top + ITEM_MARGIN; }
        int left() const { return ITEM_MARGIN; }
        int right() const { return m_totalWidth - ITEM_MARGIN; }
        int bottom() const { return m_bottom; }
        int firstLineHeight() const { return m_fontHeight + 1; }
        static int minimumHeight() { return taskIconHeight() + 2 * ITEM_MARGIN; }

        int taskIconLeft() const { return left(); }
        static int taskIconWidth() { return TASK_ICON_SIZE; }
        static int taskIconHeight() { return TASK_ICON_SIZE; }
        int taskIconRight() const { return taskIconLeft() + taskIconWidth(); }
        QRect taskIcon() const { return QRect(taskIconLeft(), top(), taskIconWidth(), taskIconHeight()); }

        int textAreaLeft() const { return taskIconRight() + ITEM_SPACING; }
        int textAreaWidth() const { return textAreaRight() - textAreaLeft(); }
        int textAreaRight() const { return fileAreaLeft() - ITEM_SPACING; }
        QRect textArea() const { return QRect(textAreaLeft(), top(), textAreaWidth(), firstLineHeight()); }

        int fileAreaLeft() const { return fileAreaRight() - fileAreaWidth(); }
        int fileAreaWidth() const { return m_realFileLength; }
        int fileAreaRight() const { return lineAreaLeft() - ITEM_SPACING; }
        QRect fileArea() const { return QRect(fileAreaLeft(), top(), fileAreaWidth(), firstLineHeight()); }

        int lineAreaLeft() const { return lineAreaRight() - lineAreaWidth(); }
        int lineAreaWidth() const { return m_maxLineLength; }
        int lineAreaRight() const { return right(); }
        QRect lineArea() const { return QRect(lineAreaLeft(), top(), lineAreaWidth(), firstLineHeight()); }

    private:
        int m_totalWidth;
        int m_maxFileLength;
        int m_maxLineLength;
        int m_realFileLength;
        int m_top;
        int m_bottom;
        int m_fontHeight;

        static const int TASK_ICON_SIZE = 16;
        static const int ITEM_MARGIN = 2;
        static const int ITEM_SPACING = 2 * ITEM_MARGIN;
    };
};

TaskView::TaskView(QWidget *parent)
    : ListView(parent)
{
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    setAutoScroll(false); // QTCREATORBUG-25101
    setUniformItemSizes(true);

    QFontMetrics fm(font());
    int vStepSize = fm.height() + 3;
    if (vStepSize < TaskDelegate::Positions::minimumHeight())
        vStepSize = TaskDelegate::Positions::minimumHeight();

    verticalScrollBar()->setSingleStep(vStepSize);
}

TaskView::~TaskView() = default;

void TaskView::resizeEvent(QResizeEvent *e)
{
    Q_UNUSED(e)
    static_cast<TaskDelegate *>(itemDelegate())->emitSizeHintChanged(selectionModel()->currentIndex());
}

void TaskView::keyReleaseEvent(QKeyEvent *e)
{
    ListView::keyReleaseEvent(e);
    if (e->key() == Qt::Key_Space) {
        const Task task = static_cast<TaskFilterModel *>(model())->task(currentIndex());
        if (!task.isNull()) {
            const QPoint toolTipPos = mapToGlobal(visualRect(currentIndex()).topLeft());
            QMetaObject::invokeMethod(this, [this, task, toolTipPos] {
                    showToolTip(task, toolTipPos); }, Qt::QueuedConnection);
        }
    }
}

bool TaskView::event(QEvent *e)
{
    if (e->type() != QEvent::ToolTip)
        return QListView::event(e);

    const auto helpEvent = static_cast<QHelpEvent*>(e);
    const Task task = static_cast<TaskFilterModel *>(model())->task(indexAt(helpEvent->pos()));
    if (task.isNull())
        return QListView::event(e);
    showToolTip(task, helpEvent->globalPos());
    e->accept();
    return true;
}

void TaskView::showToolTip(const Task &task, const QPoint &pos)
{
    const QString toolTip = task.toolTip();
    if (!toolTip.isEmpty()) {
        const auto label = new QLabel(toolTip);
        connect(label, &QLabel::linkActivated, [](const QString &link) {
            Core::EditorManager::openEditorAt(OutputLineParser::parseLinkTarget(link), {},
                                              Core::EditorManager::SwitchSplitIfAlreadyVisible);
        });
        const auto layout = new QVBoxLayout;
        layout->setContentsMargins(0, 0, 0, 0);
        layout->addWidget(label);
        ToolTip::show(pos, layout);
    } else {
        ToolTip::hideImmediately();
    }
}

/////
// TaskWindow
/////

class TaskWindowPrivate
{
public:
    ITaskHandler *handler(const QAction *action)
    {
        ITaskHandler *handler = m_actionToHandlerMap.value(action, nullptr);
        return g_taskHandlers.contains(handler) ? handler : nullptr;
    }

    Internal::TaskModel *m_model;
    Internal::TaskFilterModel *m_filter;
    Internal::TaskView *m_listview;
    Core::IContext *m_taskWindowContext;
    QMenu *m_contextMenu;
    QMap<const QAction *, ITaskHandler *> m_actionToHandlerMap;
    ITaskHandler *m_defaultHandler = nullptr;
    QToolButton *m_filterWarningsButton;
    QToolButton *m_categoriesButton;
    QMenu *m_categoriesMenu;
    QList<QAction *> m_actions;
    int m_visibleIssuesCount = 0;
};

static QToolButton *createFilterButton(const QIcon &icon, const QString &toolTip,
                                       QObject *receiver, std::function<void(bool)> lambda)
{
    auto button = new QToolButton;
    button->setIcon(icon);
    button->setToolTip(toolTip);
    button->setCheckable(true);
    button->setChecked(true);
    button->setEnabled(true);
    QObject::connect(button, &QToolButton::toggled, receiver, lambda);
    return button;
}

TaskWindow::TaskWindow() : d(std::make_unique<TaskWindowPrivate>())
{
    d->m_model = new Internal::TaskModel(this);
    d->m_filter = new Internal::TaskFilterModel(d->m_model);
    d->m_listview = new Internal::TaskView;

    auto agg = new Aggregation::Aggregate;
    agg->add(d->m_listview);
    agg->add(new Core::ItemViewFind(d->m_listview, TaskModel::Description));

    d->m_listview->setModel(d->m_filter);
    d->m_listview->setFrameStyle(QFrame::NoFrame);
    d->m_listview->setWindowTitle(displayName());
    d->m_listview->setSelectionMode(QAbstractItemView::ExtendedSelection);
    auto *tld = new Internal::TaskDelegate(this);
    d->m_listview->setItemDelegate(tld);
    d->m_listview->setWindowIcon(Icons::WINDOW.icon());
    d->m_listview->setContextMenuPolicy(Qt::ActionsContextMenu);
    d->m_listview->setAttribute(Qt::WA_MacShowFocusRect, false);

    d->m_taskWindowContext = new Core::IContext(d->m_listview);
    d->m_taskWindowContext->setWidget(d->m_listview);
    d->m_taskWindowContext->setContext(Core::Context(Core::Constants::C_PROBLEM_PANE));
    Core::ICore::addContextObject(d->m_taskWindowContext);

    connect(d->m_listview->selectionModel(), &QItemSelectionModel::currentChanged,
            this, [this](const QModelIndex &index) { d->m_listview->scrollTo(index); });
    connect(d->m_listview, &QAbstractItemView::activated,
            this, &TaskWindow::triggerDefaultHandler);
    connect(d->m_listview->selectionModel(), &QItemSelectionModel::selectionChanged,
            this, [this] {
        const Tasks tasks = d->m_filter->tasks(d->m_listview->selectionModel()->selectedIndexes());
        for (QAction * const action : std::as_const(d->m_actions)) {
            ITaskHandler * const h = d->handler(action);
            action->setEnabled(h && h->canHandle(tasks));
        }
    });

    d->m_contextMenu = new QMenu(d->m_listview);

    d->m_listview->setContextMenuPolicy(Qt::ActionsContextMenu);

    d->m_filterWarningsButton = createFilterButton(
                Utils::Icons::WARNING_TOOLBAR.icon(),
                Tr::tr("Show Warnings"), this, [this](bool show) { setShowWarnings(show); });

    d->m_categoriesButton = new QToolButton;
    d->m_categoriesButton->setIcon(Utils::Icons::FILTER.icon());
    d->m_categoriesButton->setToolTip(Tr::tr("Filter by categories"));
    d->m_categoriesButton->setProperty("noArrow", true);
    d->m_categoriesButton->setPopupMode(QToolButton::InstantPopup);

    d->m_categoriesMenu = new QMenu(d->m_categoriesButton);
    connect(d->m_categoriesMenu, &QMenu::aboutToShow, this, &TaskWindow::updateCategoriesMenu);

    d->m_categoriesButton->setMenu(d->m_categoriesMenu);

    setupFilterUi("IssuesPane.Filter");
    setFilteringEnabled(true);

    TaskHub *hub = TaskHub::instance();
    connect(hub, &TaskHub::categoryAdded, this, &TaskWindow::addCategory);
    connect(hub, &TaskHub::taskAdded, this, &TaskWindow::addTask);
    connect(hub, &TaskHub::taskRemoved, this, &TaskWindow::removeTask);
    connect(hub, &TaskHub::taskLineNumberUpdated, this, &TaskWindow::updatedTaskLineNumber);
    connect(hub, &TaskHub::taskFileNameUpdated, this, &TaskWindow::updatedTaskFileName);
    connect(hub, &TaskHub::tasksCleared, this, &TaskWindow::clearTasks);
    connect(hub, &TaskHub::categoryVisibilityChanged, this, &TaskWindow::setCategoryVisibility);
    connect(hub, &TaskHub::popupRequested, this, &TaskWindow::popup);
    connect(hub, &TaskHub::showTask, this, &TaskWindow::showTask);
    connect(hub, &TaskHub::openTask, this, &TaskWindow::openTask);

    connect(d->m_filter, &TaskFilterModel::rowsAboutToBeRemoved,
            [this](const QModelIndex &, int first, int last) {
        d->m_visibleIssuesCount -= d->m_filter->issuesCount(first, last);
        emit setBadgeNumber(d->m_visibleIssuesCount);
    });
    connect(d->m_filter, &TaskFilterModel::rowsInserted,
            [this](const QModelIndex &, int first, int last) {
        d->m_visibleIssuesCount += d->m_filter->issuesCount(first, last);
        emit setBadgeNumber(d->m_visibleIssuesCount);
    });
    connect(d->m_filter, &TaskFilterModel::modelReset, [this] {
        d->m_visibleIssuesCount = d->m_filter->issuesCount(0, d->m_filter->rowCount());
        emit setBadgeNumber(d->m_visibleIssuesCount);
    });

    SessionManager *session = SessionManager::instance();
    connect(session, &SessionManager::aboutToSaveSession, this, &TaskWindow::saveSettings);
    connect(session, &SessionManager::sessionLoaded, this, &TaskWindow::loadSettings);
}

TaskWindow::~TaskWindow()
{
    delete d->m_filterWarningsButton;
    delete d->m_listview;
    delete d->m_filter;
    delete d->m_model;
}

void TaskWindow::delayedInitialization()
{
    static bool alreadyDone = false;
    if (alreadyDone)
        return;

    alreadyDone = true;

    for (ITaskHandler *h : std::as_const(g_taskHandlers)) {
        if (h->isDefaultHandler() && !d->m_defaultHandler)
            d->m_defaultHandler = h;

        QAction *action = h->createAction(this);
        action->setEnabled(false);
        QTC_ASSERT(action, continue);
        d->m_actionToHandlerMap.insert(action, h);
        connect(action, &QAction::triggered, this, [this, action] {
            ITaskHandler *h = d->handler(action);
            if (h)
                h->handle(d->m_filter->tasks(d->m_listview->selectionModel()->selectedIndexes()));
        });
        d->m_actions << action;

        Id id = h->actionManagerId();
        if (id.isValid()) {
            Core::Command *cmd =
                Core::ActionManager::registerAction(action, id, d->m_taskWindowContext->context(), true);
            action = cmd->action();
        }
        d->m_listview->addAction(action);
    }
}

QList<QWidget*> TaskWindow::toolBarWidgets() const
{
    return {d->m_filterWarningsButton, d->m_categoriesButton, filterWidget()};
}

QString TaskWindow::displayName() const
{
    return Tr::tr("Issues");
}

QWidget *TaskWindow::outputWidget(QWidget *)
{
    return d->m_listview;
}

void TaskWindow::clearTasks(Id categoryId)
{
    d->m_model->clearTasks(categoryId);

    emit tasksChanged();
    navigateStateChanged();
}

void TaskWindow::setCategoryVisibility(Id categoryId, bool visible)
{
    if (!categoryId.isValid())
        return;

    QList<Id> categories = d->m_filter->filteredCategories();

    if (visible)
        categories.removeOne(categoryId);
    else
        categories.append(categoryId);

    d->m_filter->setFilteredCategories(categories);
}

void TaskWindow::saveSettings()
{
    QStringList categories = Utils::transform(d->m_filter->filteredCategories(), &Id::toString);
    SessionManager::setValue(QLatin1String(SESSION_FILTER_CATEGORIES), categories);
    SessionManager::setValue(QLatin1String(SESSION_FILTER_WARNINGS), d->m_filter->filterIncludesWarnings());
}

void TaskWindow::loadSettings()
{
    QVariant value = SessionManager::value(QLatin1String(SESSION_FILTER_CATEGORIES));
    if (value.isValid()) {
        QList<Id> categories = Utils::transform(value.toStringList(), &Id::fromString);
        d->m_filter->setFilteredCategories(categories);
    }
    value = SessionManager::value(QLatin1String(SESSION_FILTER_WARNINGS));
    if (value.isValid()) {
        bool includeWarnings = value.toBool();
        d->m_filter->setFilterIncludesWarnings(includeWarnings);
        d->m_filterWarningsButton->setChecked(d->m_filter->filterIncludesWarnings());
    }
}

void TaskWindow::visibilityChanged(bool visible)
{
    if (visible)
        delayedInitialization();
}

void TaskWindow::addCategory(Id categoryId, const QString &displayName, bool visible, int priority)
{
    d->m_model->addCategory(categoryId, displayName, priority);
    if (!visible) {
        QList<Id> filters = d->m_filter->filteredCategories();
        filters += categoryId;
        d->m_filter->setFilteredCategories(filters);
    }
}

void TaskWindow::addTask(const Task &task)
{
    d->m_model->addTask(task);

    emit tasksChanged();
    navigateStateChanged();

    if ((task.options & Task::FlashWorthy)
         && task.type == Task::Error
         && d->m_filter->filterIncludesErrors()
         && !d->m_filter->filteredCategories().contains(task.category)) {
        flash();
    }
}

void TaskWindow::removeTask(const Task &task)
{
    d->m_model->removeTask(task.taskId);

    emit tasksChanged();
    navigateStateChanged();
}

void TaskWindow::updatedTaskFileName(const Task &task, const QString &fileName)
{
    d->m_model->updateTaskFileName(task, fileName);
    emit tasksChanged();
}

void TaskWindow::updatedTaskLineNumber(const Task &task, int line)
{
    d->m_model->updateTaskLineNumber(task, line);
    emit tasksChanged();
}

void TaskWindow::showTask(const Task &task)
{
    int sourceRow = d->m_model->rowForTask(task);
    QModelIndex sourceIdx = d->m_model->index(sourceRow, 0);
    QModelIndex filterIdx = d->m_filter->mapFromSource(sourceIdx);
    d->m_listview->setCurrentIndex(filterIdx);
    popup(Core::IOutputPane::ModeSwitch);
}

void TaskWindow::openTask(const Task &task)
{
    int sourceRow = d->m_model->rowForTask(task);
    QModelIndex sourceIdx = d->m_model->index(sourceRow, 0);
    QModelIndex filterIdx = d->m_filter->mapFromSource(sourceIdx);
    triggerDefaultHandler(filterIdx);
}

void TaskWindow::triggerDefaultHandler(const QModelIndex &index)
{
    if (!index.isValid() || !d->m_defaultHandler)
        return;

    Task task(d->m_filter->task(index));
    if (task.isNull())
        return;

    if (!task.file.isEmpty() && !task.file.toFileInfo().isAbsolute()
            && !task.fileCandidates.empty()) {
        const FilePath userChoice = Utils::chooseFileFromList(task.fileCandidates);
        if (!userChoice.isEmpty()) {
            task.file = userChoice;
            updatedTaskFileName(task, task.file.toString());
        }
    }

    if (d->m_defaultHandler->canHandle(task)) {
        d->m_defaultHandler->handle(task);
    } else {
        if (!task.file.exists())
            d->m_model->setFileNotFound(index, true);
    }
}

void TaskWindow::setShowWarnings(bool show)
{
    d->m_filter->setFilterIncludesWarnings(show);
}

void TaskWindow::updateCategoriesMenu()
{
    using NameToIdsConstIt = QMap<QString, Id>::ConstIterator;

    d->m_categoriesMenu->clear();

    const QList<Id> filteredCategories = d->m_filter->filteredCategories();

    QMap<QString, Id> nameToIds;
    const QList<Id> ids = d->m_model->categoryIds();
    for (const Id categoryId : ids)
        nameToIds.insert(d->m_model->categoryDisplayName(categoryId), categoryId);

    const NameToIdsConstIt cend = nameToIds.constEnd();
    for (NameToIdsConstIt it = nameToIds.constBegin(); it != cend; ++it) {
        const QString &displayName = it.key();
        const Id categoryId = it.value();
        auto action = new QAction(d->m_categoriesMenu);
        action->setCheckable(true);
        action->setText(displayName);
        action->setChecked(!filteredCategories.contains(categoryId));
        connect(action, &QAction::triggered, this, [this, action, categoryId] {
            setCategoryVisibility(categoryId, action->isChecked());
        });
        d->m_categoriesMenu->addAction(action);
    }
}

int TaskWindow::taskCount(Id category) const
{
    return d->m_model->taskCount(category);
}

int TaskWindow::errorTaskCount(Id category) const
{
    return d->m_model->errorTaskCount(category);
}

int TaskWindow::warningTaskCount(Id category) const
{
    return d->m_model->warningTaskCount(category);
}

int TaskWindow::priorityInStatusBar() const
{
    return 90;
}

void TaskWindow::clearContents()
{
    // clear all tasks in all displays
    // Yeah we are that special
    TaskHub::clearTasks();
}

bool TaskWindow::hasFocus() const
{
    return d->m_listview->window()->focusWidget() == d->m_listview;
}

bool TaskWindow::canFocus() const
{
    return d->m_filter->rowCount();
}

void TaskWindow::setFocus()
{
    if (d->m_filter->rowCount()) {
        d->m_listview->setFocus();
        if (d->m_listview->currentIndex() == QModelIndex())
            d->m_listview->setCurrentIndex(d->m_filter->index(0,0, QModelIndex()));
    }
}

bool TaskWindow::canNext() const
{
    return d->m_filter->rowCount();
}

bool TaskWindow::canPrevious() const
{
    return d->m_filter->rowCount();
}

void TaskWindow::goToNext()
{
    if (!canNext())
        return;
    QModelIndex startIndex = d->m_listview->currentIndex();
    QModelIndex currentIndex = startIndex;

    if (startIndex.isValid()) {
        do {
            int row = currentIndex.row() + 1;
            if (row == d->m_filter->rowCount())
                row = 0;
            currentIndex = d->m_filter->index(row, 0);
            if (d->m_filter->hasFile(currentIndex))
                break;
        } while (startIndex != currentIndex);
    } else {
        currentIndex = d->m_filter->index(0, 0);
    }
    d->m_listview->setCurrentIndex(currentIndex);
    triggerDefaultHandler(currentIndex);
}

void TaskWindow::goToPrev()
{
    if (!canPrevious())
        return;
    QModelIndex startIndex = d->m_listview->currentIndex();
    QModelIndex currentIndex = startIndex;

    if (startIndex.isValid()) {
        do {
            int row = currentIndex.row() - 1;
            if (row < 0)
                row = d->m_filter->rowCount() - 1;
            currentIndex = d->m_filter->index(row, 0);
            if (d->m_filter->hasFile(currentIndex))
                break;
        } while (startIndex != currentIndex);
    } else {
        currentIndex = d->m_filter->index(0, 0);
    }
    d->m_listview->setCurrentIndex(currentIndex);
    triggerDefaultHandler(currentIndex);
}

void TaskWindow::updateFilter()
{
    d->m_filter->updateFilterProperties(filterText(), filterCaseSensitivity(), filterUsesRegexp(),
                                        filterIsInverted());
}

bool TaskWindow::canNavigate() const
{
    return true;
}

/////
// Delegate
/////

TaskDelegate::TaskDelegate(QObject *parent) :
    QStyledItemDelegate(parent)
{ }

TaskDelegate::~TaskDelegate() = default;

QSize TaskDelegate::sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    QStyleOptionViewItem opt = option;
    initStyleOption(&opt, index);

    QSize s;
    s.setWidth(option.rect.width());

    if (option.font == m_cachedFont && m_cachedHeight > 0) {
        s.setHeight(m_cachedHeight);
        return s;
    }

    s.setHeight(option.fontMetrics.height() + 3);
    if (s.height() < Positions::minimumHeight())
        s.setHeight(Positions::minimumHeight());
    m_cachedHeight = s.height();
    m_cachedFont = option.font;

    return s;
}

void TaskDelegate::emitSizeHintChanged(const QModelIndex &index)
{
    emit sizeHintChanged(index);
}

void TaskDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    QStyleOptionViewItem opt = option;
    initStyleOption(&opt, index);
    painter->save();

    QFontMetrics fm(opt.font);
    QColor backgroundColor;
    QColor textColor;

    auto view = qobject_cast<const QAbstractItemView *>(opt.widget);
    const bool selected = view->selectionModel()->isSelected(index);

    if (selected) {
        painter->setBrush(opt.palette.highlight().color());
        backgroundColor = opt.palette.highlight().color();
    } else {
        painter->setBrush(opt.palette.window().color());
        backgroundColor = opt.palette.window().color();
    }
    painter->setPen(Qt::NoPen);
    painter->drawRect(opt.rect);

    // Set Text Color
    if (selected)
        textColor = opt.palette.highlightedText().color();
    else
        textColor = opt.palette.text().color();

    painter->setPen(textColor);

    auto model = static_cast<TaskFilterModel *>(view->model())->taskModel();
    Positions positions(opt, model);

    // Paint TaskIconArea:
    QIcon icon = index.data(TaskModel::Icon).value<QIcon>();
    painter->drawPixmap(positions.left(), positions.top(),
                        icon.pixmap(Positions::taskIconWidth(), Positions::taskIconHeight()));

    // Paint TextArea:
    QString bottom = index.data(TaskModel::Description).toString().split(QLatin1Char('\n')).first();
    painter->setClipRect(positions.textArea());
    painter->drawText(positions.textAreaLeft(), positions.top() + fm.ascent(), bottom);
    if (fm.horizontalAdvance(bottom) > positions.textAreaWidth()) {
        // draw a gradient to mask the text
        int gradientStart = positions.textAreaRight() - ELLIPSIS_GRADIENT_WIDTH + 1;
        QLinearGradient lg(gradientStart, 0, gradientStart + ELLIPSIS_GRADIENT_WIDTH, 0);
        lg.setColorAt(0, Qt::transparent);
        lg.setColorAt(1, backgroundColor);
        painter->fillRect(gradientStart, positions.top(), ELLIPSIS_GRADIENT_WIDTH, positions.firstLineHeight(), lg);
    }

    // Paint FileArea
    QString file = index.data(TaskModel::File).toString();
    const int pos = file.lastIndexOf(QLatin1Char('/'));
    if (pos != -1)
        file = file.mid(pos +1);
    const int realFileWidth = fm.horizontalAdvance(file);
    painter->setClipRect(positions.fileArea());
    painter->drawText(qMin(positions.fileAreaLeft(), positions.fileAreaRight() - realFileWidth),
                      positions.top() + fm.ascent(), file);
    if (realFileWidth > positions.fileAreaWidth()) {
        // draw a gradient to mask the text
        int gradientStart = positions.fileAreaLeft() - 1;
        QLinearGradient lg(gradientStart + ELLIPSIS_GRADIENT_WIDTH, 0, gradientStart, 0);
        lg.setColorAt(0, Qt::transparent);
        lg.setColorAt(1, backgroundColor);
        painter->fillRect(gradientStart, positions.top(), ELLIPSIS_GRADIENT_WIDTH, positions.firstLineHeight(), lg);
    }

    // Paint LineArea
    int line = index.data(TaskModel::Line).toInt();
    int movedLine = index.data(TaskModel::MovedLine).toInt();
    QString lineText;

    if (line == -1) {
        // No line information at all
    } else if (movedLine == -1) {
        // removed the line, but we had line information, show the line in ()
        QFont f = painter->font();
        f.setItalic(true);
        painter->setFont(f);
        lineText = QLatin1Char('(') + QString::number(line) + QLatin1Char(')');
    }  else if (movedLine != line) {
        // The line was moved
        QFont f = painter->font();
        f.setItalic(true);
        painter->setFont(f);
        lineText = QString::number(movedLine);
    } else {
        lineText = QString::number(line);
    }

    painter->setClipRect(positions.lineArea());
    const int realLineWidth = fm.horizontalAdvance(lineText);
    painter->drawText(positions.lineAreaRight() - realLineWidth, positions.top() + fm.ascent(), lineText);
    painter->setClipRect(opt.rect);

    // Separator lines
    painter->setPen(QColor::fromRgb(150,150,150));
    const QRectF borderRect = QRectF(opt.rect).adjusted(0.5, 0.5, -0.5, -0.5);
    painter->drawLine(borderRect.bottomLeft(), borderRect.bottomRight());
    painter->restore();
}

} // namespace Internal
} // namespace ProjectExplorer

#include "taskwindow.moc"
