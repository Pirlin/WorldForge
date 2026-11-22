#include "App/MainWindow.h"
#include "App/EditorLog.h"

#include "Components/EditorPanels.h"
#include "Components/AssetHub.h"
#include "Components/BehaviorEditor.h"
#include "Components/NativeGameplayEditor.h"
#include "Components/GameplayAuthoring.h"
#include "Components/LiveViewport.h"
#include "Components/ProjectLauncher.h"
#include "Components/ProjectSurfaceDetails.h"
#include "Components/WindowCaption.h"
#include "State/WorldScene.h"
#include "State/WorldRuntime.h"
#include "State/GlbImport.h"
#include "State/StudioPublishReview.h"
#include "Engine/Geometry/MeshClip.h"
#include "Components/ForgeAiPanel.h"
#include "DesignSystem/Icons.h"
#include "Workspaces/StudioWorkspace.h"
#include "Workspaces/EmbeddedStudio.h"
#include "Workspaces/WorkspaceCatalog.h"

#include <QActionGroup>
#include <QCloseEvent>
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QSaveFile>
#include <QFormLayout>
#include <QCursor>
#include <QDockWidget>
#include <QFontMetrics>
#include <QFrame>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QListWidget>
#include <QMenuBar>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QStandardItemModel>
#include <QStatusBar>
#include <QTabBar>
#include <QTimer>
#include <memory>
#include <QToolBar>
#include <QToolButton>
#include <QWidgetAction>
#include <QWindow>

#include <algorithm>
#include <array>
#include <sstream>
#include <utility>

#ifdef Q_OS_WIN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#endif

namespace {
constexpr int World = 1;
constexpr int Modeling = 2;
constexpr int Sculpting = 3;
constexpr int Texture = 4;
constexpr int Material = 5;
constexpr int Rigging = 7;
constexpr int Animation = 8;

bool usesLocalPanels(int index)
{
    return index == Modeling || index == Sculpting || index == Texture || index == Material
        || index == Rigging || index == Animation;
}
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
        setMinimumHeight(215);
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

MainWindow::MainWindow(QWidget *parent, bool launchCreator) : QMainWindow(parent), launchCreator_(launchCreator)
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
    browser_->appendLog(tr("WorldForge editor started."));
    connect(liveWorld_,&LiveViewport::rendererStatusChanged,this,[this](const QString &status) {
        if (status.startsWith("Vulkan ·"))
            browser_->appendLog(status);
        else
            browser_->appendMessage(status);
    });
    auto *logs = new QTimer(this);
    logs->setInterval(250);
    connect(logs,&QTimer::timeout,this,[this] {
        for (const auto &entry : takeEditorLog())
            if (entry.level == QtInfoMsg)
                browser_->appendLog(entry.text);
            else
                browser_->appendMessage(entry.text);
    });
    logs->start();
    const auto showPanel = [this](auto show) {
        switchWorkspace(World);
        if (activeWorkspace_ == World) {
            docks_.last()->show();
            (browser_->*show)();
        }
    };
    windowMenu_->addAction(tr("Output Log"),this,[showPanel] { showPanel(&ContentBrowser::showLog); });
    windowMenu_->addAction(tr("Message Log"),this,[showPanel] { showPanel(&ContentBrowser::showMessages); });
    auto *terminal = windowMenu_->addAction(tr("Terminal"),this,[showPanel] { showPanel(&ContentBrowser::showTerminal); });
    terminal->setShortcut(QKeySequence("Ctrl+`"));

    connect(&store_, &ProjectStore::projectsChanged, this, &MainWindow::refreshProjects);
    connect(&store_, &ProjectStore::sceneChanged, this, &MainWindow::refreshStatus);
    connect(&store_, &ProjectStore::selectionChanged, this, &MainWindow::refreshStatus);
    connect(&store_, &ProjectStore::dirtyChanged, this, &MainWindow::refreshStatus);
    connect(&store_, &ProjectStore::editRejected, this, [this](const QString &reason) {
        browser_->appendMessage(reason);
        statusBar()->showMessage(reason, 5000);
    });

    QSettings settings;
    if (!restoreGeometry(settings.value("window/geometry").toByteArray()) || launchCreator_)
        setWindowState(Qt::WindowMaximized);
    const int savedWorkspace = !launchCreator_ && store_.project()
        ? std::clamp(settings.value("window/workspace", World).toInt(), 0, int(workspaces().size() - 1)) : 0;
    switchWorkspace(savedWorkspace);
    refreshProjects();

