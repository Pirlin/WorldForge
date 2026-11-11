#include "App/MainWindow.h"

#include "Components/EditorPanels.h"
#include "Components/Viewport.h"
#include "DesignSystem/Icons.h"
#include "Workspaces/StudioWorkspace.h"
#include "Workspaces/WorkspaceCatalog.h"

#include <QActionGroup>
#include <QCloseEvent>
#include <QCursor>
#include <QDockWidget>
#include <QFontMetrics>
#include <QFrame>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QMenuBar>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QProgressBar>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QStandardItemModel>
#include <QStatusBar>
#include <QTabBar>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QWindow>

#include <algorithm>
#include <array>

#ifdef Q_OS_WIN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#endif

namespace {
constexpr int World = 1;
constexpr int LayoutVersion = 2;
constexpr int StudioLayoutVersion = 3;
constexpr int ProjectIdRole = Qt::UserRole + 1;

class WorkspaceStack : public QStackedWidget {
public:
    using QStackedWidget::QStackedWidget;

    QSize sizeHint() const override
    {
        return currentWidget() ? currentWidget()->sizeHint() : QSize();
    }

    QSize minimumSizeHint() const override
    {
        return currentWidget() ? currentWidget()->minimumSizeHint().expandedTo(currentWidget()->minimumSize()) : QSize();
    }
};

QString workspaceLayoutKey(int index)
{
    return "window/workspaceLayouts/" + workspaces().at(index).name;
}

int workspaceLayoutVersion(int index)
{
    return index == World ? LayoutVersion : StudioLayoutVersion;
}

class WorkspaceTabs : public QTabBar {
public:
    using QTabBar::QTabBar;

protected:
    QSize tabSizeHint(int index) const override
    {
        auto labelFont = font();
        labelFont.setWeight(QFont::DemiBold);
        const int padding = index == World ? 38 : 34;
        return {QFontMetrics(labelFont).horizontalAdvance(tabText(index)) + padding, 34};
    }

    QSize minimumTabSizeHint(int index) const override { return tabSizeHint(index); }

    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setFont(font());
        const auto hover = mapFromGlobal(QCursor::pos());
        for (int index = 0; index < count(); ++index) {
            if (!isTabVisible(index))
                continue;
            const auto tab = tabRect(index);
            const auto surface = QRectF(tab).adjusted(0.5, 3.5, index == World ? -8.5 : -3.5, -3.5);
            const bool selected = index == currentIndex();
            if (selected) {
                QLinearGradient highlight(surface.topLeft(), surface.bottomLeft());
                highlight.setColorAt(0, QColor("#6627cb"));
                highlight.setColorAt(0.6, QColor("#4914aa"));
                highlight.setColorAt(1, QColor("#5820be"));
                painter.setPen(QPen(QColor("#ad59f6"), 1));
                painter.setBrush(highlight);
                painter.drawRoundedRect(surface, 5, 5);
            } else if (tab.contains(hover)) {
                painter.setPen(Qt::NoPen);
                painter.setBrush(QColor("#252a38"));
                painter.drawRoundedRect(surface, 4, 4);
            }

            const int inset = index == World ? 9 : 7;
            tabIcon(index).paint(&painter, QRect(tab.left() + inset, tab.center().y() - 7, 15, 15));
            painter.setPen(selected ? Qt::white : QColor("#e9e9ef"));
            auto font = this->font();
            font.setWeight(selected ? QFont::DemiBold : QFont::Normal);
            painter.setFont(font);
            const int textX = tab.left() + inset + 20;
            painter.drawText(QRect(textX, tab.top(), tab.right() - textX, tab.height()),
                             Qt::AlignVCenter | Qt::AlignLeft, tabText(index));
        }
    }
};

class TitleBar : public QWidget {
public:
    using QWidget::QWidget;

protected:
    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton && window()->windowHandle())
            window()->windowHandle()->startSystemMove();
    }

    void mouseDoubleClickEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton)
            window()->isMaximized() ? window()->showNormal() : window()->showMaximized();
    }
};

QLabel *iconLabel(const QString &icon, QWidget *parent, const QColor &color = QColor("#c5cde2"), int size = 16)
{
    auto *label = new QLabel(parent);
    label->setPixmap(editorIcon(icon, color).pixmap(size));
    return label;
}

QPushButton *actionButton(QAction *action, QWidget *parent)
{
    auto *button = new QPushButton(parent);
    auto sync = [action, button] {
        button->setText(action->text().remove('&'));
        button->setIcon(action->icon());
        button->setEnabled(action->isEnabled());
    };
    QObject::connect(action, &QAction::changed, button, sync);
    QObject::connect(button, &QPushButton::clicked, action, &QAction::trigger);
    sync();
    return button;
}

