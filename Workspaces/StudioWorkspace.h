#pragma once

#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QVariant>

class ProjectStore;
struct SceneObject;
class QDoubleSpinBox;
class QFormLayout;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QScrollArea;
class QTabBar;
class QTabWidget;
class QTreeWidget;
class QTreeWidgetItem;
class QVBoxLayout;
class QWidget;
struct Workspace;

class StudioWorkspace : public QObject {
    Q_OBJECT

public:
    StudioWorkspace(const Workspace &workspace, ProjectStore &store, QObject *parent);
    QWidget *navigator() const;
    QWidget *inspector() const;
    QWidget *tools() const;
    QWidget *surface() const { return surface_; }
    QString navigatorTitle() const;
    QString inspectorTitle() const;
    QString toolsTitle() const;
    QList<int> dockSizes() const;

signals:
    void closeRequested(int dock);
    void floatRequested(int dock);

private:
    QTabWidget *createTabs(int dock);
    QWidget *createSurface(const Workspace &workspace);
    QWidget *createCommandRow(const QJsonArray &commands, QWidget *parent);
    void rebuildNavigator();
    void filterNavigator();
    void selectNamedItem(const QString &name);
    void syncSelection();
    void refreshInspector();
    void populateAssets();
    void openCodeFile(const QString &file);
    void buildProperties(QVBoxLayout *layout, const QJsonArray &sections, const QString &key);
    const SceneObject *inspectedObject() const;
    QString currentKey() const;

    ProjectStore &store_;
    const QJsonObject config_;
    const QString reference_;
    QTreeWidget *navigator_;
    QLineEdit *navSearch_;
    QTabWidget *inspector_;
    QTabWidget *tools_;
    QScrollArea *properties_;
    QWidget *surface_ = nullptr;
    QLineEdit *assetSearch_ = nullptr;
    QTreeWidget *assets_ = nullptr;
    QPlainTextEdit *code_ = nullptr;
    QTabBar *codeTabs_ = nullptr;
    QString activeFile_;
    QString inspectorKey_;
    QLineEdit *boundName_ = nullptr;
    QHash<QString, QString> buffers_;
    QHash<QString, QVariant> values_;
    QHash<QString, QDoubleSpinBox *> commandFields_;
};