    if (!store_.loadError().isEmpty()) {
        browser_->appendMessage(store_.loadError());
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
    auto *home = file->addAction(editorIcon("home"), tr("&Project manager"), this, [this] {
        if (launcher_)
            launcher_->showRecent();
        switchWorkspace(0);
    });
    home->setShortcut(QKeySequence::Open);
    openWorld_ = file->addAction(editorIcon("world"), tr("Open World Editor"), this, [this] { switchWorkspace(World); });
#ifdef Q_OS_WIN
    file->addAction(tr("Open Studio Workspace"),this,[this] { openEmbeddedStudio(); })
        ->setProperty("requiresStudioProcess",true);
    file->addAction(tr("Edit Selected Mesh in Studio"),this,[this] { openEmbeddedStudio(true); })
        ->setProperty("requiresStudioProcess",true);
#endif
    save_ = file->addAction(editorIcon("save"), tr("&Save"), this, [this] { saveProject(true); });
    save_->setShortcut(QKeySequence::Save);
    file->addAction(tr("Export Playable Scene…"),this,[this] {
        const auto *project = store_.project();
        if (!project)
            return;
        const auto folder = project->configuration.value("location").toString();
        const auto fileName = QFileDialog::getSaveFileName(this,tr("Export Playable Scene"),
            QDir(folder).filePath(project->name + ".wfplay"),tr("WorldForge Playable Scene (*.wfplay)"));
        if (fileName.isEmpty())
            return;
        std::ostringstream data;
        QString error;
        if (!WorldRuntime::exportScene(store_,data,&error)) {
            QMessageBox::warning(this,tr("Export Playable Scene"),
                error.isEmpty() ? tr("The scene exceeds the Player export limits.") : error);
            return;
        }
        const auto bytes = std::move(data).str();
        QSaveFile output(fileName);
        if (!output.open(QIODevice::WriteOnly) || output.write(bytes.data(),qint64(bytes.size())) != qint64(bytes.size())
            || !output.commit()) {
            QMessageBox::warning(this,tr("Export Playable Scene"),output.errorString());
            return;
        }
        statusBar()->showMessage(tr("Exported %1").arg(fileName),5000);
    });
    file->addSeparator();
    file->addAction(tr("E&xit"), QKeySequence::Quit, this, &QWidget::close);

    auto *edit = mainMenu_->addMenu(tr("&Edit"));
    auto *undo = undo_ = store_.history().createUndoAction(this, tr("&Undo"));
    undo->setIcon(editorIcon("undo"));
    undo->setShortcuts(QKeySequence::Undo);
    auto *redo = redo_ = store_.history().createRedoAction(this, tr("&Redo"));
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
    tools->addAction(tr("C++ Gameplay Module…"),this,[this] { showNativeGameplay(store_,this); });
    workspaceMenu_ = tools->addMenu(tr("Workspaces"));
    auto *build = mainMenu_->addMenu(tr("&Build"));
    build->addAction(tr("Open build workspace"), this, [this] { switchWorkspace(20); });
    auto *help = mainMenu_->addMenu(tr("&Help"));
    help->addAction(tr("About WorldForge"), this, [this] {
        QMessageBox::about(this, tr("WorldForge Editor"), tr("WorldForge Editor 0.1.0\n"
            "C++20 and Qt Widgets frontend prototype.\n"
            "World Editor uses the live Vulkan viewport. The game runtime is pending."));
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
    applyStudioUpdate_ = commands_->addAction(editorIcon("box"),tr("Apply Studio Update"));
    applyStudioUpdate_->setObjectName("applyStudioUpdate");
    applyStudioUpdate_->setVisible(false);
    connect(applyStudioUpdate_,&QAction::triggered,this,&MainWindow::publishStudioMesh);
    searchStudioAssets_ = commands_->addAction(editorIcon("search"),tr("Search Assets"));
    searchStudioAssets_->setObjectName("searchStudioAssets");
    searchStudioAssets_->setVisible(false);
    connect(searchStudioAssets_,&QAction::triggered,this,[this] { showStudioAssets(activeWorkspace_); });
    commands_->addAction(save_);
    for (auto *action : {undo, redo}) {
        commands_->addAction(action);
        auto *button = qobject_cast<QToolButton *>(commands_->widgetForAction(action));
        button->setToolButtonStyle(Qt::ToolButtonIconOnly);
    }
    commands_->addSeparator();
    play_ = commands_->addAction(editorIcon("playFill", QColor("#85df83")), tr("Play"));
    play_->setShortcut(Qt::Key_F5);
    play_->setToolTip(tr("Play world · WASD move · Space jump · Esc stop"));
    simulate_ = commands_->addAction(editorIcon("box"), tr("Simulate"));
    simulate_->setToolTip(tr("Run scene physics with the editor camera"));
    stop_ = commands_->addAction(editorIcon("stop"), tr("Stop"));
    stop_->setShortcut(Qt::Key_Escape);
    connect(play_, &QAction::triggered, this, [this] { liveWorld_->start(LiveViewport::RunMode::Play); });
    connect(simulate_, &QAction::triggered, this, [this] { liveWorld_->start(LiveViewport::RunMode::Simulate); });
    connect(stop_, &QAction::triggered, this, [this] { liveWorld_->stop(); });
    forgeAi_ = commands_->addAction(editorIcon("spark", QColor("#c49aff")), tr("Forge AI"));
    forgeAi_->setObjectName("forgeAi");
    forgeAi_->setToolTip(tr("Describe a barrier edit, preview it, and apply it as one undoable change"));
    connect(forgeAi_, &QAction::triggered, this, [this] {
        if (!forgeAiPanel_) {
            forgeAiPanel_ = new ForgeAiPanel(store_, this);
            connect(forgeAiPanel_, &ForgeAiPanel::applied, this, [this](const QString &objectId) {
                store_.select(objectId);
                statusBar()->showMessage(tr("Forge AI applied the barrier. Undo reverts it as one change."), 6000);
            });
        }
        forgeAiPanel_->show();
        forgeAiPanel_->raise();
        forgeAiPanel_->activateWindow();
    });
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
    auto *logo = iconLabel("forge", header, QColor("#ad50ff"), 18);
    logo->setObjectName("forgeLogo");
    logo->setAttribute(Qt::WA_TransparentForMouseEvents);
    caption->addWidget(logo);
    caption->addSpacing(7);
    caption->addWidget(mainMenu_);
    caption->addSpacing(8);
    caption->addWidget(commands_);
    caption->addSpacing(18);
    caption->addStretch();

    caption->addWidget(new WindowCaption(*this, header->height(), header));
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
    connect(workspaceTabs_, &QTabBar::tabBarClicked, this, [this](int index) {
        if (index == activeWorkspace_ && pendingStudioWorkspace_ >= 0)
            switchWorkspace(index);
    });
}

QWidget *MainWindow::createHome()
{
    if (launchCreator_) {
        projects_ = new QStandardItemModel(this);
        launcher_ = new ProjectLauncher(projects_,this);
        projectList_ = launcher_->recentList();
        connect(projectList_->selectionModel(),&QItemSelectionModel::currentChanged,this,[this](const QModelIndex &index) {
            if (index.isValid())
                store_.switchProject(index.data(ProjectIdRole).toString());
        });
        connect(launcher_,&ProjectLauncher::openRequested,this,[this] { if (store_.project()) switchWorkspace(World); });
        connect(launcher_,&ProjectLauncher::renameRequested,this,&MainWindow::renameProject);
        connect(launcher_,&ProjectLauncher::deleteRequested,this,&MainWindow::deleteProject);
        connect(launcher_,&ProjectLauncher::createRequested,this,[this](const QString &name,const QJsonObject &configuration) {
            QString error;
            if (!store_.createStarterProject(name,configuration,error)) {
                launcher_->showError(error);
                browser_->appendMessage(error);
                return;
            }
            launcher_->showError({});
            browser_->appendLog(tr("Created project %1.").arg(name.trimmed()));
            switchWorkspace(World);
            if (configuration.value("starter").toString() == "terrain")
                liveWorld_->previewTerrain();
        });
        return launcher_;
    }
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
    name->setStyleSheet("font-size: 30px; font-weight: 650; color: #f3efff");
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
    connect(closeView, &QToolButton::clicked, this, [this, tab] {
        liveWorld_->stop();
        liveWorld_->hide();
        tab->hide();
        refreshStatus();
    });
    tabRow->addWidget(closeView);
    row->addWidget(tab);
    auto *addView = new QToolButton(header);
    addView->setIcon(editorIcon("plus"));
    addView->setToolTip(tr("Show viewport"));
    connect(addView, &QToolButton::clicked, this, [this, tab] { liveWorld_->show(); tab->show(); refreshStatus(); });
    row->addWidget(addView);
    auto *frame = new QToolButton(header);
    frame->setText(tr("Frame Scene"));
    connect(frame, &QToolButton::clicked, this, [this] { liveWorld_->frame(); });
    row->addWidget(frame);
    worldAuthoring_ = new QWidget(header);
    auto *authoring = new QHBoxLayout(worldAuthoring_);
    authoring->setContentsMargins(0,0,0,0);
    authoring->setSpacing(4);
    row->addWidget(worldAuthoring_);
    auto *cube = new QToolButton(worldAuthoring_);
    cube->setText(tr("Add Cube"));
    connect(cube, &QToolButton::clicked, this, [this] {
        store_.addCube();
        liveWorld_->frame();
    });
    authoring->addWidget(cube);
    auto *terrain = new QToolButton(worldAuthoring_);
    terrain->setText(tr("Terrain"));
    terrain->setPopupMode(QToolButton::InstantPopup);
    auto *terrainMenu = new QMenu(terrain);
    terrainMenu->addAction(tr("Add Terrain"), this, [this] { store_.addTerrain(); liveWorld_->frame(); });
    terrainMenu->addAction(tr("Add Rolling Hills with Trees"), this, [this] { store_.addTerrain(true); liveWorld_->previewTerrain(); });
    terrainMenu->addSeparator();
    auto *brushSettings = new QWidget(terrainMenu);
    auto *brushForm = new QFormLayout(brushSettings);
    brushForm->setContentsMargins(10,4,10,4);
    auto *radius = new QDoubleSpinBox(brushSettings);
    radius->setRange(0.5,64);
    radius->setValue(3);
    radius->setSuffix(tr(" m"));
    auto *strength = new QDoubleSpinBox(brushSettings);
    strength->setRange(0.05,8);
    strength->setValue(1);
    brushForm->addRow(tr("Radius"),radius);
    brushForm->addRow(tr("Strength"),strength);
    auto *brushOptions = new QWidgetAction(terrainMenu);
    brushOptions->setDefaultWidget(brushSettings);
    terrainMenu->addAction(brushOptions);
    auto *brushes = new QActionGroup(terrainMenu);
    auto *select = terrainMenu->addAction(tr("Select"), this, [this] { liveWorld_->setTerrainBrush(0,0,false); });
    brushes->addAction(select);
    select->setCheckable(true);
    select->setChecked(true);
    for (const auto &brush : {QString("Raise"), QString("Lower"), QString("Smooth")}) {
        const auto configure = [this,radius,strength,brush] {
            const float power = float(strength->value());
            liveWorld_->setTerrainBrush(float(radius->value()),brush == "Lower" ? -power : power,brush == "Smooth");
        };
        auto *action = terrainMenu->addAction(brush, this, [this, terrain, brush, configure] {
            configure();
            terrain->setText(tr("Terrain · %1").arg(brush));
            statusBar()->showMessage(tr("Click the terrain to apply the brush. Ctrl+Z undoes a stroke."),5000);
        });
        brushes->addAction(action);
        action->setCheckable(true);
        connect(radius,&QDoubleSpinBox::valueChanged,action,[action,configure] { if (action->isChecked()) configure(); });
        connect(strength,&QDoubleSpinBox::valueChanged,action,[action,configure] { if (action->isChecked()) configure(); });
    }
    auto *layers = terrainMenu->addMenu(tr("Paint Material Layer"));
    const QStringList layerNames{tr("Automatic"),tr("Grass"),tr("Rock"),tr("Soil")};
    for (int layer = 0; layer < layerNames.size(); ++layer) {
        const auto name = layerNames[layer];
        const auto configure = [this,radius,layer] { liveWorld_->setTerrainLayerBrush(float(radius->value()),layer); };
        auto *paint = layers->addAction(name,this,[this,terrain,name,configure] {
            configure();
            terrain->setText(tr("Terrain · %1").arg(name));
            statusBar()->showMessage(tr("Click the terrain to paint the selected layer."),5000);
        });
        paint->setCheckable(true);
        brushes->addAction(paint);
        connect(radius,&QDoubleSpinBox::valueChanged,paint,[paint,configure] { if (paint->isChecked()) configure(); });
    }
    connect(select, &QAction::triggered, terrain, [this,terrain] { terrain->setText(tr("Terrain")); });
    terrain->setMenu(terrainMenu);
    authoring->addWidget(terrain);
    auto *atmosphere = new QToolButton(worldAuthoring_);
    atmosphere->setText(tr("Sky & Fog"));
    connect(atmosphere,&QToolButton::clicked,this,&MainWindow::editAtmosphere);
    authoring->addWidget(atmosphere);
    auto *modeling = new QToolButton(worldAuthoring_);
    modeling->setText(tr("Edit Mesh"));
    connect(modeling, &QToolButton::clicked, this, [this] {
        openEmbeddedStudio(true);
    });
    authoring->addWidget(modeling);
    pauseRuntime_ = new QToolButton(header);
    pauseRuntime_->setText(tr("Pause"));
    pauseRuntime_->setCheckable(true);
    connect(pauseRuntime_,&QToolButton::toggled,this,[this](bool paused) {
        liveWorld_->pause(paused);
        pauseRuntime_->setText(paused ? tr("Resume") : tr("Pause"));
        stepRuntime_->setEnabled(paused);
    });
    row->addWidget(pauseRuntime_);
    stepRuntime_ = new QToolButton(header);
    stepRuntime_->setText(tr("Step"));
    stepRuntime_->setToolTip(tr("Advance one 1/120-second physics step"));
    connect(stepRuntime_,&QToolButton::clicked,this,[this] { liveWorld_->step(); });
    row->addWidget(stepRuntime_);
    row->addStretch();
    layout->addWidget(header);
    liveWorld_ = new LiveViewport(store_, false, editor);
    liveWorld_->setObjectName("worldLiveViewport");
    layout->addWidget(liveWorld_, 1);
    connect(liveWorld_, &LiveViewport::rendererStatusChanged, this, [this] {
        refreshStatus();
        updateMetrics();
    });
    // Show mission objectives from Play in the status bar and the output log as they change.
    auto *objectives = new QTimer(this);
    objectives->setInterval(250);
    auto *missionHud = new QLabel(editor);
    missionHud->setObjectName("missionObjectiveHud");
    missionHud->setTextFormat(Qt::PlainText);
    missionHud->setWordWrap(true);
    missionHud->setMargin(8);
    missionHud->hide();
    layout->insertWidget(1,missionHud);
    auto lastObjective = std::make_shared<QString>();
    connect(objectives,&QTimer::timeout,this,[this,lastObjective,missionHud] {
        const auto objective = liveWorld_->runtimeObjective();
        missionHud->setVisible(!objective.isEmpty() && liveWorld_->runtimeShowsHud());
        if (objective.isEmpty() || objective == *lastObjective)
            return;
        *lastObjective = objective;
        missionHud->setText(objective);
        statusBar()->showMessage(objective);
        browser_->appendLog(tr("Objective: %1").arg(objective));
    });
    connect(liveWorld_,&LiveViewport::runtimeChanged,this,[this,objectives,lastObjective,missionHud] {
        lastObjective->clear();
        missionHud->hide();
        if (liveWorld_->runMode() == LiveViewport::RunMode::Play)
            objectives->start();
        else
            objectives->stop();
    });
    connect(liveWorld_,&LiveViewport::runtimeChanged,this,[this] {
        const bool running = liveWorld_->runMode() != LiveViewport::RunMode::Editing;
        for (auto *dock : docks_)
            dock->widget()->setEnabled(!running);
        const QSignalBlocker block(pauseRuntime_);
        pauseRuntime_->setChecked(false);
        pauseRuntime_->setText(tr("Pause"));
        stepRuntime_->setEnabled(false);
        if (running) {
            browser_->appendLog(liveWorld_->runMode() == LiveViewport::RunMode::Play ? tr("Play started.") : tr("Simulation started."));
            statusBar()->showMessage(liveWorld_->runMode() == LiveViewport::RunMode::Play
                ? tr("WASD to move · Space to jump · E to interact · Esc to stop") : tr("Simulating the runtime copy · Esc to stop"),3000);
        } else {
            browser_->appendLog(tr("Stopped. Authored scene restored."));
        }
        refreshStatus();
    });
    return editor;
}

void MainWindow::editAtmosphere()
{
    if (!store_.project())
        return;
    const auto *environment = store_.environment();
    const auto details = environment ? environment->details : QJsonObject{};
    const auto initial = readAtmosphere(details);
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Sky, Fog & Gravity"));
    auto *form = new QFormLayout(&dialog);
    auto *sky = new QCheckBox(tr("Show gradient sky"),&dialog);
    sky->setChecked(initial.sky);
    form->addRow(sky);
    const auto colorControl = [&dialog,form](const QString &label,QColor value) {
        auto *button = new QPushButton(value.name(),&dialog);
        QObject::connect(button,&QPushButton::clicked,button,[button,&dialog,label] {
            const auto color = QColorDialog::getColor(QColor(button->text()),&dialog,label);
            if (color.isValid())
                button->setText(color.name());
        });
        form->addRow(label,button);
        return button;
    };
    auto *horizon = colorControl(tr("Horizon"),initial.horizon);
    auto *zenith = colorControl(tr("Zenith"),initial.zenith);
    auto *skyModel = new QComboBox(&dialog);
    skyModel->setAccessibleName(tr("Sky model"));
    skyModel->addItem(tr("Gradient (horizon and zenith colors)"),QStringLiteral("Gradient"));
    skyModel->addItem(tr("Physical atmosphere"),QStringLiteral("Physical"));
    skyModel->setCurrentIndex(initial.physicalSky ? 1 : 0);
    skyModel->setToolTip(tr("The physical atmosphere scatters sunlight as air does: a blue sky, a white horizon and an "
                            "orange low sun, all set by the sun's position."));
    form->addRow(tr("Sky model"),skyModel);
    const auto skyColors = [skyModel,horizon,zenith] {
        const bool gradient = skyModel->currentIndex() == 0;
        horizon->setEnabled(gradient);
        zenith->setEnabled(gradient);
    };
    skyColors();
    connect(skyModel,&QComboBox::currentIndexChanged,&dialog,skyColors);
    auto *fogColor = colorControl(tr("Fog color"),initial.fogColor);
    auto *fog = new QDoubleSpinBox(&dialog);
    fog->setAccessibleName(tr("Fog density"));
    fog->setRange(0,1);
    fog->setDecimals(3);
    fog->setSingleStep(0.01);
    fog->setValue(initial.fogDensity);
    form->addRow(tr("Fog density"),fog);
    auto *falloff = new QDoubleSpinBox(&dialog);
    falloff->setDecimals(3);
    falloff->setRange(0,2);
    falloff->setSingleStep(0.01);
    falloff->setValue(initial.fogFalloff);
    form->addRow(tr("Fog height falloff"),falloff);
    auto *fogHeight = new QDoubleSpinBox(&dialog);
    fogHeight->setRange(-1000,1000);
    fogHeight->setValue(initial.fogHeight);
    fogHeight->setSuffix(tr(" m"));
    form->addRow(tr("Fog base height"),fogHeight);
    const auto scalar = [&dialog,form](const QString &name,double minimum,double maximum,double initialValue) {
        auto *field = new QDoubleSpinBox(&dialog);
        field->setAccessibleName(name);
        field->setRange(minimum,maximum);
        field->setValue(initialValue);
        form->addRow(name,field);
        return field;
    };
    auto *sunAzimuth = scalar(tr("Default sun azimuth"),-180,180,initial.sunAzimuth);
    auto *sunElevation = scalar(tr("Default sun elevation"),-20,90,initial.sunElevation);
    sunAzimuth->setSuffix("°");
    sunElevation->setSuffix("°");
    auto *shadows = new QCheckBox(tr("Cast sun shadows"),&dialog);
    shadows->setAccessibleName(tr("Cast sun shadows"));
    shadows->setChecked(initial.shadows);
    form->addRow(shadows);
    auto *center = new QWidget(&dialog);
    auto *centerLayout = new QHBoxLayout(center);
    centerLayout->setContentsMargins(0,0,0,0);
    std::array<QDoubleSpinBox *,3> localCenter{};
    for (int i = 0; i < 3; ++i) {
        auto *axis = new QDoubleSpinBox(center);
        axis->setRange(-100000,100000);
        axis->setAccessibleName(tr("Local fog center %1").arg(QString("XYZ").at(i)));
        axis->setValue(initial.localFogCenter[i]);
        centerLayout->addWidget(axis);
        localCenter[i] = axis;
    }
    form->addRow(tr("Local fog center XYZ"),center);
    auto *localRadius = scalar(tr("Local fog radius"),0,500,initial.localFogRadius);
    localRadius->setSuffix(tr(" m"));
    localRadius->setToolTip(tr("Set the radius above zero to enable the fog volume."));
    auto *localDensity = scalar(tr("Local fog density"),0,1,initial.localFogDensity);
    localDensity->setSingleStep(0.01);
    namespace color = worldforge::color;
    auto *exposureMode = new QComboBox(&dialog);
    exposureMode->setAccessibleName(tr("Camera exposure"));
    exposureMode->addItem(tr("Manual (EV100)"),QStringLiteral("Manual"));
    exposureMode->addItem(tr("Auto (meters the view)"),QStringLiteral("Auto"));
    exposureMode->setCurrentIndex(initial.autoExposure ? 1 : 0);
    form->addRow(tr("Camera exposure"),exposureMode);
    auto *exposure = scalar(tr("Exposure value"),color::ev100FromStops(color::maximumExposure),
                            color::ev100FromStops(color::minimumExposure),color::ev100FromStops(initial.exposure));
    exposure->setSingleStep(0.5);
    exposure->setSuffix(tr(" EV100"));
    exposure->setToolTip(tr("Camera exposure at ISO 100: about 15 for a sunny day, 12 overcast, 7 indoors and 4 for a lit "
                            "street at night. Each step of 1 halves the light the camera takes in."));
    auto *compensation = scalar(tr("Exposure compensation"),-color::maximumCompensation,color::maximumCompensation,
                                initial.exposureCompensation);
    compensation->setSingleStep(0.5);
    compensation->setSuffix(tr(" stops"));
    compensation->setToolTip(tr("Brightens (positive) or darkens (negative) what auto exposure chooses."));
    const auto exposureFields = [exposureMode,exposure,compensation] {
        const bool automatic = exposureMode->currentIndex() == 1;
        exposure->setEnabled(!automatic);
        compensation->setEnabled(automatic);
    };
    exposureFields();
    connect(exposureMode,&QComboBox::currentIndexChanged,&dialog,exposureFields);
    auto *viewTransform = new QComboBox(&dialog);
    viewTransform->setAccessibleName(tr("View transform"));
    viewTransform->addItem(tr("AgX (matches Studio)"),QStringLiteral("AgX"));
    viewTransform->addItem(tr("Standard (clips highlights)"),QStringLiteral("Standard"));
    viewTransform->setCurrentIndex(initial.viewTransform == worldforge::color::ViewTransform::Standard ? 1 : 0);
    form->addRow(tr("View transform"),viewTransform);
    auto *antiAliasing = new QComboBox(&dialog);
    antiAliasing->setAccessibleName(tr("Anti-aliasing"));
    antiAliasing->addItem(tr("TAA (smooth edges)"),QStringLiteral("TAA"));
    antiAliasing->addItem(tr("Off"),QStringLiteral("Off"));
    antiAliasing->setCurrentIndex(initial.temporalAntiAliasing ? 0 : 1);
    antiAliasing->setToolTip(tr("Temporal anti-aliasing renders each frame with a slightly shifted camera and blends the "
                                "frames, which smooths jagged edges like supersampling."));
    form->addRow(tr("Anti-aliasing"),antiAliasing);
    auto *ambientOcclusion = new QCheckBox(tr("Ambient occlusion"),&dialog);
    ambientOcclusion->setAccessibleName(tr("Ambient occlusion"));
    ambientOcclusion->setChecked(initial.ambientOcclusion);
    ambientOcclusion->setToolTip(tr("Darkens sky light in corners, creases and openings that the sky cannot reach."));
    form->addRow(ambientOcclusion);
    auto *gravity = new QDoubleSpinBox(&dialog);
    gravity->setRange(0,50);
    gravity->setSuffix(tr(" m/s²"));
    gravity->setValue(details.value("gravity").toDouble(9.81));
    form->addRow(tr("Gravity"),gravity);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel,&dialog);
    connect(buttons,&QDialogButtonBox::accepted,&dialog,&QDialog::accept);
    connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);
    form->addRow(buttons);
    if (dialog.exec() == QDialog::Accepted)
        store_.setAtmosphere({{"sky",sky->isChecked()},{"horizon",horizon->text()},{"zenith",zenith->text()},
                              {"fogColor",fogColor->text()},{"fogDensity",fog->value()},{"fogFalloff",falloff->value()},
                              {"fogHeight",fogHeight->value()},{"gravity",gravity->value()},
                              {"sunAzimuth",sunAzimuth->value()},{"sunElevation",sunElevation->value()},
                              {"shadows",shadows->isChecked()},{"localFogRadius",localRadius->value()},
                              {"localFogDensity",localDensity->value()},
                              {"ev100",exposure->value()},{"exposureMode",exposureMode->currentData().toString()},
                              {"exposureCompensation",compensation->value()},{"viewTransform",viewTransform->currentData().toString()},{"antiAliasing",antiAliasing->currentData().toString()},{"ambientOcclusion",ambientOcclusion->isChecked()},{"skyModel",skyModel->currentData().toString()},
                              {"localFogCenter",QJsonArray{localCenter[0]->value(),localCenter[1]->value(),localCenter[2]->value()}}});
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
    connect(browser_, &ContentBrowser::newStudioAssetRequested, this, [this] {
        openEmbeddedStudio(false,{},true);
    });
    connect(browser_, &ContentBrowser::editStudioAssetRequested, this, [this](const QString &id) {
        openEmbeddedStudio(true,id);
    });
    connect(browser_, &ContentBrowser::openAssetRequested, this, [this](const QString &id) {
        openAssetDetails(id);
    });
    connect(browser_, &ContentBrowser::meshImported, this, [this] {
        liveWorld_->frame();
    });
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
    statusSave_ = new QLabel(left);
    statusSave_->setToolTip(tr("Local project save status"));
    row->addWidget(statusSave_);
    row->addSpacing(30);
    divider();
    row->addSpacing(11);
    statusProject_ = new QLabel(left);
    statusProject_->setTextFormat(Qt::PlainText);
    row->addWidget(statusProject_);
    row->addSpacing(31);
    divider();
    const auto metric = [this, left, row](const QString &icon, const QString &label,
                                         const QString &description, const QString &color) {
        auto *metricGroup = new QWidget(left);
        metricGroup->setToolTip(description);
        auto *reading = new QHBoxLayout(metricGroup);
        reading->setContentsMargins(0, 0, 0, 0);
        reading->setSpacing(7);
        reading->addWidget(iconLabel(icon, metricGroup, QColor("#e1e2e8"), 15));
        reading->addWidget(new QLabel(label, metricGroup));
        auto *amount = new QLabel(tr("—"), metricGroup);
        amount->setObjectName("metricValue");
        amount->setAccessibleName(label + tr(" reading"));
        amount->setMinimumWidth(label == tr("CPU") || label == tr("GPU") ? 32 : 82);
        reading->addWidget(amount);
        auto *meter = new QProgressBar(metricGroup);
        meter->setTextVisible(false);
        meter->setFixedSize(70, 4);
        meter->hide();
        meter->setStyleSheet("QProgressBar{background:#303441;border:0;border-radius:2px}"
            "QProgressBar::chunk{background:" + color + ";border-radius:2px}");
        reading->addWidget(meter);
        row->addStretch(1);
        row->addWidget(metricGroup);
        statusMetrics_.append(metricGroup);
        metricValues_.append(amount);
        metricMeters_.append(meter);
    };
    metric("screen", tr("CPU"), tr("System-wide CPU utilization"), "#9ee1a6");
    metric("hardware", tr("GPU"), tr("Busiest engine on the viewport's GPU across all processes"), "#6be6ed");
    metric("memory", tr("RAM"), tr("System physical memory in use"), "#39dfe8");
    metric("memory", tr("GPU MEM"), tr("Dedicated memory on discrete GPUs or shared memory on integrated GPUs"), "#39dfe8");
    row->addStretch(1);
    divider();
    row->addWidget(iconLabel("layers", left));
    statusRenderer_ = new QLabel(left);
    row->addWidget(statusRenderer_);
    row->addStretch(1);
    bar->addWidget(left, 1);
    statusWorkspace_ = new QLabel(bar);
    statusWorkspace_->setContentsMargins(15, 0, 25, 0);
    bar->addPermanentWidget(statusWorkspace_);
    auto *sampler = new QTimer(this);
    sampler->setInterval(1000);
    connect(sampler, &QTimer::timeout, this, &MainWindow::updateMetrics);
    sampler->start();
    updateMetrics();
}

void MainWindow::updateMetrics()
{
    if (metricValues_.size() != 4)
        return;
    const auto readings = metrics_.sample(liveWorld_->adapterCounterId(), liveWorld_->dedicatedMemory(), liveWorld_->sharedMemory());
    const auto update = [this](int index, std::optional<double> percent, const QString &label) {
        metricValues_[index]->setText(label);
        metricMeters_[index]->setVisible(percent.has_value());
        if (percent)
            metricMeters_[index]->setValue(qRound(std::clamp(*percent, 0.0, 100.0)));
    };
    const auto percent = [this](std::optional<double> value) {
        return value ? QString::number(qRound(*value)) + '%' : tr("—");
    };
    const auto memory = [this, &update](int index, std::optional<quint64> used, std::optional<quint64> total) {
        constexpr double gib = 1024.0*1024*1024;
        if (used && total && *total)
            update(index, 100.0*double(*used)/double(*total),
                   tr("%1 / %2 GiB").arg(double(*used)/gib, 0, 'f', 1).arg(double(*total)/gib, 0, 'f', 1));
        else
            update(index, std::nullopt, tr("—"));
    };
    update(0, readings.cpu, percent(readings.cpu));
    update(1, readings.gpu, percent(readings.gpu));
    memory(2, readings.ramUsed, readings.ramTotal);
    memory(3, readings.vramUsed, readings.vramTotal);
    const auto memoryKind = liveWorld_->dedicatedMemory() ? tr("Dedicated VRAM")
        : liveWorld_->sharedMemory() ? tr("Shared GPU memory") : tr("GPU memory");
    browser_->setMetrics(tr("System CPU: %1\nViewport GPU: %2\nSystem RAM: %3\n%4: %5\n%6")
        .arg(metricValues_[0]->text(), metricValues_[1]->text(), metricValues_[2]->text(),
             memoryKind, metricValues_[3]->text(), liveWorld_->rendererName()));
}

void MainWindow::resetLayout()
{
    if (usesLocalPanels(activeWorkspace_))
        return;
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

void MainWindow::openAssetDetails(const QString &assetId,int workspace)
{
    const auto *project = store_.project();
    if (!project)
        return;
    if (project->behaviors.contains(assetId)) {
        editBehaviorAsset(store_,assetId,this);
        return;
    }
    if (project->surfaces.textures.contains(assetId)) {
        projectSurfaces::showTexture(store_,assetId,this);
        return;
    }
    if (project->surfaces.materials.contains(assetId)) {
        const auto *mesh = store_.selectedMesh();
        projectSurfaces::editMaterial(store_,assetId,
            usesLocalPanels(activeWorkspace_) && !studioAssetId_.isEmpty() ? studioAssetId_ : mesh ? mesh->id : QString{},this);
        return;
    }
    if (!store_.meshAsset(assetId))
        return;
    inspectedAsset_ = assetId;
    inspectedProject_ = project->id;
    assetWorkspace_ = workspace;
    if (!assetDetails_) {
        assetDetails_ = new QDialog(this);
        assetDetails_->setObjectName("meshAssetDetails");
        assetDetails_->resize(850,500);
        auto *layout = new QVBoxLayout(assetDetails_);
        auto *content = new QHBoxLayout;
        layout->addLayout(content,1);
        assetPreview_ = new LiveViewport(store_,true,assetDetails_);
        assetPreview_->setObjectName("meshAssetPreview");
        assetPreview_->setModelShading(ModelShading::Material);
        content->addWidget(assetPreview_,3);
        assetSummary_ = new QLabel(assetDetails_);
        assetSummary_->setObjectName("meshAssetSummary");
        assetSummary_->setTextFormat(Qt::PlainText);
        assetSummary_->setTextInteractionFlags(Qt::TextSelectableByMouse);
        assetSummary_->setAlignment(Qt::AlignTop);
        assetSummary_->setWordWrap(true);
        auto *scroll = new QScrollArea(assetDetails_);
        scroll->setWidgetResizable(true);
        scroll->setWidget(assetSummary_);
        content->addWidget(scroll,2);
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close,assetDetails_);
        editInspectedAsset_ = buttons->addButton(tr("Edit in Studio"),QDialogButtonBox::ActionRole);
        auto *place = buttons->addButton(tr("Add to Level"),QDialogButtonBox::ActionRole);
        place->setObjectName("placeInspectedAsset");
        layout->addWidget(buttons);
        connect(buttons,&QDialogButtonBox::rejected,assetDetails_,&QDialog::reject);
        connect(editInspectedAsset_,&QPushButton::clicked,assetDetails_,[this] {
            assetDetails_->hide();
            openEmbeddedStudio(true,inspectedAsset_,false,assetWorkspace_);
        });
        connect(place,&QPushButton::clicked,assetDetails_,[this] { store_.placeMesh(inspectedAsset_); });
        connect(&store_,&ProjectStore::sceneChanged,this,&MainWindow::refreshAssetDetails);
        connect(&store_,&ProjectStore::projectsChanged,this,&MainWindow::refreshAssetDetails);
        connect(assetDetails_,&QDialog::finished,assetDetails_,[this](int) {
            inspectedProject_.clear();
            inspectedAsset_.clear();
        });
    }
    refreshAssetDetails();
    assetDetails_->show();
    assetDetails_->raise();
    assetDetails_->activateWindow();
}

void MainWindow::refreshAssetDetails()
{
    if (!assetDetails_)
        return;
    const auto *project = store_.project();
    const auto *asset = project && project->id == inspectedProject_ ? store_.meshAsset(inspectedAsset_) : nullptr;
    if (!asset) {
        assetDetails_->hide();
        return;
    }
    const auto &mesh = asset->evaluated();
    QStringList materials;
    for (const auto &material : mesh.materials)
        materials << QString::fromStdString(material.name);
    const auto placements = std::ranges::count(project->objects,asset->id,&SceneObject::meshAsset);
    const auto source = !asset->studioSourceHash.isEmpty() ? tr("Editable Studio asset")
        : asset->sourcePath.isEmpty() ? tr("WorldForge project mesh") : asset->sourcePath;
    assetDetails_->setWindowTitle(tr("%1 — Asset Details").arg(asset->name));
    auto summary = tr("%1\n\n%2 vertices · %3 faces\n%4 level placement(s)\n\nMaterial slots\n%5\n\nSource\n%6")
        .arg(asset->clip ? tr("Animated Mesh") : tr("Static Mesh")).arg(mesh.vertices.size()).arg(mesh.faces.size())
        .arg(placements).arg(materials.join('\n'),source);
    if (asset->clip)
        summary += tr("\n\nAnimation\n%1 · %2 seconds").arg(QString::fromStdString(asset->clip->name))
            .arg(asset->clip->duration(),0,'f',2);
    auto layouts = QStringList{tr("%1 (render)").arg(asset->activeUvSet)};
    for (auto it = asset->otherUvSets.cbegin(); it != asset->otherUvSets.cend(); ++it)
        layouts << (it->needsUnwrap ? tr("%1 (rebuild required)").arg(it.key()) : it.key());
    assetSummary_->setText(summary + tr("\n\nUV layers\n%1").arg(layouts.join('\n')));
    assetPreview_->setAssetPreview(asset->evaluatedMesh ? asset->evaluatedMesh : asset->mesh);
    const bool editable = asset->modifiers.empty();
    editInspectedAsset_->setEnabled(editable);
    editInspectedAsset_->setToolTip(editable ? QString{} : tr("Apply WorldForge modifiers before opening this mesh in Studio."));
}

void MainWindow::showStudioAssets(int workspace)
{
    const auto *project = store_.project();
    if (!project || (workspace != World && !usesLocalPanels(workspace)) || nextStudioSession_)
        return;
    if (workspace == World)
        workspace = Modeling;
    const auto projectId = project->id;
    QDialog dialog(this);
    dialog.setObjectName("studioAssetSearch");
    dialog.setWindowTitle(tr("Search Project Assets"));
    dialog.resize(620,420);
    auto *layout = new QVBoxLayout(&dialog);
    auto *search = new QLineEdit(&dialog);
    search->setObjectName("studioAssetQuery");
    search->setPlaceholderText(tr("Search by mesh, material, or file name…"));
    search->setAccessibleName(tr("Search project assets"));
    search->setClearButtonEnabled(true);
    search->addAction(editorIcon("search"),QLineEdit::LeadingPosition);
    layout->addWidget(search);
    auto *kind = new QComboBox(&dialog);
    kind->setObjectName("projectAssetKind");
    enum Kind { All, Meshes, Textures, Materials };
    kind->addItem(tr("All assets"),All);
    kind->addItem(tr("Meshes"),Meshes);
    kind->addItem(tr("Textures"),Textures);
    kind->addItem(tr("Materials"),Materials);
    layout->addWidget(kind);
    auto *list = new QListWidget(&dialog);
    list->setObjectName("studioAssetResults");
    list->setAccessibleName(tr("Matching project assets"));
    list->setSortingEnabled(true);
    layout->addWidget(list,1);
    auto *summary = new QLabel(&dialog);
    layout->addWidget(summary);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close,&dialog);
    auto *details = buttons->addButton(tr("Open Details"),QDialogButtonBox::ActionRole);
    auto *edit = buttons->addButton(tr("Edit in %1").arg(workspaces().at(workspace).name),QDialogButtonBox::AcceptRole);
    edit->setObjectName("studioAssetEdit");
    auto *place = buttons->addButton(tr("Add to Level"),QDialogButtonBox::ActionRole);
    place->setObjectName("studioAssetPlace");
    auto *import = buttons->addButton(tr("Import GLB…"),QDialogButtonBox::ActionRole);
    import->setObjectName("studioAssetImport");
    auto *create = buttons->addButton(tr("New Studio Asset…"),QDialogButtonBox::ActionRole);
    create->setObjectName("studioAssetNew");
    layout->addWidget(buttons);
    auto *addSurface = new QDialogButtonBox(&dialog);
    auto *imageImport = addSurface->addButton(tr("Import Texture…"),QDialogButtonBox::ActionRole);
    imageImport->setObjectName("importProjectTexture");
    auto *createMaterial = addSurface->addButton(tr("New Material…"),QDialogButtonBox::ActionRole);
    createMaterial->setObjectName("newProjectMaterial");
    layout->insertWidget(layout->count()-1,addSurface);

