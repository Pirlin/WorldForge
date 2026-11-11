#include "Workspaces/ModelingWorkspace.h"
#include "Components/LiveViewport.h"
#include "DesignSystem/Icons.h"
#include "State/ProjectStore.h"

#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QSplitter>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>

namespace wf = worldforge;

class UvCanvas : public QWidget {
public:
    explicit UvCanvas(QWidget *parent) : QWidget(parent)
    {
        setMinimumSize(160, 160);
        setAccessibleName(tr("UV canvas"));
        setToolTip(tr("Click a face to select it. Drag the selected UV polygon to move it."));
    }
    void setMesh(QSharedPointer<const wf::Mesh> mesh, int selected)
    {
        mesh_ = std::move(mesh);
        selected_ = selected;
        update();
    }
    std::function<void(int)> select;
    std::function<void(wf::Point2)> move;

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.fillRect(rect(), QColor("#141822"));
        const auto bounds = tile();
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
        for (std::size_t i = 0; i < mesh_->faces.size(); ++i)
            if (int(i) != selected_)
                painter.drawPolygon(polygon(i));
        if (selected_ >= 0 && std::size_t(selected_) < mesh_->faces.size()) {
            painter.setPen(QPen(QColor("#cba6ff"), 2));
            painter.setBrush(QColor(142, 78, 225, 90));
            painter.drawPolygon(polygon(selected_));
        }
    }
    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() != Qt::LeftButton || !mesh_)
            return;
        start_ = event->position();
        if (selected_ >= 0 && polygon(selected_).containsPoint(start_, Qt::OddEvenFill)) {
            dragging_ = true;
            return;
        }
        for (std::size_t i = mesh_->faces.size(); i > 0; --i) {
            if (polygon(i-1).containsPoint(start_, Qt::OddEvenFill)) {
                select(int(i-1));
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
            polygon << QPointF(bounds.left() + corner.uv[0]*bounds.width(),
                               bounds.bottom() - corner.uv[1]*bounds.height());
        return polygon;
    }
    QSharedPointer<const wf::Mesh> mesh_;
    int selected_ = -1;
    bool dragging_ = false;
    QPointF start_;
};