class HomeHero : public QWidget {
public:
    explicit HomeHero(QWidget *parent)
        : QWidget(parent), art_(workspaceArt(worldWorkspace()).copy(0, 0, 620, 420))
    {
        setMinimumHeight(275);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHints(QPainter::SmoothPixmapTransform | QPainter::Antialiasing);
        QPainterPath clip;
        const auto bounds = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        clip.addRoundedRect(bounds, 9, 9);
        painter.setClipPath(clip);
        painter.fillRect(rect(), QColor("#18213a"));
        if (!art_.isNull()) {
            const qreal scale = std::min(qreal(art_.width()) / width(), qreal(art_.height()) / height());
            const QSizeF size(width() * scale, height() * scale);
            painter.drawPixmap(QRectF(rect()), art_,
                QRectF(QPointF((art_.width() - size.width()) / 2, (art_.height() - size.height()) * 0.37), size));
        }
        QLinearGradient gradient(0, 0, width(), 0);
        gradient.setColorAt(0, QColor(12, 16, 31, 245));
        gradient.setColorAt(0.52, QColor(12, 16, 31, 182));
        gradient.setColorAt(1, QColor(12, 16, 31, 15));
        painter.fillRect(rect(), gradient);
        painter.setPen(QColor("#46506d"));
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(bounds, 9, 9);
    }

private:
    QPixmap art_;
};
}

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent)
{
    setWindowFlags(Qt::Window | Qt::FramelessWindowHint | Qt::WindowSystemMenuHint
        | Qt::WindowMinMaxButtonsHint | Qt::WindowCloseButtonHint);
    setWindowIcon(editorIcon("forge", QColor("#b27aff")));
    resize(1672, 941);
    setMinimumSize(1060, 680);
    setDockNestingEnabled(true);
    setDockOptions(AllowNestedDocks | AllowTabbedDocks | GroupedDragging);
    setCorner(Qt::BottomLeftCorner, Qt::LeftDockWidgetArea);
    setCorner(Qt::BottomRightCorner, Qt::RightDockWidgetArea);

    createActions();
    createNavigation();
    pages_ = new WorkspaceStack(this);
    pages_->addWidget(createHome());
    pages_->addWidget(createEditor());
    setCentralWidget(pages_);
    createDocks();
    createStatusBar();

    connect(&store_, &ProjectStore::projectsChanged, this, &MainWindow::refreshProjects);
    connect(&store_, &ProjectStore::sceneChanged, this, &MainWindow::refreshStatus);
    connect(&store_, &ProjectStore::selectionChanged, this, &MainWindow::refreshStatus);
    connect(&store_, &ProjectStore::dirtyChanged, this, &MainWindow::refreshStatus);
    connect(&store_, &ProjectStore::editRejected, this, [this](const QString &reason) {
        browser_->appendLog(reason);
        statusBar()->showMessage(reason, 5000);
    });

    QSettings settings;
    if (!restoreGeometry(settings.value("window/geometry").toByteArray()))
        setWindowState(Qt::WindowMaximized);
    const int savedWorkspace = std::clamp(settings.value("window/workspace", World).toInt(), 0, int(workspaces().size() - 1));
    switchWorkspace(savedWorkspace);
    refreshProjects();

    if (!store_.loadError().isEmpty()) {
        browser_->appendLog(store_.loadError());
        QTimer::singleShot(0, this, [this] { QMessageBox::warning(this, tr("Project recovery"), store_.loadError()); });
    }
}

