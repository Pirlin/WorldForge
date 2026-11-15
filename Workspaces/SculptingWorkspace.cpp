#include "Workspaces/SculptingWorkspace.h"
#include "Components/LiveViewport.h"
#include "State/ProjectStore.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QVBoxLayout>
#include <algorithm>
#include <utility>

namespace wf = worldforge;

SculptingWorkspace::SculptingWorkspace(ProjectStore &store,QWidget *parent)
    : QWidget(parent),store_(store),viewport_(new LiveViewport(store,true,this)),object_(new QComboBox(this)),
      brush_(new QComboBox(this)),radius_(new QDoubleSpinBox(this)),strength_(new QDoubleSpinBox(this)),
      symmetry_(new QCheckBox(tr("Symmetry X"),this)),status_(new QLabel(this))
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0,0,0,0);
    auto *controls = new QWidget(this);
    auto *tools = new QHBoxLayout(controls);
    tools->setContentsMargins(8,4,8,4);
    layout->addWidget(controls);
    const auto field = [this,tools](const QString &name,QWidget *widget) {
        auto *label = new QLabel(name,this);
        label->setBuddy(widget);
        widget->setAccessibleName(name);
        tools->addWidget(label);
        tools->addWidget(widget);
    };
    object_->setMinimumWidth(160);
    field(tr("Mesh"),object_);
    brush_->addItems({tr("Draw"),tr("Smooth"),tr("Inflate"),tr("Flatten")});
    field(tr("Brush"),brush_);
    radius_->setDecimals(3);
    radius_->setRange(0.001,1000);
    radius_->setValue(0.25);
    radius_->setSuffix(tr(" m"));
    field(tr("Radius"),radius_);
    strength_->setRange(1,100);
    strength_->setDecimals(0);
    strength_->setValue(50);
    strength_->setSuffix(tr(" %"));
    field(tr("Strength"),strength_);
    tools->addWidget(symmetry_);
    auto *sphere = new QPushButton(tr("Add Sculpt Sphere"),this);
    sphere->setEnabled(store_.project());
    tools->addWidget(sphere);
    auto *frame = new QPushButton(tr("Frame"),this);
    tools->addWidget(frame);
    tools->addStretch();
    layout->addWidget(viewport_,1);
    status_->setContentsMargins(8,0,8,4);
    status_->setAccessibleName(tr("Sculpting result"));
    layout->addWidget(status_);
    viewport_->setSculptMode(true);

    connect(frame,&QPushButton::clicked,viewport_,&LiveViewport::frame);
    connect(sphere,&QPushButton::clicked,this,[this] {
        QString error;
        if (!store_.addMeshPrimitive(wf::Primitive::Sphere,{2,2,2},96,64,tr("Sculpt Sphere"),error))
            status_->setText(error);
    });
    connect(object_,&QComboBox::currentIndexChanged,this,[this](int index) {
        if (index >= 0)
            store_.select(object_->itemData(index).toString());
    });
    connect(&store_,&ProjectStore::sceneChanged,this,&SculptingWorkspace::refresh);
    connect(&store_,&ProjectStore::selectionChanged,this,&SculptingWorkspace::refresh);
    connect(&store_,&ProjectStore::projectsChanged,sphere,[this,sphere] { sphere->setEnabled(store_.project()); });
    connect(viewport_,&LiveViewport::sculptStrokeBegan,this,[this] {
        const auto *asset = store_.selectedMesh();
        if (!asset || stroke_)
            return;
        auto candidate = QSharedPointer<wf::Mesh>::create(*asset->mesh);
        stroke_.emplace(Stroke{asset->id,asset->mesh,std::move(candidate),wf::SculptStroke(*asset->mesh)});
    });
    connect(viewport_,&LiveViewport::sculptSample,this,[this](const QVector3D &position,const QVector3D &normal,Qt::KeyboardModifiers mods) {
        if (!stroke_)
            return;
        auto &stroke = *stroke_;
        const auto brush = mods.testFlag(Qt::ShiftModifier) ? wf::SculptBrush::Smooth : wf::SculptBrush(brush_->currentIndex());
        const auto strength = float(strength_->value()/100)*(mods.testFlag(Qt::ControlModifier) ? -1.0f : 1.0f);
        if (stroke.brush.dab(*stroke.mesh,{position.x(),position.y(),position.z()},{normal.x(),normal.y(),normal.z()},
                             float(radius_->value()),strength,brush,symmetry_->isChecked())) {
            stroke.changed = true;
            viewport_->setModelPreview(stroke.mesh);
        }
    });
    connect(viewport_,&LiveViewport::sculptStrokeFinished,this,&SculptingWorkspace::finish);
    refresh();
}

void SculptingWorkspace::refresh()
{
    const auto *asset = store_.selectedMesh();
    if (stroke_ && (!asset || asset->id != stroke_->id || asset->mesh != stroke_->base))
        finish(false);
    const QSignalBlocker blocked(object_);
    object_->clear();
    object_->addItem(tr("Select a mesh"),QString{});
    if (const auto *project = store_.project())
        for (const auto &object : project->objects)
            if (!object.meshAsset.isEmpty())
                object_->addItem(object.name,object.id);
    object_->setCurrentIndex(std::max(0,object_->findData(store_.selection())));
    status_->setText(asset ? tr("%1 vertices · %2 faces · Shift smooth · Ctrl invert · Esc cancel")
        .arg(asset->mesh->vertices.size()).arg(asset->mesh->faces.size()) : tr("Select an existing mesh or add a sculpt sphere."));
}

void SculptingWorkspace::finish(bool commit)
{
    if (!stroke_)
        return;
    auto stroke = std::exchange(stroke_,std::nullopt);
    viewport_->setModelPreview({});
    if (!commit || !stroke->changed)
        return;
    QString error;
    if (!store_.updateMesh(stroke->id,std::move(*stroke->mesh),tr("Sculpt Stroke"),error,stroke->base.data()))
        status_->setText(error);
}

void SculptingWorkspace::hideEvent(QHideEvent *event)
{
    finish(false);
    QWidget::hideEvent(event);
}