ModelingWorkspace::ModelingWorkspace(ProjectStore &store, QWidget *parent)
    : QWidget(parent), store_(store), viewport_(new LiveViewport(store, true, this)), uv_(new UvCanvas(this)),
      outliner_(new QListWidget(this)), summary_(new QLabel(this)), message_(new QLabel(this)),
      amount_(new QDoubleSpinBox(this)), edge_(new QSpinBox(this))
{
    setObjectName("modelingWorkspace");
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(3);
    auto *header = new QHBoxLayout;
    header->setContentsMargins(5, 3, 5, 0);
    layout->addLayout(header);
    const auto tool = [this, header](const QString &name, const QString &icon = {}) {
        auto *button = new QToolButton(this);
        button->setText(name);
        if (!icon.isEmpty())
            button->setIcon(editorIcon(icon));
        button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        header->addWidget(button);
        return button;
    };
    connect(tool(tr("Add Cube"), "plus"), &QToolButton::clicked, &store_, &ProjectStore::addCube);
    connect(tool(tr("Import GLB"), "import"), &QToolButton::clicked, this, [this] {
        const auto path = QFileDialog::getOpenFileName(this, tr("Import GLB"), {}, tr("Binary glTF (*.glb)"));
        if (!path.isEmpty()) {
            QString error;
            message_->setText(store_.importMesh(path, error) ? tr("Mesh imported.") : error);
        }
    });
    connect(tool(tr("Add Instance"), "box"), &QToolButton::clicked, &store_, &ProjectStore::addMeshInstance);
    connect(tool(tr("Frame"), "focus"), &QToolButton::clicked, viewport_, &LiveViewport::frame);
    auto *uv = tool(tr("UV Editing"), "grid");
    uv->setCheckable(true);
    connect(uv, &QToolButton::toggled, uv_, &QWidget::setVisible);
    header->addStretch(1);

    auto *columns = new QSplitter(Qt::Horizontal, this);
    columns->setChildrenCollapsible(false);
    auto *canvas = new QSplitter(Qt::Horizontal, columns);
    canvas->setChildrenCollapsible(false);
    canvas->addWidget(uv_);
    canvas->addWidget(viewport_);
    canvas->setStretchFactor(0, 1);
    canvas->setStretchFactor(1, 1);
    uv_->hide();
    columns->addWidget(canvas);
    auto *right = new QWidget(columns);
    auto *rightLayout = new QVBoxLayout(right);
    rightLayout->setContentsMargins(5, 4, 6, 4);
    rightLayout->addWidget(new QLabel(tr("Model Outliner"), right));
    outliner_->setAccessibleName(tr("Model Outliner"));
    outliner_->setObjectName("meshOutliner");
    outliner_->setMaximumHeight(240);
    rightLayout->addWidget(outliner_, 1);
    rightLayout->addWidget(new QLabel(tr("Modeling Toolkit"), right));
    summary_->setWordWrap(true);
    rightLayout->addWidget(summary_);

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
    rightLayout->addLayout(form);

    const auto command = [this, right, rightLayout](const QString &name, const QString &id, auto operation) {
        auto *button = new QPushButton(name, right);
        button->setObjectName(id);
        connect(button, &QPushButton::clicked, this, [this, name, operation] { edit(name, operation); });
        rightLayout->addWidget(button);
    };
    command(tr("Extrude Face"), "meshExtrude", [this](wf::Mesh &mesh, std::size_t face) {
        return wf::extrudeFace(mesh, face, float(amount_->value()));
    });
    command(tr("Bevel Perimeter"), "meshBevel", [this](wf::Mesh &mesh, std::size_t face) {
        return wf::bevelFace(mesh, face, float(amount_->value()));
    });
    command(tr("Loop Cut"), "meshLoopCut", [this](wf::Mesh &mesh, std::size_t face) {
        return wf::cutQuadLoop(mesh, face, std::size_t(edge_->value()-1));
    });
    command(tr("Mirror X"), "meshMirror", [](wf::Mesh &mesh, std::size_t) { return wf::mirrorMeshX(mesh); });
    command(tr("Unwrap Selected Face"), "meshUnwrap", wf::unwrapFace);
    auto *reimport = new QPushButton(tr("Reimport Source"), right);
    rightLayout->addWidget(reimport);
    connect(reimport, &QPushButton::clicked, this, [this] {
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
    message_->setWordWrap(true);
    message_->setAccessibleName(tr("Modeling result"));
    rightLayout->addWidget(message_);
    rightLayout->addStretch(1);
    right->setMinimumWidth(212);
    columns->addWidget(right);
    columns->setStretchFactor(0, 1);
    columns->setStretchFactor(1, 0);
    columns->setSizes({1100, 280});
    layout->addWidget(columns, 1);

    connect(outliner_, &QListWidget::currentItemChanged, this, [this](QListWidgetItem *item) {
        store_.select(item ? item->data(Qt::UserRole).toString() : QString());
    });
    connect(viewport_, &LiveViewport::facePicked, this, &ModelingWorkspace::selectFace);
    connect(&store_, &ProjectStore::sceneChanged, this, &ModelingWorkspace::refresh);
    connect(&store_, &ProjectStore::selectionChanged, this, &ModelingWorkspace::refresh);
    uv_->select = [this](int face) { selectFace(face); };
    uv_->move = [this](wf::Point2 delta) {
        edit(tr("Move UVs"), [delta](wf::Mesh &mesh, std::size_t face) { return wf::translateFaceUV(mesh, face, delta); });
    };
    refresh();
}

void ModelingWorkspace::refresh()
{
    const auto selection = store_.selection();
    if (object_ != selection) {
        object_ = selection;
        face_ = -1;
        message_->clear();
    }
    const QSignalBlocker blocked(outliner_);
    outliner_->clear();
    if (const auto *project = store_.project()) {
        for (const auto &object : project->objects) {
            if (object.meshAsset.isEmpty())
                continue;
            auto *item = new QListWidgetItem(editorIcon("box"), object.name, outliner_);
            item->setData(Qt::UserRole, object.id);
            if (object.id == selection)
                outliner_->setCurrentItem(item);
        }
    }
    const auto *asset = store_.selectedMesh();
    if (!asset || (face_ >= 0 && std::size_t(face_) >= asset->mesh->faces.size()))
        face_ = -1;
    selectFace(face_);
}

void ModelingWorkspace::selectFace(int face)
{
    const auto *asset = store_.selectedMesh();
    face_ = asset && face >= 0 && std::size_t(face) < asset->mesh->faces.size() ? face : -1;
    viewport_->setFace(face_);
    uv_->setMesh(asset ? asset->mesh : QSharedPointer<const wf::Mesh>{}, face_);
    summary_->setText(asset ? tr("%1 · %2 vertices · %3 faces\n%4")
        .arg(asset->name).arg(asset->mesh->vertices.size()).arg(asset->mesh->faces.size())
        .arg(face_ >= 0 ? tr("Face %1 selected").arg(face_+1) : tr("Click a face in the viewport."))
        : tr("Add a cube or import a GLB to start modeling."));
}

void ModelingWorkspace::edit(const QString &label,
    const std::function<std::string(wf::Mesh &, std::size_t)> &operation)
{
    const auto *asset = store_.selectedMesh();
    if (!asset) {
        message_->setText(tr("Select a mesh in the Model Outliner."));
        return;
    }
    auto mesh = *asset->mesh;
    auto error = QString::fromStdString(operation(mesh, std::size_t(face_)));
    if (error.isEmpty() && store_.updateMesh(asset->id, std::move(mesh), label, error)) {
        message_->setText(tr("%1 applied.").arg(label));
        return;
    }
    message_->setText(error);
}
