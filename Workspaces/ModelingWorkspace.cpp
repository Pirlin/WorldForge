#include "Workspaces/ModelingWorkspace.h"
#include "Components/LiveViewport.h"
#include "DesignSystem/Icons.h"
#include "Engine/Geometry/MeshRemesh.h"
#include "Engine/Geometry/MeshTopology.h"
#include "Engine/Geometry/MeshUv.h"
#include "State/ProjectStore.h"
#include "State/GlbImport.h"

#include <QAction>
#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QColorDialog>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QDropEvent>
#include <QDrag>
#include <QMimeData>
#include <QEventLoop>
#include <QFileDialog>
#include <QFile>
#include <QImageReader>
#include <QFormLayout>
#include <QHideEvent>
#include <QInputDialog>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QProgressDialog>
#include <QQuaternion>
#include <QSaveFile>
#include <QScrollArea>
#include <QGroupBox>
#include <QGridLayout>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTableWidget>
#include <QTreeWidget>
#include <QHeaderView>
#include <QSplitter>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <future>
#include <iterator>
#include <limits>
#include <numbers>
#include <thread>

namespace wf = worldforge;

namespace {
class ModelOutliner : public QTreeWidget {
public:
    using QTreeWidget::QTreeWidget;
    std::function<void(const QString &,const QString &)> reparent;
protected:
    void startDrag(Qt::DropActions) override
    {
        const auto *item = currentItem();
        if (!item || item->data(0,Qt::UserRole).toString().isEmpty())
            return;
        const auto source = item->data(0,Qt::UserRole).toString();
        auto *payload = new QMimeData;
        payload->setData("application/x-worldforge-object",source.toUtf8());
        QDrag drag(this);
        drag.setMimeData(payload);
        drag.exec(Qt::MoveAction);
    }
    void dragEnterEvent(QDragEnterEvent *event) override
    {
        if (event->source() == this && event->mimeData()->hasFormat("application/x-worldforge-object"))
            event->acceptProposedAction();
    }
    void dragMoveEvent(QDragMoveEvent *event) override
    {
        if (event->source() == this)
            event->acceptProposedAction();
    }
    void dropEvent(QDropEvent *event) override
    {
        if (event->source() != this || !reparent)
            return;
        const auto child = QString::fromUtf8(event->mimeData()->data("application/x-worldforge-object"));
        const auto *item = itemAt(event->position().toPoint());
        const auto parent = item ? item->data(0,Qt::UserRole).toString() : QString{};
        if (!child.isEmpty() && child != parent) {
            event->acceptProposedAction();
            reparent(child,parent);
        }
    }
};

class RevolveProfileView : public QWidget {
public:
    explicit RevolveProfileView(QWidget *parent) : QWidget(parent)
    {
        setMinimumHeight(125);
        setAccessibleName(tr("Revolve profile preview"));
    }

    void setPoints(std::vector<wf::Point2> controls,std::vector<wf::Point2> outline)
    {
        controls_ = std::move(controls);
        outline_ = std::move(outline);
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.fillRect(rect(),QColor("#10131c"));
        if (controls_.empty())
            return;
        painter.setRenderHint(QPainter::Antialiasing);
        const auto area = rect().adjusted(22,12,-22,-12);
        const auto [low,high] = std::ranges::minmax_element(controls_,{},[](auto p) { return p[1]; });
        const auto wide = std::ranges::max_element(controls_,{},[](auto p) { return p[0]; });
        const auto scale = std::min(area.width()/std::max(0.1f,(*wide)[0]),
            area.height()/std::max(0.1f,(*high)[1]-(*low)[1]));
        const QPointF origin(area.left(),area.center().y()+((*high)[1]+(*low)[1])*0.5*scale);
        const auto project = [&](wf::Point2 p) { return origin+QPointF(p[0]*scale,-p[1]*scale); };
        painter.setPen(QPen(QColor("#718098"),1,Qt::DashLine));
        painter.drawLine(area.topLeft(),area.bottomLeft());
        QPolygonF polyline;
        for (const auto &p : outline_)
            polyline.append(project(p));
        painter.setPen(QPen(QColor("#ae7aff"),2));
        painter.drawPolyline(polyline);
        painter.setBrush(QColor("#e3ceff"));
        for (const auto &p : controls_)
            painter.drawEllipse(project(p),3,3);
    }

private:
    std::vector<wf::Point2> controls_, outline_;
};
}

class UvCanvas : public QWidget {
public:
    explicit UvCanvas(QWidget *parent) : QWidget(parent)
    {
        setMinimumSize(160, 160);
        setAccessibleName(tr("UV canvas"));
        setToolTip(tr("Click a face to select it. Drag the selected UV polygon to move it."));
    }
    void setMesh(QSharedPointer<const wf::Mesh> mesh,int selected,const std::set<wf::MeshEdge> &seams,const wf::UvPins &pins)
    {
        if (mesh_ != mesh || selected_ != selected)
            dragging_ = false;
        if (mesh_ != mesh || seams_ != seams)
            islands_.clear();
        mesh_ = std::move(mesh);
        selected_ = selected;
        seams_ = seams;
        pins_ = pins;
        update();
    }
    void setIslandMode(bool enabled)
    {
        islandMode_ = enabled;
        update();
    }
    void setChecker(bool enabled)
    {
        checker_ = enabled;
        update();
    }
    void setPinMode(bool enabled)
    {
        pinMode_ = enabled;
        dragging_ = false;
        setCursor(enabled ? Qt::CrossCursor : Qt::ArrowCursor);
        update();
    }
    std::function<void(int)> select;
    std::function<void(wf::Point2)> move;
    std::function<void(wf::UvCorner)> togglePin;

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.fillRect(rect(), QColor("#141822"));
        const auto bounds = tile();
        if (checker_)
            for (int row = 0; row < 8; ++row)
                for (int column = 0; column < 8; ++column)
                    painter.fillRect(QRectF(bounds.x()+column*bounds.width()/8,bounds.y()+row*bounds.height()/8,
                        bounds.width()/8,bounds.height()/8),(row+column)%2 ? QColor("#2a3041") : QColor("#202535"));
        painter.setPen(QColor("#343b4b"));
        for (int i = 0; i <= 8; ++i) {
            const qreal x = bounds.left() + bounds.width()*i/8;
            const qreal y = bounds.top() + bounds.height()*i/8;
            painter.drawLine(QPointF(x, bounds.top()), QPointF(x, bounds.bottom()));
            painter.drawLine(QPointF(bounds.left(), y), QPointF(bounds.right(), y));
        }
        painter.setPen(QColor("#b9c2d4"));
        painter.drawText(QPointF(12, 19), tr("UV Editing"));
        if (!mesh_)
            return;
        painter.setPen(QColor("#717a91"));
        painter.setBrush(Qt::NoBrush);
        const auto island = highlighted();
        const std::set<std::size_t> highlightedFaces(island.begin(),island.end());
        for (std::size_t i = 0; i < mesh_->faces.size(); ++i) {
            const bool selected = highlightedFaces.contains(i);
            painter.setPen(selected ? QPen(QColor("#cba6ff"),2) : QPen(QColor("#717a91")));
            painter.setBrush(selected ? QBrush(QColor(142,78,225,90)) : Qt::NoBrush);
            painter.drawPolygon(polygon(i));
        }
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor("#cba6ff"));
        if (pinMode_ && selected_ >= 0 && std::size_t(selected_) < mesh_->faces.size())
            for (const auto &corner : mesh_->faces[std::size_t(selected_)].corners)
                painter.drawEllipse(uvPoint(corner.uv,bounds),2,2);
        painter.setBrush(QColor("#ffac68"));
        for (const auto &[face,corner] : pins_)
            painter.drawEllipse(uvPoint(mesh_->faces[face].corners[corner].uv,bounds),4,4);
    }
    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() != Qt::LeftButton || !mesh_)
            return;
        if (pinMode_) {
            const auto bounds = tile();
            std::optional<wf::UvCorner> hit;
            qreal nearest = 8;
            for (std::size_t f = mesh_->faces.size(); f-- > 0;) {
                const auto &corners = mesh_->faces[f].corners;
                for (std::size_t c = 0; c < corners.size(); ++c) {
                    const auto offset = uvPoint(corners[c].uv,bounds)-event->position();
                    const auto distance = std::hypot(offset.x(),offset.y());
                    const wf::UvCorner corner{f,c};
                    const auto priority = [&](wf::UvCorner point) {
                        return 2*int(pins_.contains(point))+int(int(point.first) == selected_);
                    };
                    if (distance < nearest-0.05 || (hit && std::abs(distance-nearest) <= 0.05
                            && priority(corner) > priority(*hit))) {
                        nearest = distance;
                        hit = corner;
                    }
                }
            }
            if (hit)
                togglePin(*hit);
            return;
        }
        start_ = event->position();
        if (std::ranges::any_of(highlighted(),[&](auto face) { return polygon(face).containsPoint(start_,Qt::OddEvenFill); })) {
            dragging_ = true;
            return;
        }
        for (std::size_t i = mesh_->faces.size(); i > 0; --i) {
            if (polygon(i-1).containsPoint(start_, Qt::OddEvenFill)) {
                select(int(i-1));
                dragging_ = selected_ == int(i-1);
                return;
            }
        }
        select(-1);
    }
    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (!dragging_ || event->button() != Qt::LeftButton)
            return;
        dragging_ = false;
        const auto delta = event->position() - start_;
        if (delta.manhattanLength() >= 2)
            move({float(delta.x()/tile().width()), float(-delta.y()/tile().height())});
    }

private:
    wf::UvIsland highlighted()
    {
        if (!mesh_ || selected_ < 0 || std::size_t(selected_) >= mesh_->faces.size())
            return {};
        if (islandMode_) {
            if (islands_.empty())
                islands_ = wf::uvIslands(*mesh_,seams_,true);
            for (const auto &island : islands_)
                if (std::ranges::find(island,std::size_t(selected_)) != island.end())
                    return island;
        }
        return {std::size_t(selected_)};
    }
    QRectF tile() const
    {
        const qreal size = std::max(1, std::min(width()-38, height()-60));
        return {(width()-size)/2, (height()-size)/2, size, size};
    }
    QPolygonF polygon(std::size_t face) const
    {
        QPolygonF polygon;
        const auto bounds = tile();
        for (const auto &corner : mesh_->faces[face].corners)
            polygon << uvPoint(corner.uv,bounds);
        return polygon;
    }
    static QPointF uvPoint(wf::Point2 uv,const QRectF &bounds)
    {
        return {bounds.left()+uv[0]*bounds.width(),bounds.bottom()-uv[1]*bounds.height()};
    }
    QSharedPointer<const wf::Mesh> mesh_;
    std::set<wf::MeshEdge> seams_;
    wf::UvPins pins_;
    std::vector<wf::UvIsland> islands_;
    int selected_ = -1;
    bool dragging_ = false;
    bool islandMode_ = false, checker_ = false, pinMode_ = false;
    QPointF start_;
};

