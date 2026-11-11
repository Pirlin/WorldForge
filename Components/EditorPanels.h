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
    std::array<QComboBox *, 3> materials_{};
    QComboBox *mesh_;
    QComboBox *lodGroup_;
    QSpinBox *lods_;
    QCheckBox *autoLod_;
    QCheckBox *collision_;
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
    void showMessages();

signals:
    void closeRequested();
    void floatRequested();
    void saveRequested();

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
    int sort_ = 0;
};
