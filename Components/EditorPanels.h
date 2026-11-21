#pragma once
#include "State/ProjectStore.h"

#include <QWidget>
#include <array>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QListView;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QStandardItemModel;
class QTabWidget;
class QTreeView;

class Outliner : public QWidget {
public:
    explicit Outliner(ProjectStore &store, QWidget *parent = nullptr);
    void focusSearch();

private:
    void rebuild();
    void syncSelection();

    ProjectStore &store_;
    QLineEdit *search_;
    QTreeView *tree_;
    QStandardItemModel *model_;
    bool showDemo_ = false;
};

class Inspector : public QWidget {
    Q_OBJECT

public:
    explicit Inspector(ProjectStore &store, QWidget *parent = nullptr);

signals:
    void closeRequested();
    void floatRequested();

private:
    void refresh();
    void apply();

    ProjectStore &store_;
    QLabel *type_;
    QLabel *identity_;
    QWidget *form_;
    QLineEdit *name_;
    QComboBox *group_;
    QComboBox *mesh_;
    QComboBox *materialSlot_;
    QComboBox *behavior_ = nullptr;
    QPushButton *behaviorComponents_ = nullptr;
    QLabel *meshInfo_;
    QLabel *geometryState_;
    QPushButton *materialColor_;
    QDoubleSpinBox *intensity_;
    QComboBox *bodyMode_;
    QComboBox *colliderShape_ = nullptr;
    QDoubleSpinBox *bounce_;
    QDoubleSpinBox *terrainSize_;
    QDoubleSpinBox *foliageDensity_ = nullptr;
    QDoubleSpinBox *foliageSeed_ = nullptr;
    QDoubleSpinBox *foliageWind_ = nullptr;
    QDoubleSpinBox *spin_ = nullptr;
    QDoubleSpinBox *mass_ = nullptr;
    QDoubleSpinBox *friction_ = nullptr;
    QComboBox *jointType_ = nullptr;
    QComboBox *jointTarget_ = nullptr;
    QComboBox *jointAxis_ = nullptr;
    std::array<QDoubleSpinBox *,3> jointOffset_{};
    QCheckBox *visible_;
    QTreeView *components_;
    QStandardItemModel *componentModel_;
    std::array<QDoubleSpinBox *, 9> axes_{};
};

class ContentBrowser : public QWidget {
    Q_OBJECT

public:
    explicit ContentBrowser(ProjectStore &store, QWidget *parent = nullptr);
    void appendLog(const QString &message);
    void appendMessage(const QString &message);
    void showMessages();
    void showLog();
    void showTerminal();
    void setMetrics(const QString &report);

signals:
    void closeRequested();
    void floatRequested();
    void saveRequested();
    void meshImported();
    void newStudioAssetRequested();
    void editStudioAssetRequested(const QString &assetId);
    void openAssetRequested(const QString &assetId);

private:
    void rebuild();
    void syncSelection();

    ProjectStore &store_;
    QLineEdit *search_;
    QTreeView *folders_;
    QListView *tiles_;
    QStandardItemModel *folderModel_;
    QStandardItemModel *assetModel_;
    QTabWidget *tabs_;
    QLabel *path_;
    QLabel *count_;
    QPlainTextEdit *log_;
    QPlainTextEdit *messages_;
    QLabel *profiler_;
    int sort_ = 0;
};