void MainWindow::createActions()
{
    mainMenu_ = new QMenuBar(this);
    mainMenu_->setObjectName("mainMenu");
    mainMenu_->setNativeMenuBar(false);
    mainMenu_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    auto *file = mainMenu_->addMenu(tr("&File"));
    auto *newProject = file->addAction(editorIcon("plus"), tr("&New project…"), this, &MainWindow::createProject);
    newProject->setShortcut(QKeySequence::New);
    renameProject_ = file->addAction(tr("Rename project…"), this, &MainWindow::renameProject);
    deleteProject_ = file->addAction(editorIcon("trash"), tr("Delete project…"), this, &MainWindow::deleteProject);
    file->addSeparator();
    auto *home = file->addAction(editorIcon("home"), tr("&Project manager"), this, [this] { switchWorkspace(0); });
    home->setShortcut(QKeySequence::Open);
    openWorld_ = file->addAction(editorIcon("world"), tr("Open World Editor"), this, [this] { switchWorkspace(World); });
    save_ = file->addAction(editorIcon("save"), tr("&Save"), this, [this] { saveProject(true); });
    save_->setShortcut(QKeySequence::Save);
    file->addSeparator();
    file->addAction(tr("E&xit"), QKeySequence::Quit, this, &QWidget::close);

    auto *edit = mainMenu_->addMenu(tr("&Edit"));
    auto *undo = store_.history().createUndoAction(this, tr("&Undo"));
    undo->setIcon(editorIcon("undo"));
    undo->setShortcuts(QKeySequence::Undo);
    auto *redo = store_.history().createRedoAction(this, tr("&Redo"));
    redo->setIcon(editorIcon("redo"));
    redo->setShortcuts(QKeySequence::Redo);
    edit->addAction(undo);
    edit->addAction(redo);
    edit->addSeparator();
    deleteObject_ = edit->addAction(editorIcon("trash"), tr("Delete scene object"), &store_, &ProjectStore::deleteObject);
    edit->addAction(editorIcon("search"), tr("Find in scene"), QKeySequence::Find, this, [this] {
        if (activeWorkspace_ != World)
            switchWorkspace(World);
        docks_[0]->show();
        docks_[0]->raise();
        outliner_->focusSearch();
    });

    windowMenu_ = mainMenu_->addMenu(tr("&Window"));
    auto *tools = mainMenu_->addMenu(tr("&Tools"));
    workspaceMenu_ = tools->addMenu(tr("Workspaces"));
    auto *build = mainMenu_->addMenu(tr("&Build"));
    build->addAction(tr("Open build workspace"), this, [this] { switchWorkspace(20); });
    auto *help = mainMenu_->addMenu(tr("&Help"));
    help->addAction(tr("About WorldForge"), this, [this] {
        QMessageBox::about(this, tr("WorldForge Editor"), tr("WorldForge Editor 0.1.0\n"
            "C++20 and Qt Widgets frontend prototype.\n"
            "Scene editing uses local demo data. Play mode is simulated."));
    });

    commands_ = new QToolBar(tr("Commands"), this);
    commands_->setObjectName("commands");
    commands_->setFixedHeight(34);
    commands_->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    commands_->setMovable(false);
    commands_->setFloatable(false);
    commands_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    commands_->setIconSize({16, 16});
    const auto dropdown = [this](const QString &title, const QString &icon, QMenu *menu) {
        auto *action = commands_->addAction(editorIcon(icon), title);
        action->setMenu(menu);
        static_cast<QToolButton *>(commands_->widgetForAction(action))->setPopupMode(QToolButton::InstantPopup);
        return action;
    };
    commands_->addAction(save_);
    for (auto *action : {undo, redo}) {
        commands_->addAction(action);
        auto *button = qobject_cast<QToolButton *>(commands_->widgetForAction(action));
        button->setToolButtonStyle(Qt::ToolButtonIconOnly);
    }
    commands_->addSeparator();
    play_ = new QAction(editorIcon("playFill", QColor("#83eb73")), tr("Play"), this);
    commands_->addAction(play_);
    play_->setCheckable(true);
    play_->setShortcut(Qt::Key_F5);
    connect(play_, &QAction::toggled, this, [this](bool running) {
        play_->setText(running ? tr("Stop") : tr("Play"));
        play_->setIcon(editorIcon(running ? "stop" : "playFill", running ? QColor("#e9919e") : QColor("#83eb73")));
        viewport_->setPreviewRunning(running);
        browser_->appendLog(running ? tr("Simulated play preview started.") : tr("Simulated play preview stopped."));
        refreshStatus();
    });
    auto *simulate = new QMenu(commands_);
    simulate->addAction(tr("Toggle simulated play"), this, [this] { play_->trigger(); });
    dropdown(tr("Simulate"), "box", simulate);
    commands_->addSeparator();
    dropdown(tr("Build"), "hammer", build);
    auto *source = new QMenu(commands_);
    source->addAction(tr("Open Team workspace"), this, [this] { switchWorkspace(22); });
    dropdown(tr("Source Control"), "cloud", source);
    commands_->addSeparator();

    const auto selector = [this, &dropdown](const QString &name, const QString &icon, const QString &key,
                                            const QStringList &choices) {
        auto *menu = new QMenu(name, commands_);
        const int selected = std::clamp(QSettings().value(key, 0).toInt(), 0, int(choices.size() - 1));
        auto *display = dropdown(choices.at(selected), icon, menu);
        display->setToolTip(name);
        auto *options = new QActionGroup(menu);
        for (qsizetype index = 0; index < choices.size(); ++index) {
            auto *option = menu->addAction(choices.at(index));
            options->addAction(option);
            option->setCheckable(true);
            option->setChecked(index == selected);
            connect(option, &QAction::triggered, display, [display, option, key, index] {
                display->setText(option->text());
                QSettings().setValue(key, int(index));
            });
        }
    };
    selector(tr("Target platform"), "screen", "preview/platform",
             {tr("Desktop (Windows)"), tr("Desktop (macOS)"), tr("Desktop (Linux)")});
    selector(tr("Build configuration"), "settings", "preview/configuration",
             {tr("Development"), tr("Debug"), tr("Release")});
    commands_->addSeparator();
    auto *notifications = commands_->addAction(editorIcon("bell"), tr("Notifications"));
    connect(notifications, &QAction::triggered, this, [this] {
        if (activeWorkspace_ != World)
            switchWorkspace(World);
        docks_[2]->show();
        docks_[2]->raise();
        browser_->showMessages();
    });
    auto *account = commands_->addAction(editorIcon("person"), tr("Team"));
    connect(account, &QAction::triggered, this, [this] { switchWorkspace(22); });
    for (auto *action : {notifications, account})
        qobject_cast<QToolButton *>(commands_->widgetForAction(action))->setToolButtonStyle(Qt::ToolButtonIconOnly);
}