    const auto selectedId = [list] {
        const auto *row = list->currentItem();
        return row ? row->data(Qt::UserRole).toString() : QString{};
    };
    const auto syncButtons = [&] {
        const auto id = selectedId();
        const auto *asset = store_.meshAsset(id);
        const auto *active = store_.project();
        const bool surface = active && (active->surfaces.textures.contains(id) || active->surfaces.materials.contains(id));
        const bool supported = asset && asset->modifiers.empty();
        edit->setEnabled(supported);
        details->setEnabled(asset || surface);
        edit->setToolTip(asset && !supported ? tr("Apply WorldForge modifiers before opening this mesh in Studio.") : QString{});
        place->setEnabled(asset != nullptr);
    };
    const auto refresh = [&](const QString &preferred) {
        const QSignalBlocker block(list);
        list->clear();
        const auto *active = store_.project();
        if (!active || active->id != projectId) {
            dialog.reject();
            return;
        }
        const auto query = search->text().trimmed();
        const int category = kind->currentData().toInt();
        QListWidgetItem *preferredRow = nullptr;
        const auto addRow = [&](const QString &id,const QString &title,const QString &icon,const QString &tooltip) {
            auto *row = new QListWidgetItem(editorIcon(icon),title,list);
            row->setData(Qt::UserRole,id);
            row->setToolTip(tooltip);
            if (id == preferred)
                preferredRow = row;
        };
        if (category == All || category == Meshes) {
            for (const auto &asset : active->assets) {
                QStringList materials;
                for (const auto &material : asset.evaluated().materials)
                    materials << QString::fromStdString(material.name);
                const auto file = QFileInfo(asset.sourcePath).fileName();
                if (!asset.name.contains(query,Qt::CaseInsensitive) && !file.contains(query,Qt::CaseInsensitive)
                    && !materials.join(' ').contains(query,Qt::CaseInsensitive))
                    continue;
                addRow(asset.id,asset.name,"box",tr("%1 vertices · %2 faces\nMaterials: %3").arg(asset.evaluated().vertices.size())
                    .arg(asset.evaluated().faces.size()).arg(materials.join(", ")));
            }
        }
        if (category == All || category == Textures)
            for (auto it = active->surfaces.textures.cbegin(); it != active->surfaces.textures.cend(); ++it)
                if (it->name.contains(query,Qt::CaseInsensitive) || it->fileName.contains(query,Qt::CaseInsensitive))
                    addRow(it.key(),it->name,"layers",tr("Texture · %1 × %2").arg(it->width).arg(it->height));
        if (category == All || category == Materials)
            for (auto it = active->surfaces.materials.cbegin(); it != active->surfaces.materials.cend(); ++it)
                if (it->name.contains(query,Qt::CaseInsensitive))
                    addRow(it.key(),it->name,"box",tr("Project material"));
        list->setCurrentItem(preferredRow ? preferredRow : list->item(0));
        summary->setText(list->count() ? tr("%n matching asset(s)",nullptr,list->count())
            : tr("No matching project assets."));
        syncButtons();
    };
    connect(search,&QLineEdit::textChanged,&dialog,[&] { refresh(selectedId()); });
    connect(kind,&QComboBox::currentIndexChanged,&dialog,[&] { refresh(selectedId()); });
    connect(list,&QListWidget::currentItemChanged,&dialog,syncButtons);
    bool inspecting = false;
    const auto inspect = [&] {
        if (details->isEnabled()) {
            inspecting = true;
            dialog.accept();
        }
    };
    connect(list,&QListWidget::itemActivated,&dialog,inspect);
    connect(details,&QPushButton::clicked,&dialog,inspect);
    connect(edit,&QPushButton::clicked,&dialog,&QDialog::accept);
    connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);
    connect(place,&QPushButton::clicked,&dialog,[&] {
        if (!store_.project() || store_.project()->id != projectId)
            return;
        store_.placeMesh(selectedId());
        statusBar()->showMessage(tr("Added mesh to the level."),5000);
        dialog.reject();
    });
    connect(import,&QPushButton::clicked,&dialog,[&] {
        const auto file = QFileDialog::getOpenFileName(&dialog,tr("Import GLB"),{},tr("Binary glTF (*.glb)"));
        if (file.isEmpty() || !store_.project() || store_.project()->id != projectId)
            return;
        QString error;
        if (!store_.importMesh(file,error)) {
            QMessageBox::warning(&dialog,tr("Import GLB"),error);
            return;
        }
        search->clear();
        kind->setCurrentIndex(Meshes);
        if (const auto *asset = store_.selectedMesh())
            refresh(asset->id);
        statusBar()->showMessage(tr("Imported %1 into the project and level.").arg(QFileInfo(file).fileName()),5000);
    });
    connect(imageImport,&QPushButton::clicked,&dialog,[&] {
        const auto id = projectSurfaces::importTexture(store_,&dialog);
        if (!id.isEmpty()) {
            kind->setCurrentIndex(Textures);
            search->clear();
            refresh(id);
        }
    });
    connect(createMaterial,&QPushButton::clicked,&dialog,[&] {
        const auto id = projectSurfaces::editMaterial(store_,{},studioAssetId_,&dialog);
        if (!id.isEmpty()) {
            kind->setCurrentIndex(Materials);
            search->clear();
            refresh(id);
        }
    });
    bool creating = false;
    connect(create,&QPushButton::clicked,&dialog,[&] { creating = true; dialog.accept(); });
    refresh(studioAssetId_);
    edit->setDefault(true);
    search->setFocus();
    if (dialog.exec() != QDialog::Accepted || !store_.project() || store_.project()->id != projectId)
        return;
    if (creating)
        openEmbeddedStudio(false,{},true,workspace);
    else if (inspecting)
        openAssetDetails(selectedId(),workspace);
    else if (edit->isEnabled() && !selectedId().isEmpty())
        openEmbeddedStudio(true,selectedId(),false,workspace);
}

