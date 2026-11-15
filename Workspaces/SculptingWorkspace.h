#pragma once

#include "Engine/Geometry/SculptBrush.h"

#include <QSharedPointer>
#include <QWidget>
#include <optional>

class ProjectStore;
class LiveViewport;
class QComboBox;
class QDoubleSpinBox;
class QCheckBox;
class QLabel;

class SculptingWorkspace : public QWidget {
    Q_OBJECT
public:
    explicit SculptingWorkspace(ProjectStore &store,QWidget *parent = nullptr);

protected:
    void hideEvent(QHideEvent *event) override;

private:
    struct Stroke {
        QString id;
        QSharedPointer<const worldforge::Mesh> base;
        QSharedPointer<worldforge::Mesh> mesh;
        worldforge::SculptStroke brush;
        bool changed = false;
    };
    void refresh();
    void finish(bool commit);
    ProjectStore &store_;
    LiveViewport *viewport_;
    QComboBox *object_;
    QComboBox *brush_;
    QDoubleSpinBox *radius_;
    QDoubleSpinBox *strength_;
    QCheckBox *symmetry_;
    QLabel *status_;
    std::optional<Stroke> stroke_;
};