void MainWindow::createNavigation()
{
    auto *shell = new QWidget(this);
    auto *shellRows = new QVBoxLayout(shell);
    shellRows->setContentsMargins(0, 0, 0, 0);
    shellRows->setSpacing(0);

    auto *header = new TitleBar(shell);
    header->setObjectName("appHeader");
    header->setFixedHeight(38);
    auto *caption = new QHBoxLayout(header);
    caption->setContentsMargins(10, 0, 0, 0);
    caption->setSpacing(0);
    auto *logo = iconLabel("forge", header, QColor("#ad50ff"), 24);
    logo->setObjectName("forgeLogo");
    logo->setAttribute(Qt::WA_TransparentForMouseEvents);
    caption->addWidget(logo);
    caption->addSpacing(10);
    caption->addWidget(mainMenu_);
    caption->addSpacing(8);
    caption->addWidget(commands_);
    caption->addSpacing(18);
    caption->addStretch();

    const auto captionButton = [header, caption](const QString &icon, const QString &label) {
        auto *button = new QToolButton(header);
        button->setObjectName(icon == "close" ? "captionClose" : "captionButton");
        button->setIcon(editorIcon(icon));
        button->setIconSize({14, 14});
        button->setFixedSize(38, 36);
        button->setToolTip(label);
        button->setAccessibleName(label);
        caption->addWidget(button);
        return button;
    };
    connect(captionButton("minimize", tr("Minimize")), &QToolButton::clicked, this, &QWidget::showMinimized);
    connect(captionButton("maximize", tr("Maximize or restore")), &QToolButton::clicked, this, [this] {
        isMaximized() ? showNormal() : showMaximized();
    });
    connect(captionButton("close", tr("Close")), &QToolButton::clicked, this, &QWidget::close);
    shellRows->addWidget(header);

    auto *bar = new QWidget(shell);
    bar->setObjectName("navigation");
    bar->setFixedHeight(38);
    auto *row = new QHBoxLayout(bar);
    row->setContentsMargins(10, 0, 0, 0);
    row->setSpacing(0);
    workspaceTabs_ = new WorkspaceTabs(bar);
    workspaceTabs_->setObjectName("workspaceTabs");
    workspaceTabs_->setAccessibleName(tr("Workspaces"));
    workspaceTabs_->setIconSize({15, 15});
    workspaceTabs_->setExpanding(false);
    workspaceTabs_->setUsesScrollButtons(true);
    workspaceTabs_->setElideMode(Qt::ElideNone);
    workspaceTabs_->setDrawBase(false);
    row->addWidget(workspaceTabs_, 1);
    shellRows->addWidget(bar);
    setMenuWidget(shell);

    auto *actions = new QActionGroup(this);
    for (qsizetype i = 0; i < workspaces().size(); ++i) {
        const auto &space = workspaces()[i];
        const auto icon = editorIcon(space.icon);
        workspaceTabs_->addTab(icon, space.name);
        workspaceTabs_->setTabToolTip(int(i), space.name);
        if (QList<int>{0, 13, 15, 19}.contains(int(i)))
            workspaceTabs_->setTabVisible(int(i), false);
        auto *action = workspaceMenu_->addAction(icon, space.name, this, [this, i] { switchWorkspace(int(i)); });
        action->setCheckable(true);
        actions->addAction(action);
        workspaceActions_.append(action);
    }
    connect(workspaceTabs_, &QTabBar::currentChanged, this, &MainWindow::switchWorkspace);
}

QWidget *MainWindow::createHome()
{
    auto *home = new QWidget(this);
    auto *layout = new QVBoxLayout(home);
    layout->setContentsMargins(24, 22, 24, 24);
    layout->setSpacing(18);

    auto *hero = new HomeHero(home);
    auto *banner = new QVBoxLayout(hero);
    banner->setContentsMargins(30, 24, 30, 24);
    banner->setSpacing(7);
    banner->addStretch();
    auto *name = new QLabel(tr("WorldForge"), hero);
    name->setStyleSheet("font-size: 45px; font-weight: 650; color: #f3efff");
    banner->addWidget(name);
    homeProject_ = new QLabel(hero);
    homeProject_->setTextFormat(Qt::PlainText);
    homeProject_->setStyleSheet("font-size: 16px; color: #d4c5f0");
    banner->addWidget(homeProject_);
    banner->addSpacing(11);
    auto *heroButtons = new QHBoxLayout;
    auto *open = actionButton(openWorld_, hero);
    open->setObjectName("primary");
    heroButtons->addWidget(open);
    auto *newButton = new QPushButton(editorIcon("plus"), tr("New project"), hero);
    connect(newButton, &QPushButton::clicked, this, &MainWindow::createProject);
    heroButtons->addWidget(newButton);
    heroButtons->addStretch();
    banner->addLayout(heroButtons);
    banner->addStretch();
    layout->addWidget(hero, 2);

    auto *projectsCard = new QFrame(home);
    projectsCard->setObjectName("homeCard");
    auto *card = new QVBoxLayout(projectsCard);
    card->setContentsMargins(15, 12, 15, 14);
    auto *heading = new QHBoxLayout;
    auto *title = new QLabel(tr("Projects"), projectsCard);
    title->setStyleSheet("font-size: 17px; font-weight: 600");
    heading->addWidget(title);
    heading->addStretch();
    heading->addWidget(actionButton(renameProject_, projectsCard));
    heading->addWidget(actionButton(deleteProject_, projectsCard));
    card->addLayout(heading);

    projectList_ = new QListView(projectsCard);
    projectList_->setAccessibleName(tr("Local projects"));
    projectList_->setIconSize({21, 21});
    projects_ = new QStandardItemModel(this);
    projectList_->setModel(projects_);
    card->addWidget(projectList_, 1);
    connect(projectList_->selectionModel(), &QItemSelectionModel::currentChanged, this, [this](const QModelIndex &index) {
        if (index.isValid())
            store_.switchProject(index.data(ProjectIdRole).toString());
    });
    connect(projectList_, &QListView::activated, this, [this] { switchWorkspace(World); });
    layout->addWidget(projectsCard, 1);
    return home;
}