void MainWindow::openEmbeddedStudio(bool editSelected,const QString &assetId,bool newAsset,int workspace,QString newAssetName)
{
    const auto *project = store_.project();
    const auto *asset = assetId.isEmpty() ? store_.selectedMesh() : store_.meshAsset(assetId);
    if (newAsset && !project)
        return;
    if (editSelected && (!project || !asset)) {
        QMessageBox::information(this,tr("Edit Selected Mesh in Studio"),tr("Select a mesh in the level first."));
        return;
    }
    if (editSelected && !asset->modifiers.empty()) {
        QMessageBox::information(this,tr("Edit Selected Mesh in Studio"),
            tr("Apply WorldForge modifiers before opening this mesh in Studio."));
        return;
    }
    if (nextStudioSession_)
        return;
    if (newAsset && newAssetName.isEmpty()) {
        const auto projectId = project->id;
        bool accepted = false;
        newAssetName = QInputDialog::getText(this,tr("New Studio Asset"),tr("Asset name"),
            QLineEdit::Normal,tr("Studio Mesh"),&accepted).trimmed();
        if (!accepted || newAssetName.isEmpty() || !store_.project() || store_.project()->id != projectId)
            return;
        project = store_.project();
    }
    if (embeddedStudio_ && embeddedStudio_->isRunning() && (newAsset || (editSelected
        && (studioProjectId_ != project->id || studioAssetId_ != asset->id ||
            studioAssetRevision_ != asset->mesh || studioSourceRevision_ != asset->studioSourceHash)))) {
        const auto projectId = project->id;
        const auto targetId = editSelected ? asset->id : QString{};
        QMessageBox confirm(QMessageBox::Question,tr("Switch Studio Asset"),
            tr("Switching closes the current Studio session. To keep unpublished edits, cancel and publish them or save a Studio asset copy first."),
            QMessageBox::Cancel,this);
        auto *discard = confirm.addButton(tr("Discard Draft and Switch"),QMessageBox::DestructiveRole);
        confirm.setDefaultButton(QMessageBox::Cancel);
        confirm.exec();
        if (confirm.clickedButton() != discard || !store_.project() || store_.project()->id != projectId)
            return;
        if (!embeddedStudio_->closeForAssetSwitch()) {
            statusBar()->showMessage(tr("Studio could not close the current session. Try again when the workspace is ready."),6000);
            return;
        }
        nextStudioSession_ = [this,projectId,targetId,editSelected,newAsset,workspace,newAssetName] {
            if (store_.project() && store_.project()->id == projectId)
                openEmbeddedStudio(editSelected,targetId,newAsset,workspace,newAssetName);
        };
        statusBar()->showMessage(tr("Switching Studio asset…"),5000);
        refreshStatus();
        return;
    }
    if (!embeddedStudio_) {
        embeddedStudio_ = new EmbeddedStudio(pages_);
        pages_->addWidget(embeddedStudio_);
        connect(embeddedStudio_,&EmbeddedStudio::workspaceReady,this,[this] {
            if (pendingStudioWorkspace_ >= 0) {
                const int workspace = pendingStudioWorkspace_;
                pendingStudioWorkspace_ = -1;
                showStudioWorkspace(workspace);
            }
        });
        connect(embeddedStudio_,&EmbeddedStudio::sessionClosed,this,[this](const QString &message) {
            auto next = std::exchange(nextStudioSession_,{});
            pendingStudioWorkspace_ = -1;
            if (pages_->currentWidget() == embeddedStudio_)
                switchWorkspace(World);
            if (!message.isEmpty())
                statusBar()->showMessage(message,8000);
            else if (next)
                QTimer::singleShot(0,this,std::move(next));
        });
    }
    if (newAsset) {
        if (!embeddedStudio_->prepareAsset({},{})) {
            QMessageBox::warning(this,tr("New Studio Asset"),tr("Could not prepare a Studio session."));
            return;
        }
        studioProjectId_ = project->id;
        studioAssetId_.clear();
        studioAssetRevision_.clear();
        studioSourceRevision_.clear();
        studioNewAssetName_ = newAssetName;
        studioAppliedRevision_.clear();
    } else if (editSelected && !embeddedStudio_->isRunning()) {
        QString error;
        if (asset->clip && !QFileInfo::exists(store_.studioSourcePath(*asset))) {
            QMessageBox::warning(this,tr("Edit Selected Mesh in Studio"),tr("The editable Studio character source is missing."));
            return;
        }
        const auto glb = store_.exportStudioInput(*asset,error);
        if (glb.isEmpty() || !embeddedStudio_->prepareAsset(glb,store_.studioSourcePath(*asset))) {
            QMessageBox::warning(this,tr("Edit Selected Mesh in Studio"),
                error.isEmpty() ? tr("Could not prepare the mesh for Studio.") : error);
            return;
        }
        studioProjectId_ = project->id;
        studioAssetId_ = asset->id;
        studioAssetRevision_ = asset->mesh;
        studioSourceRevision_ = asset->studioSourceHash;
        studioNewAssetName_.clear();
        studioAppliedRevision_.clear();
    } else if (!editSelected && !embeddedStudio_->isRunning()) {
        embeddedStudio_->clearAsset();
        studioProjectId_.clear();
        studioAssetId_.clear();
        studioAssetRevision_.clear();
        studioSourceRevision_.clear();
        studioNewAssetName_.clear();
        studioAppliedRevision_.clear();
    }
    pendingStudioWorkspace_ = workspace;
    if (activeWorkspace_ >= 0) {
        const QSignalBlocker blocker(workspaceTabs_);
        workspaceTabs_->setCurrentIndex(activeWorkspace_);
    }
    embeddedStudio_->showWorkspace(workspaces().at(workspace).name);
}