ModelingWorkspace::ModelingWorkspace(ProjectStore &store, QWidget *parent)
    : QWidget(parent), store_(store), viewport_(new LiveViewport(store, true, this)), uv_(new UvCanvas(this)),
      outliner_(new ModelOutliner(this)), summary_(new QLabel(this)), message_(new QLabel(this)),
      amount_(new QDoubleSpinBox(this)), edge_(new QSpinBox(this))
{
    setObjectName("modelingWorkspace");
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(3);
    auto *header = new QHBoxLayout;
    header->setContentsMargins(5, 3, 5, 0);
    layout->addLayout(header);
    const auto button = [this](QHBoxLayout *row, const QString &name, const QString &icon = {}) {
        auto *button = new QToolButton(this);
        button->setText(name);
        if (!icon.isEmpty())
            button->setIcon(editorIcon(icon));
        button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        row->addWidget(button);
        return button;
    };
    const auto tool = [header,&button](const QString &name, const QString &icon = {}) { return button(header,name,icon); };
    auto *multiEdit = tool(tr("Multi-Object"),"move");
    auto *multiMenu = new QMenu(multiEdit);
    multiEdit->setMenu(multiMenu);
    multiEdit->setPopupMode(QToolButton::InstantPopup);
    multiMenu->addAction(tr("Capture Selected Components"),this,[this] {
        if (!commitPendingTransform())
            return;
        const auto *asset = store_.selectedMesh();
        if (!asset || domain_ == ModelDomain::Object || selected_.empty()) {
            message_->setText(tr("Select vertices, edges, or faces on the active mesh first."));
            return;
        }
        capturedComponents_.insert(store_.selection(),modelVertices(*asset->mesh,domain_,selected_));
        message_->setText(tr("Captured components on %1 object(s). Select another object or apply a transform.")
            .arg(capturedComponents_.size()));
    });
    multiMenu->addAction(tr("Transform Captured Components…"),this,&ModelingWorkspace::transformCapturedComponents);
    multiMenu->addAction(tr("Clear Captured Components"),this,[this] {
        capturedComponents_.clear();
        message_->setText(tr("Captured components cleared."));
    });
    connect(tool(tr("Add Cube"), "plus"), &QToolButton::clicked, this, [this] { if (commitPendingTransform()) store_.addCube(); });
    connect(tool(tr("Half Cube")), &QToolButton::clicked, this, [this] { if (commitPendingTransform()) store_.addCube(true); });
    connect(tool(tr("Revolve…"),"spin"),&QToolButton::clicked,this,[this] { revolve(); });
    auto *primitives = tool(tr("Primitives"),"plus");
    connect(primitives,&QToolButton::clicked,this,[this] {
        if (!commitPendingTransform())
            return;
        QDialog dialog(this);
        dialog.setWindowTitle(tr("Add Primitive"));
        auto *form = new QFormLayout(&dialog);
        auto *shape = new QComboBox(&dialog);
        shape->setAccessibleName(tr("Primitive shape"));
        shape->addItems({tr("Cube"),tr("Plane"),tr("Sphere"),tr("Cylinder"),tr("Cone"),tr("Torus"),tr("Filled Circle")});
        form->addRow(tr("Shape"),shape);
        std::array<QDoubleSpinBox *,3> dimensions;
        const QString names[]{tr("Width"),tr("Depth"),tr("Height")};
        for (int i = 0; i < 3; ++i) {
            auto *field = dimensions[i] = new QDoubleSpinBox(&dialog);
            field->setAccessibleName(names[i]);
            field->setRange(0.01,10000);
            field->setDecimals(3);
            field->setValue(1);
            field->setSuffix(tr(" m"));
            form->addRow(names[i],field);
        }
        auto *segments = new QSpinBox(&dialog);
        segments->setAccessibleName(tr("Segments"));
        segments->setRange(3,128);
        segments->setValue(24);
        form->addRow(tr("Segments"),segments);
        auto *rings = new QSpinBox(&dialog);
        rings->setAccessibleName(tr("Rings"));
        rings->setRange(3,128);
        rings->setValue(12);
        form->addRow(tr("Rings"),rings);
        const auto updateFields = [=](int index) {
            dimensions[2]->setEnabled(index != int(wf::Primitive::Plane) && index != int(wf::Primitive::Circle));
            segments->setEnabled(index >= int(wf::Primitive::Sphere));
            rings->setEnabled(index == int(wf::Primitive::Sphere) || index == int(wf::Primitive::Torus));
        };
        connect(shape,&QComboBox::currentIndexChanged,&dialog,updateFields);
        updateFields(shape->currentIndex());
        auto *errorLabel = new QLabel(&dialog);
        errorLabel->setWordWrap(true);
        form->addRow(errorLabel);
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,&dialog);
        form->addRow(buttons);
        connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);
        connect(buttons,&QDialogButtonBox::accepted,&dialog,[&,this] {
            const wf::Point3 size{float(dimensions[0]->value()),float(dimensions[1]->value()),float(dimensions[2]->value())};
            QString error;
            if (store_.addMeshPrimitive(wf::Primitive(shape->currentIndex()),size,unsigned(segments->value()),
                                       unsigned(rings->value()),shape->currentText(),error))
                dialog.accept();
            else
                errorLabel->setText(error.isEmpty() ? tr("The project could not add the mesh.") : error);
        });
        dialog.exec();
    });
    connect(tool(tr("Import GLB"), "import"), &QToolButton::clicked, this, [this] {
        if (!commitPendingTransform())
            return;
        const auto path = QFileDialog::getOpenFileName(this, tr("Import GLB"), {}, tr("Binary glTF (*.glb)"));
        if (!path.isEmpty()) {
            QString error;
            message_->setText(store_.importMesh(path, error) ? tr("Mesh imported.") : error);
        }
    });
    connect(tool(tr("Export GLB"),"save"),&QToolButton::clicked,this,[this] {
        if (!commitPendingTransform())
            return;
        const auto *asset = store_.selectedMesh();
        if (!asset) {
            message_->setText(tr("Select a mesh to export."));
            return;
        }
        const auto mesh = asset->evaluatedMesh ? asset->evaluatedMesh : asset->mesh;
        const auto name = asset->name;
        auto path = QFileDialog::getSaveFileName(this,tr("Export Evaluated Mesh"),name+".glb",tr("Binary glTF (*.glb)"));
        if (path.isEmpty())
            return;
        if (!path.endsWith(".glb",Qt::CaseInsensitive))
            path += ".glb";
        QString error;
        const auto data = exportGlb(*mesh,name,error);
        if (data.isEmpty()) {
            message_->setText(error);
            return;
        }
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit()) {
            message_->setText(tr("Could not save GLB. %1").arg(file.errorString()));
            return;
        }
        message_->setText(tr("Exported %1.").arg(path));
    });
    connect(tool(tr("Add Instance"), "box"), &QToolButton::clicked, this,[this] {
        if (commitPendingTransform())
            store_.addMeshInstance();
    });
    connect(tool(tr("Frame"), "focus"), &QToolButton::clicked, viewport_, &LiveViewport::frame);
    auto *uv = tool(tr("UV Editing"), "grid");
    uv->setCheckable(true);
    connect(uv, &QToolButton::toggled, uv_, &QWidget::setVisible);
    auto *uvTools = tool(tr("UV Tools"));
    uvTools->setPopupMode(QToolButton::InstantPopup);
    auto *uvMenu = new QMenu(uvTools);
    uvTools->setMenu(uvMenu);
    const std::pair<QString,UvCommand> uvCommands[]{
        {tr("Mark Seams"),UvCommand::Mark},{tr("Clear Seams"),UvCommand::Clear},
        {tr("Seams from Sharp Edges"),UvCommand::Sharp},{tr("Unwrap Islands…"),UvCommand::Unwrap},
        {tr("Pack Islands…"),UvCommand::Pack},{tr("Normalize Texel Density…"),UvCommand::Density},
        {tr("Stitch UV Edges"),UvCommand::Stitch},
        {tr("Planar Projection"),UvCommand::Planar},{tr("Box Projection"),UvCommand::Box},
        {tr("Cylinder Projection"),UvCommand::Cylinder},{tr("Inspect UVs"),UvCommand::Inspect}
    };
    for (const auto &[name,command] : uvCommands)
        uvMenu->addAction(name,this,[this,command] { editUvs(command); });
    auto *pinMode = uvMenu->addAction(tr("Pin UV Corners"));
    pinMode->setCheckable(true);
    connect(pinMode,&QAction::toggled,this,[this](bool enabled) {
        uv_->setPinMode(enabled);
        message_->setText(enabled ? tr("Click a UV corner to pin or unpin it.") : QString{});
    });
    uvMenu->addSeparator();
    auto *uvSets = uvMenu->addMenu(tr("UV Sets"));
    connect(uvSets,&QMenu::aboutToShow,this,[this,uvSets] {
        uvSets->clear();
        const auto *asset = store_.selectedMesh();
        if (!asset || transform_ || knife_ || retopology_) {
            uvSets->addAction(tr("Select an idle mesh"))->setEnabled(false);
            return;
        }
        uvSets->addAction(tr("%1 (active)").arg(asset->activeUvSet))->setEnabled(false);
        for (auto layer = asset->otherUvSets.cbegin(); layer != asset->otherUvSets.cend(); ++layer) {
            const auto name = layer.key();
            const bool rebuild = layer->needsUnwrap;
            uvSets->addAction(rebuild ? tr("Rebuild %1…").arg(name) : name,this,[this,name,rebuild] {
                if (rebuild && QMessageBox::question(this,tr("Rebuild UV Set"),
                    tr("Replace the older layout with a copy of the current UV set? You can unwrap and edit that copy."),
                    QMessageBox::Yes | QMessageBox::Cancel,QMessageBox::Cancel) != QMessageBox::Yes)
                    return;
                QString error;
                message_->setText(store_.selectMeshUvSet(name,false,rebuild,error) ? tr("Active UV set: %1").arg(name) : error);
            });
        }
        uvSets->addSeparator();
        uvSets->addAction(tr("Duplicate Current Set…"),this,[this] {
            bool accepted = false;
            const auto name = QInputDialog::getText(this,tr("Add UV Set"),tr("Name"),QLineEdit::Normal,{},&accepted);
            if (!accepted)
                return;
            QString error;
            message_->setText(store_.selectMeshUvSet(name,true,false,error) ? tr("Active UV set: %1").arg(name.trimmed()) : error);
        });
    });
    uvMenu->addAction(tr("Transform Island…"),this,&ModelingWorkspace::transformUvIsland);
    auto *islandMode = uvMenu->addAction(tr("Drag Whole Islands"));
    islandMode->setCheckable(true);
    connect(islandMode,&QAction::toggled,this,[this](bool enabled) {
        uvIslands_ = enabled;
        uv_->setIslandMode(enabled);
    });
    auto *checker = uvMenu->addAction(tr("Checker Grid"));
    checker->setCheckable(true);
    connect(checker,&QAction::toggled,uv_,&UvCanvas::setChecker);
    uvTools->hide();
    connect(uv,&QToolButton::toggled,uvTools,&QWidget::setVisible);
    header->addStretch(1);
    reference_ = new QComboBox(this);
    reference_->setAccessibleName(tr("Reference mesh"));
    reference_->setToolTip(tr("Show another mesh as a reference for surface snapping."));
    reference_->setMaximumWidth(185);
    header->addWidget(reference_);
    connect(reference_,&QComboBox::currentIndexChanged,this,[this] {
        finishRetopology(false);
        viewport_->setModelReference(reference_->currentData().toString());
    });

    auto *modes = new QHBoxLayout;
    modes->setContentsMargins(5,0,5,0);
    modes->setSpacing(5);
    auto *modeGroup = new QButtonGroup(this);
    domains_[0] = button(modes,tr("Object"));
    editMode_ = button(modes,tr("Edit"));
    for (auto *mode : {domains_[0],editMode_}) {
        mode->setCheckable(true);
        modeGroup->addButton(mode);
    }
    domains_[0]->setChecked(true);
    connect(domains_[0],&QToolButton::clicked,this,[this] { setDomain(ModelDomain::Object); });
    connect(editMode_,&QToolButton::clicked,this,&ModelingWorkspace::enterEditMode);
    const QString domains[]{tr("Vertex"),tr("Edge"),tr("Face")};
    auto *selectionGroup = new QButtonGroup(this);
    for (int i = 1; i <= 3; ++i) {
        auto *mode = domains_[i] = button(modes,domains[i-1]);
        mode->setCheckable(true);
        selectionGroup->addButton(mode,i);
    }
    selectionGroup->setExclusive(false);
    connect(selectionGroup,&QButtonGroup::idClicked,this,[this](int id) { setDomain(ModelDomain(id)); });
    xray_ = button(modes,tr("X-Ray"));
    xray_->setCheckable(true);
    xray_->setToolTip(tr("Select through surfaces with box selection. Hidden vertices and edges can also be picked."));
    connect(xray_,&QToolButton::toggled,this,&ModelingWorkspace::refreshSelection);
    symmetry_ = button(modes,tr("Symmetry X"));
    symmetry_->setCheckable(true);
    symmetry_->setToolTip(tr("Mirror component transforms across the mesh's local X plane."));
    connect(symmetry_,&QToolButton::toggled,this,&ModelingWorkspace::previewTransform);
    auto *select = button(modes,tr("Select"),"list");
    select->setPopupMode(QToolButton::InstantPopup);
    auto *selectMenu = new QMenu(select);
    select->setMenu(selectMenu);
    const std::pair<QString,SelectionCommand> commands[]{
        {tr("Connected"),SelectionCommand::Connected},{tr("Grow"),SelectionCommand::Grow},
        {tr("Shrink"),SelectionCommand::Shrink},{tr("Border"),SelectionCommand::Border},
        {tr("Edge Loop"),SelectionCommand::Loop},{tr("Edge Ring"),SelectionCommand::Ring}
    };
    for (const auto &[name,command] : commands)
        selectMenu->addAction(name,this,[this,command] { expandSelection(command); });
    auto *convert = selectMenu->addMenu(tr("Convert To"));
    for (int i = 1; i < 4; ++i)
        convert->addAction(domains[i-1],this,[this,i] { convertSelection(ModelDomain(i)); });
    connect(selectMenu,&QMenu::aboutToShow,this,[this,selectMenu,convert] {
        const bool editing = domain_ != ModelDomain::Object && store_.selectedMesh() && !knife_;
        const bool hasSelection = editing && !selected_.empty();
        for (int i = 0; i < 6; ++i)
            selectMenu->actions()[i]->setEnabled(i == 3 ? editing : hasSelection && (i < 4 || domain_ == ModelDomain::Edge));
        convert->setEnabled(hasSelection);
    });
    meshCommands_ = button(modes,tr("Mesh"),"box");
    meshCommands_->setPopupMode(QToolButton::InstantPopup);
    auto *meshMenu = new QMenu(meshCommands_);
    meshCommands_->setMenu(meshMenu);
    const std::pair<QString,TopologyCommand> meshCommands[]{
        {tr("Weld by Distance…"),TopologyCommand::WeldDistance},{tr("Weld to Active"),TopologyCommand::WeldActive},
        {tr("Fill Boundary"),TopologyCommand::Fill},{tr("Grid Fill…"),TopologyCommand::GridFill},
        {tr("Bridge Boundaries"),TopologyCommand::Bridge},{tr("Dissolve Edges"),TopologyCommand::Dissolve},
        {tr("Limited Dissolve…"),TopologyCommand::LimitedDissolve},{tr("Bevel Edges…"),TopologyCommand::Bevel},
        {tr("Chamfer Vertices…"),TopologyCommand::Chamfer},{tr("Slide Edge…"),TopologyCommand::SlideEdge},
        {tr("Slide Vertex…"),TopologyCommand::SlideVertex},{tr("Rotate Edge"),TopologyCommand::RotateEdge},
        {tr("Delete Components"),TopologyCommand::Delete}
    };
    for (const auto &[name,command] : meshCommands) {
        auto *action = meshMenu->addAction(name,this,[this,name,command] { editTopology(command,name); });
        connect(meshMenu,&QMenu::aboutToShow,action,[this,action,command] {
            const auto vertex = command == TopologyCommand::WeldDistance || command == TopologyCommand::WeldActive
                || command == TopologyCommand::Chamfer || command == TopologyCommand::SlideVertex;
            action->setEnabled(!selected_.empty() && !knife_ && !retopology_ && (command == TopologyCommand::Delete
                || domain_ == (vertex ? ModelDomain::Vertex : ModelDomain::Edge)));
        });
    }
    meshMenu->addAction(tr("Bisect Mesh…"),this,&ModelingWorkspace::bisect);
    meshMenu->addAction(tr("Spin Boundary…"),this,&ModelingWorkspace::spin);
    meshMenu->addAction(tr("Target Weld Tool"),this,[this] { setPairTool(PairTool::TargetWeld); });
    meshMenu->addAction(tr("Connect Tool"),this,[this] { setPairTool(PairTool::Connect); });
    meshMenu->addAction(tr("Interactive Loop Cut"),this,[this] { setPairTool(PairTool::LoopCut); });
    auto *deform = meshMenu->addMenu(tr("Deform"));
    deform->addAction(tr("Shear X by Y…"),this,[this] { deformSelection(Deform::Shear); });
    deform->addAction(tr("To Sphere…"),this,[this] { deformSelection(Deform::ToSphere); });
    deform->addAction(tr("Shrink / Fatten…"),this,[this] { deformSelection(Deform::AlongNormals); });
    meshMenu->addAction(tr("Voxel Remesh…"),this,&ModelingWorkspace::remesh);
    auto *retopology = meshMenu->addMenu(tr("Retopology"));
    retopology->addAction(tr("Draw Quad"),this,[this] { startRetopology(false); });
    retopology->addAction(tr("Extend Selected Edge"),this,[this] { startRetopology(true); });
    retopology->addAction(tr("Draw Quad Strip"),this,[this] { startRetopology(false,true); });
    retopology->addAction(tr("Extend Quad Strip"),this,[this] { startRetopology(true,true); });
    retopology->addAction(tr("Conform to Reference"),this,[this] { conformOrRelax(false); });
    retopology->addAction(tr("Relax Vertices…"),this,[this] { conformOrRelax(true); });
    knifeTool_ = button(modes,tr("Knife"));
    knifeTool_->setCheckable(true);
    knifeTool_->setToolTip(tr("Click successive edges across faces. Enter applies the path and Escape cancels."));
    connect(knifeTool_,&QToolButton::clicked,this,&ModelingWorkspace::startKnife);
    modes->addSpacing(9);
    const QString names[]{tr("Move"),tr("Rotate"),tr("Scale")};
    const QString icons[]{"move","rotate","scale"};
    for (int i = 0; i < 3; ++i) {
        tools_[i] = button(modes,names[i],icons[i]);
        tools_[i]->setCheckable(true);
        connect(tools_[i],&QToolButton::clicked,this,[this,i] { beginTransform(Transform(i)); });
    }
    modes->addStretch(1);
    ortho_ = button(modes,tr("Ortho"));
    ortho_->setCheckable(true);
    ortho_->setToolTip(tr("Toggle orthographic projection. Numpad 1, 3 and 7 align the view."));
    connect(ortho_,&QToolButton::toggled,viewport_,&LiveViewport::setModelOrthographic);
    layout->addLayout(modes);
    auto *display = new QHBoxLayout;
    display->setContentsMargins(5,0,5,0);
    display->addWidget(new QLabel(tr("Viewport"),this));
    shading_ = new QButtonGroup(this);
    const QString shadingModes[]{tr("Wireframe"),tr("Solid"),tr("Material Preview"),tr("Shaded + Wire")};
    const QString shortcuts[]{tr(" (4)"),tr(" (5)"),tr(" (6)"),QString()};
    for (int i = 0; i < 4; ++i) {
        auto *mode = button(display,shadingModes[i]);
        mode->setCheckable(true);
        mode->setToolTip(shadingModes[i]+shortcuts[i]);
        shading_->addButton(mode,i);
    }
    shading_->button(int(ModelShading::ShadedWire))->setChecked(true);
    connect(shading_,&QButtonGroup::idToggled,this,[this](int mode,bool active) {
        if (active)
            viewport_->setModelShading(ModelShading(mode));
    });
    display->addStretch(1);
    layout->addLayout(display);

    transformBar_ = new QWidget(this);
    auto *options = new QHBoxLayout(transformBar_);
    options->setContentsMargins(5,0,5,1);
    options->setSpacing(5);
    transformLabel_ = new QLabel(transformBar_);
    options->addWidget(transformLabel_);
    const QString axes[]{"X","Y","Z"};
    for (int i = 0; i < 3; ++i) {
        auto *value = values_[i] = new QDoubleSpinBox(transformBar_);
        value->setDecimals(4);
        value->setKeyboardTracking(false);
        value->setAccessibleName(tr("Transform %1").arg(axes[i]));
        value->setPrefix(axes[i]+"  ");
        options->addWidget(value);
        connect(value,&QDoubleSpinBox::valueChanged,this,[this,i](double amount) {
            if (transform_) {
                transform_->dragValues[i] = float(amount);
                previewTransform();
            }
        });
    }
    axis_ = new QComboBox(transformBar_);
    axis_->setAccessibleName(tr("Transform constraint"));
    axis_->addItems({tr("Free"),"X","Y","Z"});
    options->addWidget(axis_);
    connect(axis_,&QComboBox::currentIndexChanged,this,&ModelingWorkspace::previewTransform);
    orientation_ = new QComboBox(transformBar_);
    orientation_->setAccessibleName(tr("Transform orientation"));
    orientation_->addItems({tr("Local"),tr("World")});
    options->addWidget(orientation_);
    pivotMode_ = new QComboBox(transformBar_);
    pivotMode_->setAccessibleName(tr("Transform pivot"));
    pivotMode_->addItems({tr("Median Pivot"),tr("Active Pivot")});
    options->addWidget(pivotMode_);
    for (auto *choice : {orientation_,pivotMode_})
        connect(choice,&QComboBox::currentIndexChanged,this,&ModelingWorkspace::previewTransform);
    snap_ = new QComboBox(transformBar_);
    snap_->setAccessibleName(tr("Transform snapping"));
    snap_->addItems({tr("Snap Off"),tr("Grid Snap"),tr("Vertex Snap"),tr("Surface Snap")});
    options->addWidget(snap_);
    snapStep_ = new QDoubleSpinBox(transformBar_);
    snapStep_->setRange(0.0001,100);
    snapStep_->setDecimals(4);
    snapStep_->setValue(0.1);
    snapStep_->setAccessibleName(tr("Grid snap increment"));
    options->addWidget(snapStep_);
    snapStep_->hide();
    connect(snap_,&QComboBox::currentIndexChanged,this,[this](int index) {
        snapStep_->setVisible(index == 1 && snap_->isVisible());
        previewTransform();
    });
    soft_ = button(options,tr("Soft"));
    soft_->setCheckable(true);
    soft_->setAccessibleName(tr("Soft Selection"));
    soft_->setToolTip(tr("Transform connected vertices with distance falloff."));
    softRadius_ = new QDoubleSpinBox(transformBar_);
    softRadius_->setRange(0.001,10000);
    softRadius_->setDecimals(3);
    softRadius_->setSuffix(tr(" m"));
    softRadius_->setValue(1);
    softRadius_->setAccessibleName(tr("Soft selection radius"));
    softRadius_->hide();
    options->addWidget(softRadius_);
    connect(soft_,&QToolButton::toggled,this,[this](bool enabled) {
        softRadius_->setVisible(enabled);
        previewTransform();
    });
    connect(softRadius_,&QDoubleSpinBox::valueChanged,this,[this] {
        if (transform_)
            transform_->falloff.clear();
        previewTransform();
    });
    options->addStretch(1);
    apply_ = button(options,tr("Apply"),"check");
    connect(apply_,&QToolButton::clicked,this,[this] { finishTransform(true); });
    auto *cancel = button(options,tr("Cancel"),"close");
    connect(cancel,&QToolButton::clicked,this,[this] { finishTransform(false); });
    for (const auto &[key,apply] : {std::pair{Qt::Key_Escape,false},std::pair{Qt::Key_Return,true}}) {
        auto *action = new QAction(transformBar_);
        action->setShortcut(QKeySequence(key));
        action->setShortcutContext(Qt::WidgetWithChildrenShortcut);
        transformBar_->addAction(action);
        connect(action,&QAction::triggered,this,[this,apply] { finishTransform(apply); });
    }
    transformBar_->hide();
    layout->addWidget(transformBar_);

    auto *columns = columns_ = new QSplitter(Qt::Horizontal, this);
    columns->setChildrenCollapsible(false);
    auto *canvas = new QSplitter(Qt::Horizontal, columns);
    canvas->setChildrenCollapsible(false);
    canvas->addWidget(uv_);
    auto *viewportPane = new QWidget(canvas);
    auto *viewportLayout = new QHBoxLayout(viewportPane);
    viewportLayout->setContentsMargins(0,0,0,0);
    viewportLayout->setSpacing(2);
    auto *shelfScroll = new QScrollArea(viewportPane);
    shelfScroll->setWidgetResizable(true);
    shelfScroll->setFrameShape(QFrame::NoFrame);
    shelfScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    shelfScroll->setFixedWidth(49);
    auto *shelf = new QWidget(shelfScroll);
    auto *shelfLayout = new QVBoxLayout(shelf);
    shelfLayout->setContentsMargins(2,3,2,3);
    shelfLayout->setSpacing(3);
    shelfScroll->setWidget(shelf);
    viewportLayout->addWidget(shelfScroll);
    viewportLayout->addWidget(viewport_,1);
    canvas->addWidget(viewportPane);
    canvas->setStretchFactor(0, 1);
    canvas->setStretchFactor(1, 1);
    uv_->hide();
    columns->addWidget(canvas);
    auto *inspector = new QSplitter(Qt::Vertical,columns);
    inspector->setChildrenCollapsible(false);
    auto *outlinerPane = new QWidget(inspector);
    auto *outlinerLayout = new QVBoxLayout(outlinerPane);
    outlinerLayout->setContentsMargins(5,4,6,4);
    outlinerLayout->addWidget(new QLabel(tr("Model Outliner"),outlinerPane));
    auto *detailsPane = new QWidget(inspector);
    auto *detailsLayout = new QVBoxLayout(detailsPane);
    detailsLayout->setContentsMargins(0,4,0,0);
    detailsLayout->setSpacing(3);
    auto *detailsHeader = new QHBoxLayout;
    detailsHeader->setContentsMargins(5,0,6,0);
    detailsHeader->addWidget(new QLabel(tr("Details"),detailsPane));
    detailsHeader->addStretch(1);
    detailsLayout->addLayout(detailsHeader);
    auto *detailsScroll = new QScrollArea(detailsPane);
    detailsScroll->setWidgetResizable(true);
    detailsScroll->setFrameShape(QFrame::NoFrame);
    detailsLayout->addWidget(detailsScroll,1);
    auto *right = new QWidget(detailsScroll);
    detailsScroll->setWidget(right);
    inspector->setSizes({200,520});
    inspector->setStretchFactor(1,1);
    auto *rightLayout = new QVBoxLayout(right);
    rightLayout->setContentsMargins(5, 4, 6, 4);
    auto *filter = outlinerFilter_ = new QLineEdit(outlinerPane);
    filter->setPlaceholderText(tr("Search mesh objects…"));
    filter->setAccessibleName(tr("Search mesh objects"));
    outlinerLayout->addWidget(filter);
    connect(filter,&QLineEdit::textChanged,this,&ModelingWorkspace::filterOutliner);
    outliner_->setAccessibleName(tr("Model Outliner"));
    outliner_->setObjectName("meshOutliner");
    outliner_->setDragDropMode(QAbstractItemView::InternalMove);
    static_cast<ModelOutliner *>(outliner_)->reparent = [this](const QString &child,const QString &parent) {
        setParentFromOutliner(child,parent);
    };
    outliner_->setHeaderLabels({tr("Name"),tr("Visible")});
    outliner_->header()->setSectionResizeMode(0,QHeaderView::Stretch);
    outliner_->header()->setSectionResizeMode(1,QHeaderView::ResizeToContents);
    outliner_->header()->setStretchLastSection(false);
    outlinerLayout->addWidget(outliner_, 1);
    outliner_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(outliner_,&QTreeWidget::customContextMenuRequested,this,[this](QPoint position) {
        auto *item = outliner_->itemAt(position);
        if (!item || !item->parent())
            return;
        outliner_->setCurrentItem(item);
        if (!store_.selectedMesh())
            return;
        QMenu menu;
        menu.addAction(tr("Set Parent…"),this,[this] {
            if (!commitPendingTransform() || !store_.selectedObject())
                return;
            const auto child = store_.selection();
            QDialog dialog(this);
            dialog.setWindowTitle(tr("Set Parent"));
            auto *form = new QFormLayout(&dialog);
            auto *parents = new QComboBox(&dialog);
            for (const auto &object : store_.project()->objects)
                if (object.id != child && !object.meshAsset.isEmpty())
                    parents->addItem(object.name,object.id);
            if (!parents->count())
                return;
            form->addRow(tr("Parent"),parents);
            auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel,&dialog);
            form->addRow(buttons);
            connect(buttons,&QDialogButtonBox::accepted,&dialog,&QDialog::accept);
            connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);
            if (dialog.exec() == QDialog::Accepted)
                setParentFromOutliner(child,parents->currentData().toString());
        });
        auto *unparent = menu.addAction(tr("Unparent"),this,[this] { setParentFromOutliner(store_.selection(),{}); });
        unparent->setEnabled(!store_.selectedObject()->parentId.isEmpty());
        menu.addAction(tr("Move to Scene Group…"),this,[this] {
            const auto *object = store_.selectedObject();
            if (!object || !commitPendingTransform())
                return;
            auto next = *store_.selectedObject();
            bool accepted = false;
            const auto group = QInputDialog::getText(this,tr("Move to Scene Group"),tr("Group name"),
                QLineEdit::Normal,next.group,&accepted).trimmed();
            if (accepted && next.group != group) {
                next.group = group;
                store_.updateObject(next,tr("Move Mesh to Scene Group"));
            }
        });
        menu.addAction(tr("Rename"),this,[this] {
            const auto *object = store_.selectedObject();
            if (!object)
                return;
            const auto copy = *object;
            bool accepted = false;
            const auto name = QInputDialog::getText(this,tr("Rename Mesh Object"),tr("Name"),
                QLineEdit::Normal,copy.name,&accepted).trimmed();
            if (accepted && !name.isEmpty()) {
                if (const auto *current = store_.object(copy.id)) {
                    auto next = *current;
                    next.name = name;
                    store_.updateObject(next,tr("Rename Mesh Object"));
                }
            }
        });
        const auto command = [this,&menu](const QString &name,auto operation) {
            menu.addAction(name,this,[this,operation,name] {
                QString error;
                message_->setText(operation(error) ? tr("%1 applied.").arg(name) : error);
            });
        };
        menu.addAction(tr("Duplicate Linked"),&store_,&ProjectStore::addMeshInstance);
        command(tr("Duplicate Independent"),[this](QString &error) { return store_.duplicateMesh(error); });
        command(tr("Make Unique"),[this](QString &error) { return store_.makeMeshUnique(error); });
        menu.addSeparator();
        command(tr("Center Origin"),[this](QString &error) { return store_.bakeMeshTransform(true,error); });
        command(tr("Apply Rotation and Scale"),[this](QString &error) { return store_.bakeMeshTransform(false,error); });
        menu.addAction(tr("Join Meshes…"),this,[this] {
            const auto *project = store_.project();
            if (!project)
                return;
            QDialog dialog(this);
            dialog.setWindowTitle(tr("Join Meshes"));
            auto *layout = new QVBoxLayout(&dialog);
            layout->addWidget(new QLabel(tr("Choose meshes to join into the active object. Modifiers are applied to the joined result."),&dialog));
            auto *objects = new QListWidget(&dialog);
            objects->setAccessibleName(tr("Meshes to join"));
            for (const auto &object : project->objects) {
                if (object.id == store_.selection() || object.meshAsset.isEmpty())
                    continue;
                auto *item = new QListWidgetItem(object.name,objects);
                item->setData(Qt::UserRole,object.id);
                item->setCheckState(Qt::Unchecked);
            }
            layout->addWidget(objects);
            auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,&dialog);
            layout->addWidget(buttons);
            connect(buttons,&QDialogButtonBox::accepted,&dialog,&QDialog::accept);
            connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);
            if (dialog.exec() != QDialog::Accepted)
                return;
            QStringList ids;
            for (int row = 0; row < objects->count(); ++row)
                if (const auto *item = objects->item(row); item->checkState() == Qt::Checked)
                    ids.append(item->data(Qt::UserRole).toString());
            QString error;
            message_->setText(store_.joinMeshes(ids,error) ? tr("Meshes joined.") : error);
        });
        if (domain_ == ModelDomain::Face && !selected_.empty())
            command(tr("Separate Selected Faces"),[this](QString &error) {
                std::vector<std::size_t> faces;
                for (const auto component : selected_)
                    faces.push_back(std::size_t(component.index));
                return store_.separateMeshFaces(faces,error);
            });
        const auto hidden = !store_.selectedObject()->visible;
        menu.addAction(hidden ? tr("Show in World") : tr("Hide in World"),this,[this] {
            if (const auto *object = store_.selectedObject()) {
                auto edited = *object;
                edited.visible = !edited.visible;
                store_.updateObject(edited,tr("Toggle Mesh Visibility"));
            }
        });
        menu.exec(outliner_->viewport()->mapToGlobal(position));
    });
    const auto detailsLink = [this,detailsScroll,detailsHeader,&button](const QString &title,const QString &icon,QWidget *section) {
        auto *link = button(detailsHeader,title,icon);
        link->setToolButtonStyle(Qt::ToolButtonIconOnly);
        link->setAccessibleName(title);
        link->setToolTip(title);
        connect(link,&QToolButton::clicked,this,[detailsScroll,section] {
            detailsScroll->ensureWidgetVisible(section,0,8);
        });
    };
    summary_->setWordWrap(true);
    summary_->setAccessibleName(tr("Model selection"));
    rightLayout->addWidget(summary_);
    auto *markup = new QGroupBox(tr("Viewport Markup"),right);
    auto *markupLayout = new QVBoxLayout(markup);
    measurementMode_ = new QComboBox(markup);
    measurementMode_->setAccessibleName(tr("Measurement mode"));
    measurementMode_->addItems({tr("Distance"),tr("Angle")});
    markupLayout->addWidget(measurementMode_);
    measurement_ = new QLabel(tr("Measure: pick two surface points."),markup);
    measurement_->setWordWrap(true);
    measurement_->setAccessibleName(tr("Surface measurement"));
    markupLayout->addWidget(measurement_);
    auto *clearMarkup = new QPushButton(tr("Clear Annotations"),markup);
    eraseAnnotation_ = new QToolButton(markup);
    eraseAnnotation_->setText(tr("Erase Annotation"));
    eraseAnnotation_->setIcon(editorIcon("annotate"));
    eraseAnnotation_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    eraseAnnotation_->setCheckable(true);
    auto *clearMeasurement = new QPushButton(tr("Clear Measurement"),markup);
    markupLayout->addWidget(clearMeasurement);
    markupLayout->addWidget(eraseAnnotation_);
    markupLayout->addWidget(clearMarkup);
    connect(eraseAnnotation_,&QToolButton::clicked,this,[this](bool active) {
        setSurfaceTool(active ? SurfaceTool::EraseAnnotation : SurfaceTool::None);
    });
    connect(clearMeasurement,&QPushButton::clicked,this,[this] {
        measurementPoints_.clear();
        measurement_->setText(tr("Choose Measure to start a new reading."));
        viewport_->setModelMarkup({},annotationPoints_);
    });
    connect(measurementMode_,&QComboBox::currentIndexChanged,this,[this,clearMeasurement](int mode) {
        clearMeasurement->click();
        if (measureTool_->isChecked())
            setSurfaceTool(mode == 0 ? SurfaceTool::Measure : SurfaceTool::MeasureAngle);
    });
    connect(clearMarkup,&QPushButton::clicked,this,[this] {
        if (!commitPendingTransform())
            return;
        if (const auto *object = store_.selectedObject(); store_.selectedMesh() && object && object->details.contains("modelAnnotations")) {
            auto updated = *object;
            updated.details.remove("modelAnnotations");
            store_.updateObject(updated,tr("Clear Model Annotations"));
        }
    });
    auto *transformGroup = new QGroupBox(tr("Object Transform"),right);
    auto *objectForm = new QGridLayout(transformGroup);
    objectForm->setContentsMargins(6,5,6,6);
    objectForm->setHorizontalSpacing(3);
    objectForm->setVerticalSpacing(4);
    const QString fields[]{tr("Location"),tr("Rotation"),tr("Scale")};
    objectParent_ = new QLabel(transformGroup);
    objectWorld_ = new QLabel(transformGroup);
    objectWorld_->setWordWrap(true);
    objectForm->addWidget(objectParent_,3,0,1,4);
    objectForm->addWidget(objectWorld_,4,0,1,4);
    for (int row = 0; row < 3; ++row) {
        objectForm->addWidget(new QLabel(fields[row],transformGroup),row,0);
        for (int axis = 0; axis < 3; ++axis) {
            const int index = row*3+axis;
            auto *value = objectTransform_[index] = new QDoubleSpinBox(transformGroup);
            value->setRange(-100000,100000);
            value->setDecimals(3);
            value->setKeyboardTracking(false);
            value->setButtonSymbols(QAbstractSpinBox::NoButtons);
            value->setPrefix(axes[axis]+" ");
            value->setAccessibleName(fields[row]+" "+axes[axis]);
            value->setMinimumWidth(49);
            value->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Fixed);
            objectForm->addWidget(value,row,axis+1);
            objectForm->setColumnStretch(axis+1,1);
            connect(value,&QDoubleSpinBox::editingFinished,this,[this,value,row,axis] {
                const auto next = float(value->value());
                if (!commitPendingTransform())
                    return;
                const auto *object = store_.selectedObject();
                if (!object)
                    return;
                auto updated = *object;
                auto &component = row == 0 ? updated.position : row == 1 ? updated.rotation : updated.scale;
                if (row == 2 && std::abs(next) < 0.001f) {
                    refreshSelection();
                    message_->setText(tr("Object scale must stay nonzero."));
                } else if (std::abs(next-component[axis]) > 0.00051f) {
                    component[axis] = next;
                    store_.updateObject(updated,tr("Edit Object Transform"));
                }
            });
        }
    }
    rightLayout->addWidget(transformGroup);
    detailsLink(tr("Object Transform"),"move",transformGroup);
    auto *properties = new QGroupBox(tr("Materials and Textures"),right);
    auto *propertyLayout = new QVBoxLayout(properties);
    auto *visible = new QCheckBox(tr("Visible in World Editor"),properties);
    auto *materialInfo = new QLabel(properties);
    materialInfo->setWordWrap(true);
    materialInfo->setAccessibleName(tr("Mesh material slots"));
    propertyLayout->addWidget(visible);
    materialSlot_ = new QComboBox(properties);
    materialSlot_->setAccessibleName(tr("Active material slot"));
    propertyLayout->addWidget(materialSlot_);
    propertyLayout->addWidget(materialInfo);
    auto *materialActions = new QHBoxLayout;
    auto *newSlot = new QPushButton(tr("New Slot…"),properties);
    assignMaterial_ = new QPushButton(tr("Assign Faces"),properties);
    selectMaterialFaces_ = new QPushButton(tr("Select Faces"),properties);
    materialActions->addWidget(newSlot);
    materialActions->addWidget(assignMaterial_);
    materialActions->addWidget(selectMaterialFaces_);
    propertyLayout->addLayout(materialActions);
    connect(newSlot,&QPushButton::clicked,this,[this] { editMaterialSlots(true); });
    connect(assignMaterial_,&QPushButton::clicked,this,[this] { editMaterialSlots(false); });
    connect(selectMaterialFaces_,&QPushButton::clicked,this,[this] {
        if (!commitPendingTransform())
            return;
        const auto *asset = store_.selectedMesh();
        if (!asset || materialSlot_->currentIndex() < 0)
            return;
        setDomain(ModelDomain::Face);
        QList<ModelComponent> faces;
        for (std::size_t i = 0; i < asset->mesh->faces.size(); ++i)
            if (asset->mesh->faces[i].material == std::uint32_t(materialSlot_->currentIndex()))
                faces.append({int(i)});
        selectComponents(faces,Qt::NoModifier);
    });
    auto *assignTexture = new QPushButton(tr("Material Preview Settings…"),properties);
    propertyLayout->addWidget(assignTexture);
    connect(assignTexture,&QPushButton::clicked,this,&ModelingWorkspace::assignBaseColorTexture);
    rightLayout->addWidget(properties);
    rightLayout->addWidget(markup);
    detailsLink(tr("Materials and Textures"),"layers",properties);
    detailsLink(tr("Viewport Markup"),"measure",markup);
    const auto updateMaterialInfo = [this,materialInfo] {
        const auto *asset = store_.selectedMesh();
        const auto slot = materialSlot_->currentIndex();
        if (!asset || slot < 0 || std::size_t(slot) >= asset->mesh->materials.size()) {
            materialInfo->clear();
            return;
        }
        const auto &material = asset->mesh->materials[slot];
        const auto maps = material.previewTextures();
        const QString names[]{tr("Base Color"),tr("Normal"),tr("Metallic / Roughness"),tr("Ambient Occlusion"),tr("Emissive")};
        QStringList assigned;
        for (std::size_t i = 0; i < maps.size(); ++i)
            if (!maps[i]->empty())
                assigned.append(names[i]);
        const auto count = std::ranges::count(asset->mesh->faces,std::uint32_t(slot),&wf::MeshFace::material);
        materialInfo->setText(tr("%1 face(s) · %2").arg(count).arg(assigned.isEmpty() ? tr("No texture maps") : assigned.join(", ")));
    };
    connect(materialSlot_,&QComboBox::currentIndexChanged,this,updateMaterialInfo);
    const auto updateProperties = [this,visible,assignTexture,clearMarkup,newSlot,updateMaterialInfo] {
        const auto *asset = store_.selectedMesh();
        const auto *object = asset ? store_.selectedObject() : nullptr;
        const QSignalBlocker blocker(visible);
        visible->setEnabled(object);
        visible->setChecked(object && object->visible);
        assignTexture->setEnabled(asset);
        newSlot->setEnabled(asset);
        clearMarkup->setEnabled(object && object->details.contains("modelAnnotations"));
        const QSignalBlocker slotBlocker(materialSlot_);
        const auto previous = materialSlot_->currentIndex();
        materialSlot_->clear();
        if (asset) {
            for (const auto &material : asset->mesh->materials)
                materialSlot_->addItem(QString::fromStdString(material.name));
            if (previous >= 0 && previous < materialSlot_->count())
                materialSlot_->setCurrentIndex(previous);
        }
        materialSlot_->setEnabled(asset);
        updateMaterialInfo();
    };
    connect(&store_,&ProjectStore::selectionChanged,this,updateProperties);
    connect(&store_,&ProjectStore::sceneChanged,this,updateProperties);
    connect(visible,&QCheckBox::toggled,this,[this,updateProperties](bool show) {
        if (!commitPendingTransform()) {
            updateProperties();
            return;
        }
        if (const auto *object = store_.selectedObject(); object && object->visible != show) {
            auto next = *object;
            next.visible = show;
            store_.updateObject(next,tr("Change Mesh Visibility"));
        }
    });
    updateProperties();

    facePanel_ = new QWidget(right);
    auto *faceLayout = new QVBoxLayout(facePanel_);
    faceLayout->setContentsMargins(0,0,0,0);
    faceLayout->setSpacing(4);
    auto *form = new QFormLayout;
    amount_->setRange(-1000, 1000);
    amount_->setDecimals(3);
    amount_->setSingleStep(0.05);
    amount_->setValue(0.25);
    amount_->setSuffix(tr(" m"));
    amount_->setAccessibleName(tr("Distance or bevel width"));
    form->addRow(tr("Distance / width"), amount_);
    edge_->setRange(1, 4);
    edge_->setAccessibleName(tr("Loop start edge"));
    form->addRow(tr("Loop start edge"), edge_);
    auto *loopOptions = new QWidget(this);
    auto *loopLayout = new QHBoxLayout(loopOptions);
    loopLayout->setContentsMargins(5,0,5,0);
    loopLayout->addWidget(new QLabel(tr("Loop Cut"),loopOptions));
    cuts_ = new QSpinBox(this);
    cuts_->setRange(1,64);
    cuts_->setAccessibleName(tr("Loop cut count"));
    form->addRow(tr("Cut count"),cuts_);
    auto *interactiveCuts = new QSpinBox(loopOptions);
    interactiveCuts->setAccessibleName(tr("Interactive cut count"));
    interactiveCuts->setRange(cuts_->minimum(),cuts_->maximum());
    loopLayout->addWidget(new QLabel(tr("Cuts"),loopOptions));
    loopLayout->addWidget(interactiveCuts);
    slide_ = new QDoubleSpinBox(this);
    slide_->setRange(-95,95);
    slide_->setDecimals(1);
    slide_->setSuffix(tr(" %"));
    slide_->setAccessibleName(tr("Loop cut slide"));
    form->addRow(tr("Cut slide"),slide_);
    auto *interactiveSlide = new QDoubleSpinBox(loopOptions);
    interactiveSlide->setAccessibleName(tr("Interactive cut slide"));
    interactiveSlide->setRange(slide_->minimum(),slide_->maximum());
    interactiveSlide->setDecimals(slide_->decimals());
    interactiveSlide->setSuffix(slide_->suffix());
    loopLayout->addWidget(new QLabel(tr("Slide"),loopOptions));
    loopLayout->addWidget(interactiveSlide);
    loopLayout->addStretch();
    loopOptions->hide();
    layout->insertWidget(layout->indexOf(transformBar_)+1,loopOptions);
    connect(interactiveCuts,&QSpinBox::valueChanged,cuts_,&QSpinBox::setValue);
    connect(cuts_,&QSpinBox::valueChanged,interactiveCuts,&QSpinBox::setValue);
    connect(interactiveSlide,&QDoubleSpinBox::valueChanged,slide_,&QDoubleSpinBox::setValue);
    connect(slide_,&QDoubleSpinBox::valueChanged,interactiveSlide,&QDoubleSpinBox::setValue);
    connect(cuts_,&QSpinBox::valueChanged,this,[this] {
        if (pairTool_ == PairTool::LoopCut)
            previewLoopCut();
    });
    connect(slide_,&QDoubleSpinBox::valueChanged,this,[this] {
        if (pairTool_ == PairTool::LoopCut)
            previewLoopCut();
    });
    faceControls_.append({edge_,cuts_,slide_});
    regionControls_.append(amount_);
    faceLayout->addLayout(form);
    rightLayout->addWidget(facePanel_);

    const auto command = [this, faceLayout](const QString &name, const QString &id, auto operation) {
        auto *button = new QPushButton(name, facePanel_);
        button->setObjectName(id);
        faceControls_.append(button);
        connect(button, &QPushButton::clicked, this, [this, name, operation] { edit(name, operation); });
        faceLayout->addWidget(button);
    };
    command(tr("Extrude Face"), "meshExtrude", [this](wf::Mesh &mesh, std::size_t face) {
        return wf::extrudeFace(mesh, face, float(amount_->value()));
    });
    command(tr("Bevel Perimeter"), "meshBevel", [this](wf::Mesh &mesh, std::size_t face) {
        return wf::bevelFace(mesh, face, float(amount_->value()));
    });
    command(tr("Loop Cut"), "meshLoopCut", [this](wf::Mesh &mesh, std::size_t face) {
        return wf::cutQuadLoop(mesh,face,std::size_t(edge_->value()-1),unsigned(cuts_->value()),float(slide_->value()/100));
    });
    auto *region = new QToolButton(facePanel_);
    region->setText(tr("Face Operations"));
    region->setPopupMode(QToolButton::InstantPopup);
    auto *regionMenu = new QMenu(region);
    region->setMenu(regionMenu);
    regionControls_.append(region);
    faceLayout->addWidget(region);
    const auto faceOperation = [this,regionMenu](const QString &name,auto operation) {
        regionMenu->addAction(name,this,[this,name,operation] { editFaces(name,operation); });
    };
    faceOperation(tr("Extrude Region"),[this](wf::Mesh &mesh,std::span<const std::size_t> faces) {
        return wf::extrudeFaces(mesh,faces,float(amount_->value()),false);
    });
    faceOperation(tr("Extrude Individual"),[this](wf::Mesh &mesh,std::span<const std::size_t> faces) {
        return wf::extrudeFaces(mesh,faces,float(amount_->value()),true);
    });
    faceOperation(tr("Inset Faces"),[this](wf::Mesh &mesh,std::span<const std::size_t> faces) {
        return wf::insetFaces(mesh,faces,float(amount_->value()));
    });
    faceOperation(tr("Triangulate Faces"),wf::triangulateFaces);
    faceOperation(tr("Subdivide Faces"),wf::subdivideFaces);
    faceOperation(tr("Split Faces"),wf::splitFaces);
    faceOperation(tr("Shade Smooth"),[](wf::Mesh &mesh,std::span<const std::size_t> faces) {
        return wf::setFaceShading(mesh,faces,true);
    });
    faceOperation(tr("Shade Flat"),[](wf::Mesh &mesh,std::span<const std::size_t> faces) {
        return wf::setFaceShading(mesh,faces,false);
    });
    auto *mirror = new QPushButton(tr("Toggle Mirror X Modifier"), right);
    mirror->setObjectName("meshMirror");
    connect(mirror, &QPushButton::clicked, this, [this] {
        if (!commitPendingTransform())
            return;
        const auto *asset = store_.selectedMesh();
        const bool enable = !asset || std::ranges::none_of(asset->modifiers,[](const auto &modifier) {
            return modifier.enabled && modifier.kind == wf::ModifierKind::Mirror && modifier.axis == 0;
        });
        QString error;
        if (store_.setMirrorX(enable, error))
            message_->setText(enable ? tr("Mirror X enabled. World shows the evaluated mesh.") : tr("Mirror X disabled."));
        else
            message_->setText(error);
    });
    rightLayout->addWidget(mirror);
    auto *modifiers = new QPushButton(tr("Modifiers…"),right);
    connect(modifiers,&QPushButton::clicked,this,&ModelingWorkspace::openModifierStack);
    rightLayout->addWidget(modifiers);
    detailsLink(tr("Modifiers"),"box",mirror);
    auto *assetTools = new QToolButton(right);
    assetTools->setText(tr("Mesh Data"));
    assetTools->setPopupMode(QToolButton::InstantPopup);
    auto *assetMenu = new QMenu(assetTools);
    assetTools->setMenu(assetMenu);
    rightLayout->addWidget(assetTools);
    auto *editProfile = assetMenu->addAction(tr("Edit Revolve Profile…"),this,[this] { revolve(true); });
    connect(assetMenu,&QMenu::aboutToShow,editProfile,[this,editProfile] {
        const auto *asset = store_.selectedMesh();
        editProfile->setEnabled(asset && !asset->revolveProfile.isEmpty());
    });
    detailsLink(tr("Mesh Data"),"grid",assetTools);
    assetMenu->addAction(tr("Inspect Topology"),this,[this] {
        const auto *asset = store_.selectedMesh();
        if (!asset)
            return;
        const auto &mesh = asset->evaluated();
        const wf::MeshTopology topology(mesh);
        std::size_t borders = 0, nonManifold = 0, winding = 0, triangles = 0, invalid = 0;
        for (const auto &[edge,faces] : topology.edgeFaces) {
            borders += faces.size() == 1;
            nonManifold += faces.size() > 2;
            if (faces.size() != 2)
                continue;
            const auto direction = [&](std::size_t i) {
                const auto &sides = topology.faceEdges[i];
                const auto index = std::ranges::find(sides,edge)-sides.begin();
                return mesh.faces[i].corners[index].vertex == edge.first;
            };
            winding += direction(faces[0]) == direction(faces[1]);
        }
        for (const auto &face : mesh.faces) {
            std::size_t count = 0;
            wf::visitFaceTriangles(mesh,face,[&](const auto &) { ++count; });
            invalid += count == 0;
            triangles += count;
        }
        message_->setText(tr("%1 triangles · %2 boundary edges · %3 non-manifold edges · %4 winding conflicts · %5 untriangulable faces")
            .arg(triangles).arg(borders).arg(nonManifold).arg(winding).arg(invalid));
    });
    assetMenu->addAction(tr("Generate LOD…"),this,&ModelingWorkspace::editMeshLod);
    auto *lodPreview = assetMenu->addAction(tr("Preview LOD in Object Mode"));
    lodPreview->setCheckable(true);
    connect(lodPreview,&QAction::toggled,viewport_,&LiveViewport::setModelLodPreview);
    assetMenu->addAction(tr("Collision Proxy…"),this,&ModelingWorkspace::editCollisionProxy);
    command(tr("Unwrap Selected Face"), "meshUnwrap", wf::unwrapFace);
    const auto shelfTool = [this,shelf,shelfLayout](const QString &label,const QString &icon,auto action) {
        auto *item = new QToolButton(shelf);
        item->setIcon(editorIcon(icon));
        item->setIconSize({19,19});
        item->setAccessibleName(tr("Toolbar: %1").arg(label));
        item->setToolTip(label);
        item->setFixedSize(30,30);
        shelfLayout->addWidget(item,0,Qt::AlignHCenter);
        connect(item,&QToolButton::clicked,this,action);
        return item;
    };
    const auto addShelfAction = [&](const QString &label,const QString &icon,QAction *action) {
        if (action)
            shelfTool(label,icon,[this,action] { if (commitPendingTransform()) action->trigger(); });
    };
    const auto menuAction = [](QMenu *menu,const QString &name) -> QAction * {
        for (auto *action : menu->actions())
            if (action->text() == name)
                return action;
        return nullptr;
    };
    shelfTool(tr("Select / Box Select"),"mouse",[this] {
        if (!commitPendingTransform())
            return;
        finishKnife(false);
        finishRetopology(false);
        setSurfaceTool(SurfaceTool::None);
        setPairTool(PairTool::None);
        activeTool_.reset();
        refreshSelection();
    });
    for (int i = 0; i < 3; ++i)
        shelfTool(names[i],icons[i],[this,i] { beginTransform(Transform(i)); });
    measureTool_ = shelfTool(tr("Measure"),"measure",[this](bool checked) {
        setSurfaceTool(!checked ? SurfaceTool::None
            : measurementMode_->currentIndex() == 0 ? SurfaceTool::Measure : SurfaceTool::MeasureAngle);
    });
    annotateTool_ = shelfTool(tr("Annotate"),"annotate",[this](bool checked) {
        setSurfaceTool(checked ? SurfaceTool::Annotate : SurfaceTool::None);
    });
    measureTool_->setCheckable(true);
    annotateTool_->setCheckable(true);
    weldTool_ = shelfTool(tr("Target Weld"),"magnet",[this](bool checked) {
        setPairTool(checked ? PairTool::TargetWeld : PairTool::None);
    });
    connectTool_ = shelfTool(tr("Connect"),"bridge",[this](bool checked) {
        setPairTool(checked ? PairTool::Connect : PairTool::None);
    });
    weldTool_->setCheckable(true);
    connectTool_->setCheckable(true);
    shelfTool(tr("Add Primitive"),"plus",[primitives] { primitives->click(); });
    addShelfAction(tr("Extrude Region"),"extrude",menuAction(regionMenu,tr("Extrude Region")));
    addShelfAction(tr("Inset Faces"),"inset",menuAction(regionMenu,tr("Inset Faces")));
    shelfTool(tr("Bevel Edges"),"bevel",[this] { editTopology(TopologyCommand::Bevel,tr("Bevel Edges")); });
    loopCutTool_ = shelfTool(tr("Loop Cut"),"loopcut",[this](bool checked) {
        setPairTool(checked ? PairTool::LoopCut : PairTool::None);
    });
    loopCutTool_->setCheckable(true);
    connect(loopCutTool_,&QToolButton::toggled,loopOptions,&QWidget::setVisible);
    shelfTool(tr("Knife / Multi-Cut"),"knife",[this] { if (commitPendingTransform()) startKnife(); });
    shelfTool(tr("Spin"),"spin",[this] { spin(); });
    addShelfAction(tr("Bisect"),"split",menuAction(meshMenu,tr("Bisect Mesh…")));
    addShelfAction(tr("Quad Draw"),"grid",menuAction(retopology,tr("Draw Quad")));
    shelfTool(tr("Weld to Active"),"magnet",[this] { editTopology(TopologyCommand::WeldActive,tr("Weld to Active")); });
    shelfTool(tr("Edge Slide"),"move",[this] { editTopology(TopologyCommand::SlideEdge,tr("Slide Edge")); });
    shelfTool(tr("Bridge Boundaries"),"bridge",[this] { editTopology(TopologyCommand::Bridge,tr("Bridge Boundaries")); });
    addShelfAction(tr("Smooth Vertices"),"sculpt",menuAction(retopology,tr("Relax Vertices…")));
    addShelfAction(tr("Shear"),"split",menuAction(deform,tr("Shear X by Y…")));
    addShelfAction(tr("To Sphere"),"globe",menuAction(deform,tr("To Sphere…")));
    addShelfAction(tr("Shrink / Fatten"),"scale",menuAction(deform,tr("Shrink / Fatten…")));
    shelfLayout->addStretch();
    auto *reimport = new QPushButton(tr("Reimport Source"), right);
    rightLayout->addWidget(reimport);
    connect(reimport, &QPushButton::clicked, this, [this] {
        if (!commitPendingTransform())
            return;
        const auto *asset = store_.selectedMesh();
        const bool hasEdits = asset && asset->locallyEdited && !asset->sourcePath.isEmpty();
        if (hasEdits && QMessageBox::question(this, tr("Reimport Source"),
                tr("Replace the local mesh edits with the source geometry? You can undo the replacement."),
                QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes)
            return;
        QString error;
        if (store_.reimportMesh(error, hasEdits)) {
            selectFace(-1);
            message_->setText(tr("Source is up to date."));
        } else {
            message_->setText(error);
        }
    });
    message_->setWordWrap(false);
    message_->setMinimumHeight(message_->fontMetrics().height());
    message_->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Fixed);
    message_->setAccessibleName(tr("Modeling result"));
    layout->addWidget(message_);
    rightLayout->addStretch(1);
    right->setMinimumWidth(212);
    columns->addWidget(inspector);
    columns->setStretchFactor(0, 1);
    columns->setStretchFactor(1, 0);
    resetLayout();
    layout->addWidget(columns, 1);

    connect(outliner_, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem *item) {
        const auto id = item ? item->data(0,Qt::UserRole).toString() : QString();
        if (id.isEmpty())
            return;
        if (commitPendingTransform())
            store_.select(id);
        else
            refresh();
    });
    connect(outliner_,&QTreeWidget::itemChanged,this,[this](QTreeWidgetItem *item,int column) {
        if (column != 1 || !item->parent())
            return;
        const auto id = item->data(0,Qt::UserRole).toString();
        const bool visible = item->checkState(1) == Qt::Checked;
        QTimer::singleShot(0,this,[this,id,visible] {
            if (!commitPendingTransform()) {
                rebuildOutliner();
                refresh();
                return;
            }
            if (const auto *object = store_.object(id); object && object->visible != visible) {
                auto next = *object;
                next.visible = visible;
                store_.updateObject(next,tr("Change Mesh Visibility"));
            }
        });
    });
    connect(viewport_, &LiveViewport::componentsPicked, this, &ModelingWorkspace::selectComponents);
    connect(viewport_, &LiveViewport::modelDragged, this, &ModelingWorkspace::dragTransform);
    connect(viewport_,&LiveViewport::modelHandlePressed,this,[this](int axis) {
        if (activeTool_)
            beginTransform(*activeTool_);
        if (transform_)
            axis_->setCurrentIndex(axis+1);
    });
    connect(viewport_,&LiveViewport::modelHandleReleased,this,[this] { finishTransform(true); });
    connect(viewport_, &LiveViewport::modelKeyPressed, this, &ModelingWorkspace::modelKey);
    connect(viewport_,&LiveViewport::modelCutPointPicked,this,&ModelingWorkspace::knifePoint);
    connect(viewport_,&LiveViewport::modelSurfacePicked,this,&ModelingWorkspace::retopologyPoint);
    connect(viewport_,&LiveViewport::modelSurfaceToolPoint,this,&ModelingWorkspace::sampleSurface);
    connect(viewport_,&LiveViewport::modelSurfaceToolReleased,this,&ModelingWorkspace::finishAnnotation);
    connect(viewport_,&LiveViewport::modelPairPoint,this,&ModelingWorkspace::pickPairPoint);
    connect(&store_, &ProjectStore::sceneChanged, this, [this] {
        capturedComponents_.clear();
        finishRetopology(false);
        rebuildOutliner();
        refresh();
    });
    connect(&store_, &ProjectStore::selectionChanged, this, &ModelingWorkspace::refresh);
    uv_->select = [this](int face) { selectFace(face); };
    uv_->togglePin = [this](wf::UvCorner corner) {
        if (transform_ || knife_ || retopology_)
            return;
        QString error;
        if (store_.toggleMeshUvPin(corner,error))
            message_->setText(store_.selectedMesh()->uvPins.contains(corner) ? tr("UV corner pinned.") : tr("UV corner unpinned."));
        else
            message_->setText(error);
    };
    uv_->move = [this](wf::Point2 delta) {
        if (uvIslands_)
            moveUvIsland(delta);
        else
            edit(tr("Move UVs"), [delta](wf::Mesh &mesh, std::size_t face) { return wf::translateFaceUV(mesh, face, delta); });
    };
    rebuildOutliner();
    refresh();
}