QWidget *MainWindow::createEditor()
{
    auto *editor = new QWidget(this);
    auto *layout = new QVBoxLayout(editor);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(1);
    auto *header = new QWidget(editor);
    header->setObjectName("viewportHeader");
    header->setFixedHeight(24);
    auto *row = new QHBoxLayout(header);
    row->setContentsMargins(0, 0, 6, 0);
    row->setSpacing(4);
    auto *tab = new QWidget(header);
    tab->setObjectName("selectedViewportTab");
    auto *tabRow = new QHBoxLayout(tab);
    tabRow->setContentsMargins(7, 0, 7, 0);
    tabRow->setSpacing(6);
    tabRow->addWidget(iconLabel("forge", tab, QColor("#b984ff"), 14));
    auto *viewportTitle = new QLabel(tr("Viewport 1"), tab);
    viewportTitle->setProperty("heading", true);
    tabRow->addWidget(viewportTitle);
    auto *closeView = new QToolButton(tab);
    closeView->setIcon(editorIcon("close"));
    closeView->setIconSize({12, 12});
    closeView->setToolTip(tr("Close viewport"));
    connect(closeView, &QToolButton::clicked, this, [this, tab] { viewport_->hide(); tab->hide(); });
    tabRow->addWidget(closeView);
    row->addWidget(tab);
    auto *addView = new QToolButton(header);
    addView->setIcon(editorIcon("plus"));
    addView->setToolTip(tr("Show viewport"));
    connect(addView, &QToolButton::clicked, this, [this, tab] { viewport_->show(); tab->show(); });
    row->addWidget(addView);
    row->addStretch();
    layout->addWidget(header);
    viewport_ = new Viewport(store_, editor);
    layout->addWidget(viewport_, 1);
    return editor;
}

void MainWindow::createDocks()
{
    outliner_ = new Outliner(store_, this);
    browser_ = new ContentBrowser(store_, this);
    const auto makeDock = [this](const QString &id, const QString &title, const QString &icon,
                                 QWidget *contents, Qt::DockWidgetArea area) {
        auto *dock = new QDockWidget(title, this);
        dock->setObjectName(id);
        dock->setAllowedAreas(Qt::AllDockWidgetAreas);
        auto *stack = new WorkspaceStack(dock);
        stack->addWidget(contents);
        dock->setWidget(stack);
        dockPages_.append(stack);
        auto *header = new QWidget(dock);
        header->setObjectName("dockHeader");
        auto *row = new QHBoxLayout(header);
        header->setFixedHeight(26);
        row->setContentsMargins(7, 0, 5, 0);
        row->setSpacing(6);
        auto *symbol = iconLabel(icon, header, QColor("#a9b5d3"));
        row->addWidget(symbol);
        auto *label = new QLabel(title, header);
        label->setProperty("heading", true);
        label->setAttribute(Qt::WA_TransparentForMouseEvents);
        row->addWidget(label);
        if (area == Qt::LeftDockWidgetArea) {
            navigatorIcon_ = symbol;
            navigatorTitle_ = label;
        }
        row->addStretch();
        auto *floating = new QToolButton(header);
        floating->setIcon(editorIcon("float"));
        floating->setAccessibleName(tr("Float panel"));
        floating->setToolTip(tr("Float or dock panel"));
        connect(floating, &QToolButton::clicked, dock, [dock] { dock->setFloating(!dock->isFloating()); });
        row->addWidget(floating);
        auto *close = new QToolButton(header);
        close->setIcon(editorIcon("close"));
        close->setAccessibleName(tr("Close panel"));
        close->setToolTip(tr("Close panel"));
        connect(close, &QToolButton::clicked, dock, &QWidget::close);
        row->addWidget(close);
        dock->setTitleBarWidget(header);
        addDockWidget(area, dock);
        windowMenu_->addAction(dock->toggleViewAction());
        docks_.append(dock);
    };
    makeDock("sceneOutliner", tr("Scene Outliner"), "graph", outliner_, Qt::LeftDockWidgetArea);
    makeDock("inspector", tr("Details"), "settings", new Inspector(store_, this), Qt::RightDockWidgetArea);
    makeDock("contentBrowser", tr("Content Browser"), "asset", browser_, Qt::BottomDockWidgetArea);
    for (auto *dock : {docks_[1], docks_[2]}) {
        // These panels provide their own tab headers.
        auto *header = new QWidget(dock);
        header->setFixedHeight(0);
        auto *old = dock->titleBarWidget();
        dock->setTitleBarWidget(header);
        old->deleteLater();
        connect(dock, &QDockWidget::topLevelChanged, dock, [dock, header](bool floating) {
            dock->setTitleBarWidget(floating ? nullptr : header);
            header->setVisible(!floating);
        });
    }
    auto *inspector = static_cast<Inspector *>(dockPages_[1]->widget(0));
    connect(inspector, &Inspector::closeRequested, docks_[1], &QWidget::close);
    connect(browser_, &ContentBrowser::closeRequested, docks_[2], &QWidget::close);
    connect(inspector, &Inspector::floatRequested, this, [this] { docks_[1]->setFloating(!docks_[1]->isFloating()); });
    connect(browser_, &ContentBrowser::floatRequested, this, [this] { docks_[2]->setFloating(!docks_[2]->isFloating()); });
    connect(browser_, &ContentBrowser::saveRequested, this, [this] { saveProject(true); });
    windowMenu_->addSeparator();
    windowMenu_->addAction(tr("Reset panel layout"), this, [this] {
        if (activeWorkspace_ == 0)
            switchWorkspace(World);
        resetLayout();
    });
}

