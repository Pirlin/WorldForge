#pragma once
#include "State/ProjectStore.h"
#include "App/SystemMetrics.h"

#include <QMainWindow>
#include <functional>

class ContentBrowser;
class Outliner;
class StudioWorkspace;
class LiveViewport;
class EmbeddedStudio;
class ForgeAiPanel;
class ProjectLauncher;
class QLabel;
class QDialog;
class QListView;
class QMenu;
class QMenuBar;
class QStackedWidget;
class QStandardItemModel;
class QTabBar;
class QToolBar;
class QToolButton;
class QProgressBar;
class QPushButton;

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr, bool launchCreator = false);

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
    void updateMetrics();
    void editAtmosphere();
    void switchWorkspace(int index);
    void showStudioWorkspace(int index);
    void openEmbeddedStudio(bool editSelected = false, const QString &assetId = {}, bool newAsset = false,
                            int workspace = 2, QString newAssetName = {});
    void publishStudioMesh();
    void showStudioAssets(int workspace);
    void openAssetDetails(const QString &assetId,int workspace = 2);
    void refreshAssetDetails();
    StudioWorkspace *studio(int index);
    void refreshProjects();
    void refreshStatus();
    void createProject();
    void renameProject();
    void deleteProject();
    bool saveProject(bool announce);

    ProjectStore store_;
    QStackedWidget *pages_ = nullptr;
    EmbeddedStudio *embeddedStudio_ = nullptr;
    QDialog *assetDetails_ = nullptr;
    LiveViewport *assetPreview_ = nullptr;
    QLabel *assetSummary_ = nullptr;
    QPushButton *editInspectedAsset_ = nullptr;
    QString inspectedAsset_;
    QString inspectedProject_;
    int assetWorkspace_ = 2;
    QString studioProjectId_;
    QString studioAssetId_;
    QString studioSourceRevision_;
    QString studioNewAssetName_;
    QString studioAppliedRevision_;
    QSharedPointer<const worldforge::Mesh> studioAssetRevision_;
    QTabBar *workspaceTabs_ = nullptr;
    QMenu *workspaceMenu_ = nullptr;
    QMenu *windowMenu_ = nullptr;
    QMenuBar *mainMenu_ = nullptr;
    QToolBar *commands_ = nullptr;
    QAction *save_ = nullptr;
    QAction *applyStudioUpdate_ = nullptr;
    QAction *searchStudioAssets_ = nullptr;
    QAction *forgeAi_ = nullptr;
    ForgeAiPanel *forgeAiPanel_ = nullptr;
    QAction *renameProject_ = nullptr;
    QAction *deleteProject_ = nullptr;
    QAction *openWorld_ = nullptr;
    QAction *deleteObject_ = nullptr;
    QAction *play_ = nullptr;
    QAction *simulate_ = nullptr;
    QAction *stop_ = nullptr;
    QAction *undo_ = nullptr;
    QAction *redo_ = nullptr;
    QWidget *worldAuthoring_ = nullptr;
    QToolButton *pauseRuntime_ = nullptr;
    QToolButton *stepRuntime_ = nullptr;
    QStandardItemModel *projects_ = nullptr;
    QListView *projectList_ = nullptr;
    QLabel *homeProject_ = nullptr;
    ProjectLauncher *launcher_ = nullptr;
    bool launchCreator_ = false;
    QLabel *navigatorTitle_ = nullptr;
    QLabel *navigatorIcon_ = nullptr;
    QLabel *statusReady_ = nullptr;
    QLabel *statusProject_ = nullptr;
    QLabel *statusWorkspace_ = nullptr;
    QLabel *statusRenderer_ = nullptr;
    QLabel *statusSave_ = nullptr;
    SystemMetrics metrics_;
    QList<QLabel *> metricValues_;
    QList<QProgressBar *> metricMeters_;
    QList<QWidget *> statusMetrics_;
    Outliner *outliner_ = nullptr;
    LiveViewport *liveWorld_ = nullptr;
    QString worldViewProject_;
    ContentBrowser *browser_ = nullptr;
    QList<QDockWidget *> docks_;
    QList<QStackedWidget *> dockPages_;
    QList<QAction *> workspaceActions_;
    QHash<int, StudioWorkspace *> studios_;
    QHash<int, QWidget *> gameplayWorkspaces_;
    QHash<int, QByteArray> workspaceLayouts_;
    int activeWorkspace_ = -1;
    int pendingStudioWorkspace_ = -1;
    std::function<void()> nextStudioSession_;
};