void ModelingWorkspace::resetLayout()
{
    columns_->setSizes({1100, 280});
}

void ModelingWorkspace::rebuildOutliner()
{
    const QSignalBlocker blocked(outliner_);
    QMap<QString,bool> expansion;
    for (int i = 0; i < outliner_->topLevelItemCount(); ++i) {
        const auto *group = outliner_->topLevelItem(i);
        expansion.insert(group->text(0),group->isExpanded());
    }
    outliner_->clear();
    outlinerItems_.clear();
    QMap<QString,QTreeWidgetItem *> groups;
    if (const auto *project = store_.project()) {
        for (const auto &object : project->objects) {
            if (object.meshAsset.isEmpty())
                continue;
            auto *&group = groups[object.group];
            if (!group) {
                group = new QTreeWidgetItem(outliner_,{object.group.isEmpty() ? tr("Ungrouped") : object.group});
                group->setIcon(0,editorIcon("folder"));
                group->setFlags((group->flags() & ~(Qt::ItemIsSelectable|Qt::ItemIsDragEnabled)) | Qt::ItemIsDropEnabled);
                group->setExpanded(expansion.value(group->text(0),true));
            }
            auto *item = new QTreeWidgetItem(group,{object.name});
            item->setIcon(0,editorIcon("box"));
            item->setData(0,Qt::UserRole,object.id);
            item->setFlags(item->flags() | Qt::ItemIsDragEnabled | Qt::ItemIsDropEnabled);
            item->setCheckState(1,object.visible ? Qt::Checked : Qt::Unchecked);
            item->setToolTip(1,tr("Visibility in World Editor"));
            outlinerItems_.insert(object.id,item);
        }
        for (const auto &object : project->objects) {
            auto *item = outlinerItems_.value(object.id);
            auto *parent = outlinerItems_.value(object.parentId);
            if (item && parent) {
                item->parent()->removeChild(item);
                parent->addChild(item);
                parent->setExpanded(true);
            }
        }
    }
    filterOutliner();
}