void MainWindow::createStatusBar()
{
    auto *bar = statusBar();
    bar->setFixedHeight(40);
    bar->setSizeGripEnabled(false);
    auto *left = new QWidget(bar);
    left->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    auto *row = new QHBoxLayout(left);
    row->setContentsMargins(17, 0, 10, 0);
    row->setSpacing(10);
    const auto divider = [left, row] {
        auto *line = new QFrame(left);
        line->setObjectName("statusDivider");
        line->setFixedSize(1, 15);
        row->addWidget(line);
    };
    row->addWidget(iconLabel("checkCircle", left, QColor("#46d578"), 17));
    statusReady_ = new QLabel(left);
    row->addWidget(statusReady_);
    row->addSpacing(10);
    divider();
    row->addSpacing(10);
    row->addWidget(iconLabel("branch", left));
    auto *branch = new QLabel(tr("Synced (main)"), left);
    branch->setToolTip(tr("Sample source-control status"));
    row->addWidget(branch);
    row->addSpacing(30);
    divider();
    row->addSpacing(11);
    statusProject_ = new QLabel(left);
    statusProject_->setTextFormat(Qt::PlainText);
    row->addWidget(statusProject_);
    row->addSpacing(31);
    divider();
    const auto metric = [this, left, row](const QString &icon, const QString &label,
                         const QString &value, int percent, const QString &color) {
        auto *metricGroup = new QWidget(left);
        metricGroup->setToolTip(tr("Simulated performance reading"));
        auto *reading = new QHBoxLayout(metricGroup);
        reading->setContentsMargins(0, 0, 0, 0);
        reading->setSpacing(7);
        reading->addWidget(iconLabel(icon, metricGroup, QColor("#e1e2e8"), 15));
        reading->addWidget(new QLabel(label, metricGroup));
        auto *amount = new QLabel(value, metricGroup);
        amount->setObjectName("metricValue");
        reading->addWidget(amount);
        auto *meter = new QProgressBar(metricGroup);
        meter->setTextVisible(false);
        meter->setFixedSize(70, 4);
        meter->setValue(percent);
        meter->setStyleSheet("QProgressBar{background:#303441;border:0;border-radius:2px}"
            "QProgressBar::chunk{background:" + color + ";border-radius:2px}");
        reading->addWidget(meter);
        row->addStretch(1);
        row->addWidget(metricGroup);
        statusMetrics_.append(metricGroup);
    };
    metric("screen", tr("CPU"), "14%", 25, "#9ee1a6");
    metric("hardware", tr("GPU"), "38%", 38, "#6be6ed");
    metric("memory", tr("RAM"), "24.6 / 64 GB", 39, "#39dfe8");
    metric("memory", tr("VRAM"), "7.8 / 24 GB", 33, "#39dfe8");
    row->addStretch(1);
    divider();
    row->addWidget(iconLabel("layers", left));
    auto *renderer = new QLabel(tr("Renderer: Vulkan (RTX)"), left);
    renderer->setToolTip(tr("Sample renderer status for the frontend preview"));
    row->addWidget(renderer);
    row->addStretch(1);
    bar->addWidget(left, 1);
    statusWorkspace_ = new QLabel(bar);
    statusWorkspace_->setContentsMargins(15, 0, 25, 0);
    bar->addPermanentWidget(statusWorkspace_);
}

