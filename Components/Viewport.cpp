#include "Components/Viewport.h"

#include "DesignSystem/Icons.h"
#include "Workspaces/WorkspaceCatalog.h"

#include <QActionGroup>
#include <QHBoxLayout>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QToolButton>

#include <algorithm>
#include <cmath>
#include <utility>

namespace {
constexpr qreal WorldUnitsPerArtPixel = 2.0;
constexpr int ViewportToolbarBand = 33;

void drawTag(QPainter &painter, const QPointF &anchor, const QString &text, const QColor &border,
             Qt::Alignment alignment)
{
    const auto width = painter.fontMetrics().horizontalAdvance(text) + 18;
    QRectF box(anchor, QSizeF(width, 23));
    if (alignment == Qt::AlignRight)
        box.moveRight(anchor.x());
    painter.setPen(border);
    painter.setBrush(QColor(13, 19, 34, 235));
    painter.drawRoundedRect(box, 4, 4);
    painter.setPen(QColor("#e8edfa"));
    painter.drawText(box, Qt::AlignCenter, text);
}
}

Viewport::Viewport(ProjectStore &store, QWidget *parent)
    : QWidget(parent), store_(store), tools_(new QWidget(this))
{
    setObjectName("viewport");
    setMinimumSize(340, 250);
    setAccessibleName(tr("Scene viewport preview"));
    setToolTip(tr("Concept artwork and simulated scene tools"));
    tools_->setObjectName("viewportTools");
    auto *row = new QHBoxLayout(tools_);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(1);
    const auto button = [this, row](const QString &icon, const QString &title, bool menu = false) {
        auto *control = new QToolButton(tools_);
        if (!icon.isEmpty())
            control->setIcon(editorIcon(icon));
        control->setIconSize({15, 15});
        control->setFixedHeight(26);
        control->setToolTip(title);
        control->setAccessibleName(title);
        if (menu) {
            control->setText(title);
            control->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
            control->setPopupMode(QToolButton::InstantPopup);
        } else {
            control->setFixedWidth(33);
        }
        row->addWidget(control);
        return control;
    };
    auto *perspective = button("globe", tr("Perspective"), true);
    perspective->setMinimumWidth(119);
    auto *camera = new QMenu(perspective);
    camera->addAction(tr("Perspective"), this, [perspective] { perspective->setText(tr("Perspective")); });
    camera->addAction(tr("Top grid"), this, [this, perspective] {
        perspective->setText(tr("Top"));
        showGrid_ = true;
        update();
    });
    perspective->setMenu(camera);
    row->addSpacing(7);
    auto *lit = button("lit", tr("Lit"), true);
    auto *lighting = new QMenu(lit);
    lighting->addAction(tr("Lit"), this, [lit] { lit->setText(tr("Lit")); });
    lighting->addAction(tr("Reference artwork"), this, [lit] { lit->setText(tr("Artwork")); });
    lit->setMenu(lighting);
    row->addSpacing(7);
    auto *show = button({}, tr("Show"), true);
    show->setMinimumWidth(58);
    auto *menu = new QMenu(show);
    auto *markers = menu->addAction(tr("Object markers"));
    markers->setCheckable(true);
    markers->setChecked(showMarkers_);
    connect(markers, &QAction::toggled, this, [this](bool checked) { showMarkers_ = checked; update(); });
    auto *grid = menu->addAction(tr("Grid overlay"));
    grid->setCheckable(true);
    connect(grid, &QAction::toggled, this, [this](bool checked) { showGrid_ = checked; update(); });
    show->setMenu(menu);
    row->addStretch();
    auto *group = new QActionGroup(this);
    group->setExclusive(true);

    const QList<std::pair<Tool, QString>> toolIcons{
        {Tool::Select, "mouse"}, {Tool::Move, "move"}, {Tool::Rotate, "rotate"}, {Tool::Scale, "scale"}};
    const QStringList toolLabels{tr("Select"), tr("Move"), tr("Rotate"), tr("Scale")};
    for (qsizetype index = 0; index < toolIcons.size(); ++index) {
        const auto &[mode, icon] = toolIcons[index];
        auto *control = button(icon, toolLabels[index]);
        auto *action = group->addAction(editorIcon(icon), toolLabels[index]);
        action->setCheckable(true);
        action->setChecked(mode == Tool::Select);
        control->setDefaultAction(action);
        connect(action, &QAction::triggered, this, [this, mode] { tool_ = mode; });
    }
    row->addSpacing(8);
    auto *snapping = button("magnet", tr("Toggle snapping"));
    snapping->setCheckable(true);
    snapping->setChecked(snap_);
    connect(snapping, &QToolButton::toggled, this, [this](bool on) { snap_ = on; });
    secondaryTools_.append(snapping);
    const auto stepMenu = [this, button](const QString &icon, const QString &initial,
                              const QString &label, const QList<qreal> &steps, auto updateStep) {
        auto *control = button(icon, initial, true);
        control->setToolTip(label);
        control->setAccessibleName(label);
        secondaryTools_.append(control);
        auto *popup = new QMenu(control);
        for (const auto step : steps) {
            const auto text = QString::number(step);
            popup->addAction(text, this, [control, updateStep, step, text] {
                updateStep(step);
                control->setText(text);
            });
        }
        control->setMenu(popup);
    };
    stepMenu("snap", "10°", tr("Translation snap"), {1, 5, 10, 25, 50},
        [this](qreal step) { translationStep_ = int(step); });
    stepMenu("rotate", "0.25", tr("Scale snap"), {0.1, 0.25, 0.5, 1},
        [this](qreal step) { scaleStep_ = step; });
    stepMenu("rotate", "10°", tr("Rotation snap"), {5, 10, 15, 30, 45, 90},
        [this](qreal step) { rotationStep_ = int(step); });
    auto *markerToggle = button("eye", tr("Toggle object markers"));
    connect(markerToggle, &QToolButton::clicked, markers, &QAction::trigger);
    auto *gridToggle = button("snap", tr("Toggle viewport grid"));
    connect(gridToggle, &QToolButton::clicked, grid, &QAction::trigger);
    auto *center = button("rotate", tr("Select world center"));
    connect(center, &QToolButton::clicked, this, [this] {
        if (store_.object("castle-main"))
            store_.select("castle-main");
    });
    secondaryTools_.append({markerToggle, gridToggle, center});
    auto *fullscreen = button("expand", tr("Toggle fullscreen"));
    connect(fullscreen, &QToolButton::clicked, this, [this] {
        auto *editor = window();
        editor->setWindowState(editor->windowState() ^ Qt::WindowFullScreen);
    });

    connect(&store_, &ProjectStore::sceneChanged, this, qOverload<>(&Viewport::update));
    connect(&store_, &ProjectStore::selectionChanged, this, qOverload<>(&Viewport::update));
    setWorkspace(worldWorkspace());
}