void ModelingWorkspace::filterOutliner()
{
    const auto query = outlinerFilter_->text().trimmed();
    for (int row = 0; row < outliner_->topLevelItemCount(); ++row) {
        auto *group = outliner_->topLevelItem(row);
        const auto filter = [&](auto &&self,QTreeWidgetItem *item,bool ancestor) -> bool {
            const bool matches = ancestor || item->text(0).contains(query,Qt::CaseInsensitive);
            bool visible = matches;
            for (int i = 0; i < item->childCount(); ++i)
                visible |= self(self,item->child(i),matches);
            item->setHidden(!visible);
            if (visible && !query.isEmpty() && item->childCount())
                item->setExpanded(true);
            return visible;
        };
        filter(filter,group,false);
    }
}

void ModelingWorkspace::setParentFromOutliner(const QString &child,const QString &parent)
{
    if (!commitPendingTransform())
        return;
    QString error;
    message_->setText(store_.reparentObject(child,parent,error) ? tr("Hierarchy updated.") : error);
}

void ModelingWorkspace::refresh()
{
    const auto selection = store_.selection();
    const auto *asset = store_.selectedMesh();
    const auto mesh = asset ? asset->mesh : QSharedPointer<const wf::Mesh>{};
    if (object_ != selection || observed_ != mesh) {
        setPairTool(PairTool::None);
        measurementPoints_.clear();
        measurement_->setText(tr("Measure: pick two surface points."));
        setSurfaceTool(SurfaceTool::None);
        viewport_->setModelMarkup({},{});
    }
    if (transform_ && (object_ != selection || !asset || asset->mesh != transform_->base))
        finishTransform(false);
    if (knife_ && (object_ != selection || !asset || asset->mesh != knife_->base || asset->uvSeams != knife_->baseSeams))
        finishKnife(false);
    if (retopology_ && (object_ != selection || !asset || asset->mesh != retopology_->base || !store_.object(retopology_->source)))
        finishRetopology(false);
    const auto sameTopology = [](const wf::Mesh &a,const wf::Mesh &b) {
        return a.vertices.size() == b.vertices.size() && std::ranges::equal(a.faces,b.faces,[](const auto &left,const auto &right) {
            return std::ranges::equal(left.corners,right.corners,std::ranges::equal_to{},
                &wf::MeshCorner::vertex,&wf::MeshCorner::vertex);
        });
    };
    if (object_ != selection || (observed_ != mesh && (!observed_ || !mesh || !sameTopology(*observed_,*mesh)))) {
        object_ = selection;
        selected_.clear();
        active_.reset();
        message_->clear();
    }
    observed_ = mesh;
    const QSignalBlocker blocked(outliner_);
    outliner_->setCurrentItem(outlinerItems_.value(selection));
    const auto previousReference = reference_->currentData().toString();
    {
        const QSignalBlocker referenceSignals(reference_);
        reference_->clear();
        reference_->addItem(tr("Reference: None"),QString());
        if (const auto *project = store_.project())
            for (const auto &object : project->objects)
                if (object.id != selection && !object.meshAsset.isEmpty())
                    reference_->addItem(object.name,object.id);
        reference_->setCurrentIndex(std::max(0,reference_->findData(previousReference)));
    }
    viewport_->setModelReference(reference_->currentData().toString());
    if (transform_)
        previewTransform();
    else
        refreshSelection();
}

void ModelingWorkspace::enterEditMode()
{
    if (domain_ == ModelDomain::Object)
        setDomain(ModelDomain::Face);
}

void ModelingWorkspace::hideEvent(QHideEvent *event)
{
    setPairTool(PairTool::None);
    setSurfaceTool(SurfaceTool::None);
    finishTransform(false);
    finishKnife(false);
    finishRetopology(false);
    QWidget::hideEvent(event);
}

void ModelingWorkspace::setDomain(ModelDomain domain)
{
    if (domain_ != domain) {
        if (!commitPendingTransform()) {
            refreshSelection();
            return;
        }
        finishKnife(false);
        finishRetopology(false);
        setPairTool(PairTool::None);
        domain_ = domain;
        selected_.clear();
        active_.reset();
        refreshSelection();
        viewport_->frame();
        return;
    }
    refreshSelection();
}

void ModelingWorkspace::selectComponents(const QList<ModelComponent> &components, Qt::KeyboardModifiers modifiers)
{
    if (transform_ || knife_ || retopology_)
        return;
    const bool add = modifiers.testFlag(Qt::ShiftModifier);
    const bool toggle = modifiers.testFlag(Qt::ControlModifier);
    const bool subtract = modifiers.testFlag(Qt::AltModifier);
    if (!add && !toggle && !subtract) {
        selected_.clear();
        active_.reset();
    }
    for (const auto component : components) {
        if (subtract || (toggle && selected_.contains(component)))
            selected_.erase(component);
        else {
            selected_.insert(component);
            active_ = component;
        }
    }
    if (active_ && !selected_.contains(*active_))
        active_ = selected_.empty() ? std::nullopt : std::optional{*selected_.rbegin()};
    refreshSelection();
}

void ModelingWorkspace::selectFace(int face)
{
    if (transform_ || knife_ || retopology_)
        return;
    if (face < 0 && domain_ != ModelDomain::Face)
        return;
    setDomain(ModelDomain::Face);
    selectComponents(face >= 0 ? QList<ModelComponent>{{face}} : QList<ModelComponent>{},Qt::NoModifier);
}

void ModelingWorkspace::convertSelection(ModelDomain destination)
{
    if (!commitPendingTransform())
        return;
    const auto *asset = store_.selectedMesh();
    if (!asset || destination == domain_ || selected_.empty())
        return;
    const auto &mesh = *asset->mesh;
    const auto vertices = modelVertices(mesh,domain_,selected_);
    std::set<ModelComponent> converted;
    if (destination == ModelDomain::Vertex) {
        for (const int vertex : vertices)
            converted.insert({vertex});
    } else {
        const wf::MeshTopology topology(mesh);
        const auto component = [](wf::MeshEdge edge) { return ModelComponent{int(edge.first),int(edge.second)}; };
        if (destination == ModelDomain::Edge) {
            for (const auto &[edge,faces] : topology.edgeFaces) {
                if (domain_ == ModelDomain::Face ? std::ranges::any_of(faces,[&](auto face) {
                    return selected_.contains({int(face)});
                }) : vertices.contains(int(edge.first)) && vertices.contains(int(edge.second)))
                    converted.insert(component(edge));
            }
        } else {
            for (std::size_t i = 0; i < topology.faceEdges.size(); ++i) {
                const auto &edges = topology.faceEdges[i];
                if (std::ranges::all_of(edges,[&](auto edge) {
                    return domain_ == ModelDomain::Edge ? selected_.contains(component(edge))
                        : vertices.contains(int(edge.first)) && vertices.contains(int(edge.second));
                }))
                    converted.insert({int(i)});
            }
        }
    }
    setDomain(destination);
    selectComponents(QList<ModelComponent>(converted.begin(),converted.end()),Qt::NoModifier);
}

void ModelingWorkspace::expandSelection(SelectionCommand command)
{
    if (!commitPendingTransform())
        return;
    const auto *asset = store_.selectedMesh();
    if (!asset || domain_ == ModelDomain::Object)
        return;
    const wf::MeshTopology topology(*asset->mesh);
    const auto edgeKey = [](ModelComponent component) {
        return wf::MeshEdge{wf::VertexIndex(component.index),wf::VertexIndex(component.end)};
    };
    const auto edgeComponent = [](wf::MeshEdge edge) { return ModelComponent{int(edge.first),int(edge.second)}; };
    std::set<ModelComponent> result;
    if (command == SelectionCommand::Border) {
        const auto vertices = modelVertices(*asset->mesh,domain_,selected_);
        for (const auto &[edge,faces] : topology.edgeFaces) {
            if (domain_ == ModelDomain::Face && !selected_.empty()) {
                const auto inside = std::ranges::count_if(faces,[&](auto face) { return selected_.contains({int(face)}); });
                if (inside > 0 && (faces.size() == 1 || std::size_t(inside) < faces.size()))
                    result.insert(edgeComponent(edge));
            } else if (faces.size() == 1 && (selected_.empty() || (vertices.contains(int(edge.first))
                                                                  && vertices.contains(int(edge.second))))) {
                result.insert(edgeComponent(edge));
            }
        }
        setDomain(ModelDomain::Edge);
    } else {
        if (selected_.empty())
            return;
        const auto neighbors = [&](ModelComponent component) {
            std::vector<ModelComponent> next;
            if (domain_ == ModelDomain::Vertex) {
                for (const auto &edge : topology.vertexEdges[component.index])
                    next.push_back({int(edge.first == wf::VertexIndex(component.index) ? edge.second : edge.first)});
            } else if (domain_ == ModelDomain::Face) {
                for (const auto &edge : topology.faceEdges[component.index])
                    for (const auto face : topology.edgeFaces.at(edge))
                        if (int(face) != component.index)
                            next.push_back({int(face)});
            } else {
                const auto edge = edgeKey(component);
                const auto &faces = topology.edgeFaces.at(edge);
                if (command == SelectionCommand::Ring) {
                    for (const auto face : faces) {
                        const auto &sides = topology.faceEdges[face];
                        if (sides.size() == 4) {
                            const auto i = std::ranges::find(sides,edge)-sides.begin();
                            next.push_back(edgeComponent(sides[(i+2)%4]));
                        }
                    }
                } else {
                    for (const auto vertex : {edge.first,edge.second}) {
                        const auto &edges = topology.vertexEdges[vertex];
                        std::optional<wf::MeshEdge> opposite;
                        for (const auto &candidate : edges) {
                            if (candidate == edge)
                                continue;
                            if (command != SelectionCommand::Loop) {
                                next.push_back(edgeComponent(candidate));
                                continue;
                            }
                            const auto &candidateFaces = topology.edgeFaces.at(candidate);
                            const bool boundary = faces.size() == 1 && candidateFaces.size() == 1;
                            const bool regular = faces.size() == 2 && candidateFaces.size() == 2 && edges.size() == 4
                                && std::ranges::all_of(faces,[&](auto face) { return topology.faceEdges[face].size() == 4; })
                                && std::ranges::none_of(faces,[&](auto face) {
                                    return std::ranges::find(candidateFaces,face) != candidateFaces.end();
                                });
                            if (boundary || regular) {
                                if (opposite) {
                                    opposite.reset();
                                    break;
                                }
                                opposite = candidate;
                            }
                        }
                        if (opposite)
                            next.push_back(edgeComponent(*opposite));
                    }
                }
            }
            return next;
        };
        if (command == SelectionCommand::Shrink) {
            for (const auto component : selected_)
                if (std::ranges::all_of(neighbors(component),[&](auto neighbor) { return selected_.contains(neighbor); }))
                    result.insert(component);
        } else {
            result = selected_;
            std::vector<ModelComponent> queue(selected_.begin(),selected_.end());
            for (std::size_t i = 0; i < queue.size(); ++i)
                for (const auto neighbor : neighbors(queue[i]))
                    if (result.insert(neighbor).second && command != SelectionCommand::Grow)
                        queue.push_back(neighbor);
        }
    }
    selectComponents(QList<ModelComponent>(result.begin(),result.end()),Qt::NoModifier);
}

void ModelingWorkspace::refreshSelection()
{
    const auto *asset = store_.selectedMesh();
    assignMaterial_->setEnabled(asset && domain_ == ModelDomain::Face && !selected_.empty() && !knife_ && !retopology_);
    selectMaterialFaces_->setEnabled(asset && !knife_ && !retopology_);
    const bool editing = domain_ != ModelDomain::Object;
    const bool oneFace = domain_ == ModelDomain::Face && selected_.size() == 1;
    const int face = domain_ == ModelDomain::Face && active_ ? active_->index : -1;
    if (knife_ && knife_->candidate)
        viewport_->setModelSelection(ModelDomain::Face,knife_->resultFaces,false);
    else if (retopology_ && retopology_->candidate && !retopology_->edge)
        viewport_->setModelSelection(ModelDomain::Face,{{0}},false);
    else
        viewport_->setModelSelection(domain_,selected_,xray_->isChecked());
    uv_->setMesh(asset ? asset->mesh : QSharedPointer<const wf::Mesh>{},face,
        asset ? asset->uvSeams : std::set<wf::MeshEdge>{},asset ? asset->uvPins : wf::UvPins{});
    domains_[0]->setChecked(!editing);
    editMode_->setChecked(editing);
    for (int i = 1; i < 4; ++i) {
        domains_[i]->setChecked(int(domain_) == i);
        domains_[i]->setVisible(editing);
    }
    xray_->setVisible(editing);
    symmetry_->setVisible(editing);
    meshCommands_->setVisible(editing);
    knifeTool_->setVisible(editing);
    knifeTool_->setEnabled(asset && !retopology_);
    knifeTool_->setChecked(knife_.has_value());
    for (int i = 0; i < 3; ++i) {
        tools_[i]->setVisible(true);
        tools_[i]->setEnabled(asset && (!editing || !selected_.empty()));
        tools_[i]->setChecked(activeTool_ && int(*activeTool_) == i);
    }
    const auto *object = asset ? store_.selectedObject() : nullptr;
    pivotMode_->setVisible(editing);
    for (int i = 0; i < 9; ++i) {
        auto *field = objectTransform_[i];
        const QSignalBlocker blocker(field);
        field->setEnabled(object);
        const auto value = !object ? QVector3D{} : i < 3 ? object->position : i < 6 ? object->rotation : object->scale;
        field->setValue(value[i%3]);
    }
    const auto *parent = object ? store_.object(object->parentId) : nullptr;
    objectParent_->setText(!object ? QString{} :
        parent ? tr("Parent: %1 · Local channels above").arg(parent->name) : tr("Parent: None"));
    if (object) {
        const auto world = store_.worldMatrix(object->id);
        const auto vectorText = [](QVector3D p) {
            return QString("%1, %2, %3").arg(p.x(),0,'f',2).arg(p.y(),0,'f',2).arg(p.z(),0,'f',2);
        };
        const auto x = world.mapVector({1,0,0}), y = world.mapVector({0,1,0}), z = world.mapVector({0,0,1});
        const auto right = x.normalized();
        const auto up = (y-right*QVector3D::dotProduct(y,right)).normalized();
        const auto forward = QVector3D::crossProduct(right,up);
        objectWorld_->setText(tr("World location: %1\nWorld rotation: %2\nWorld scale: %3")
            .arg(vectorText(world.map(QVector3D{})),vectorText(QQuaternion::fromAxes(right,up,forward).toEulerAngles()),
                vectorText({x.length(),y.length(),QVector3D::dotProduct(z,forward)})));
    } else
        objectWorld_->clear();
    if (!transform_ && !knife_ && !retopology_)
        refreshGizmo();
    for (auto *control : faceControls_)
        control->setEnabled(oneFace && !transform_ && !knife_ && !retopology_);
    for (auto *control : regionControls_)
        control->setEnabled(domain_ == ModelDomain::Face && !selected_.empty() && !transform_ && !knife_ && !retopology_);
    facePanel_->setVisible(domain_ == ModelDomain::Face);
    const QString descriptions[]{tr("Object Mode"),tr("Vertex"),tr("Edge"),tr("Face")};
    const auto status = !editing ? descriptions[0] : tr("%1 · %2 selected")
        .arg(descriptions[int(domain_)]).arg(selected_.size());
    summary_->setText(asset ? tr("%1 · %2 vertices · %3 faces\n%4")
        .arg(asset->name).arg(asset->mesh->vertices.size()).arg(asset->mesh->faces.size())
        .arg(status)
        : tr("Add a cube or import a GLB to start modeling."));
    if (asset && !asset->modifiers.empty())
        summary_->setText(summary_->text() + tr("\n%1 modifiers · %2 evaluated faces")
            .arg(asset->modifiers.size()).arg(asset->evaluated().faces.size()));
    if (asset && asset->lodCell > 0)
        summary_->setText(summary_->text() + (asset->lodMesh
            ? tr("\nLOD · %1 triangles · starts at %2 m").arg(asset->lodMesh->faces.size()).arg(asset->lodDistance)
            : tr("\nLOD needs a new cell size after this edit.")));
}

bool ModelingWorkspace::commitPendingTransform()
{
    return !transform_ || finishTransform(true);
}

void ModelingWorkspace::deformSelection(Deform tool)
{
    if (!commitPendingTransform() || knife_ || retopology_)
        return;
    const auto *asset = store_.selectedMesh();
    if (!asset || selected_.empty()) {
        message_->setText(tr("Select mesh components to deform."));
        return;
    }
    const auto base = asset->mesh;
    const auto id = asset->id;
    const auto vertices = modelVertices(*base,domain_,selected_);
    if (vertices.empty())
        return;
    const bool sphere = tool == Deform::ToSphere;
    const bool normals = tool == Deform::AlongNormals;
    const auto name = sphere ? tr("To Sphere") : normals ? tr("Shrink / Fatten") : tr("Shear X by Y");
    bool accepted = false;
    const auto amount = float(QInputDialog::getDouble(this,name,
        sphere ? tr("Blend (0–1)") : normals ? tr("Distance (m)") : tr("Shear factor"),
        sphere ? 1 : 0.1,sphere ? 0 : -1000,sphere ? 1 : 1000,4,&accepted));
    if (!accepted || amount == 0)
        return;
    const auto point = [](const wf::Point3 &p) { return QVector3D(p[0],p[1],p[2]); };
    QVector3D pivot;
    for (const auto index : vertices)
        pivot += point(base->vertices[index]);
    pivot /= float(vertices.size());
    float radius = 0;
    if (sphere) {
        for (const auto index : vertices)
            radius += (point(base->vertices[index])-pivot).length();
        radius /= float(vertices.size());
    }
    std::vector<QVector3D> directions;
    if (normals) {
        directions.resize(base->vertices.size());
        for (const auto &face : base->faces) {
            const auto direction = point(wf::faceNormal(*base,face));
            for (const auto &corner : face.corners)
                directions[corner.vertex] += direction;
        }
    }
    auto mesh = *base;
    for (const auto index : vertices) {
        auto position = point(base->vertices[index]);
        const auto offset = position-pivot;
        if (sphere && !offset.isNull())
            position += (offset.normalized()*radius-offset)*amount;
        else if (normals)
            position += directions[index].normalized()*amount;
        else if (!sphere)
            position.setX(position.x()+offset.y()*amount);
        mesh.vertices[index] = {position.x(),position.y(),position.z()};
    }
    for (auto &face : mesh.faces)
        if (std::ranges::any_of(face.corners,[&](const auto &c) { return vertices.contains(int(c.vertex)); }))
            for (auto &corner : face.corners)
                corner.normal = {};
    QString error;
    message_->setText(store_.updateMesh(id,std::move(mesh),name,error,base.data()) ? tr("%1 applied.").arg(name) : error);
}

void ModelingWorkspace::refreshGizmo()
{
    const auto *asset = store_.selectedMesh();
    const auto vertices = asset ? modelVertices(*asset->mesh,domain_,selected_) : std::set<int>{};
    const bool objectMode = domain_ == ModelDomain::Object;
    viewport_->setObjectPreview(objectMode && surfaceTool_ == SurfaceTool::None);
    if (!asset || (!objectMode && vertices.empty()) || !activeTool_ || surfaceTool_ != SurfaceTool::None || pairTool_ != PairTool::None) {
        viewport_->setModelPreview({});
        return;
    }
    ModelGizmo gizmo{{},{QVector3D(1,0,0),QVector3D(0,1,0),QVector3D(0,0,1)}};
    if (objectMode) {
        const auto world = store_.worldMatrix(store_.selection());
        gizmo.pivot = world.map(QVector3D{});
        if (orientation_->currentIndex() == 0)
            for (auto &axis : gizmo.axes)
                axis = world.mapVector(axis);
        gizmo.kind = ModelHandle(*activeTool_);
        gizmo.passive = true;
        viewport_->setModelPreview({},gizmo);
        return;
    }
    const auto pivot = pivotMode_->currentIndex() == 1 && active_ ? modelVertices(*asset->mesh,domain_,{*active_}) : vertices;
    const auto &indices = pivot.empty() ? vertices : pivot;
    for (const auto index : indices) {
        const auto &p = asset->mesh->vertices[index];
        gizmo.pivot += {p[0],p[1],p[2]};
    }
    gizmo.pivot /= float(indices.size());
    if (orientation_->currentIndex() == 1 && store_.selectedObject()) {
        const auto &object = *store_.selectedObject();
        const auto basis = store_.worldMatrix(object.id);
        bool invertible = false;
        const auto inverse = basis.inverted(&invertible);
        if (invertible)
            for (auto &axis : gizmo.axes)
                axis = inverse.mapVector(axis);
    }
    gizmo.kind = ModelHandle(*activeTool_);
    gizmo.passive = true;
    viewport_->setModelPreview({},gizmo);
}

