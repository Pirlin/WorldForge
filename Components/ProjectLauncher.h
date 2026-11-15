#pragma once

#include <QJsonObject>
#include <QWidget>

class QListView;
class QAbstractItemModel;
class QLineEdit;
class QComboBox;
class QPushButton;
class QToolButton;
class QLabel;
class QStackedWidget;

class ProjectLauncher final : public QWidget {
    Q_OBJECT

public:
    explicit ProjectLauncher(QAbstractItemModel *projects,QWidget *parent = nullptr);
    QListView *recentList() const { return recent_; }
    void showCreator();
    void showRecent();
    void showError(const QString &message);

signals:
    void createRequested(const QString &name,const QJsonObject &configuration);
    void openRequested();
    void renameRequested();
    void deleteRequested();

private:
    void updateLocation();
    void selectTemplate(int index);
    void preview(bool animated = false);
    void submit();

    QStackedWidget *pages_ = nullptr;
    QListView *recent_ = nullptr;
    QLineEdit *name_ = nullptr;
    QLineEdit *location_ = nullptr;
    QComboBox *quality_ = nullptr;
    QComboBox *starterContent_ = nullptr;
    QLabel *previewTitle_ = nullptr;
    QLabel *previewText_ = nullptr;
    QLabel *previewFeatures_ = nullptr;
    QLabel *recommended_ = nullptr;
    QLabel *error_ = nullptr;
    QPushButton *previewArt_ = nullptr;
    QToolButton *play_ = nullptr;
    QList<QPushButton *> cards_;
    QString category_;
    int selected_ = 0;
    QString folder_;
};