void Viewport::setWorkspace(const Workspace &workspace)
{
    workspace_ = &workspace;
    art_ = workspaceArt(workspace);
    tools_->setVisible(worldActive());
    update();
}

bool Viewport::worldActive() const
{
    return workspace_ == &worldWorkspace();
}

void Viewport::setPreviewRunning(bool running)
{
    running_ = running;
    update();
}

QRectF Viewport::artSource() const
{
    return art_.rect();
}

QPointF Viewport::markerPosition(const Marker &marker, const SceneObject &object) const
{
    const auto source = artSource();
    const auto offset = object.position - marker.origin;
    const QPointF point(marker.normalized.x() * art_.width() + offset.x() / WorldUnitsPerArtPixel,
                        marker.normalized.y() * art_.height() - offset.z() / WorldUnitsPerArtPixel);
    return {(point.x() - source.left()) * width() / source.width(),
            ViewportToolbarBand + (point.y() - source.top()) * (height() - ViewportToolbarBand) / source.height()};
}

void Viewport::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    tools_->setGeometry(8, 6, width() - 16, 26);
    for (auto *control : secondaryTools_)
        control->setVisible(width() >= 735);
}

void Viewport::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHints(QPainter::Antialiasing | QPainter::SmoothPixmapTransform);
    QPainterPath clip;
    clip.addRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 5, 5);
    painter.setClipPath(clip);
    painter.fillRect(rect(), QColor("#0d1424"));

    const auto *project = store_.project();
    const bool hasWorldArt = project && project->id == "mythic-valley";
    const bool drawArt = !art_.isNull() && (!worldActive() || hasWorldArt);
    if (drawArt && worldActive()) {
        painter.save();
        painter.translate(0, ViewportToolbarBand);
        painter.scale(1, -1);
        painter.drawPixmap(QRectF(0, 0, width(), ViewportToolbarBand), art_,
            QRectF(0, 0, art_.width(), ViewportToolbarBand));
        painter.restore();
        painter.drawPixmap(QRectF(0, ViewportToolbarBand, width(), height() - ViewportToolbarBand), art_, artSource());
    } else if (drawArt) {
        QSize fitted = art_.size();
        fitted.scale(size(), Qt::KeepAspectRatio);
        QRect bounds(QPoint(), fitted);
        bounds.moveCenter(rect().center());
        painter.drawPixmap(bounds, art_);
    }

    if (!drawArt || (worldActive() && showGrid_)) {
        painter.setPen(QColor(142, 159, 196, drawArt ? 58 : 29));
        const int step = drawArt ? 48 : 32;
        for (int x = width() / 2 % step; x < width(); x += step)
            painter.drawLine(x, 0, x, height());
        for (int y = height() / 2 % step; y < height(); y += step)
            painter.drawLine(0, y, width(), y);
    }

    if (!drawArt) {
        const bool emptyWorld = worldActive() && (!project || project->objects.isEmpty());
        const auto icon = editorIcon(workspace_->icon, QColor("#aa78f3"));
        icon.paint(&painter, QRect(width() / 2 - 19, height() / 2 - 58, 38, 38));
        auto title = painter.font();
        title.setPixelSize(22);
        title.setWeight(QFont::DemiBold);
        painter.setFont(title);
        painter.setPen(QColor("#e8edfa"));
        const QString heading = worldActive() ? (project ? project->name : tr("No project open")) : workspace_->name;
        painter.drawText(QRect(12, height() / 2 - 12, width() - 24, 32), Qt::AlignCenter, heading);
        title.setPixelSize(13);
        title.setWeight(QFont::Normal);
        painter.setFont(title);
        painter.setPen(QColor("#a2aec8"));
        const QString note = emptyWorld
            ? (project ? tr("Add an object from the Scene Outliner to begin.") : tr("Create or open a project from Home."))
            : (worldActive() ? tr("Select and edit scene objects in the surrounding panels.") : tr("Workspace preview"));
        painter.drawText(QRect(12, height() / 2 + 20, width() - 24, 32), Qt::AlignCenter, note);
    }

    if (drawArt && worldActive() && showMarkers_) {
        for (const auto &marker : markers_) {
            const auto *object = store_.object(marker.id);
            if (!object || !object->visible)
                continue;
            const auto point = markerPosition(marker, *object);
            const bool active = marker.id == store_.selection();
            painter.setPen(QPen(active ? QColor("#d9b7ff") : QColor("#edf1fa"), active ? 2 : 1));
            painter.setBrush(active ? QColor("#7c3aed") : QColor(15, 22, 36, 160));
            painter.drawEllipse(point, active ? 7 : 4, active ? 7 : 4);
            if (active) {
                const auto anchor = point + QPointF(-11, 12);
                drawTag(painter, anchor, object->name, QColor("#aa68fa"), Qt::AlignRight);
            }
        }
    }

    if (worldActive() && project && (!drawArt || project->name != "MythicValley"))
        drawTag(painter, QPointF(width() - 8, height() - 31), tr("World: %1").arg(project->name), QColor("#536480"), Qt::AlignRight);
    if (running_)
        drawTag(painter, QPointF(width() - 12, 12), tr("Play preview running"), QColor("#52d58b"), Qt::AlignRight);

    painter.setClipping(false);
    painter.setPen(QColor("#35445d"));
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 5, 5);
}