void ModelingWorkspace::beginTransform(Transform tool)
{
    setPairTool(PairTool::None);
    setSurfaceTool(SurfaceTool::None);
    if (transform_ && transform_->tool == tool) {
        refreshSelection();
        return;
    }
    if (!commitPendingTransform())
        return;
    activeTool_ = tool;
    const auto *asset = store_.selectedMesh();
    const bool objectMode = domain_ == ModelDomain::Object;
    if (!asset || (!objectMode && selected_.empty())) {
        refreshSelection();
        return;
    }
    finishKnife(false);
    finishRetopology(false);
    auto vertices = modelVertices(*asset->mesh,domain_,selected_);
    if (vertices.empty() && !objectMode)
        return;
    transform_.emplace();
    transform_->tool = tool;
    transform_->asset = asset->id;
    transform_->base = asset->mesh;
    transform_->vertices = std::move(vertices);
    if (objectMode)
        transform_->objectBase = *store_.selectedObject();
    const bool scale = tool == Transform::Scale;
    const bool rotate = tool == Transform::Rotate;
    const float identity = scale ? 1.0f : 0.0f;
    transform_->dragValues = {identity,identity,identity};
    for (auto *value : values_) {
        const QSignalBlocker blocked(value);
        value->setRange(scale ? 0.0001 : rotate ? -36000 : -100000,scale ? 10000 : rotate ? 36000 : 100000);
        value->setSuffix(scale ? QString() : rotate ? tr("°") : tr(" m"));
        value->setSingleStep(scale ? 0.1 : rotate ? 5 : 0.1);
        value->setValue(identity);
    }
    const QSignalBlocker blocked(axis_);
    axis_->setCurrentIndex(0);
    transformLabel_->setText(tools_[int(tool)]->text());
    snap_->setVisible(tool == Transform::Move && !objectMode);
    snapStep_->setVisible(tool == Transform::Move && !objectMode && snap_->currentIndex() == 1);
    transformBar_->show();
    refreshSelection();
    previewTransform();
}

void ModelingWorkspace::previewTransform()
{
    if (!transform_) {
        if (!knife_ && !retopology_)
            refreshGizmo();
        return;
    }
    auto &edit = *transform_;
    const auto *object = store_.selectedObject();
    if (!object)
        return;
    if (edit.objectBase) {
        const auto &base = *edit.objectBase;
        const auto parent = store_.parentMatrix(base);
        const auto world = store_.worldMatrix(base.id);
        const bool local = orientation_->currentIndex() == 0;
        const bool scale = edit.tool == Transform::Scale;
        const float identity = scale ? 1.0f : 0.0f;
        const auto axis = axis_->currentIndex()-1;
        QVector3D amount(identity,identity,identity);
        std::array<QVector3D,3> axes;
        for (int i = 0; i < 3; ++i) {
            const bool enabled = axis < 0 || axis == i;
            values_[i]->setEnabled(enabled);
            if (enabled)
                amount[i] = float(values_[i]->value());
            QVector3D direction;
            direction[i] = 1;
            axes[i] = local ? world.mapVector(direction).normalized() : direction;
        }
        auto next = base;
        const auto pivot = world.map(QVector3D{});
        if (edit.tool == Transform::Move) {
            QVector3D movement;
            for (int i = 0; i < 3; ++i)
                movement += axes[i]*amount[i];
            next.position += parent.inverted().mapVector(movement);
        } else if (local || (base.parentId.isEmpty() && base.parentOffset.isIdentity() && edit.tool == Transform::Rotate)) {
            if (scale)
                next.scale *= amount;
            else if (!amount.isNull()) {
                const auto previous = QQuaternion::fromEulerAngles(base.rotation);
                const auto change = QQuaternion::fromEulerAngles(amount);
                next.rotation = (local ? previous*change : change*previous).toEulerAngles();
            }
        } else {
            QMatrix4x4 operation;
            operation.translate(pivot);
            if (scale)
                operation.scale(amount);
            else
                operation.rotate(QQuaternion::fromEulerAngles(amount));
            operation.translate(-pivot);
            next.parentOffset = store_.worldMatrix(base.parentId).inverted()*operation*parent;
        }
        edit.pivot = pivot;
        edit.objectCandidate = next;
        viewport_->setObjectPreview(true,&next);
        viewport_->setModelPreview({},ModelGizmo{store_.parentMatrix(next).map(next.position),axes,axis,false,false,
            ModelHandle(edit.tool)});
        apply_->setEnabled(true);
        return;
    }
    edit.localToSpace.setToIdentity();
    if (orientation_->currentIndex() == 1)
        edit.localToSpace = store_.worldMatrix(object->id);
    bool invertible = false;
    edit.spaceToLocal = edit.localToSpace.inverted(&invertible);
    if (!invertible) {
        edit.candidate.reset();
        viewport_->setModelPreview({});
        apply_->setEnabled(false);
        message_->setText(tr("World orientation requires an invertible object transform. Choose Local orientation."));
        return;
    }
    const auto point = [](wf::Point3 p) { return QVector3D(p[0],p[1],p[2]); };
    const auto pivotVertices = pivotMode_->currentIndex() == 1 && active_
        ? modelVertices(*edit.base,domain_,{*active_}) : edit.vertices;
    QVector3D pivot;
    for (int index : pivotVertices)
        pivot += point(edit.base->vertices[index]);
    pivot /= float(pivotVertices.size());
    edit.pivot = pivot;
    const auto pivotInSpace = edit.localToSpace.map(pivot);
    const int axis = axis_->currentIndex()-1;
    const bool scale = edit.tool == Transform::Scale;
    const float identity = scale ? 1.0f : 0.0f;
    QVector3D amounts(identity,identity,identity);
    for (int i = 0; i < 3; ++i) {
        const bool enabled = axis < 0 || axis == i;
        values_[i]->setEnabled(enabled);
        if (enabled)
            amounts[i] = float(values_[i]->value());
    }
    QMatrix4x4 operation;
    if (edit.tool == Transform::Move) {
        operation.translate(amounts);
    } else {
        operation.translate(pivotInSpace);
        if (scale) {
            operation.scale(amounts);
        } else {
            operation.rotate(amounts.z(),0,0,1);
            operation.rotate(amounts.y(),0,1,0);
            operation.rotate(amounts.x(),1,0,0);
        }
        operation.translate(-pivotInSpace);
    }
    std::array<QVector3D,3> axes;
    for (int i = 0; i < 3; ++i) {
        QVector3D direction;
        direction[i] = 1;
        axes[i] = edit.spaceToLocal.mapVector(direction);
    }
    const auto matrix = edit.spaceToLocal*operation*edit.localToSpace;
    const bool vertexSnap = edit.tool == Transform::Move && snap_->currentIndex() == 2;
    const bool surfaceSnap = edit.tool == Transform::Move && snap_->currentIndex() == 3;
    ModelGizmo gizmo{matrix.map(pivot),axes,axis,vertexSnap,surfaceSnap,ModelHandle(edit.tool)};
    if (amounts == QVector3D(identity,identity,identity)) {
        edit.candidate = edit.base;
        viewport_->setModelPreview({},gizmo);
        apply_->setEnabled(true);
        message_->clear();
        return;
    }
    auto candidate = QSharedPointer<wf::Mesh>::create(*edit.base);
    const bool soft = soft_->isChecked(), symmetry = symmetry_->isChecked();
    if (soft && edit.falloff.empty())
        edit.falloff = wf::connectedFalloff(*edit.base,{edit.vertices.begin(),edit.vertices.end()},float(softRadius_->value()));
    if (symmetry && edit.mirrorPairs.empty())
        edit.mirrorPairs = wf::mirroredVerticesX(*edit.base,0.0001f);
    const auto weight = [&](std::size_t index) {
        return soft ? edit.falloff[index] : edit.vertices.contains(int(index)) ? 1.0f : 0.0f;
    };
    const auto write = [&](std::size_t index,QVector3D p) { candidate->vertices[index] = {p.x(),p.y(),p.z()}; };
    for (std::size_t index = 0; index < edit.base->vertices.size(); ++index) {
        const float influence = weight(index);
        if (influence == 0)
            continue;
        const auto original = point(edit.base->vertices[index]);
        const auto partner = symmetry ? edit.mirrorPairs[index] : wf::MissingVertex;
        if (partner != wf::MissingVertex && partner != index) {
            const float mirroredWeight = weight(partner);
            if (influence < mirroredWeight || (influence == mirroredWeight && (original.x() < 0) != (pivot.x() < 0)))
                continue;
        }
        auto p = original+(matrix.map(original)-original)*influence;
        if (partner == index)
            p.setX(original.x());
        write(index,p);
        if (partner != wf::MissingVertex && partner != index) {
            p.setX(-p.x());
            write(partner,p);
        }
    }
    const auto normals = matrix.normalMatrix();
    for (auto &face : candidate->faces) {
        if (std::ranges::none_of(face.corners,[&](const auto &corner) {
            return candidate->vertices[corner.vertex] != edit.base->vertices[corner.vertex];
        }))
            continue;
        const bool transformedTogether = !symmetry && std::ranges::all_of(face.corners,[&](const auto &corner) {
            return edit.vertices.contains(int(corner.vertex));
        });
        for (auto &corner : face.corners) {
            if (!transformedTogether) {
                corner.normal = {};
                continue;
            }
            QVector3D n;
            for (int i = 0; i < 3; ++i)
                n[i] = normals(i,0)*corner.normal[0] + normals(i,1)*corner.normal[1] + normals(i,2)*corner.normal[2];
            n.normalize();
            corner.normal = {n.x(),n.y(),n.z()};
        }
        if (wf::faceNormal(*candidate,face) == wf::Point3{}) {
            edit.candidate.reset();
            gizmo.pivot = pivot;
            viewport_->setModelPreview({},gizmo);
            apply_->setEnabled(false);
            message_->setText(tr("This transform collapses a face. Try a different value."));
            return;
        }
    }
    edit.candidate = candidate;
    viewport_->setModelPreview(std::move(candidate),gizmo);
    apply_->setEnabled(true);
    message_->clear();
}

void ModelingWorkspace::dragTransform(QPointF delta,QVector3D displacement,std::optional<QVector3D> snapTarget)
{
    if (!transform_)
        return;
    auto &edit = *transform_;
    const int axis = axis_->currentIndex()-1;
    const auto move = edit.localToSpace.mapVector(displacement);
    const float pixels = float(delta.x()-delta.y());
    const auto step = snapStep_->value();
    const bool snapToGrid = edit.tool == Transform::Move && snap_->currentIndex() == 1;
    const auto targetDelta = snapTarget ? edit.localToSpace.mapVector(*snapTarget-edit.pivot) : QVector3D{};
    for (int i = 0; i < 3; ++i) {
        if (axis >= 0 && axis != i)
            continue;
        auto &raw = edit.dragValues[i];
        if (edit.tool == Transform::Move) {
            if (edit.objectBase && orientation_->currentIndex() == 0) {
                QVector3D direction;
                direction[i] = 1;
                raw += QVector3D::dotProduct(displacement,store_.worldMatrix(edit.objectBase->id).mapVector(direction).normalized());
            } else
                raw += move[i];
        }
        else if (edit.tool == Transform::Rotate)
            raw += axis == i || (axis < 0 && i == 2) ? pixels*0.6f : 0.0f;
        else
            raw *= std::exp(std::clamp(pixels*0.01f,-5.0f,5.0f));
        raw = std::clamp(raw,float(values_[i]->minimum()),float(values_[i]->maximum()));
        const QSignalBlocker blocked(values_[i]);
        if (snapTarget)
            values_[i]->setValue(targetDelta[i]);
        else
            values_[i]->setValue(snapToGrid ? std::round(raw/step)*step : raw);
    }
    previewTransform();
}

bool ModelingWorkspace::finishTransform(bool apply)
{
    if (!transform_)
        return true;
    if (apply) {
        for (auto *value : values_)
            value->interpretText();
        if (!transform_->candidate && !transform_->objectCandidate)
            return false;
    }
    auto edit = std::move(*transform_);
    transform_.reset();
    transformBar_->hide();
    viewport_->setModelPreview({});
    bool success = true;
    if (apply && edit.objectCandidate) {
        if (*edit.objectCandidate != *edit.objectBase)
            store_.updateObject(*edit.objectCandidate,tr("Transform Mesh Object"));
    } else if (apply && !edit.objectBase && edit.candidate != edit.base) {
        const auto name = tools_[int(edit.tool)]->text();
        QString error;
        success = store_.updateMesh(edit.asset,*edit.candidate,name,error,edit.base.data());
        if (success)
            message_->setText(tr("%1 applied.").arg(name));
        else
            message_->setText(error);
    } else {
        message_->clear();
    }
    refreshSelection();
    return success;
}

void ModelingWorkspace::modelKey(int key)
{
    if (key >= Qt::Key_4 && key <= Qt::Key_6) {
        shading_->button(key-Qt::Key_4)->setChecked(true);
    } else if (key >= Qt::Key_F9 && key <= Qt::Key_F11) {
        setDomain(ModelDomain(key-Qt::Key_F9+1));
    } else if (pairTool_ != PairTool::None && key == Qt::Key_Escape) {
        setPairTool(PairTool::None);
        message_->clear();
    } else if (pairTool_ != PairTool::None && (key == Qt::Key_Return || key == Qt::Key_Enter)) {
        applyConnect();
    } else if (surfaceTool_ != SurfaceTool::None && key == Qt::Key_Escape) {
        setSurfaceTool(SurfaceTool::None);
    } else if (retopology_) {
        if (key == Qt::Key_Escape || key == Qt::Key_Return || key == Qt::Key_Enter)
            finishRetopology(key != Qt::Key_Escape);
    } else if (knife_) {
        if (key == Qt::Key_Escape || key == Qt::Key_Return || key == Qt::Key_Enter)
            finishKnife(key != Qt::Key_Escape);
    } else if (key == Qt::Key_G || key == Qt::Key_R || key == Qt::Key_S) {
        beginTransform(key == Qt::Key_G ? Transform::Move : key == Qt::Key_R ? Transform::Rotate : Transform::Scale);
    } else if (transform_) {
        if (key == Qt::Key_Escape || key == Qt::Key_Return || key == Qt::Key_Enter) {
            finishTransform(key != Qt::Key_Escape);
        } else if (key == Qt::Key_X || key == Qt::Key_Y || key == Qt::Key_Z) {
            const int axis = key-Qt::Key_X+1;
            axis_->setCurrentIndex(axis_->currentIndex() == axis ? 0 : axis);
        }
    } else if (key == Qt::Key_1 || key == Qt::Key_3 || key == Qt::Key_7) {
        ortho_->setChecked(true);
        viewport_->setModelAxisView(key == Qt::Key_1 ? 1 : key == Qt::Key_3 ? 0 : 2);
    }
}


void ModelingWorkspace::edit(const QString &label,
    const std::function<std::string(wf::Mesh &, std::size_t)> &operation)
{
    if (!commitPendingTransform())
        return;
    const auto *asset = store_.selectedMesh();
    if (!asset) {
        message_->setText(tr("Select a mesh in the Model Outliner."));
        return;
    }
    if (domain_ != ModelDomain::Face || selected_.size() != 1 || !active_ || transform_ || knife_ || retopology_) {
        message_->setText(tr("Select one face to use this tool."));
        return;
    }
    const int face = active_->index;
    auto mesh = *asset->mesh;
    auto error = QString::fromStdString(operation(mesh, std::size_t(face)));
    if (error.isEmpty() && store_.updateMesh(asset->id, std::move(mesh), label, error)) {
        selectFace(face);
        message_->setText(tr("%1 applied.").arg(label));
        return;
    }
    message_->setText(error);
}

void ModelingWorkspace::editFaces(const QString &label,
    const std::function<std::string(wf::Mesh &, std::span<const std::size_t>)> &operation)
{
    if (!commitPendingTransform())
        return;
    const auto *asset = store_.selectedMesh();
    if (!asset || domain_ != ModelDomain::Face || selected_.empty() || transform_ || knife_ || retopology_) {
        message_->setText(tr("Select faces to use this tool."));
        return;
    }
    const auto selection = selected_;
    const auto active = active_;
    std::vector<std::size_t> faces;
    for (const auto component : selection)
        faces.push_back(std::size_t(component.index));
    const auto revision = asset->mesh;
    auto mesh = *revision;
    auto error = QString::fromStdString(operation(mesh,faces));
    if (!error.isEmpty() || !store_.updateMesh(asset->id,std::move(mesh),label,error,revision.data())) {
        message_->setText(error);
        return;
    }
    selected_ = selection;
    active_ = active;
    refreshSelection();
    message_->setText(tr("%1 applied.").arg(label));
}

void ModelingWorkspace::startKnife()
{
    setPairTool(PairTool::None);
    setSurfaceTool(SurfaceTool::None);
    if (!commitPendingTransform())
        return;
    if (knife_) {
        finishKnife(false);
        return;
    }
    const auto *asset = store_.selectedMesh();
    if (!asset || domain_ == ModelDomain::Object || transform_ || retopology_)
        return;
    knife_.emplace();
    knife_->asset = asset->id;
    knife_->base = asset->mesh;
    knife_->baseSeams = asset->uvSeams;
    knife_->preferredFace = domain_ == ModelDomain::Face && active_ ? active_->index : -1;
    viewport_->setModelKnife(true);
    message_->setText(tr("Click the first edge of the cut. Escape cancels."));
    refreshSelection();
}

void ModelingWorkspace::knifePoint(ModelComponent edge,float fraction,int nearbyFace)
{
    if (!knife_)
        return;
    const wf::MeshCutPoint point{{wf::VertexIndex(edge.index),wf::VertexIndex(edge.end)},
        fraction < 0.04f ? 0.0f : fraction > 0.96f ? 1.0f : fraction};
    const auto &mesh = *knife_->base;
    if (!knife_->start) {
        knife_->start = point;
        if (knife_->preferredFace < 0)
            knife_->preferredFace = nearbyFace;
        const auto &a = mesh.vertices[point.edge.first], &b = mesh.vertices[point.edge.second];
        const QVector3D start(a[0]*(1-point.fraction)+b[0]*point.fraction,
            a[1]*(1-point.fraction)+b[1]*point.fraction,a[2]*(1-point.fraction)+b[2]*point.fraction);
        viewport_->setModelKnife(true,start);
        message_->setText(tr("Click the next edge on the face you want to cut."));
        return;
    }
    const wf::MeshTopology topology(mesh);
    const auto &a = topology.edgeFaces.at(knife_->start->edge), &b = topology.edgeFaces.at(point.edge);
    std::vector<std::size_t> shared;
    std::ranges::set_intersection(a,b,std::back_inserter(shared));
    int face = -1;
    for (const int preferred : {knife_->preferredFace,nearbyFace})
        if (preferred >= 0 && std::ranges::find(shared,std::size_t(preferred)) != shared.end()) {
            face = preferred;
            break;
        }
    if (face < 0 && shared.size() == 1)
        face = int(shared.front());
    if (face < 0) {
        message_->setText(tr("Choose the next edge on the intended face, or press Enter to apply the path."));
        return;
    }
    auto candidate = QSharedPointer<wf::Mesh>::create(mesh);
    auto path = knife_->path;
    path.push_back({std::size_t(face),*knife_->start,point});
    auto seams = knife_->baseSeams;
    const auto error = wf::knifePath(*candidate,path,&seams);
    if (!error.empty()) {
        message_->setText(QString::fromStdString(error));
        return;
    }
    knife_->resultFaces.insert({face});
    knife_->resultFaces.insert({int(candidate->faces.size()-1)});
    knife_->path = std::move(path);
    knife_->start = point;
    knife_->preferredFace = -1;
    knife_->candidate = candidate;
    knife_->resultSeams = std::move(seams);
    const auto &from = mesh.vertices[point.edge.first], &to = mesh.vertices[point.edge.second];
    const QVector3D end(from[0]*(1-point.fraction)+to[0]*point.fraction,
        from[1]*(1-point.fraction)+to[1]*point.fraction,from[2]*(1-point.fraction)+to[2]*point.fraction);
    viewport_->setModelKnife(true,end);
    viewport_->setModelPreview(candidate);
    refreshSelection();
    message_->setText(tr("%1 cut segments. Click to continue, Enter to apply, or Escape to cancel.").arg(knife_->path.size()));
}

void ModelingWorkspace::finishKnife(bool apply)
{
    if (!knife_ || (apply && !knife_->candidate))
        return;
    const auto edit = std::move(*knife_);
    knife_.reset();
    viewport_->setModelKnife(false);
    viewport_->setModelPreview({});
    if (apply) {
        QString error;
        if (store_.updateMesh(edit.asset,*edit.candidate,tr("Knife Cut"),error,edit.base.data(),edit.resultSeams)) {
            setDomain(ModelDomain::Face);
            selectComponents(QList<ModelComponent>(edit.resultFaces.begin(),edit.resultFaces.end()),Qt::NoModifier);
            message_->setText(tr("Knife cut applied."));
        } else {
            message_->setText(error);
        }
    } else {
        message_->clear();
    }
    refreshSelection();
}

void ModelingWorkspace::bisect()
{
    if (!commitPendingTransform())
        return;
    const auto *asset = store_.selectedMesh();
    if (!asset || transform_ || knife_ || retopology_)
        return;
    const auto id = asset->id;
    const auto revision = asset->mesh;
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Bisect Mesh"));
    auto *form = new QFormLayout(&dialog);
    auto *axis = new QComboBox(&dialog);
    axis->addItems({"X","Y","Z"});
    form->addRow(tr("Plane normal"),axis);
    auto *coordinate = new QDoubleSpinBox(&dialog);
    coordinate->setRange(-100000,100000);
    coordinate->setDecimals(4);
    coordinate->setSuffix(tr(" m"));
    form->addRow(tr("Local coordinate"),coordinate);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,&dialog);
    form->addRow(buttons);
    connect(buttons,&QDialogButtonBox::accepted,&dialog,&QDialog::accept);
    connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);
    if (dialog.exec() != QDialog::Accepted)
        return;
    auto mesh = *revision;
    auto error = QString::fromStdString(wf::bisectMesh(mesh,unsigned(axis->currentIndex()),float(coordinate->value())));
    if (error.isEmpty() && store_.updateMesh(id,std::move(mesh),tr("Bisect Mesh"),error,revision.data()))
        message_->setText(tr("Mesh bisected."));
    else
        message_->setText(error);
}