void MainWindow::showStudioWorkspace(int workspace)
{
    liveWorld_->stop();
    if (activeWorkspace_ > 0 && !usesLocalPanels(activeWorkspace_))
        workspaceLayouts_.insert(activeWorkspace_,saveState(workspaceLayoutVersion(activeWorkspace_)));
    for (auto *dock : docks_) {
        dock->hide();
        dock->toggleViewAction()->setEnabled(false);
    }
    activeWorkspace_ = workspace;
    {
        const QSignalBlocker blocker(workspaceTabs_);
        for (const int hidden : {0, 13, 15, 19})
            workspaceTabs_->setTabVisible(hidden,false);
        workspaceTabs_->setCurrentIndex(workspace);
    }
    workspaceActions_[workspace]->setChecked(true);
    menuWidget()->show();
    statusBar()->show();
    pages_->setCurrentWidget(embeddedStudio_);
    refreshStatus();
}

void MainWindow::publishStudioMesh()
{
    const auto *project = store_.project();
    const auto *asset = store_.meshAsset(studioAssetId_);
    const bool creating = !studioNewAssetName_.isEmpty();
    if (!project || project->id != studioProjectId_ || (!creating
        && (!asset || asset->mesh != studioAssetRevision_ || asset->studioSourceHash != studioSourceRevision_))) {
        QMessageBox::warning(this,tr("Publish Studio Mesh"),
            tr("Return to the editor, select the current target mesh, then reopen Studio."));
        return;
    }
    const auto name = creating ? studioNewAssetName_ : asset->name;
    const auto revision = embeddedStudio_->stagedRevision();
    if (revision.isEmpty()) {
        QMessageBox::information(this,tr("Publish Studio Mesh"),tr("Use File → Publish to WorldForge inside Studio first."));
        return;
    }
    if (revision == studioAppliedRevision_) {
        statusBar()->showMessage(tr("This Studio update is already applied."),5000);
        return;
    }
    QFile file(QDir(revision).filePath("mesh.glb"));
    if (!file.open(QIODevice::ReadOnly) || file.size() > 64 * 1024 * 1024) {
        QMessageBox::warning(this,tr("Publish Studio Mesh"),tr("Choose a readable GLB no larger than 64 MiB."));
        return;
    }
    const auto glb = file.read(64 * 1024 * 1024 + 1);
    QFile source(QDir(revision).filePath("source.wfasset"));
    constexpr qint64 maxStudioSource = 128 * 1024 * 1024;
    if (!source.open(QIODevice::ReadOnly) || source.size() <= 0 || source.size() > maxStudioSource) {
        QMessageBox::warning(this,tr("Publish Studio Mesh"),tr("The editable asset is missing or exceeds 128 MiB."));
        return;
    }
    const auto sourceBytes = source.read(maxStudioSource + 1);
    source.close();
    file.close();
    QString error;
    QFile uvFile(QDir(revision).filePath("uv-sets.json"));
    QByteArray uvManifest;
    if (uvFile.exists()) {
        if (!uvFile.open(QIODevice::ReadOnly) || uvFile.size() <= 0 || uvFile.size() > 8192) {
            QMessageBox::warning(this,tr("Publish Studio Mesh"),tr("The staged UV manifest is unreadable or invalid."));
            return;
        }
        uvManifest = uvFile.read(8193);
    }
    QFile clipFile(QDir(revision).filePath("clip.wfmotion"));
    QByteArray clipBytes;
    if (clipFile.exists()) {
        constexpr qsizetype limit = worldforge::MeshClip::maxSamples*24+148;
        if (!clipFile.open(QIODevice::ReadOnly) || clipFile.size() <= 0 || clipFile.size() > limit) {
            QMessageBox::warning(this,tr("Publish Studio Mesh"),tr("The staged animation clip is unreadable or invalid."));
            return;
        }
        clipBytes = clipFile.read(limit+1);
    }
    StudioMeshData studio;
    auto preview = importStudioGlb(glb,uvManifest,clipBytes,studio,error);
    if (!preview) {
        QMessageBox::warning(this,tr("Publish Studio Mesh"),error);
        return;
    }
    QStringList replacedNormalMaps;
    if (!creating) {
        const auto importedMaterials = preview->materials;
        if (!store_.preserveProjectMaterials(studioAssetId_,*preview,error)) {
            QMessageBox::warning(this,tr("Publish Studio Mesh"),error);
            return;
        }
        for (std::size_t i = 0; i < importedMaterials.size(); ++i)
            if (!preview->materials[i].sharedMaterialId.empty() && !importedMaterials[i].normalTexture.empty()
                && preview->materials[i].normalTexture != importedMaterials[i].normalTexture)
                replacedNormalMaps << QString::fromStdString(importedMaterials[i].name);
    }
    auto check = reviewStudioPublish(*project,creating ? nullptr : asset,name,*preview);
    if (studio.uvs) {
        const auto &uvLayers = *studio.uvs;
        const auto renderUv = uvLayers.active.isEmpty() ? QStringLiteral("UV Map") : uvLayers.active;
        if (asset) {
            auto previous = asset->otherUvSets.keys();
            previous << asset->activeUvSet;
            previous.removeAll(renderUv);
            for (const auto &layer : uvLayers.other.keys())
                previous.removeAll(layer);
            if (!previous.isEmpty())
                check.warnings << tr("Rebuild UV layouts missing from the published mesh: %1.").arg(previous.join(", "));
        }
    }
    if (!replacedNormalMaps.isEmpty())
        check.warnings << tr("Project materials retain their own normals for %1. Make the slots independent before publishing to use the Studio normal maps.")
            .arg(replacedNormalMaps.join(", "));
    if (!check.blocker.isEmpty()) {
        QMessageBox::warning(this,tr("Publish Studio Mesh"),check.blocker);
        return;
    }
    auto summary = check.summary();
    if (studio.clip)
        summary += tr("\n\nAnimation · %1 · %2 seconds · %3 frames")
            .arg(QString::fromStdString(studio.clip->name)).arg(studio.clip->duration(),0,'f',2).arg(studio.clip->frameCount());
    QMessageBox review(check.warnings.isEmpty() ? QMessageBox::Question : QMessageBox::Warning,
        tr("Publish Studio Mesh"),summary,QMessageBox::Apply | QMessageBox::Cancel,this);
    review.setObjectName("studioPublishReview");
    review.setTextFormat(Qt::PlainText);
    review.setDefaultButton(QMessageBox::Cancel);
    if (!check.placements.isEmpty())
        review.setDetailedText(check.placements.join('\n'));
    if (review.exec() != QMessageBox::Apply) {
        statusBar()->showMessage(tr("Studio update canceled. %1 is unchanged.").arg(name),5000);
        return;
    }
    const auto *current = store_.meshAsset(studioAssetId_);
    if (!store_.project() || store_.project()->id != studioProjectId_) {
        QMessageBox::warning(this,tr("Publish Studio Mesh"),tr("The project changed."));
        return;
    }
    if (creating) {
        if (!store_.createStudioMesh(name,std::move(*preview),sourceBytes,error,&studio)) {
            QMessageBox::warning(this,tr("Publish Studio Mesh"),error);
            return;
        }
        studioAssetId_ = store_.selectedMesh()->id;
        studioNewAssetName_.clear();
    } else if (!current || current->studioSourceHash != studioSourceRevision_
        || !store_.publishStudioMesh(studioAssetId_,glb,studioAssetRevision_.data(),error,sourceBytes,uvManifest,clipBytes)) {
        QMessageBox::warning(this,tr("Publish Studio Mesh"),error.isEmpty() ? tr("The project changed.") : error);
        return;
    }
    studioAssetRevision_ = store_.meshAsset(studioAssetId_)->mesh;
    studioSourceRevision_ = store_.meshAsset(studioAssetId_)->studioSourceHash;
    studioAppliedRevision_ = revision;
    refreshStatus();
    statusBar()->showMessage(tr("Published %1").arg(name),5000);
}