void MainWindow::resetLayout()
{
    const QList<Qt::DockWidgetArea> areas{Qt::LeftDockWidgetArea, Qt::RightDockWidgetArea, Qt::BottomDockWidgetArea};
    for (qsizetype i = 0; i < docks_.size(); ++i) {
        auto *dock = docks_[i];
        removeDockWidget(dock);
        dock->setFloating(false);
        addDockWidget(areas[i], dock);
        dock->show();
    }
    const auto sizes = activeWorkspace_ == World ? QList<int>{290, 344, 242} : studio(activeWorkspace_)->dockSizes();
    resizeDocks({docks_[0], docks_[1]}, {sizes[0], sizes[1]}, Qt::Horizontal);
    resizeDocks({docks_[2]}, {sizes[2]}, Qt::Vertical);
}

StudioWorkspace *MainWindow::studio(int index)
{
    if (auto *existing = studios_.value(index))
        return existing;
    auto *created = new StudioWorkspace(workspaces().at(index), store_, this);
    studios_.insert(index, created);
    pages_->addWidget(created->surface());
    dockPages_[0]->addWidget(created->navigator());
    dockPages_[1]->addWidget(created->inspector());
    dockPages_[2]->addWidget(created->tools());
    connect(created, &StudioWorkspace::closeRequested, this, [this](int panel) { docks_[panel]->close(); });
    connect(created, &StudioWorkspace::floatRequested, this, [this](int panel) {
        auto *dock = docks_[panel];
        dock->setFloating(!dock->isFloating());
    });
    return created;
}

void MainWindow::switchWorkspace(int index)
{
    if (index < 0 || index >= workspaces().size() || !pages_)
        return;
    if (index == activeWorkspace_)
        return;
    const bool home = index == 0;
    if (activeWorkspace_ > 0)
        workspaceLayouts_.insert(activeWorkspace_, saveState(workspaceLayoutVersion(activeWorkspace_)));
    for (auto *dock : docks_)
        dock->hide();
    activeWorkspace_ = index;
    {
        const QSignalBlocker block(workspaceTabs_);
        for (const int hidden : {0, 13, 15, 19})
            workspaceTabs_->setTabVisible(hidden, index == hidden);
        workspaceTabs_->setCurrentIndex(index);
    }
    workspaceActions_[index]->setChecked(true);
    for (auto *dock : docks_)
        dock->toggleViewAction()->setEnabled(!home);

    if (home) {
        pages_->setCurrentIndex(0);
    } else {
        const auto &workspace = workspaces().at(index);
        QStringList titles{tr("Scene Outliner"), tr("Details"), tr("Content Browser")};
        if (index == World) {
            pages_->setCurrentIndex(1);
            for (auto *stack : dockPages_)
                stack->setCurrentIndex(0);
        } else {
            auto *workspacePage = studio(index);
            pages_->setCurrentWidget(workspacePage->surface());
            dockPages_[0]->setCurrentWidget(workspacePage->navigator());
            dockPages_[1]->setCurrentWidget(workspacePage->inspector());
            dockPages_[2]->setCurrentWidget(workspacePage->tools());
            titles = {workspacePage->navigatorTitle(), workspacePage->inspectorTitle(), workspacePage->toolsTitle()};
        }
        for (qsizetype panel = 0; panel < docks_.size(); ++panel) {
            docks_[panel]->setWindowTitle(titles[panel]);
            docks_[panel]->toggleViewAction()->setText(titles[panel]);
            dockPages_[panel]->updateGeometry();
        }
        navigatorTitle_->setText(titles[0]);
        navigatorIcon_->setPixmap(editorIcon(index == World ? "graph" : workspace.icon, QColor("#a9b5d3")).pixmap(16));
        pages_->updateGeometry();
        const QSettings settings;
        const auto layout = workspaceLayouts_.contains(index) ? workspaceLayouts_.value(index)
            : settings.value(workspaceLayoutKey(index), index == World ? settings.value("window/docks") : QVariant()).toByteArray();
        if (layout.isEmpty() || !restoreState(layout, workspaceLayoutVersion(index)))
            resetLayout();
    }
    if (index != World)
        play_->setChecked(false);
    refreshStatus();
}

void MainWindow::refreshProjects()
{
    const QSignalBlocker block(projectList_->selectionModel());
    const auto *active = store_.project();
    while (projects_->rowCount() > store_.projects().size())
        projects_->removeRow(projects_->rowCount() - 1);
    for (qsizetype i = 0; i < store_.projects().size(); ++i) {
        const auto &project = store_.projects()[i];
        auto *item = projects_->item(int(i));
        if (!item) {
            item = new QStandardItem;
            item->setEditable(false);
            projects_->appendRow(item);
        }
        const bool selected = active && project.id == active->id;
        item->setText(project.name);
        item->setIcon(editorIcon("folder", QColor(selected ? "#c49aff" : "#b2bcd2")));
        item->setData(project.id, ProjectIdRole);
        if (selected)
            projectList_->setCurrentIndex(item->index());
    }
    const bool hasProject = active != nullptr;
    renameProject_->setEnabled(hasProject);
    deleteProject_->setEnabled(hasProject);
    openWorld_->setEnabled(hasProject);
    homeProject_->setText(active ? tr("Active project  /  %1").arg(active->name) : tr("Create a project to begin"));
    refreshStatus();
}