void ModelingWorkspace::transformCapturedComponents()
{
    if (!commitPendingTransform())
        return;
    if (capturedComponents_.size() < 2) {
        message_->setText(tr("Capture components on at least two mesh objects."));
        return;
    }
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Transform Captured Components"));
    auto *form = new QFormLayout(&dialog);
    form->addRow(new QLabel(tr("%1 objects · World axes · Pivot at world origin")
        .arg(capturedComponents_.size()),&dialog));
    auto *operation = new QComboBox(&dialog);
    operation->addItems({tr("Move"),tr("Rotate"),tr("Scale")});
    form->addRow(tr("Operation"),operation);
    std::array<QDoubleSpinBox *,3> values;
    const QString axes[]{"X","Y","Z"};
    for (unsigned i = 0; i < values.size(); ++i) {
        values[i] = new QDoubleSpinBox(&dialog);
        values[i]->setAccessibleName(tr("Multi-object %1").arg(axes[i]));
        values[i]->setDecimals(4);
        form->addRow(axes[i],values[i]);
    }
    const auto configure = [values](int index) {
        for (auto *value : values) {
            value->setRange(index == 2 ? 0.0001 : -100000,100000);
            value->setSuffix(index == 0 ? " m" : index == 1 ? "°" : "");
            value->setValue(index == 2 ? 1 : 0);
        }
    };
    connect(operation,&QComboBox::currentIndexChanged,&dialog,configure);
    configure(0);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel,&dialog);
    form->addRow(buttons);
    connect(buttons,&QDialogButtonBox::accepted,&dialog,&QDialog::accept);
    connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);
    if (dialog.exec() != QDialog::Accepted)
        return;
    const QVector3D amount(float(values[0]->value()),float(values[1]->value()),float(values[2]->value()));
    QMatrix4x4 matrix;
    if (operation->currentIndex() == 0)
        matrix.translate(amount);
    else if (operation->currentIndex() == 1)
        matrix.rotate(QQuaternion::fromEulerAngles(amount));
    else
        matrix.scale(amount);
    QString error;
    const auto selection = capturedComponents_;
    const bool changed = store_.transformMeshComponents(selection,matrix,error);
    message_->setText(changed ? tr("Components transformed. Undo restores all affected meshes.") : error);
}

void ModelingWorkspace::revolve(bool edit)
{
    if (!commitPendingTransform() || !store_.project())
        return;
    setPairTool(PairTool::None);
    setSurfaceTool(SurfaceTool::None);
    finishKnife(false);
    finishRetopology(false);
    const auto *asset = edit ? store_.selectedMesh() : nullptr;
    if (edit && (!asset || asset->revolveProfile.isEmpty()))
        return;
    const auto previous = asset ? asset->revolveProfile : QJsonObject{};
    const auto id = asset ? asset->id : QString{};
    const auto revision = asset ? asset->mesh : QSharedPointer<const wf::Mesh>{};
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Revolve Profile"));
    dialog.resize(450,560);
    auto *form = new QFormLayout(&dialog);
    auto *points = new QTableWidget(0,3,&dialog);
    points->setAccessibleName(tr("Profile points"));
    points->setHorizontalHeaderLabels({tr("Radius (m)"),tr("Height (m)"),tr("Weight")});
    form->addRow(points);
    auto *profileView = new RevolveProfileView(&dialog);
    form->addRow(profileView);
    const auto addPoint = [points](double radius,double height,double weight = 1.0) {
        const auto row = points->rowCount();
        points->insertRow(row);
        points->setItem(row,0,new QTableWidgetItem(QString::number(radius)));
        points->setItem(row,1,new QTableWidgetItem(QString::number(height)));
        points->setItem(row,2,new QTableWidgetItem(QString::number(weight)));
    };
    if (previous.isEmpty()) {
        addPoint(0,-1);
        addPoint(0.75,-1);
        addPoint(0.9,0);
        addPoint(0.65,1);
        addPoint(0,1);
    } else {
        for (const auto &value : previous.value("points").toArray()) {
            const auto row = value.toArray();
            if (row.size() == 3)
                addPoint(row[0].toDouble(),row[1].toDouble(),row[2].toDouble(1));
        }
    }
    auto *pointTools = new QHBoxLayout;
    auto *add = new QPushButton(tr("Add Point"),&dialog);
    auto *remove = new QPushButton(tr("Remove Point"),&dialog);
    pointTools->addWidget(add);
    pointTools->addWidget(remove);
    form->addRow(pointTools);
    connect(add,&QPushButton::clicked,&dialog,[=] {
        if (points->rowCount() < 256) {
            const int row = points->currentRow() >= 0 ? points->currentRow()+1 : std::max(0,points->rowCount()-1);
            points->insertRow(row);
            points->setItem(row,0,new QTableWidgetItem("0.5"));
            points->setItem(row,1,new QTableWidgetItem("0"));
            points->setItem(row,2,new QTableWidgetItem("1"));
            points->setCurrentCell(row,0);
        }
    });
    connect(remove,&QPushButton::clicked,&dialog,[=] {
        if (points->rowCount() > 2 && points->currentRow() >= 0)
            points->removeRow(points->currentRow());
    });
    auto *axis = new QComboBox(&dialog);
    axis->addItems({"X","Y","Z"});
    axis->setCurrentIndex(previous.value("axis").toInt(2));
    form->addRow(tr("Axis"),axis);
    auto *interpolation = new QComboBox(&dialog);
    interpolation->setAccessibleName(tr("Profile interpolation"));
    interpolation->addItems({tr("Straight Segments"),tr("Rounded Corners"),tr("NURBS")});
    interpolation->setCurrentIndex(previous.value("interpolation").toInt());
    form->addRow(tr("Profile"),interpolation);
    auto *degree = new QSpinBox(&dialog);
    degree->setAccessibleName(tr("NURBS degree"));
    degree->setRange(1,5);
    degree->setValue(previous.value("degree").toInt(3));
    form->addRow(tr("Degree"),degree);
    auto *samples = new QSpinBox(&dialog);
    samples->setRange(2,255);
    samples->setValue(previous.value("samples").toInt(32));
    form->addRow(tr("Profile Segments"),samples);
    auto *angle = new QDoubleSpinBox(&dialog);
    angle->setRange(-360,360);
    angle->setValue(previous.value("angle").toDouble(360));
    angle->setSuffix("°");
    form->addRow(tr("Sweep"),angle);
    auto *steps = new QSpinBox(&dialog);
    steps->setRange(1,256);
    steps->setValue(previous.value("steps").toInt(32));
    form->addRow(tr("Steps"),steps);
    auto *feedback = new QLabel(&dialog);
    feedback->setWordWrap(true);
    form->addRow(feedback);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel,&dialog);
    form->addRow(buttons);
    wf::Mesh result;
    QJsonObject savedProfile;
    const auto updateProfile = [&] {
        buttons->button(QDialogButtonBox::Ok)->setEnabled(false);
        const bool nurbs = interpolation->currentIndex() == 2;
        points->setColumnHidden(2,!nurbs);
        form->setRowVisible(degree,nurbs);
        form->setRowVisible(samples,nurbs);
        std::vector<wf::Point2> profile;
        std::vector<float> weights;
        QJsonArray controls;
        for (int row = 0; row < points->rowCount(); ++row) {
            bool radiusOk = false, heightOk = false;
            const auto *radiusItem = points->item(row,0), *heightItem = points->item(row,1);
            const auto radius = radiusItem ? radiusItem->text().toFloat(&radiusOk) : 0;
            const auto height = heightItem ? heightItem->text().toFloat(&heightOk) : 0;
            if (!radiusOk || !heightOk || !std::isfinite(radius) || !std::isfinite(height)
                || radius < 0 || radius > 100000 || std::abs(height) > 100000) {
                profileView->setPoints({},{});
                feedback->setText(tr("Enter a nonnegative radius and a valid height at point %1.").arg(row+1));
                return;
            }
            profile.push_back({radius,height});
            bool weightOk = false;
            const auto *weightItem = points->item(row,2);
            const auto weight = weightItem ? weightItem->text().toFloat(&weightOk) : 1.0f;
            if (nurbs && (!weightOk || !std::isfinite(weight) || weight < 0.0001f || weight > 10000)) {
                feedback->setText(tr("Enter a weight between 0.0001 and 10000 at point %1.").arg(row+1));
                return;
            }
            weights.push_back(weightOk && std::isfinite(weight) ? weight : 1.0f);
            controls.append(QJsonArray{radius,height,weights.back()});
        }
        auto outline = profile;
        if (nurbs) {
            const auto curve = wf::sampleNurbsProfile(profile,weights,unsigned(degree->value()),unsigned(samples->value()));
            if (!curve) {
                profileView->setPoints(profile,profile);
                feedback->setText(tr("NURBS needs at least one more control point than its degree."));
                return;
            }
            outline = *curve;
        }
        if (interpolation->currentIndex() == 1 && profile.size() > 2) {
            if (profile.size() > 52) {
                profileView->setPoints(profile,profile);
                feedback->setText(tr("Use at most 52 control points for rounded corners."));
                return;
            }
            const auto blend = [](wf::Point2 a,wf::Point2 b,float t) -> wf::Point2 {
                return {std::lerp(a[0],b[0],t),std::lerp(a[1],b[1],t)};
            };
            outline = {profile.front()};
            for (std::size_t i = 1; i+1 < profile.size(); ++i) {
                const auto before = blend(profile[i],profile[i-1],0.25f);
                const auto after = blend(profile[i],profile[i+1],0.25f);
                for (int sample = 0; sample <= 4; ++sample) {
                    const float t = sample/4.0f;
                    outline.push_back(blend(blend(before,profile[i],t),blend(profile[i],after,t),t));
                }
            }
            outline.push_back(profile.back());
        }
        profileView->setPoints(profile,outline);
        const auto error = wf::revolveProfile(result,outline,unsigned(axis->currentIndex()),
            float(angle->value()),unsigned(steps->value()));
        buttons->button(QDialogButtonBox::Ok)->setEnabled(error.empty());
        savedProfile = {{"points",controls},{"axis",axis->currentIndex()},{"interpolation",interpolation->currentIndex()},
            {"degree",degree->value()},{"samples",samples->value()},{"angle",angle->value()},{"steps",steps->value()}};
        feedback->setText(error.empty() ? tr("%1 vertices · %2 faces").arg(result.vertices.size()).arg(result.faces.size())
            : QString::fromStdString(error));
    };
    connect(points,&QTableWidget::itemChanged,&dialog,updateProfile);
    connect(points->model(),&QAbstractItemModel::rowsRemoved,&dialog,updateProfile);
    connect(interpolation,&QComboBox::currentIndexChanged,&dialog,updateProfile);
    connect(degree,&QSpinBox::valueChanged,&dialog,updateProfile);
    connect(samples,&QSpinBox::valueChanged,&dialog,updateProfile);
    connect(axis,&QComboBox::currentIndexChanged,&dialog,updateProfile);
    connect(angle,&QDoubleSpinBox::valueChanged,&dialog,updateProfile);
    connect(steps,&QSpinBox::valueChanged,&dialog,updateProfile);
    updateProfile();
    connect(buttons,&QDialogButtonBox::accepted,&dialog,&QDialog::accept);
    connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);
    if (dialog.exec() != QDialog::Accepted)
        return;
    QString error;
    const bool saved = edit ? store_.updateRevolvedMesh(id,std::move(result),savedProfile,revision.data(),error)
        : store_.addRevolvedMesh(std::move(result),savedProfile,error);
    message_->setText(saved ? (edit ? tr("Revolved profile updated.") : tr("Revolved profile added.")) : error);
}

void ModelingWorkspace::spin()
{
    setPairTool(PairTool::None);
    if (!commitPendingTransform())
        return;
    setSurfaceTool(SurfaceTool::None);
    const auto *asset = store_.selectedMesh();
    if (!asset || domain_ != ModelDomain::Edge || selected_.empty() || knife_ || retopology_) {
        message_->setText(tr("Select open boundary edges in Edge mode to spin."));
        return;
    }
    const auto id = asset->id;
    const auto revision = asset->mesh;
    std::vector<wf::MeshEdge> profile;
    for (const auto &edge : selected_)
        profile.emplace_back(std::minmax(wf::VertexIndex(edge.index),wf::VertexIndex(edge.end)));
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Spin Boundary"));
    auto *form = new QFormLayout(&dialog);
    auto *axis = new QComboBox(&dialog);
    axis->addItems({"X","Y","Z"});
    axis->setCurrentIndex(2);
    axis->setAccessibleName(tr("Spin axis"));
    form->addRow(tr("Local axis"),axis);
    auto *degrees = new QDoubleSpinBox(&dialog);
    degrees->setRange(-360,360);
    degrees->setValue(90);
    degrees->setSuffix("°");
    form->addRow(tr("Angle"),degrees);
    auto *steps = new QSpinBox(&dialog);
    steps->setRange(1,256);
    steps->setValue(12);
    form->addRow(tr("Steps"),steps);
    std::array<QDoubleSpinBox *,3> center;
    const QString axes[]{"X","Y","Z"};
    for (unsigned i = 0; i < center.size(); ++i) {
        center[i] = new QDoubleSpinBox(&dialog);
        center[i]->setRange(-100000,100000);
        center[i]->setDecimals(3);
        form->addRow(tr("Pivot %1").arg(axes[i]),center[i]);
    }
    auto *hint = new QLabel(tr("A full turn welds the sweep seam and keeps the source faces separate."),&dialog);
    hint->setWordWrap(true);
    form->addRow(hint);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel,&dialog);
    form->addRow(buttons);
    connect(buttons,&QDialogButtonBox::accepted,&dialog,&QDialog::accept);
    connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);
    auto *validation = new QLabel(&dialog);
    validation->setWordWrap(true);
    form->insertRow(form->rowCount()-1,validation);
    QSharedPointer<wf::Mesh> preview;
    const auto updatePreview = [&] {
        auto candidate = QSharedPointer<wf::Mesh>::create(*revision);
        const wf::Point3 pivot{float(center[0]->value()),float(center[1]->value()),float(center[2]->value())};
        const auto error = wf::spinEdges(*candidate,profile,unsigned(axis->currentIndex()),
            pivot,float(degrees->value()),unsigned(steps->value()));
        preview = error.empty() ? candidate : QSharedPointer<wf::Mesh>{};
        validation->setText(error.empty() ? tr("%1 new faces").arg(candidate->faces.size()-revision->faces.size())
            : QString::fromStdString(error));
        buttons->button(QDialogButtonBox::Ok)->setEnabled(bool(preview));
        viewport_->setModelPreview(preview);
    };
    connect(axis,&QComboBox::currentIndexChanged,&dialog,updatePreview);
    connect(degrees,&QDoubleSpinBox::valueChanged,&dialog,updatePreview);
    connect(steps,&QSpinBox::valueChanged,&dialog,updatePreview);
    for (auto *coordinate : center) {
        coordinate->setKeyboardTracking(false);
        connect(coordinate,&QDoubleSpinBox::valueChanged,&dialog,updatePreview);
    }
    updatePreview();
    const auto accepted = dialog.exec() == QDialog::Accepted;
    refreshSelection();
    if (!accepted || !preview)
        return;
    QString error;
    if (store_.updateMesh(id,*preview,tr("Spin Boundary"),error,revision.data()))
        message_->setText(tr("Boundary spun."));
    else
        message_->setText(error);
}

void ModelingWorkspace::setPairTool(PairTool tool)
{
    if (tool != PairTool::None) {
        if (!store_.selectedMesh() || !commitPendingTransform()) {
            tool = PairTool::None;
        } else {
            setSurfaceTool(SurfaceTool::None);
            finishKnife(false);
            finishRetopology(false);
            if (tool == PairTool::LoopCut)
                setDomain(ModelDomain::Edge);
            else if (domain_ != ModelDomain::Vertex && domain_ != ModelDomain::Edge)
                setDomain(ModelDomain::Vertex);
        }
    }
    weldTool_->setChecked(tool == PairTool::TargetWeld);
    connectTool_->setChecked(tool == PairTool::Connect);
    loopCutTool_->setChecked(tool == PairTool::LoopCut);
    if (pairTool_ == tool)
        return;
    pairTool_ = tool;
    pairStart_.reset();
    loopFace_ = -1;
    connectPreview_.reset();
    connectSeams_.clear();
    viewport_->setModelPairPicking(tool != PairTool::None);
    refreshSelection();
    if (tool != PairTool::None)
        message_->setText(tool == PairTool::TargetWeld
            ? tr("Drag a vertex or boundary edge onto its target. Escape exits.")
            : tool == PairTool::Connect ? tr("Click two vertices or edges on one face. Enter applies. Escape cancels.")
            : tr("Click a quad edge to preview a loop cut. Enter applies. Escape cancels."));
}

void ModelingWorkspace::pickPairPoint(ModelComponent component,bool pressed,int nearbyFace)
{
    const auto *asset = store_.selectedMesh();
    if (!asset || pairTool_ == PairTool::None)
        return;
    if (pairTool_ == PairTool::LoopCut) {
        if (!pressed && component.index >= 0) {
            pairStart_ = component;
            loopFace_ = nearbyFace;
            previewLoopCut();
        }
        return;
    }
    const bool weld = pairTool_ == PairTool::TargetWeld;
    const bool startsPair = weld ? pressed : !pressed && !pairStart_;
    if (startsPair) {
        if (connectPreview_) {
            connectPreview_.reset();
            connectSeams_.clear();
            viewport_->setModelPreview({});
        }
        pairStart_.reset();
        if (component.index >= 0) {
            pairStart_ = component;
            selectComponents({component},Qt::NoModifier);
            if (!weld)
                message_->setText(tr("Click the second component on the same face."));
        }
        return;
    }
    if (pressed || !pairStart_)
        return;
    const auto source = *std::exchange(pairStart_,{});
    if (component.index < 0 || source == component) {
        message_->setText(tr("Choose a different target component."));
        return;
    }
    const auto id = asset->id;
    const auto revision = asset->mesh;
    auto mesh = *revision;
    auto seams = asset->uvSeams;
    std::string failure;
    if (domain_ == ModelDomain::Vertex) {
        const std::array vertices{wf::VertexIndex(source.index),wf::VertexIndex(component.index)};
        failure = weld ? wf::weldVertices(mesh,vertices,0,vertices[1]) : wf::connectVertices(mesh,vertices[0],vertices[1]);
    } else {
        const wf::MeshEdge a{wf::VertexIndex(source.index),wf::VertexIndex(source.end)};
        const wf::MeshEdge b{wf::VertexIndex(component.index),wf::VertexIndex(component.end)};
        failure = weld ? wf::weldBoundaryEdges(mesh,a,b) : wf::connectEdges(mesh,a,b,&seams);
    }
    auto error = QString::fromStdString(failure);
    if (!weld) {
        if (!error.isEmpty()) {
            message_->setText(error);
            return;
        }
        connectPreview_ = QSharedPointer<wf::Mesh>::create(std::move(mesh));
        connectSeams_ = std::move(seams);
        viewport_->setModelPreview(connectPreview_);
        message_->setText(tr("Connect preview. Enter applies. Escape cancels."));
        return;
    }
    const auto title = tr("Target Weld");
    if (!error.isEmpty() || !store_.updateMesh(id,std::move(mesh),title,error,revision.data())) {
        message_->setText(error);
        return;
    }
    setPairTool(PairTool::TargetWeld);
    message_->setText(tr("%1 applied. Escape exits.").arg(title));
}

void ModelingWorkspace::previewLoopCut()
{
    const auto *asset = store_.selectedMesh();
    if (!asset || !pairStart_)
        return;
    const auto &mesh = *asset->mesh;
    const wf::MeshTopology topology(mesh);
    const wf::MeshEdge picked{wf::VertexIndex(pairStart_->index),wf::VertexIndex(pairStart_->end)};
    const auto faces = topology.edgeFaces.find(picked);
    if (faces == topology.edgeFaces.end())
        return;
    const auto quad = std::ranges::find_if(faces->second,[&](auto face) { return mesh.faces[face].corners.size() == 4; });
    const auto face = loopFace_ >= 0 && std::ranges::find(faces->second,std::size_t(loopFace_)) != faces->second.end()
        && mesh.faces[loopFace_].corners.size() == 4 ? std::optional{std::size_t(loopFace_)}
        : quad != faces->second.end() ? std::optional{*quad} : std::nullopt;
    connectPreview_.reset();
    connectSeams_.clear();
    viewport_->setModelPreview({});
    if (!face) {
        message_->setText(tr("Loop Cut needs an edge of a quad face."));
        return;
    }
    auto preview = QSharedPointer<wf::Mesh>::create(mesh);
    auto seams = asset->uvSeams;
    const auto &edges = topology.faceEdges[*face];
    const auto offset = std::size_t(std::ranges::find(edges,picked)-edges.begin());
    const auto error = wf::cutQuadLoop(*preview,*face,offset,unsigned(cuts_->value()),float(slide_->value()/100),&seams);
    if (!error.empty()) {
        message_->setText(QString::fromStdString(error));
        return;
    }
    connectPreview_ = preview;
    connectSeams_ = std::move(seams);
    viewport_->setModelPreview(preview);
    message_->setText(tr("Loop Cut preview. Enter applies. Escape cancels."));
}

void ModelingWorkspace::applyConnect()
{
    const auto *asset = store_.selectedMesh();
    if (!asset || !connectPreview_)
        return;
    const auto tool = pairTool_;
    const auto title = tool == PairTool::LoopCut ? tr("Loop Cut") : tr("Connect");
    const auto preview = connectPreview_;
    QString error;
    if (!store_.updateMesh(asset->id,*preview,title,error,asset->mesh.data(),connectSeams_)) {
        message_->setText(error);
        return;
    }
    setPairTool(tool);
    message_->setText(tr("%1 applied. Escape exits.").arg(title));
}

void ModelingWorkspace::setSurfaceTool(SurfaceTool tool)
{
    if (tool != SurfaceTool::None)
        setPairTool(PairTool::None);
    if (tool != SurfaceTool::None && (!store_.selectedMesh() || !commitPendingTransform()))
        tool = SurfaceTool::None;
    const bool changed = surfaceTool_ != tool;
    measureTool_->setChecked(tool == SurfaceTool::Measure || tool == SurfaceTool::MeasureAngle);
    annotateTool_->setChecked(tool == SurfaceTool::Annotate);
    eraseAnnotation_->setChecked(tool == SurfaceTool::EraseAnnotation);
    if (!changed)
        return;
    surfaceTool_ = tool;
    if (tool != SurfaceTool::None) {
        finishKnife(false);
        finishRetopology(false);
        message_->setText(tool == SurfaceTool::Measure ? tr("Click two points on the mesh.")
            : tool == SurfaceTool::MeasureAngle ? tr("Click three points on the mesh. The middle point is the angle's vertex.")
            : tool == SurfaceTool::Annotate ? tr("Drag across the mesh to draw. Escape exits Annotate.")
            : tr("Click an annotation stroke to erase it. Escape exits."));
    }
    refreshGizmo();
    drawingAnnotation_ = false;
    annotationPoints_.clear();
    viewport_->setModelSurfaceTool(tool != SurfaceTool::None);
    viewport_->setModelMarkup(measurementPoints_,{});
}

