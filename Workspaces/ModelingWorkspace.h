#pragma once

#include <QWidget>
#include <functional>

#include "Engine/Geometry/Mesh.h"

class QLabel;
class QListWidget;
class QDoubleSpinBox;
class QSpinBox;
class ProjectStore;
class LiveViewport;
class UvCanvas;

class ModelingWorkspace : public QWidget {
    Q_OBJECT

public:
    explicit ModelingWorkspace(ProjectStore &store, QWidget *parent = nullptr);

private:
    void refresh();
    void selectFace(int face);
    void edit(const QString &label, const std::function<std::string(worldforge::Mesh &, std::size_t)> &operation);

    ProjectStore &store_;
    LiveViewport *viewport_;
    UvCanvas *uv_;
    QListWidget *outliner_;
    QLabel *summary_;
    QLabel *message_;
    QDoubleSpinBox *amount_;
    QSpinBox *edge_;
    QString object_;
    int face_ = -1;
};