void MainWindow::switchWorkspace(int index)
{
    if (index < 0 || index >= workspaces().size() || !pages_)
        return;
    const bool leavingStudio = embeddedStudio_ && pages_->currentWidget() == embeddedStudio_;
    if (usesLocalPanels(index)) {
        const bool editSelection = activeWorkspace_ == World && store_.selectedMesh()
            && (index == Modeling || index == Sculpting || index == Texture
                || (store_.selectedMesh()->clip && (index == Rigging || index == Animation)));
        openEmbeddedStudio(editSelection,{},false,index);
        return;
    }
    pendingStudioWorkspace_ = -1;
    if (index == activeWorkspace_ && !leavingStudio)
        return;
    liveWorld_->stop();
    const bool home = index == 0;
    if (launcher_) {
        menuWidget()->setVisible(!home);
        statusBar()->setVisible(!home);
    }
    if (!leavingStudio && activeWorkspace_ > 0 && !usesLocalPanels(activeWorkspace_))
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
        dock->toggleViewAction()->setEnabled(!home && !usesLocalPanels(index));

    if (home) {
        pages_->setCurrentIndex(0);
    } else {
        const auto &workspace = workspaces().at(index);
        QStringList titles{tr("Scene Outliner"), tr("Details"), tr("Content Browser")};
        if (index == World) {
            pages_->setCurrentIndex(1);
            for (auto *stack : dockPages_)
                stack->setCurrentIndex(0);
        } else if (workspace.name == "Asset Hub" || hasGameplayAuthoring(workspace.name)) {
            auto *panel = gameplayWorkspaces_.value(index);
            if (!panel) {
                if (workspace.name == "Asset Hub")
                    panel = createAssetHub(store_,[this](const QString &id) { openAssetDetails(id); },pages_);
                else {
                    panel = createGameplayAuthoring(workspace.name,store_,pages_);
                    panel->setObjectName("gameplayWorkspace"+workspace.name);
                }
                gameplayWorkspaces_.insert(index,panel);
                pages_->addWidget(panel);
            }
            pages_->setCurrentWidget(panel);
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
    const auto id = active ? active->id : QString();
    if (worldViewProject_ != id) {
        worldViewProject_ = id;
        liveWorld_->frame();
    }
    renameProject_->setEnabled(hasProject);
    deleteProject_->setEnabled(hasProject);
    openWorld_->setEnabled(hasProject);
    if (homeProject_)
        homeProject_->setText(active ? tr("Active project  /  %1").arg(active->name) : tr("Create a project to begin"));
    refreshStatus();
}

void MainWindow::refreshStatus()
{
    const auto *project = store_.project();
    const QString projectName = project ? project->name : tr("No project");
    const auto &workspace = workspaces()[std::max(activeWorkspace_, 0)];
    const auto mode = liveWorld_->runMode();
    const bool running = mode != LiveViewport::RunMode::Editing;
    statusReady_->setText(mode == LiveViewport::RunMode::Play ? tr("Playing")
        : running ? tr("Simulating") : project ? tr("Ready") : tr("No project"));
    statusSave_->setText(store_.isDirty() ? tr("Unsaved") : tr("Local"));
    statusProject_->setText(tr("Project: %1%2").arg(projectName, store_.isDirty() ? "*" : ""));
    statusWorkspace_->setText(workspace.name);
    statusRenderer_->setText(activeWorkspace_ == World
        ? (liveWorld_->hasRenderedFrame() ? tr("Renderer: Vulkan") : tr("Vulkan viewport"))
        : activeWorkspace_ == Modeling ? tr("Mesh authoring")
        : activeWorkspace_ == Sculpting ? tr("Sculpt authoring")
        : activeWorkspace_ == Texture || activeWorkspace_ == Material ? tr("Material preview")
        : activeWorkspace_ == Rigging || activeWorkspace_ == Animation ? tr("Studio authoring")
        : workspace.name == "Programming" ? tr("Behavior authoring")
        : tr("Reference preview"));
    statusRenderer_->setToolTip(liveWorld_->rendererName());
    setWindowTitle(tr("WorldForge — %1%2 — %3").arg(projectName, store_.isDirty() ? "*" : "", workspace.name));
    const bool canRun = activeWorkspace_ == World && project && liveWorld_->isVisible()
        && liveWorld_->hasRenderedFrame() && !running;
    play_->setEnabled(canRun);
    simulate_->setEnabled(canRun);
    stop_->setVisible(running);
    stop_->setEnabled(running);
    worldAuthoring_->setEnabled(!running && project);
    if (forgeAi_)
        forgeAi_->setEnabled(!running && project && activeWorkspace_ == World);
    pauseRuntime_->setVisible(running);
    stepRuntime_->setVisible(running);
    undo_->setEnabled(!running && store_.history().canUndo());
    redo_->setEnabled(!running && store_.history().canRedo());
    save_->setEnabled(!running && (project != nullptr || store_.isDirty()));
    const auto *studioAsset = store_.meshAsset(studioAssetId_);
    const bool studioAssetOpen = project && studioProjectId_ == project->id
        && embeddedStudio_ && pages_->currentWidget() == embeddedStudio_
        && (studioAsset || !studioNewAssetName_.isEmpty());
    applyStudioUpdate_->setVisible(studioAssetOpen);
    searchStudioAssets_->setVisible(activeWorkspace_ == World || usesLocalPanels(activeWorkspace_));
    searchStudioAssets_->setEnabled(project && !nextStudioSession_);
    if (studioAssetOpen)
        applyStudioUpdate_->setToolTip(tr("Apply Studio Update to %1").arg(studioAsset ? studioAsset->name : studioNewAssetName_));
    deleteObject_->setEnabled(!running && activeWorkspace_ == World && store_.selectedObject() != nullptr);
}

void MainWindow::createProject()
{
    liveWorld_->stop();
    if (launcher_) {
        launcher_->showCreator();
        switchWorkspace(0);
        return;
    }
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
    liveWorld_->stop();
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
    liveWorld_->stop();
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
        browser_->appendMessage(error);
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
    if (embeddedStudio_ && embeddedStudio_->isRunning()
        && QMessageBox::question(this,tr("Close WorldForge"),
            tr("Close WorldForge and the open Studio session? Unsaved Studio edits can be lost."),
            QMessageBox::Close | QMessageBox::Cancel,QMessageBox::Cancel) != QMessageBox::Close) {
        event->ignore();
        return;
    }
    liveWorld_->stop();
    if (store_.isDirty() && !saveProject(false)) {
        event->ignore();
        return;
    }
    QSettings settings;
    settings.setValue("window/geometry", saveGeometry());
    if ((!embeddedStudio_ || pages_->currentWidget() != embeddedStudio_)
        && activeWorkspace_ > 0 && !usesLocalPanels(activeWorkspace_))
        workspaceLayouts_.insert(activeWorkspace_, saveState(workspaceLayoutVersion(activeWorkspace_)));
    for (auto it = workspaceLayouts_.cbegin(); it != workspaceLayouts_.cend(); ++it)
        settings.setValue(workspaceLayoutKey(it.key()), it.value());
    settings.setValue("window/workspace", activeWorkspace_);
    QMainWindow::closeEvent(event);
}