void ModelingWorkspace::sampleSurface(const QVector3D &position,bool first)
{
    const auto *object = store_.selectedObject();
    if (!object)
        return;
    const bool angle = surfaceTool_ == SurfaceTool::MeasureAngle;
    const auto scaleVector = [object](QVector3D delta) {
        for (int i = 0; i < 3; ++i)
            delta[i] *= object->scale[i];
        return delta;
    };
    if (surfaceTool_ == SurfaceTool::EraseAnnotation && first) {
        if (const auto stroke = viewport_->annotationNear(position)) {
            auto updated = *object;
            auto strokes = updated.details.value("modelAnnotations").toArray();
            strokes.removeAt(*stroke);
            if (strokes.isEmpty())
                updated.details.remove("modelAnnotations");
            else
                updated.details.insert("modelAnnotations",strokes);
            store_.updateObject(updated,tr("Erase Model Annotation"));
            message_->setText(tr("Annotation erased. Undo restores it."));
        }
    } else if ((surfaceTool_ == SurfaceTool::Measure || angle) && first) {
        const int needed = angle ? 3 : 2;
        if (measurementPoints_.size() >= needed)
            measurementPoints_.clear();
        measurementPoints_.append(position);
        if (measurementPoints_.size() == needed) {
            const auto firstArm = scaleVector(measurementPoints_[0]-measurementPoints_[1]);
            if (angle) {
                const auto secondArm = scaleVector(measurementPoints_[2]-measurementPoints_[1]);
                if (firstArm.lengthSquared() <= 1e-10f || secondArm.lengthSquared() <= 1e-10f) {
                    measurement_->setText(tr("Angle needs three distinct points."));
                } else {
                    const auto cosine = std::clamp(QVector3D::dotProduct(firstArm.normalized(),secondArm.normalized()),-1.0f,1.0f);
                    measurement_->setText(tr("Angle: %1°").arg(std::acos(cosine)*180.0/std::numbers::pi,0,'f',2));
                }
            } else {
                measurement_->setText(tr("Distance: %1 m").arg(firstArm.length(),0,'f',4));
            }
        } else {
            measurement_->setText(tr("Measure: %1 of %2 points.").arg(measurementPoints_.size()).arg(needed));
        }
        message_->setText(measurement_->text());
    } else if (surfaceTool_ == SurfaceTool::Annotate) {
        if (first) {
            drawingAnnotation_ = true;
            annotationPoints_.clear();
        }
        if (drawingAnnotation_ && annotationPoints_.size() < 2048
            && (annotationPoints_.isEmpty() || annotationPoints_.back().distanceToPoint(position) > 0.0001f))
            annotationPoints_.append(position);
    }
    viewport_->setModelMarkup(measurementPoints_,annotationPoints_);
}

void ModelingWorkspace::finishAnnotation()
{
    if (!std::exchange(drawingAnnotation_,false))
        return;
    const auto points = std::exchange(annotationPoints_,{});
    viewport_->setModelMarkup(measurementPoints_,{});
    const auto *object = store_.selectedObject();
    if (!object || points.size() < 2)
        return;
    auto edited = *object;
    auto strokes = edited.details.value("modelAnnotations").toArray();
    if (strokes.size() >= 128) {
        message_->setText(tr("This object has 128 annotations. Clear them to draw more."));
        return;
    }
    QJsonArray stroke;
    for (const auto &point : points)
        stroke.append(QJsonArray{point.x(),point.y(),point.z()});
    strokes.append(stroke);
    edited.details.insert("modelAnnotations",strokes);
    store_.updateObject(edited,tr("Draw Model Annotation"));
}

void ModelingWorkspace::editMaterialSlots(bool create)
{
    if (!commitPendingTransform() || knife_ || retopology_)
        return;
    const auto *asset = store_.selectedMesh();
    if (!asset)
        return;
    const auto id = asset->id;
    const auto revision = asset->mesh;
    const auto selection = domain_ == ModelDomain::Face ? selected_ : std::set<ModelComponent>{};
    auto mesh = *revision;
    int slot = materialSlot_->currentIndex();
    if (create) {
        bool accepted = false;
        const auto name = QInputDialog::getText(this,tr("New Material Slot"),tr("Name"),QLineEdit::Normal,
            tr("Material %1").arg(mesh.materials.size()+1),&accepted).trimmed();
        if (!accepted || name.isEmpty())
            return;
        slot = int(mesh.materials.size());
        mesh.materials.push_back({name.toStdString()});
    } else if (slot < 0 || selection.empty()) {
        return;
    }
    for (const auto face : selection)
        mesh.faces[face.index].material = std::uint32_t(slot);
    QString error;
    if (store_.updateMesh(id,std::move(mesh),create ? tr("Create Material Slot") : tr("Assign Material Slot"),
            error,revision.data())) {
        materialSlot_->setCurrentIndex(slot);
        message_->setText(create ? tr("Material slot created.") : tr("Material assigned."));
    } else {
        message_->setText(error);
    }
}

void ModelingWorkspace::assignBaseColorTexture()
{
    if (!commitPendingTransform())
        return;
    const auto *asset = store_.selectedMesh();
    if (!asset)
        return;
    const auto id = asset->id;
    const auto revision = asset->mesh;
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Material Preview Settings"));
    auto *form = new QFormLayout(&dialog);
    auto *slot = new QComboBox(&dialog);
    for (const auto &material : revision->materials)
        slot->addItem(QString::fromStdString(material.name));
    slot->setCurrentIndex(std::max(0,materialSlot_->currentIndex()));
    form->addRow(tr("Material slot"),slot);
    auto *metallic = new QDoubleSpinBox(&dialog);
    auto *roughness = new QDoubleSpinBox(&dialog);
    metallic->setAccessibleName(tr("Material metallic"));
    roughness->setAccessibleName(tr("Material roughness"));
    for (auto *field : {metallic,roughness}) {
        field->setRange(0,1);
        field->setDecimals(3);
        field->setSingleStep(0.05);
    }
    form->addRow(tr("Metallic"),metallic);
    form->addRow(tr("Roughness"),roughness);
    auto *channel = new QComboBox(&dialog);
    channel->setAccessibleName(tr("Material map channel"));
    channel->addItems({tr("Base Color"),tr("Normal"),tr("Metallic / Roughness"),tr("Ambient Occlusion"),tr("Emissive")});
    form->addRow(tr("Texture"),channel);
    auto *emissive = new QDoubleSpinBox(&dialog);
    emissive->setRange(0,1);
    emissive->setDecimals(3);
    emissive->setAccessibleName(tr("Emission multiplier"));
    form->addRow(tr("Emission"),emissive);
    auto *colorButton = new QPushButton(tr("Choose Base Color…"),&dialog);
    form->addRow(tr("Base Color"),colorButton);
    std::map<int,QColor> pendingColors;
    const auto showColor = [revision,slot,colorButton,&pendingColors] {
        const auto &base = revision->materials[slot->currentIndex()].color;
        const auto original = QColor::fromRgbF(std::clamp(base[0],0.0f,1.0f),
            std::clamp(base[1],0.0f,1.0f),std::clamp(base[2],0.0f,1.0f));
        const auto found = pendingColors.find(slot->currentIndex());
        const auto color = found == pendingColors.end() ? original : found->second;
        QPixmap swatch(16,16);
        swatch.fill(color);
        colorButton->setIcon(QIcon(swatch));
        return color;
    };
    connect(slot,&QComboBox::currentIndexChanged,&dialog,[showColor] { showColor(); });
    connect(colorButton,&QPushButton::clicked,&dialog,[&] {
        const auto picked = QColorDialog::getColor(showColor(),&dialog,tr("Base Color"));
        if (picked.isValid()) {
            pendingColors[slot->currentIndex()] = picked;
            showColor();
        }
    });
    showColor();
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel,&dialog);
    auto *choose = buttons->addButton(tr("Choose Image…"),QDialogButtonBox::ActionRole);
    auto *remove = buttons->addButton(tr("Remove Texture"),QDialogButtonBox::ActionRole);
    const auto updateChannel = [revision,slot,channel,remove] {
        remove->setEnabled(!revision->materials[slot->currentIndex()].previewTextures()[channel->currentIndex()]->empty());
    };
    const auto loadSlot = [revision,slot,metallic,roughness,emissive,updateChannel] {
        const auto &material = revision->materials[slot->currentIndex()];
        metallic->setValue(material.metallic);
        roughness->setValue(material.roughness);
        emissive->setValue(*std::ranges::max_element(material.emissive));
        updateChannel();
    };
    connect(slot,&QComboBox::currentIndexChanged,&dialog,loadSlot);
    connect(channel,&QComboBox::currentIndexChanged,&dialog,updateChannel);
    loadSlot();
    const auto initialEmission = emissive->value();
    form->addRow(buttons);
    bool clear = false;
    bool chooseImage = false;
    connect(choose,&QPushButton::clicked,&dialog,[&] { chooseImage = true; dialog.accept(); });
    connect(remove,&QPushButton::clicked,&dialog,[&] { clear = true; dialog.accept(); });
    connect(buttons,&QDialogButtonBox::accepted,&dialog,&QDialog::accept);
    connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);
    if (dialog.exec() != QDialog::Accepted)
        return;
    auto mesh = *revision;
    for (const auto &[index,color] : pendingColors) {
        auto &rgba = mesh.materials[index].color;
        rgba[0] = float(color.redF());
        rgba[1] = float(color.greenF());
        rgba[2] = float(color.blueF());
    }
    auto &material = mesh.materials[slot->currentIndex()];
    material.metallic = float(metallic->value());
    material.roughness = float(roughness->value());
    const auto previousEmission = *std::ranges::max_element(material.emissive);
    if (std::abs(emissive->value()-previousEmission) > 0.00051) {
        const auto multiplier = float(emissive->value());
        for (auto &factor : material.emissive)
            factor = previousEmission > 0 ? multiplier*factor/previousEmission : multiplier;
    }
    auto &texture = *material.previewTextures()[channel->currentIndex()];
    if (clear) {
        texture.clear();
    } else if (chooseImage) {
        const auto path = QFileDialog::getOpenFileName(this,tr("Choose %1 Image").arg(channel->currentText()),{},
            tr("PNG or JPEG (*.png *.jpg *.jpeg)"));
        if (path.isEmpty())
            return;
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly) || file.size() > qsizetype(wf::MeshMaterial::maxTextureBytes)) {
            message_->setText(tr("Choose a readable PNG or JPEG under 16 MiB."));
            return;
        }
        QImageReader reader(&file);
        reader.setDecideFormatFromContent(true);
        const auto dimensions = reader.size();
        const auto format = reader.format();
        if ((format != "png" && format != "jpeg") || !dimensions.isValid()
            || dimensions.width() > 8192 || dimensions.height() > 8192
            || qint64(dimensions.width())*dimensions.height() > 32*1024*1024 || reader.read().isNull()
            || !file.seek(0)) {
            message_->setText(tr("The image must be PNG or JPEG and fit the 32-megapixel limit."));
            return;
        }
        const auto bytes = file.readAll();
        texture.assign(bytes.begin(),bytes.end());
        if (channel->currentIndex() == 4 && material.emissive == wf::Point3{} && initialEmission == emissive->value())
            material.emissive = {1,1,1};
    }
    QString error;
    if (store_.updateMesh(id,std::move(mesh),tr("Edit Material Preview"),error,revision.data())) {
        shading_->button(int(ModelShading::Material))->setChecked(true);
        message_->setText(clear ? tr("Texture removed.") : chooseImage
            ? tr("Texture assigned.") : tr("Material preview settings updated."));
    } else {
        message_->setText(error);
    }
}

void ModelingWorkspace::startRetopology(bool extend,bool continuous)
{
    setPairTool(PairTool::None);
    setSurfaceTool(SurfaceTool::None);
    if (!commitPendingTransform())
        return;
    const auto *asset = store_.selectedMesh();
    if (!asset || transform_ || knife_ || retopology_)
        return;
    const auto reference = reference_->currentData().toString();
    std::optional<wf::MeshEdge> edge;
    if (extend) {
        if (reference.isEmpty() || domain_ != ModelDomain::Edge || selected_.size() != 1) {
            message_->setText(tr("Choose a reference mesh and select one edge of the retopology patch."));
            return;
        }
        const auto component = *selected_.begin();
        edge = wf::MeshEdge{wf::VertexIndex(component.index),wf::VertexIndex(component.end)};
        const wf::MeshTopology topology(*asset->mesh);
        if (topology.edgeFaces.at(*edge).size() != 1) {
            message_->setText(tr("Select an open boundary edge to extend."));
            return;
        }
    }
    retopology_.emplace();
    retopology_->asset = asset->id;
    retopology_->source = reference.isEmpty() ? store_.selection() : reference;
    retopology_->base = asset->mesh;
    retopology_->edge = edge;
    retopology_->continuous = continuous;
    viewport_->setModelRetopology(true);
    message_->setText(extend ? tr("Click two points on the reference to extend the edge.")
        : tr("Click four points in perimeter order to draw a quad on the reference."));
    refreshSelection();
}

void ModelingWorkspace::retopologyPoint(const QVector3D &position,const QVector3D &normal)
{
    if (!retopology_ || retopology_->candidate)
        return;
    auto &edit = *retopology_;
    edit.points.append(position);
    edit.normal += normal;
    viewport_->setModelRetopology(true,edit.points);
    const int required = edit.edge ? 2 : 4;
    if (edit.points.size() < required) {
        message_->setText(tr("%1 of %2 surface points. Escape cancels.").arg(edit.points.size()).arg(required));
        return;
    }
    const auto point = [](QVector3D p) -> wf::Point3 { return {p.x(),p.y(),p.z()}; };
    if (edit.edge) {
        auto candidate = QSharedPointer<wf::Mesh>::create(*edit.base);
        const auto error = wf::extendQuadBoundary(*candidate,*edit.edge,{point(edit.points[0]),point(edit.points[1])});
        if (error.empty())
            edit.candidate = candidate;
        else
            message_->setText(QString::fromStdString(error));
    } else {
        std::array<wf::Point3,4> points;
        for (int i = 0; i < 4; ++i)
            points[i] = point(edit.points[i]);
        if (auto patch = wf::makeRetopologyQuad(points,point(edit.normal)))
            edit.candidate = QSharedPointer<wf::Mesh>::create(std::move(*patch));
        else
            message_->setText(tr("These points do not form a valid quad. Click four points in perimeter order."));
    }
    if (!edit.candidate) {
        edit.points.clear();
        edit.normal = {};
        viewport_->setModelRetopology(true);
        return;
    }
    viewport_->setModelPreview(edit.candidate);
    if (!edit.edge)
        viewport_->setModelSelection(ModelDomain::Face,{{0}},false);
    message_->setText(tr("Quad preview. Press Enter to apply or Escape to cancel."));
}

void ModelingWorkspace::finishRetopology(bool apply)
{
    if (!retopology_ || (apply && !retopology_->candidate))
        return;
    const auto edit = std::move(*retopology_);
    retopology_.reset();
    viewport_->setModelRetopology(false);
    viewport_->setModelPreview({});
    if (!apply) {
        message_->clear();
    } else {
        QString error;
        const bool success = edit.edge
            ? store_.updateMesh(edit.asset,*edit.candidate,tr("Extend Retopology Edge"),error,edit.base.data())
            : store_.addRetopologyPatch(*edit.candidate,error);
        if (success) {
            if (const auto source = reference_->findData(edit.source); source >= 0)
                reference_->setCurrentIndex(source);
            if (edit.edge || edit.continuous) {
                const auto first = int(edit.candidate->vertices.size())-2;
                if (edit.continuous)
                    setDomain(ModelDomain::Edge);
                selectComponents({{first,first+1}},Qt::NoModifier);
            }
            message_->setText(edit.edge ? tr("Quad added. The new outer edge is selected.") : tr("Retopology patch created."));
            if (edit.continuous) {
                startRetopology(true,true);
                return;
            }
        } else {
            message_->setText(error);
        }
    }
    refreshSelection();
}

void ModelingWorkspace::conformOrRelax(bool relax)
{
    if (!commitPendingTransform())
        return;
    const auto *asset = store_.selectedMesh();
    if (!asset || transform_ || knife_ || retopology_ || selected_.empty())
        return;
    const auto id = asset->id;
    const auto revision = asset->mesh;
    const auto vertices = modelVertices(*revision,domain_,selected_);
    const bool reference = !reference_->currentData().toString().isEmpty();
    if (!relax && !reference) {
        message_->setText(tr("Choose a reference mesh to conform to."));
        return;
    }
    auto mesh = *revision;
    if (relax) {
        bool accepted = false;
        const auto strength = QInputDialog::getDouble(this,tr("Relax Vertices"),tr("Strength (%)"),50,0.1,100,1,&accepted);
        if (!accepted)
            return;
        const std::vector<wf::VertexIndex> indices(vertices.begin(),vertices.end());
        const auto error = wf::relaxVertices(mesh,indices,float(strength/100));
        if (!error.empty()) {
            message_->setText(QString::fromStdString(error));
            return;
        }
    }
    if (reference) {
        for (int vertex : vertices) {
            auto &p = mesh.vertices[vertex];
            if (relax && p == revision->vertices[vertex])
                continue;
            const auto projected = viewport_->conformToReference({p[0],p[1],p[2]});
            if (!projected) {
                message_->setText(tr("Some vertices miss the reference in this view. Reposition the view and try again."));
                return;
            }
            p = {projected->x(),projected->y(),projected->z()};
        }
        for (auto &face : mesh.faces)
            if (std::ranges::any_of(face.corners,[&](const auto &corner) {
                return mesh.vertices[corner.vertex] != revision->vertices[corner.vertex];
            }))
                for (auto &corner : face.corners)
                    corner.normal = {};
    }
    QString error;
    const auto action = relax ? tr("Relax Vertices") : tr("Conform to Reference");
    message_->setText(store_.updateMesh(id,std::move(mesh),action,error,revision.data()) ? tr("%1 applied.").arg(action) : error);
}

void ModelingWorkspace::remesh()
{
    if (!commitPendingTransform())
        return;
    const auto *asset = store_.selectedMesh();
    if (!asset || transform_ || knife_ || retopology_)
        return;
    const auto id = asset->id;
    const auto revision = asset->mesh;
    bool accepted = false;
    const auto size = QInputDialog::getDouble(this,tr("Voxel Remesh"),tr("Cell size (m)"),0.1,0.0001,100000,4,&accepted);
    if (!accepted)
        return;

    using Result = std::pair<wf::Mesh,std::string>;
    std::packaged_task<Result(std::stop_token)> task([revision,size](std::stop_token stop) {
        auto mesh = *revision;
        auto error = wf::remeshVoxels(mesh,float(size),stop);
        return Result{std::move(mesh),std::move(error)};
    });
    auto result = task.get_future();
    std::jthread worker(std::move(task));
    QProgressDialog progress(tr("Rebuilding mesh surface…"),tr("Cancel"),0,0,this);
    progress.setWindowTitle(tr("Voxel Remesh"));
    progress.setWindowModality(Qt::ApplicationModal);
    progress.setMinimumDuration(0);
    connect(&progress,&QProgressDialog::canceled,this,[&worker] { worker.request_stop(); });
    QEventLoop wait;
    QTimer timer;
    connect(&timer,&QTimer::timeout,&wait,[&] {
        if (result.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready)
            wait.quit();
    });
    progress.show();
    timer.start(20);
    wait.exec();
    timer.stop();
    worker.join();
    progress.hide();
    if (progress.wasCanceled()) {
        message_->setText(tr("Voxel Remesh cancelled."));
        return;
    }
    Result rebuilt;
    try {
        rebuilt = result.get();
    } catch (const std::exception &error) {
        message_->setText(tr("Voxel Remesh failed. %1").arg(QString::fromUtf8(error.what())));
        return;
    }
    if (!rebuilt.second.empty()) {
        message_->setText(QString::fromStdString(rebuilt.second));
        return;
    }
    const auto *current = store_.selectedMesh();
    if (!current || current->id != id || current->mesh != revision) {
        message_->setText(tr("The selected mesh changed. Run Voxel Remesh again."));
        return;
    }
    auto preview = QSharedPointer<wf::Mesh>::create(std::move(rebuilt.first));
    viewport_->setModelSelection(ModelDomain::Object,{},false);
    viewport_->setModelPreview(preview);
    const auto apply = QMessageBox::question(this,tr("Voxel Remesh Preview"),
        tr("The preview has %1 vertices and %2 triangles. Apply it to the base mesh?")
            .arg(preview->vertices.size()).arg(preview->faces.size()),
        QMessageBox::Apply | QMessageBox::Discard,QMessageBox::Discard) == QMessageBox::Apply;
    viewport_->setModelPreview({});
    refreshSelection();
    if (apply) {
        QString error;
        message_->setText(store_.updateMesh(id,std::move(*preview),tr("Voxel Remesh"),error,revision.data())
            ? tr("Voxel Remesh applied.") : error);
    }
}

void ModelingWorkspace::editMeshLod()
{
    const auto *asset = store_.selectedMesh();
    if (!asset || transform_ || knife_ || retopology_)
        return;
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Generate Mesh LOD"));
    auto *form = new QFormLayout(&dialog);
    auto *cell = new QDoubleSpinBox(&dialog);
    cell->setRange(0,10000);
    cell->setDecimals(4);
    cell->setSuffix(tr(" m"));
    cell->setValue(asset->lodCell > 0 ? asset->lodCell : 0.2);
    form->addRow(tr("Reduction cell size (0 disables LOD)"),cell);
    auto *distance = new QDoubleSpinBox(&dialog);
    distance->setRange(0.1,100000);
    distance->setSuffix(tr(" m"));
    distance->setValue(asset->lodDistance);
    form->addRow(tr("Switch distance"),distance);
    auto *message = new QLabel(&dialog);
    message->setWordWrap(true);
    form->addRow(message);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,&dialog);
    form->addRow(buttons);
    connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);
    connect(buttons,&QDialogButtonBox::accepted,&dialog,[&] {
        QString error;
        if (store_.setMeshLod(float(cell->value()),float(distance->value()),error))
            dialog.accept();
        else
            message->setText(error);
    });
    dialog.exec();
}

void ModelingWorkspace::editCollisionProxy()
{
    const auto *object = store_.selectedObject();
    const auto *asset = store_.selectedMesh();
    if (!object || !asset || transform_ || knife_ || retopology_)
        return;
    auto next = *object;
    const auto &vertices = asset->evaluated().vertices;
    auto low = vertices.front(), high = low;
    for (const auto &point : vertices)
        for (int axis = 0; axis < 3; ++axis) {
            low[axis] = std::min(low[axis],point[axis]);
            high[axis] = std::max(high[axis],point[axis]);
        }
    const auto proxy = next.details.value("collisionProxy").toObject();
    const auto storedCenter = proxy.value("center").toArray(), storedHalf = proxy.value("half").toArray();
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Collision Proxy"));
    auto *form = new QFormLayout(&dialog);
    auto *body = new QComboBox(&dialog);
    body->addItems({tr("Disabled"),tr("Static"),tr("Dynamic")});
    const auto bodyType = next.details.value("physics").toString();
    body->setCurrentIndex(bodyType == "Static" ? 1 : bodyType == "Dynamic" ? 2 : 0);
    form->addRow(tr("Body"),body);
    auto *shape = new QComboBox(&dialog);
    shape->addItems({tr("Box"),tr("Sphere"),tr("Capsule (Z)")});
    shape->setCurrentIndex(std::clamp(next.details.value("colliderShape").toInt(),0,2));
    form->addRow(tr("Shape"),shape);
    auto *custom = new QCheckBox(tr("Use custom local bounds"),&dialog);
    custom->setChecked(storedCenter.size() == 3 && storedHalf.size() == 3);
    form->addRow(custom);
    std::array<QDoubleSpinBox *,3> center, half;
    const char *axes[]{"X","Y","Z"};
    for (int axis = 0; axis < 3; ++axis) {
        const auto field = [&](const QString &label,double min,double value) {
            auto *spin = new QDoubleSpinBox(&dialog);
            spin->setRange(min,100000);
            spin->setDecimals(4);
            spin->setSuffix(tr(" m"));
            spin->setValue(value);
            spin->setEnabled(custom->isChecked());
            connect(custom,&QCheckBox::toggled,spin,&QWidget::setEnabled);
            form->addRow(label,spin);
            return spin;
        };
        center[axis] = field(tr("Center %1").arg(axes[axis]),-100000,storedCenter.size() == 3
            ? storedCenter[axis].toDouble() : (low[axis]+high[axis])*0.5f);
        half[axis] = field(tr("Half extent %1").arg(axes[axis]),0.025,storedHalf.size() == 3
            ? storedHalf[axis].toDouble() : (high[axis]-low[axis])*0.5f);
    }
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,&dialog);
    form->addRow(buttons);
    connect(buttons,&QDialogButtonBox::accepted,&dialog,&QDialog::accept);
    connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);
    if (dialog.exec() != QDialog::Accepted)
        return;
    if (body->currentIndex())
        next.details.insert("physics",body->currentIndex() == 1 ? "Static" : "Dynamic");
    else
        next.details.remove("physics");
    next.details.insert("colliderShape",shape->currentIndex());
    if (custom->isChecked()) {
        QJsonArray position, extents;
        for (int axis = 0; axis < 3; ++axis) {
            position.append(center[axis]->value());
            extents.append(half[axis]->value());
        }
        next.details.insert("collisionProxy",QJsonObject{{"center",position},{"half",extents}});
    } else {
        next.details.remove("collisionProxy");
    }
    store_.updateObject(next,tr("Edit Collision Proxy"));
}

