#pragma once
#include "State/ProjectStore.h"

#include <QPixmap>
#include <QWidget>

class QToolButton;
struct Workspace;

class Viewport : public QWidget {
public:
    explicit Viewport(ProjectStore &store, QWidget *parent = nullptr);
    void setWorkspace(const Workspace &workspace);
    void setPreviewRunning(bool running);

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;

private:
    enum class Tool { Select, Move, Rotate, Scale };
    struct Marker { QString id; QPointF normalized; QVector3D origin = {}; };

    QRectF artSource() const;
    QPointF markerPosition(const Marker &marker, const SceneObject &object) const;
    bool worldActive() const;

    ProjectStore &store_;
    const Workspace *workspace_ = nullptr;
    QPixmap art_;
    QWidget *tools_;
    QList<QWidget *> secondaryTools_;
    Tool tool_ = Tool::Select;
    bool showMarkers_ = false;
    bool showGrid_ = false;
    bool snap_ = true;
    int translationStep_ = 10;
    int rotationStep_ = 10;
    qreal scaleStep_ = 0.25;
    bool running_ = false;
    QString draggedId_;
    QPoint dragStart_;
    QList<Marker> markers_{{"castle-main", {0.80, 0.38}, {12568, 420, 3620}}, {"archway", {0.14, 0.58}},
        {"landscape", {0.51, 0.35}}, {"river", {0.65, 0.79}}, {"hero-knight", {0.36, 0.77}}};
};