void MainWindow::refreshStatus()
{
    const auto *project = store_.project();
    const QString projectName = project ? project->name : tr("No project");
    const auto &workspace = workspaces()[std::max(activeWorkspace_, 0)];
    statusReady_->setText(play_->isChecked() ? tr("Simulating") : tr("Ready"));
    statusProject_->setText(tr("Project: %1%2").arg(projectName, store_.isDirty() ? "*" : ""));
    statusWorkspace_->setText(workspace.name);
    setWindowTitle(tr("WorldForge — %1%2 — %3").arg(projectName, store_.isDirty() ? "*" : "", workspace.name));
    save_->setEnabled(project != nullptr || store_.isDirty());
    play_->setEnabled(activeWorkspace_ == World && project != nullptr);
    if (!play_->isEnabled() && play_->isChecked())
        play_->setChecked(false);
    deleteObject_->setEnabled(activeWorkspace_ == World && store_.selectedObject() != nullptr);
}

void MainWindow::createProject()
{
    bool ok = false;
    const auto name = QInputDialog::getText(this, tr("New project"), tr("Project name"),
                                          QLineEdit::Normal, {}, &ok);
    if (!ok)
        return;
    if (!store_.createProject(name)) {
        QMessageBox::warning(this, tr("New project"), tr("Enter a unique project name of 1 to 64 characters."));
        return;
    }
    browser_->appendLog(tr("Created project %1.").arg(name.trimmed()));
    switchWorkspace(World);
}

void MainWindow::renameProject()
{
    if (!store_.project())
        return;
    bool ok = false;
    const auto name = QInputDialog::getText(this, tr("Rename project"), tr("Project name"),
                                          QLineEdit::Normal, store_.project()->name, &ok);
    if (ok && !store_.renameProject(name))
        QMessageBox::warning(this, tr("Rename project"), tr("Enter a unique project name of 1 to 64 characters."));
}

void MainWindow::deleteProject()
{
    if (!store_.project())
        return;
    const auto name = store_.project()->name;
    if (QMessageBox::question(this, tr("Delete project"), tr("Delete “%1” and its scene objects?").arg(name),
                              QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes)
        return;
    store_.deleteProject();
    browser_->appendLog(tr("Deleted project %1.").arg(name));
}

bool MainWindow::saveProject(bool announce)
{
    QString error;
    if (!store_.save(&error)) {
        QMessageBox::warning(this, tr("Save project"), error);
        return false;
    }
    if (announce) {
        statusBar()->showMessage(tr("Projects saved locally."), 2200);
        browser_->appendLog(tr("Saved local projects."));
    }
    return true;
}

void MainWindow::resizeEvent(QResizeEvent *event)
{
    QMainWindow::resizeEvent(event);
    for (qsizetype index = 0; index < statusMetrics_.size(); ++index)
        statusMetrics_[index]->setVisible(width() >= (index < 2 ? 1110 : 1350));
}

bool MainWindow::nativeEvent(const QByteArray &eventType, void *message, qintptr *result)
{
#ifdef Q_OS_WIN
    const auto *msg = static_cast<const MSG *>(message);
    if (eventType == "windows_generic_MSG" && msg->message == WM_NCHITTEST && !isMaximized() && !isFullScreen()) {
        POINT point{GET_X_LPARAM(msg->lParam), GET_Y_LPARAM(msg->lParam)};
        RECT bounds{};
        if (ScreenToClient(msg->hwnd, &point) && GetClientRect(msg->hwnd, &bounds)) {
            const int edge = int(5 * devicePixelRatioF());
            const bool left = point.x < edge;
            const bool right = point.x >= bounds.right - edge;
            const bool top = point.y < edge;
            const bool bottom = point.y >= bounds.bottom - edge;
            if (top || bottom || left || right) {
                if (top)
                    *result = left ? HTTOPLEFT : (right ? HTTOPRIGHT : HTTOP);
                else if (bottom)
                    *result = left ? HTBOTTOMLEFT : (right ? HTBOTTOMRIGHT : HTBOTTOM);
                else
                    *result = left ? HTLEFT : HTRIGHT;
                return true;
            }
        }
    }
#endif
    return QMainWindow::nativeEvent(eventType, message, result);
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (store_.isDirty() && !saveProject(false)) {
        event->ignore();
        return;
    }
    QSettings settings;
    settings.setValue("window/geometry", saveGeometry());
    if (activeWorkspace_ > 0)
        workspaceLayouts_.insert(activeWorkspace_, saveState(workspaceLayoutVersion(activeWorkspace_)));
    for (auto it = workspaceLayouts_.cbegin(); it != workspaceLayouts_.cend(); ++it)
        settings.setValue(workspaceLayoutKey(it.key()), it.value());
    settings.setValue("window/workspace", activeWorkspace_);
    QMainWindow::closeEvent(event);
}