void ModelingWorkspace::openModifierStack()
{
    if (!commitPendingTransform())
        return;
    if (!store_.selectedMesh() || transform_ || knife_ || retopology_)
        return;
    const QStringList names{tr("Mirror"),tr("Perimeter Bevel"),tr("Subdivision Surface"),tr("Array"),
        tr("Solidify"),tr("Weld"),tr("Weighted Normal"),tr("Decimate by Cell Size"),tr("Shrinkwrap"),tr("Boolean")};
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Modifier Stack"));
    dialog.resize(550,340);
    auto *layout = new QVBoxLayout(&dialog);
    auto *list = new QListWidget(&dialog);
    list->setAccessibleName(tr("Modifier order"));
    layout->addWidget(list);
    auto *message = new QLabel(&dialog);
    message->setWordWrap(true);
    layout->addWidget(message);
    const auto refresh = [&](int row) {
        const QSignalBlocker blocked(list);
        list->clear();
        if (const auto *asset = store_.selectedMesh()) {
            for (const auto &item : asset->modifiers) {
                auto *entry = new QListWidgetItem(names.at(int(item.kind)),list);
                entry->setCheckState(item.enabled ? Qt::Checked : Qt::Unchecked);
            }
            list->setCurrentRow(std::clamp(row,0,std::max(0,list->count()-1)));
        }
    };
    const auto editSettings = [&](wf::MeshModifier modifier,bool adding) -> std::optional<wf::MeshModifier> {
        QDialog settings(&dialog);
        settings.setWindowTitle(adding ? tr("Add Modifier") : tr("Modifier Settings"));
        auto *form = new QFormLayout(&settings);
        auto *type = new QComboBox(&settings);
        type->addItems(names);
        type->setCurrentIndex(int(modifier.kind));
        type->setEnabled(adding);
        form->addRow(tr("Type"),type);
        auto *axis = new QComboBox(&settings);
        axis->addItems({"X","Y","Z"});
        axis->setCurrentIndex(int(modifier.axis));
        form->addRow(tr("Axis"),axis);
        auto *mirrorSide = new QComboBox(&settings);
        mirrorSide->addItems({tr("Automatic (positive on a crossing)"),tr("Positive"),tr("Negative")});
        mirrorSide->setCurrentIndex(int(modifier.mirrorSide));
        form->addRow(tr("Source side"),mirrorSide);
        auto *count = new QSpinBox(&settings);
        form->addRow(tr("Count / levels"),count);
        auto *amount = new QDoubleSpinBox(&settings);
        amount->setDecimals(5);
        amount->setSuffix(tr(" m"));
        form->addRow(tr("Distance / width"),amount);
        auto *face = new QSpinBox(&settings);
        face->setRange(1,2'000'000);
        face->setValue(int(modifier.face)+1);
        form->addRow(tr("Face number"),face);
        auto *target = new QComboBox(&settings);
        target->setAccessibleName(tr("Modifier target asset"));
        target->setToolTip(tr("Uses the target's evaluated geometry in asset-local coordinates."));
        target->addItem(tr("Choose a mesh asset"),QString());
        const auto *active = store_.selectedMesh();
        for (const auto &asset : store_.project()->assets)
            if (asset.id != active->id)
                target->addItem(asset.name,asset.id);
        target->setCurrentIndex(std::max(0,target->findData(QString::fromStdString(modifier.targetAsset))));
        form->addRow(tr("Target asset"),target);
        auto *booleanOperation = new QComboBox(&settings);
        booleanOperation->addItems({tr("Difference"),tr("Union"),tr("Intersection")});
        booleanOperation->setCurrentIndex(int(modifier.booleanOperation));
        form->addRow(tr("Operation"),booleanOperation);
        std::array<QDoubleSpinBox *,3> targetOffset{};
        const QStringList offsetLabels{tr("Cutter offset X"),tr("Cutter offset Y"),tr("Cutter offset Z")};
        for (std::size_t i = 0; i < targetOffset.size(); ++i) {
            auto *field = new QDoubleSpinBox(&settings);
            field->setDecimals(5);
            field->setRange(-100000,100000);
            field->setSuffix(tr(" m"));
            field->setValue(modifier.targetOffset[i]);
            form->addRow(offsetLabels.at(qsizetype(i)),field);
            targetOffset[i] = field;
        }
        const auto showFields = [&](int index) {
            const auto kind = wf::ModifierKind(index);
            form->setRowVisible(axis,kind == wf::ModifierKind::Mirror || kind == wf::ModifierKind::Array);
            form->setRowVisible(mirrorSide,kind == wf::ModifierKind::Mirror);
            form->setRowVisible(count,kind == wf::ModifierKind::Array || kind == wf::ModifierKind::Subdivision);
            form->setRowVisible(amount,kind == wf::ModifierKind::Array || kind == wf::ModifierKind::PerimeterBevel
                || kind == wf::ModifierKind::Weld || kind == wf::ModifierKind::Solidify
                || kind == wf::ModifierKind::Decimate || kind == wf::ModifierKind::Shrinkwrap);
            form->setRowVisible(face,kind == wf::ModifierKind::PerimeterBevel);
            form->setRowVisible(target,kind == wf::ModifierKind::Shrinkwrap || kind == wf::ModifierKind::Boolean);
            form->setRowVisible(booleanOperation,kind == wf::ModifierKind::Boolean);
            for (auto *field : targetOffset)
                form->setRowVisible(field,kind == wf::ModifierKind::Boolean);
            count->setRange(kind == wf::ModifierKind::Subdivision ? 1 : 2,kind == wf::ModifierKind::Subdivision ? 3 : 100);
            amount->setRange(kind == wf::ModifierKind::Array || kind == wf::ModifierKind::Shrinkwrap ? -100000 : 0.00001,100000);
            count->setValue(kind == wf::ModifierKind::Subdivision && adding ? 1 : int(modifier.count));
            amount->setValue(adding && kind == wf::ModifierKind::Array ? 1.25
                : adding && kind == wf::ModifierKind::Shrinkwrap ? 0.001 : modifier.amount);
        };
        connect(type,&QComboBox::currentIndexChanged,&settings,showFields);
        showFields(type->currentIndex());
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,&settings);
        form->addRow(buttons);
        connect(buttons,&QDialogButtonBox::accepted,&settings,&QDialog::accept);
        connect(buttons,&QDialogButtonBox::rejected,&settings,&QDialog::reject);
        if (settings.exec() != QDialog::Accepted)
            return {};
        return wf::MeshModifier{wf::ModifierKind(type->currentIndex()),modifier.enabled,unsigned(axis->currentIndex()),
            unsigned(count->value()),float(amount->value()),std::size_t(face->value()-1),
            target->currentData().toString().toStdString(),wf::MirrorSide(mirrorSide->currentIndex()),
            wf::BooleanOperation(booleanOperation->currentIndex()),
            {float(targetOffset[0]->value()),float(targetOffset[1]->value()),float(targetOffset[2]->value())}};
    };
    const auto update = [&](std::vector<wf::MeshModifier> stack,int row) {
        QString error;
        if (store_.setMeshModifiers(std::move(stack),error))
            message->clear();
        else
            message->setText(error);
        refresh(row);
    };
    auto *actions = new QHBoxLayout;
    layout->addLayout(actions);
    const auto button = [&](const QString &text,auto action) {
        auto *item = new QPushButton(text,&dialog);
        actions->addWidget(item);
        connect(item,&QPushButton::clicked,&dialog,action);
    };
    button(tr("Add"),[&] {
        auto stack = store_.selectedMesh()->modifiers;
        if (const auto item = editSettings({},true)) {
            stack.push_back(*item);
            update(std::move(stack),list->count());
        }
    });
    button(tr("Edit"),[&] {
        const int row = list->currentRow();
        if (row < 0)
            return;
        auto stack = store_.selectedMesh()->modifiers;
        if (const auto item = editSettings(stack[row],false)) {
            stack[row] = *item;
            update(std::move(stack),row);
        }
    });
    for (int direction : {-1,1})
        button(direction < 0 ? tr("Up") : tr("Down"),[&,direction] {
            const int row = list->currentRow(), destination = row+direction;
            if (row < 0 || destination < 0 || destination >= list->count())
                return;
            auto stack = store_.selectedMesh()->modifiers;
            std::swap(stack[row],stack[destination]);
            update(std::move(stack),destination);
        });
    button(tr("Copy"),[&] {
        const int row = list->currentRow();
        if (row < 0)
            return;
        auto stack = store_.selectedMesh()->modifiers;
        const auto copy = stack[row];
        stack.insert(stack.begin()+row+1,copy);
        update(std::move(stack),row+1);
    });
    button(tr("Remove"),[&] {
        const int row = list->currentRow();
        if (row < 0)
            return;
        auto stack = store_.selectedMesh()->modifiers;
        stack.erase(stack.begin()+row);
        update(std::move(stack),row);
    });
    button(tr("Apply Through Selected"),[&] {
        const int row = list->currentRow();
        if (row < 0)
            return;
        QString error;
        if (store_.applyMeshModifiers(std::size_t(row),error)) {
            refresh(0);
            message->setText(tr("Applied modifiers through the selected row."));
        } else {
            message->setText(error);
        }
    });
    connect(list,&QListWidget::itemChanged,&dialog,[&](QListWidgetItem *item) {
        const int row = list->row(item);
        const auto *asset = store_.selectedMesh();
        if (!asset || row < 0 || std::size_t(row) >= asset->modifiers.size())
            return;
        const bool wasEnabled = asset->modifiers[row].enabled;
        const bool enabled = item->checkState() == Qt::Checked;
        if (wasEnabled == enabled)
            return;
        auto stack = asset->modifiers;
        stack[row].enabled = enabled;
        QString error;
        if (store_.setMeshModifiers(std::move(stack),error)) {
            message->clear();
        } else {
            const QSignalBlocker blocked(list);
            item->setCheckState(wasEnabled ? Qt::Checked : Qt::Unchecked);
            message->setText(error);
        }
    });
    auto *close = new QDialogButtonBox(QDialogButtonBox::Close,&dialog);
    connect(close,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);
    layout->addWidget(close);
    refresh(0);
    dialog.exec();
}

void ModelingWorkspace::moveUvIsland(wf::Point2 delta)
{
    const auto *asset = store_.selectedMesh();
    if (!asset || domain_ != ModelDomain::Face || !active_ || transform_ || knife_ || retopology_)
        return;
    const auto charts = wf::uvIslands(*asset->mesh,asset->uvSeams,true);
    const auto face = std::size_t(active_->index);
    const auto chart = std::ranges::find_if(charts,[face](const auto &faces) {
        return std::ranges::find(faces,face) != faces.end();
    });
    if (chart == charts.end())
        return;
    auto mesh = *asset->mesh;
    for (const auto index : *chart)
        for (auto &corner : mesh.faces[index].corners) {
            corner.uv[0] += delta[0];
            corner.uv[1] += delta[1];
        }
    QString error;
    message_->setText(store_.updateMesh(asset->id,std::move(mesh),tr("Move UV Island"),error,asset->mesh.data())
        ? tr("UV island moved.") : error);
}

void ModelingWorkspace::transformUvIsland()
{
    const auto *asset = store_.selectedMesh();
    if (!asset || domain_ != ModelDomain::Face || !active_ || transform_ || knife_ || retopology_) {
        message_->setText(tr("Select a face in the UV island first."));
        return;
    }
    const auto id = asset->id;
    const auto revision = asset->mesh;
    const auto charts = wf::uvIslands(*revision,asset->uvSeams,true);
    const auto activeFace = std::size_t(active_->index);
    const auto chart = std::ranges::find_if(charts,[activeFace](const auto &faces) {
        return std::ranges::find(faces,activeFace) != faces.end();
    });
    if (chart == charts.end())
        return;
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Transform UV Island"));
    auto *form = new QFormLayout(&dialog);
    const auto field = [&](const QString &label,double low,double high,double initial) {
        auto *value = new QDoubleSpinBox(&dialog);
        value->setRange(low,high);
        value->setDecimals(4);
        value->setValue(initial);
        value->setAccessibleName(label);
        form->addRow(label,value);
        return value;
    };
    auto *degrees = field(tr("Rotation (degrees)"),-360,360,0);
    auto *scale = field(tr("Scale"),0.001,100,1);
    auto *u = field(tr("Offset U"),-100,100,0), *v = field(tr("Offset V"),-100,100,0);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,&dialog);
    form->addRow(buttons);
    connect(buttons,&QDialogButtonBox::accepted,&dialog,&QDialog::accept);
    connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);
    if (dialog.exec() != QDialog::Accepted)
        return;
    QPointF low(std::numeric_limits<float>::max(),std::numeric_limits<float>::max()), high(-low);
    for (auto index : *chart)
        for (const auto &corner : revision->faces[index].corners) {
            low.setX(std::min(low.x(),qreal(corner.uv[0])));
            low.setY(std::min(low.y(),qreal(corner.uv[1])));
            high.setX(std::max(high.x(),qreal(corner.uv[0])));
            high.setY(std::max(high.y(),qreal(corner.uv[1])));
        }
    const auto center = (low+high)*0.5;
    const double angle = degrees->value()*std::numbers::pi/180.0;
    const double a = std::cos(angle)*scale->value(), b = std::sin(angle)*scale->value();
    auto mesh = *revision;
    for (auto index : *chart)
        for (auto &corner : mesh.faces[index].corners) {
            const auto p = QPointF(corner.uv[0],corner.uv[1])-center;
            corner.uv = {float(center.x()+a*p.x()-b*p.y()+u->value()),float(center.y()+b*p.x()+a*p.y()+v->value())};
        }
    QString error;
    message_->setText(store_.updateMesh(id,std::move(mesh),tr("Transform UV Island"),error,revision.data())
        ? tr("UV island transformed.") : error);
}

void ModelingWorkspace::editUvs(UvCommand command)
{
    if (!commitPendingTransform())
        return;
    const auto *asset = store_.selectedMesh();
    if (!asset || transform_ || knife_ || retopology_)
        return;
    const auto id = asset->id;
    const auto revision = asset->mesh;
    auto mesh = *revision;
    auto seams = asset->uvSeams;
    const auto pins = asset->uvPins;
    if (command == UvCommand::Inspect) {
        const auto report = wf::inspectUvs(mesh,seams);
        message_->setText(tr("%1 islands · %2 corners outside 0–1 · %3 degenerate triangles · %4 overlapping triangle pairs")
            .arg(report.islands).arg(report.outside).arg(report.degenerate).arg(report.overlaps));
        return;
    }
    QString label;
    std::string error;
    switch (command) {
    case UvCommand::Mark:
    case UvCommand::Clear:
    case UvCommand::Stitch: {
        if (domain_ != ModelDomain::Edge || selected_.empty()) {
            message_->setText(tr("Select mesh edges first."));
            return;
        }
        std::vector<wf::MeshEdge> edges;
        for (const auto item : selected_)
            edges.emplace_back(wf::VertexIndex(item.index),wf::VertexIndex(item.end));
        if (command == UvCommand::Mark) {
            label = tr("Mark Seams");
            seams.insert(edges.begin(),edges.end());
        } else {
            label = command == UvCommand::Clear ? tr("Clear Seams") : tr("Stitch UV Edges");
            for (const auto &edge : edges)
                seams.erase(edge);
            if (command == UvCommand::Stitch)
                error = wf::stitchUvEdges(mesh,edges,seams);
        }
        break;
    }
    case UvCommand::Sharp: {
        label = tr("Seams from Sharp Edges");
        const auto sharp = wf::sharpUvSeams(mesh);
        seams.insert(sharp.begin(),sharp.end());
        break;
    }
    case UvCommand::Unwrap:
    case UvCommand::Pack:
    case UvCommand::Density: {
        label = command == UvCommand::Unwrap ? tr("Unwrap Islands")
            : command == UvCommand::Pack ? tr("Pack Islands") : tr("Normalize Texel Density");
        bool accepted = false;
        const auto padding = QInputDialog::getDouble(this,label,tr("Padding in UV units"),0.02,0,0.249,4,&accepted);
        if (!accepted)
            return;
        error = command == UvCommand::Unwrap ? wf::unwrapUvIslands(mesh,seams,float(padding),pins)
            : command == UvCommand::Pack ? wf::packUvIslands(mesh,seams,float(padding),pins)
            : wf::normalizeUvDensity(mesh,seams,float(padding),pins);
        break;
    }
    case UvCommand::Planar:
    case UvCommand::Box:
    case UvCommand::Cylinder: {
        const auto projection = command == UvCommand::Planar ? wf::UvProjection::Planar
            : command == UvCommand::Box ? wf::UvProjection::Box : wf::UvProjection::Cylinder;
        label = command == UvCommand::Planar ? tr("Planar Projection")
            : command == UvCommand::Box ? tr("Box Projection") : tr("Cylinder Projection");
        std::vector<std::size_t> faces;
        if (domain_ == ModelDomain::Object) {
            for (std::size_t i = 0; i < mesh.faces.size(); ++i)
                faces.push_back(i);
        } else if (domain_ == ModelDomain::Face) {
            for (const auto item : selected_)
                faces.push_back(std::size_t(item.index));
        }
        error = wf::projectUvFaces(mesh,faces,projection);
        break;
    }
    case UvCommand::Inspect:
        return;
    }
    QString result = QString::fromStdString(error);
    if (result.isEmpty() && store_.updateMesh(id,std::move(mesh),label,result,revision.data(),std::move(seams)))
        message_->setText(tr("%1 applied.").arg(label));
    else
        message_->setText(result);
}

void ModelingWorkspace::editTopology(TopologyCommand command,const QString &label)
{
    setPairTool(PairTool::None);
    if (!commitPendingTransform())
        return;
    const auto *asset = store_.selectedMesh();
    if (!asset || selected_.empty() || transform_ || knife_ || retopology_)
        return;
    const auto id = asset->id;
    const auto revision = asset->mesh;
    std::vector<wf::MeshEdge> edges;
    std::vector<wf::VertexIndex> vertices;
    for (const auto component : selected_) {
        if (domain_ == ModelDomain::Edge)
            edges.emplace_back(wf::VertexIndex(component.index),wf::VertexIndex(component.end));
        else if (domain_ == ModelDomain::Vertex)
            vertices.push_back(wf::VertexIndex(component.index));
    }
    const auto number = [&](const QString &caption,double value,double min,double max,int decimals) {
        bool accepted = false;
        const auto entered = QInputDialog::getDouble(this,label,caption,value,min,max,decimals,&accepted);
        return accepted ? std::optional{float(entered)} : std::nullopt;
    };
    auto mesh = *revision;
    std::string error;
    switch (command) {
    case TopologyCommand::WeldDistance:
        if (const auto distance = number(tr("Distance (m)"),0.001,0.00001,100,5))
            error = wf::weldVertices(mesh,vertices,*distance);
        else
            return;
        break;
    case TopologyCommand::WeldActive:
        if (!active_ || domain_ != ModelDomain::Vertex)
            return;
        error = wf::weldVertices(mesh,vertices,0,wf::VertexIndex(active_->index));
        break;
    case TopologyCommand::Fill:
        error = wf::fillBoundary(mesh,edges);
        break;
    case TopologyCommand::GridFill: {
        bool accepted = false;
        const auto span = QInputDialog::getInt(this,label,tr("Columns"),2,1,128,1,&accepted);
        if (!accepted)
            return;
        error = wf::gridFillBoundary(mesh,edges,unsigned(span));
        break;
    }
    case TopologyCommand::Bridge:
        error = wf::bridgeBoundaries(mesh,edges);
        break;
    case TopologyCommand::Dissolve:
        error = wf::dissolveEdges(mesh,edges);
        break;
    case TopologyCommand::LimitedDissolve:
        if (const auto angle = number(tr("Maximum angle (degrees)"),5,0,180,1))
            error = wf::dissolveEdges(mesh,edges,*angle);
        else
            return;
        break;
    case TopologyCommand::Bevel: {
        QDialog dialog(this);
        dialog.setWindowTitle(tr("Bevel Edges"));
        auto *form = new QFormLayout(&dialog);
        auto *width = new QDoubleSpinBox(&dialog);
        width->setDecimals(4);
        width->setRange(0.0001,100);
        width->setValue(0.1);
        width->setSuffix(tr(" m"));
        auto *segments = new QSpinBox(&dialog);
        segments->setRange(1,16);
        form->addRow(tr("Width"),width);
        form->addRow(tr("Segments"),segments);
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,&dialog);
        form->addRow(buttons);
        connect(buttons,&QDialogButtonBox::accepted,&dialog,&QDialog::accept);
        connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);
        if (dialog.exec() != QDialog::Accepted)
            return;
        error = wf::bevelEdges(mesh,edges,float(width->value()),unsigned(segments->value()));
        break;
    }
    case TopologyCommand::Chamfer:
        if (const auto width = number(tr("Width (m)"),0.1,0.0001,100,4))
            error = wf::bevelVertices(mesh,vertices,*width);
        else
            return;
        break;
    case TopologyCommand::SlideEdge:
        if (edges.size() != 1) {
            message_->setText(tr("Select one edge to slide."));
            return;
        }
        if (const auto slide = number(tr("Slide toward the first or second face (%)"),25,-99,99,1))
            error = wf::slideEdge(mesh,edges.front(),*slide/100);
        else
            return;
        break;
    case TopologyCommand::SlideVertex:
        if (vertices.size() != 2 || !active_) {
            message_->setText(tr("Select two connected vertices. The active vertex slides toward the other."));
            return;
        }
        if (const auto slide = number(tr("Slide toward the other vertex (%)"),25,0.1,99,1)) {
            const auto from = wf::VertexIndex(active_->index);
            error = wf::slideVertex(mesh,from,vertices.front() == from ? vertices.back() : vertices.front(),*slide/100);
        } else {
            return;
        }
        break;
    case TopologyCommand::RotateEdge:
        if (edges.size() != 1) {
            message_->setText(tr("Select one edge shared by two triangles."));
            return;
        }
        error = wf::rotateEdge(mesh,edges.front());
        break;
    case TopologyCommand::Delete: {
        std::set<std::size_t> faces;
        if (domain_ == ModelDomain::Face) {
            for (const auto component : selected_)
                faces.insert(std::size_t(component.index));
        } else {
            const wf::MeshTopology topology(mesh);
            for (auto vertex : vertices)
                edges.insert(edges.end(),topology.vertexEdges[vertex].begin(),topology.vertexEdges[vertex].end());
            for (const auto &edge : edges) {
                const auto &attached = topology.edgeFaces.at(edge);
                faces.insert(attached.begin(),attached.end());
            }
        }
        error = wf::deleteFaces(mesh,std::vector<std::size_t>(faces.begin(),faces.end()));
        break;
    }
    }
    QString result = QString::fromStdString(error);
    if (!result.isEmpty() || !store_.updateMesh(id,std::move(mesh),label,result,revision.data())) {
        message_->setText(result);
        return;
    }
    if (command != TopologyCommand::SlideVertex && command != TopologyCommand::SlideEdge) {
        selected_.clear();
        active_.reset();
    }
    refreshSelection();
    message_->setText(tr("%1 applied.").arg(label));
}