void Viewport::mousePressEvent(QMouseEvent *event)
{
    if (!worldActive() || art_.isNull() || event->button() != Qt::LeftButton
        || !store_.project() || store_.project()->id != "mythic-valley")
        return;

    draggedId_.clear();
    for (const auto &marker : markers_) {
        const auto *object = store_.object(marker.id);
        if (object && object->visible && (markerPosition(marker, *object) - event->position()).manhattanLength() <= 31) {
            store_.select(marker.id);
            draggedId_ = marker.id;
            dragStart_ = event->position().toPoint();
            break;
        }
    }
}

void Viewport::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton || draggedId_.isEmpty())
        return;
    const auto id = std::exchange(draggedId_, {});
    const auto *record = store_.object(id);
    const auto delta = event->position().toPoint() - dragStart_;
    if (!record || tool_ == Tool::Select || delta.manhattanLength() < 3)
        return;

    auto object = *record;
    const auto snapped = [this](qreal value, qreal step) {
        return snap_ ? std::round(value / step) * step : value;
    };
    switch (tool_) {
    case Tool::Move: {
        const auto source = artSource();
        object.position += QVector3D(
            float(snapped(delta.x() * source.width() / width() * WorldUnitsPerArtPixel, translationStep_)),
            0, float(snapped(-delta.y() * source.height() / (height() - ViewportToolbarBand) * WorldUnitsPerArtPixel, translationStep_)));
        store_.updateObject(object, tr("Move %1").arg(object.name));
        break;
    }
    case Tool::Rotate:
        object.rotation += QVector3D(0, float(snapped(delta.x(), rotationStep_)), 0);
        store_.updateObject(object, tr("Rotate %1").arg(object.name));
        break;
    case Tool::Scale:
        object.scale *= float(1 + snapped(std::exp(std::clamp(delta.x() / 180.0, -1.0, 1.0)) - 1, scaleStep_));
        store_.updateObject(object, tr("Scale %1").arg(object.name));
        break;
    case Tool::Select:
        break;
    }
}
