#pragma once
#include "State/ProjectStore.h"

#include <QMainWindow>

class ContentBrowser;
class Outliner;
class StudioWorkspace;
class Viewport;
class QLabel;
class QListView;
class QMenu;
class QMenuBar;
class QStackedWidget;
class QStandardItemModel;
class QTabBar;
class QToolBar;

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);

protected:
    void closeEvent(QCloseEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    bool nativeEvent(const QByteArray &eventType, void *message, qintptr *result) override;

private:
    void createActions();
    void createNavigation();
    QWidget *createHome();
    QWidget *createEditor();
    void createDocks();
    void createStatusBar();
    void resetLayout();
    void switchWorkspace(int index);
    StudioWorkspace *studio(int index);
    void refreshProjects();
    void refreshStatus();
    void createProject();
    void renameProject();
    void deleteProject();
    bool saveProject(bool announce);

    ProjectStore store_;
    QStackedWidget *pages_ = nullptr;
    QTabBar *workspaceTabs_ = nullptr;
    QMenu *workspaceMenu_ = nullptr;
    QMenu *windowMenu_ = nullptr;
    QMenuBar *mainMenu_ = nullptr;
    QToolBar *commands_ = nullptr;
    QAction *save_ = nullptr;
    QAction *play_ = nullptr;
    QAction *renameProject_ = nullptr;
    QAction *deleteProject_ = nullptr;
    QAction *openWorld_ = nullptr;
    QAction *deleteObject_ = nullptr;
    QStandardItemModel *projects_ = nullptr;
    QListView *projectList_ = nullptr;
    QLabel *homeProject_ = nullptr;
    QLabel *navigatorTitle_ = nullptr;
    QLabel *navigatorIcon_ = nullptr;
    QLabel *statusReady_ = nullptr;
    QLabel *statusProject_ = nullptr;
    QLabel *statusWorkspace_ = nullptr;
    QList<QWidget *> statusMetrics_;
    Outliner *outliner_ = nullptr;
    Viewport *viewport_ = nullptr;
    ContentBrowser *browser_ = nullptr;
    QList<QDockWidget *> docks_;
    QList<QStackedWidget *> dockPages_;
    QList<QAction *> workspaceActions_;
    QHash<int, StudioWorkspace *> studios_;
    QHash<int, QByteArray> workspaceLayouts_;
    int activeWorkspace_ = -1;
};
