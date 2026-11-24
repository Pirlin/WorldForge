#include "Workspaces/SurfaceWorkspace.h"
#include "Components/MaterialLayerControls.h"

#include "Components/LiveViewport.h"
#include "State/ProjectStore.h"
#include <QBuffer>
#include <QButtonGroup>
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFile>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QImageReader>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPushButton>
#include <QSignalBlocker>
#include <QScrollArea>
#include <QSaveFile>
#include <QSplitter>
#include <QSpinBox>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QMouseEvent>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>
#include <functional>
#include <utility>

namespace wf = worldforge;

namespace {
constexpr int blankMapSize = 1024;
constexpr int maxPaintPixels = 4096 * 4096;

void drawUvLayout(QPainter &painter,const wf::Mesh &mesh,int slot,const QRectF &tile)
{
    painter.setClipRect(tile);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setBrush(Qt::NoBrush);
    for (const auto &face : mesh.faces) {
        if (slot >= 0 && face.material != std::uint32_t(slot))
            continue;
        QPolygonF polygon;
        for (const auto &corner : face.corners)
            polygon.append({tile.left()+corner.uv[0]*tile.width(),tile.bottom()-corner.uv[1]*tile.height()});
        painter.setPen(QPen(QColor("#27183a"),3));
        painter.drawPolygon(polygon);
        painter.setPen(QPen(QColor("#d6b4ff"),1));
        painter.drawPolygon(polygon);
    }
}

QImage decode(QImageReader &reader,int limit,QSize *original = nullptr)
{
    const auto size = reader.size();
    const auto format = reader.format();
    if (original)
        *original = size;
    if ((format != "png" && format != "jpeg") || !size.isValid() || size.width() > 8192
        || size.height() > 8192 || qint64(size.width())*size.height() > 32*1024*1024)
        return {};
    if (size.width() > limit || size.height() > limit)
        reader.setScaledSize(size.scaled(limit,limit,Qt::KeepAspectRatio).expandedTo({1,1}));
    return reader.read();
}
}

class TextureUvView : public QWidget {
public:
    explicit TextureUvView(QWidget *parent = nullptr) : QWidget(parent)
    {
        setMinimumSize(220,180);
        setAccessibleName(tr("Texture and UV preview"));
        setToolTip(tr("Wheel to zoom. Middle-drag to pan."));
        setFocusPolicy(Qt::ClickFocus);
    }

    void fit()
    {
        zoom_ = 1;
        pan_ = {};
        update();
    }

    void setAllSlots(bool enabled)
    {
        allSlots_ = enabled;
        update();
    }

    void setContent(QImage image,QSharedPointer<const wf::Mesh> mesh,int slot,bool overlay)
    {
        image_ = std::move(image);
        mesh_ = std::move(mesh);
        slot_ = slot;
        overlay_ = overlay;
        update();
    }

    std::function<bool()> paintStarted;
    std::function<void()> paintChanged;
    std::function<void(bool)> paintFinished;

    void setPaintEnabled(bool enabled)
    {
        paintEnabled_ = enabled;
        if (enabled)
            setCursor(Qt::CrossCursor);
        else
            unsetCursor();
    }

    bool startStroke(QImage base,QColor color,int width,double opacity)
    {
        paintBase_ = std::move(base);
        brushColor_ = color;
        brushWidth_ = width;
        opacity_ = opacity;
        paintLayer_ = QImage(paintBase_.size(),QImage::Format_ARGB32_Premultiplied);
        if (paintLayer_.isNull()) {
            paintBase_ = {};
            return false;
        }
        paintLayer_.fill(Qt::transparent);
        return true;
    }

    QImage paintedImage(int previewLimit = 0) const
    {
        if (paintLayer_.isNull())
            return {};
        const auto dimensions = paintBase_.size();
        const bool reduce = previewLimit && (dimensions.width() > previewLimit || dimensions.height() > previewLimit);
        auto result = reduce ? paintBase_.scaled(dimensions.scaled(previewLimit,previewLimit,Qt::KeepAspectRatio)
            .expandedTo({1,1}),Qt::IgnoreAspectRatio,Qt::SmoothTransformation) : paintBase_;
        QPainter painter(&result);
        painter.setRenderHint(QPainter::SmoothPixmapTransform,reduce);
        painter.setOpacity(opacity_);
        painter.drawImage(result.rect(),paintLayer_);
        return result;
    }

    void clearStroke()
    {
        paintBase_ = {};
        paintLayer_ = {};
        update();
    }

protected:
    bool event(QEvent *event) override
    {
        if (!paintLayer_.isNull() && event->type() == QEvent::ShortcutOverride
            && static_cast<QKeyEvent *>(event)->key() == Qt::Key_Escape) {
            event->accept();
            return true;
        }
        return QWidget::event(event);
    }

    void keyPressEvent(QKeyEvent *event) override
    {
        if (!paintLayer_.isNull() && event->key() == Qt::Key_Escape) {
            paintFinished(false);
            event->accept();
            return;
        }
        QWidget::keyPressEvent(event);
    }

    void focusOutEvent(QFocusEvent *event) override
    {
        if (!paintLayer_.isNull())
            paintFinished(false);
        QWidget::focusOutEvent(event);
    }

    void wheelEvent(QWheelEvent *event) override
    {
        if (!paintLayer_.isNull()) {
            event->accept();
            return;
        }
        const auto next = std::clamp(zoom_*std::exp(event->angleDelta().y()/650.0),0.25,32.0);
        const auto anchor = event->position()-QRectF(rect()).center();
        pan_ = anchor+(pan_-anchor)*(next/zoom_);
        zoom_ = next;
        update();
        event->accept();
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton && paintEnabled_ && tileRect().contains(event->position())) {
            if (paintStarted()) {
                lastPaint_ = imagePoint(event->position());
                drawPaint(lastPaint_);
            }
            event->accept();
        } else if (event->button() == Qt::MiddleButton && paintLayer_.isNull()) {
            last_ = event->position();
            setCursor(Qt::ClosedHandCursor);
            event->accept();
        }
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (!paintLayer_.isNull()) {
            if (event->buttons().testFlag(Qt::LeftButton)) {
                const auto point = imagePoint(event->position());
                if (point != lastPaint_)
                    drawPaint(point);
            } else {
                paintFinished(false);
            }
        } else if (event->buttons().testFlag(Qt::MiddleButton)) {
            pan_ += event->position()-last_;
            last_ = event->position();
            update();
        }
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton && !paintLayer_.isNull()) {
            const auto point = imagePoint(event->position());
            if (point != lastPaint_)
                drawPaint(point);
            paintFinished(true);
            event->accept();
        } else if (event->button() == Qt::MiddleButton) {
            setPaintEnabled(paintEnabled_);
        }
    }

    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.fillRect(rect(),QColor("#11141d"));
        const auto tile = tileRect();
        for (int y = 0; y < 10; ++y)
            for (int x = 0; x < 10; ++x)
                painter.fillRect(QRectF(tile.left()+x*tile.width()/10,tile.top()+y*tile.height()/10,
                    tile.width()/10,tile.height()/10),
                    QColor((x+y)%2 ? "#292f40" : "#222737"));
        const auto &base = paintBase_.isNull() ? image_ : paintBase_;
        if (!base.isNull()) {
            painter.setRenderHint(QPainter::SmoothPixmapTransform);
            painter.drawImage(tile,base);
        }
        if (!paintLayer_.isNull()) {
            painter.setOpacity(opacity_);
            painter.drawImage(tile,paintLayer_);
            painter.setOpacity(1);
        }
        painter.setPen(QColor("#66718a"));
        painter.drawRect(tile);
        if (mesh_ && overlay_)
            drawUvLayout(painter,*mesh_,allSlots_ ? -1 : slot_,tile);
    }

private:
    QRectF tileRect() const
    {
        const auto imageSize = paintBase_.isNull() ? image_.size() : paintBase_.size();
        const auto fitted = (imageSize.isEmpty() ? QSize(1,1) : imageSize)
            .scaled(size()-QSize(36,36),Qt::KeepAspectRatio);
        QRectF tile(QPointF{},QSizeF(fitted)*zoom_);
        tile.moveCenter(QRectF(rect()).center()+pan_);
        return tile;
    }

    QPointF imagePoint(QPointF point) const
    {
        const auto tile = tileRect();
        return {(point.x()-tile.left())/tile.width()*paintBase_.width(),
            (point.y()-tile.top())/tile.height()*paintBase_.height()};
    }

    void drawPaint(QPointF point)
    {
        QPainter painter(&paintLayer_);
        painter.setRenderHint(QPainter::Antialiasing);
        if (point == lastPaint_) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(brushColor_);
            painter.drawEllipse(point,brushWidth_/2.0,brushWidth_/2.0);
        } else {
            painter.setPen(QPen(brushColor_,brushWidth_,Qt::SolidLine,Qt::RoundCap));
            painter.drawLine(lastPaint_,point);
        }
        lastPaint_ = point;
        update();
        paintChanged();
    }

    QImage image_;
    QImage paintBase_, paintLayer_;
    QColor brushColor_;
    QPointF lastPaint_;
    int brushWidth_ = 0;
    double opacity_ = 1;
    bool paintEnabled_ = false;
    QSharedPointer<const wf::Mesh> mesh_;
    int slot_ = -1;
    bool overlay_ = true;
    bool allSlots_ = false;
    double zoom_ = 1;
    QPointF pan_, last_;
};

SurfaceWorkspace::SurfaceWorkspace(ProjectStore &store,bool textureWorkspace,QWidget *parent)
    : QWidget(parent),store_(store)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(5,4,5,4);
    auto *toolbar = new QHBoxLayout;
    object_ = new QComboBox(this);
    object_->setAccessibleName(tr("Surface mesh object"));
    slot_ = new QComboBox(this);
    slot_->setAccessibleName(tr("Surface material slot"));
    uvSet_ = new QComboBox(this);
    uvSet_->setAccessibleName(tr("Surface UV set"));
    uvSet_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    uvSet_->setMinimumContentsLength(10);
    toolbar->addWidget(new QLabel(tr("Mesh"),this));
    toolbar->addWidget(object_,1);
    toolbar->addWidget(new QLabel(tr("Material Slot"),this));
    toolbar->addWidget(slot_,1);
    toolbar->addWidget(new QLabel(tr("UV Set"),this));
    toolbar->addWidget(uvSet_,1);
    auto *frame = new QPushButton(tr("Frame Mesh"),this);
    toolbar->addWidget(frame);
    layout->addLayout(toolbar);

    split_ = new QSplitter(Qt::Horizontal,this);
    split_->setChildrenCollapsible(false);
    auto *maps = new QWidget(split_);
    auto *mapLayout = new QVBoxLayout(maps);
    mapLayout->setContentsMargins(4,4,4,4);
    mapLayout->addWidget(new QLabel(tr("Texture Maps"),maps));
    auto *mapScroll = new QScrollArea(maps);
    mapScroll->setWidgetResizable(true);
    auto *mapList = new QWidget(mapScroll);
    auto *mapButtons = new QVBoxLayout(mapList);
    mapButtons->setContentsMargins(0,0,0,0);
    mapScroll->setWidget(mapList);
    mapLayout->addWidget(mapScroll,1);
    auto *picker = new QButtonGroup(maps);
    const auto names = materialMapNames();
    for (int i = 0; i < int(channels_.size()); ++i) {
        auto *button = channels_[i] = new QToolButton(mapList);
        button->setText(names[i]);
        button->setAccessibleName(names[i]);
        button->setCheckable(true);
        button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        button->setIconSize({48,42});
        mapButtons->addWidget(button);
        picker->addButton(button,i);
    }
    mapButtons->addStretch(1);
    channels_[0]->setChecked(true);
    imageInfo_ = new QLabel(maps);
    imageInfo_->setWordWrap(true);
    imageInfo_->setAccessibleName(tr("Texture image information"));
    mapLayout->addWidget(imageInfo_);
    import_ = new QPushButton(tr("Import Map…"),maps);
    exportMap_ = new QPushButton(tr("Export Map…"),maps);
    remove_ = new QPushButton(tr("Remove Map"),maps);
    mapLayout->addWidget(import_);
    mapLayout->addWidget(exportMap_);
    mapLayout->addWidget(remove_);

    auto *views = new QSplitter(Qt::Horizontal,split_);
    views->setChildrenCollapsible(false);
    auto *uvPane = new QWidget(views);
    auto *uvLayout = new QVBoxLayout(uvPane);
    uvLayout->setContentsMargins(3,0,3,0);
    if (textureWorkspace) {
        auto *brush = new QHBoxLayout;
        paint_ = new QCheckBox(tr("Paint"),uvPane);
        paint_->setToolTip(tr("Paint on the Base Color image. Release to save the stroke or press Esc to cancel."));
        brushColor_ = new QPushButton(tr("Brush Color…"),uvPane);
        const auto updateSwatch = [this] {
            QPixmap swatch(20,14);
            swatch.fill(paintColor_);
            brushColor_->setIcon(QIcon(swatch));
        };
        updateSwatch();
        connect(brushColor_,&QPushButton::clicked,this,[this,updateSwatch] {
            const auto color = QColorDialog::getColor(paintColor_,this,tr("Brush Color"));
            if (color.isValid()) {
                paintColor_ = color;
                updateSwatch();
            }
        });
        brushSize_ = new QSpinBox(uvPane);
        brushSize_->setRange(1,512);
        brushSize_->setValue(32);
        brushSize_->setSuffix(tr(" px"));
        brushSize_->setAccessibleName(tr("Brush diameter"));
        brushSize_->setToolTip(tr("Brush diameter in texture pixels"));
        brushOpacity_ = new QSpinBox(uvPane);
        brushOpacity_->setRange(1,100);
        brushOpacity_->setValue(100);
        brushOpacity_->setSuffix("%");
        brushOpacity_->setAccessibleName(tr("Brush opacity"));
        brushOpacity_->setToolTip(tr("Brush opacity"));
        brush->addWidget(paint_);
        brush->addWidget(brushColor_);
        brush->addWidget(brushSize_);
        brush->addWidget(brushOpacity_);
        brush->addStretch();
        uvLayout->addLayout(brush);
    }
    showUvs_ = new QCheckBox(tr("Show UV Overlay"),uvPane);
    showUvs_->setChecked(true);
    allSlots_ = new QCheckBox(tr("All Slots"),uvPane);
    allSlots_->setToolTip(tr("Show UV outlines from every material slot."));
    auto *uvTools = new QHBoxLayout;
    auto *fitUv = new QPushButton(tr("Fit Image"),uvPane);
    uvTools->addWidget(showUvs_);
    uvTools->addWidget(allSlots_);
    uvTools->addStretch();
    uvTools->addWidget(fitUv);
    uvLayout->addLayout(uvTools);
    uv_ = new TextureUvView(uvPane);
    connect(allSlots_,&QCheckBox::toggled,uv_,&TextureUvView::setAllSlots);
    connect(fitUv,&QPushButton::clicked,uv_,&TextureUvView::fit);
    uvLayout->addWidget(uv_,1);
    auto *uvActions = new QHBoxLayout;
    uvActions->addStretch();
    inspectUvs_ = new QPushButton(tr("Inspect UVs"),uvPane);
    exportUvs_ = new QPushButton(tr("Export UV Layout…"),uvPane);
    connect(inspectUvs_,&QPushButton::clicked,this,&SurfaceWorkspace::inspectUvLayout);
    connect(exportUvs_,&QPushButton::clicked,this,&SurfaceWorkspace::exportUvLayout);
    uvActions->addWidget(inspectUvs_);
    uvActions->addWidget(exportUvs_);
    uvLayout->addLayout(uvActions);
    viewport_ = new LiveViewport(store,true,views);
    viewport_->setModelShading(ModelShading::Material);
    if (textureWorkspace) {
        paintPreviewTimer_ = new QTimer(this);
        paintPreviewTimer_->setSingleShot(true);
        paintPreviewTimer_->setInterval(65);
        connect(paintPreviewTimer_,&QTimer::timeout,this,[this] {
            if (paintTarget_)
                viewport_->setBaseColorPreview(paintTarget_->slot,uv_->paintedImage(512));
        });
        uv_->paintStarted = [this] { return beginPaint(); };
        uv_->paintChanged = [this] {
            if (!paintPreviewTimer_->isActive())
                paintPreviewTimer_->start();
        };
        uv_->paintFinished = [this](bool commit) { finishPaint(commit); };
        connect(paint_,&QCheckBox::toggled,this,[this](bool enabled) {
            if (!enabled)
                finishPaint(false);
            uv_->setPaintEnabled(enabled && paint_->isEnabled());
        });
    }
    views->setSizes(textureWorkspace ? QList<int>{580,500} : QList<int>{410,640});
    views->setStretchFactor(0,textureWorkspace ? 2 : 1);
    views->setStretchFactor(1,2);

    auto *scroll = new QScrollArea(split_);
    scroll->setWidgetResizable(true);
    auto *properties = new QWidget(scroll);
    auto *form = new QFormLayout(properties);
    form->addRow(new QLabel(tr("Material Properties"),properties));
    name_ = new QLineEdit(properties);
    name_->setAccessibleName(tr("Material name"));
    name_->setMaxLength(64);
    form->addRow(tr("Name"),name_);
    color_ = new QPushButton(tr("Edit Color…"),properties);
    form->addRow(tr("Base Color"),color_);
    metallic_ = new QDoubleSpinBox(properties);
    roughness_ = new QDoubleSpinBox(properties);
    emission_ = new QDoubleSpinBox(properties);
    clearCoat_ = new QDoubleSpinBox(properties);
    coatRoughness_ = new QDoubleSpinBox(properties);
    for (auto *field : {metallic_,roughness_,emission_,clearCoat_,coatRoughness_}) {
        field->setRange(0,1);
        field->setDecimals(3);
        field->setSingleStep(0.05);
    }
    metallic_->setAccessibleName(tr("Surface metallic"));
    roughness_->setAccessibleName(tr("Surface roughness"));
    emission_->setAccessibleName(tr("Surface emission"));
    clearCoat_->setAccessibleName(tr("Clear coat weight"));
    coatRoughness_->setAccessibleName(tr("Clear coat roughness"));
    form->addRow(tr("Metallic"),metallic_);
    form->addRow(tr("Roughness"),roughness_);
    form->addRow(tr("Clear coat weight"),clearCoat_);
    form->addRow(tr("Clear coat roughness"),coatRoughness_);
    layers_ = new MaterialLayerControls(properties);
    form->addRow(layers_);
    form->addRow(tr("Emission"),emission_);
    apply_ = new QPushButton(tr("Apply Material"),properties);
    link_ = new QPushButton(tr("Link Existing Material…"),properties);
    unlink_ = new QPushButton(tr("Make Material Independent"),properties);
    assign_ = new QPushButton(tr("Assign Slot to All Faces"),properties);
    sharedInfo_ = new QLabel(properties);
    sharedInfo_->setWordWrap(true);
    form->addRow(apply_);
    form->addRow(link_);
    form->addRow(unlink_);
    form->addRow(assign_);
    form->addRow(sharedInfo_);
    scroll->setWidget(properties);
    scroll->setMinimumWidth(220);
    split_->setStretchFactor(1,1);
    resetLayout();
    layout->addWidget(split_,1);
    message_ = new QLabel(this);
    message_->setAccessibleName(tr("Surface operation result"));
    message_->setWordWrap(true);
    layout->addWidget(message_);

    connect(object_,&QComboBox::currentIndexChanged,this,[this] { store_.select(object_->currentData().toString()); });
    connect(slot_,&QComboBox::currentIndexChanged,this,[this] { refreshSlot(); });
    connect(uvSet_,&QComboBox::activated,this,[this](int index) {
        const auto name = uvSet_->itemData(index).toString();
        const auto *asset = store_.selectedMesh();
        if (!asset || name == asset->activeUvSet)
            return;
        QString error;
        if (store_.selectMeshUvSet(name,false,false,error)) {
            message_->setText(tr("Active UV set: %1").arg(name));
        } else {
            refresh();
            message_->setText(error);
        }
    });
    connect(picker,&QButtonGroup::idClicked,this,[this](int id) { channel_ = id; refreshMaps(); });
    connect(showUvs_,&QCheckBox::toggled,this,[this] {
        const auto *asset = store_.selectedMesh();
        uv_->setContent(images_[channel_],asset ? asset->mesh : QSharedPointer<const wf::Mesh>{},slot_->currentIndex(),
            showUvs_->isChecked());
    });
    connect(frame,&QPushButton::clicked,viewport_,&LiveViewport::frame);
    connect(import_,&QPushButton::clicked,this,[this] { changeTexture(false); });
    connect(exportMap_,&QPushButton::clicked,this,&SurfaceWorkspace::exportTexture);
    connect(remove_,&QPushButton::clicked,this,[this] { changeTexture(true); });
    connect(apply_,&QPushButton::clicked,this,&SurfaceWorkspace::applyMaterial);
    connect(link_,&QPushButton::clicked,this,&SurfaceWorkspace::linkMaterial);
    connect(assign_,&QPushButton::clicked,this,&SurfaceWorkspace::assignMaterial);
    connect(unlink_,&QPushButton::clicked,this,[this] {
        const auto *asset = store_.selectedMesh();
        const int slot = slot_->currentIndex();
        if (!asset || slot < 0)
            return;
        auto mesh = *asset->mesh;
        mesh.materials[slot].sharedMaterialId.clear();
        QString error;
        message_->setText(store_.updateMesh(asset->id,std::move(mesh),tr("Make Material Independent"),error,asset->mesh.data())
            ? tr("The slot now has its own material.") : error);
    });
    connect(color_,&QPushButton::clicked,this,[this] {
        const auto next = QColorDialog::getColor(tint_,this,tr("Base Color"));
        if (next.isValid()) {
            tint_ = next;
            QPixmap swatch(20,14);
            swatch.fill(tint_);
            color_->setIcon(QIcon(swatch));
        }
    });
    connect(&store_,&ProjectStore::sceneChanged,this,&SurfaceWorkspace::refresh);
    connect(&store_,&ProjectStore::selectionChanged,this,&SurfaceWorkspace::refresh);
    refresh();
}

void SurfaceWorkspace::resetLayout()
{
    split_->setSizes({178,900,245});
}

void SurfaceWorkspace::hideEvent(QHideEvent *event)
{
    finishPaint(false);
    QWidget::hideEvent(event);
}

void SurfaceWorkspace::refresh()
{
    const QSignalBlocker blockObjects(object_), blockSlots(slot_), blockUvSets(uvSet_);
    const auto *asset = store_.selectedMesh();
    if (!asset || assetId_ != asset->id)
        uv_->fit();
    const auto previous = asset && assetId_ == asset->id ? slot_->currentIndex() : 0;
    object_->clear();
    if (const auto *project = store_.project())
        for (const auto &object : project->objects)
            if (!object.meshAsset.isEmpty())
                object_->addItem(object.name,object.id);
    object_->setCurrentIndex(object_->findData(store_.selection()));
    slot_->clear();
    assetId_ = asset ? asset->id : QString{};
    if (asset)
        for (const auto &material : asset->mesh->materials)
            slot_->addItem(QString::fromStdString(material.name));
    slot_->setCurrentIndex(std::clamp(previous,0,std::max(0,slot_->count()-1)));
    uvSet_->clear();
    uvSet_->setEnabled(asset != nullptr);
    if (asset) {
        uvSet_->addItem(asset->activeUvSet,asset->activeUvSet);
        for (auto set = asset->otherUvSets.cbegin(); set != asset->otherUvSets.cend(); ++set)
            uvSet_->addItem(set->needsUnwrap ? tr("%1 (needs unwrap)").arg(set.key()) : set.key(),set.key());
    }
    refreshSlot();
}

void SurfaceWorkspace::refreshSlot()
{
    const auto *asset = store_.selectedMesh();
    const int index = slot_->currentIndex();
    const auto *material = asset && index >= 0 && std::size_t(index) < asset->mesh->materials.size()
        ? &asset->mesh->materials[index] : nullptr;
    for (auto *control : std::array<QWidget *,10>{name_,metallic_,roughness_,emission_,clearCoat_,coatRoughness_,color_,apply_,import_,link_})
        control->setEnabled(material != nullptr);
    refreshMaps();
    name_->setText(material ? QString::fromStdString(material->name) : QString{});
    metallic_->setValue(material ? material->metallic : 0);
    roughness_->setValue(material ? material->roughness : 0);
    clearCoat_->setValue(material ? material->clearCoat : 0);
    coatRoughness_->setValue(material ? material->clearCoatRoughness : 0);
    layers_->setEnabled(material != nullptr);
    layers_->setLayers(material ? material->layers : wf::MaterialLayers{});
    emission_->setValue(material ? *std::ranges::max_element(material->emissive) : 0);
    tint_ = material ? QColor::fromRgbF(std::clamp(material->color[0],0.0f,1.0f),
        std::clamp(material->color[1],0.0f,1.0f),std::clamp(material->color[2],0.0f,1.0f)) : QColor("#808080");
    QPixmap swatch(20,14);
    swatch.fill(tint_);
    color_->setIcon(QIcon(swatch));
    const bool shared = material && !material->sharedMaterialId.empty();
    unlink_->setEnabled(shared);
    assign_->setEnabled(material && std::ranges::any_of(asset->mesh->faces,[index](const auto &face) {
        return face.material != std::uint32_t(index);
    }));
    sharedInfo_->setText(shared ? tr("Linked material. Edits apply to all linked slots.") : QString{});
}

void SurfaceWorkspace::refreshMaps()
{
    finishPaint(false);
    const auto *asset = store_.selectedMesh();
    const int index = slot_->currentIndex();
    const auto *material = asset && index >= 0 && std::size_t(index) < asset->mesh->materials.size()
        ? &asset->mesh->materials[index] : nullptr;
    const auto names = materialMapNames();
    QSize dimensions;
    for (int i = 0; i < int(channels_.size()); ++i) {
        images_[i] = {};
        if (material) {
            const auto &bytes = *material->previewTextures()[i];
            if (!bytes.empty()) {
                QBuffer input;
                input.setData(QByteArray::fromRawData(reinterpret_cast<const char *>(bytes.data()),bytes.size()));
                input.open(QIODevice::ReadOnly);
                QImageReader reader(&input);
                images_[i] = decode(reader,i == channel_ ? 1536 : 96,i == channel_ ? &dimensions : nullptr);
            }
        }
        QPixmap thumb(48,42);
        thumb.fill(QColor("#252b3b"));
        if (!images_[i].isNull()) {
            QPainter painter(&thumb);
            painter.setRenderHint(QPainter::SmoothPixmapTransform);
            const auto size = images_[i].size().scaled(48,42,Qt::KeepAspectRatio);
            painter.drawImage(QRect(QPoint((48-size.width())/2,(42-size.height())/2),size),images_[i]);
        }
        channels_[i]->setIcon(QIcon(thumb));
    }
    const bool assigned = material && !material->previewTextures()[channel_]->empty();
    if (paint_) {
        const bool canPaint = material && channel_ == 0 && (!assigned || (!images_[0].isNull()
            && qint64(dimensions.width())*dimensions.height() <= maxPaintPixels));
        paint_->setEnabled(canPaint);
        paint_->setToolTip(channel_ == 0 && assigned && !canPaint
            ? tr("Painting requires a readable base color map of up to 16 megapixels.")
            : tr("Paint on the Base Color image. Release to save the stroke or press Esc to cancel."));
        brushColor_->setEnabled(canPaint);
        brushSize_->setEnabled(canPaint);
        brushOpacity_->setEnabled(canPaint);
        uv_->setPaintEnabled(canPaint && paint_->isChecked());
    }
    exportUvs_->setEnabled(material != nullptr);
    inspectUvs_->setEnabled(material != nullptr);
    remove_->setEnabled(assigned);
    exportMap_->setEnabled(assigned && !images_[channel_].isNull());
    imageInfo_->setText(!material ? tr("Select a mesh with a material slot.") : !assigned ? tr("No %1 map").arg(names[channel_])
        : images_[channel_].isNull() ? tr("Image could not be decoded") : tr("%1 × %2 px").arg(dimensions.width()).arg(dimensions.height()));
    uv_->setContent(images_[channel_],asset ? asset->mesh : QSharedPointer<const wf::Mesh>{},index,showUvs_->isChecked());
}

bool SurfaceWorkspace::beginPaint()
{
    const auto *asset = store_.selectedMesh();
    const int slot = slot_->currentIndex();
    if (!asset || slot < 0 || channel_ != 0)
        return false;
    const auto &bytes = asset->mesh->materials[slot].baseColorTexture;
    QImage base;
    if (bytes.empty()) {
        base = QImage(blankMapSize,blankMapSize,QImage::Format_ARGB32);
        base.fill(Qt::white);
    } else {
        QBuffer input;
        input.setData(QByteArray::fromRawData(reinterpret_cast<const char *>(bytes.data()),qsizetype(bytes.size())));
        input.open(QIODevice::ReadOnly);
        QImageReader reader(&input);
        base = decode(reader,8192).convertToFormat(QImage::Format_ARGB32);
    }
    if (base.isNull()) {
        message_->setText(tr("Could not open the base color map for painting."));
        return false;
    }
    if (!uv_->startStroke(std::move(base),paintColor_,brushSize_->value(),brushOpacity_->value()/100.0)) {
        message_->setText(tr("Could not allocate the paint layer."));
        return false;
    }
    paintTarget_ = PaintTarget{asset->id,asset->mesh,slot};
    return true;
}

void SurfaceWorkspace::finishPaint(bool commit)
{
    if (!paintTarget_)
        return;
    const auto target = std::exchange(paintTarget_,std::nullopt);
    paintPreviewTimer_->stop();
    viewport_->setBaseColorPreview(-1,{});
    const auto result = commit ? uv_->paintedImage() : QImage{};
    uv_->clearStroke();
    if (!commit)
        return;
    QByteArray bytes;
    QBuffer output(&bytes);
    output.open(QIODevice::WriteOnly);
    if (!result.save(&output,"PNG") || std::size_t(bytes.size()) > wf::MeshMaterial::maxTextureBytes) {
        message_->setText(tr("Could not save the stroke. The painted PNG must fit within 16 MiB."));
        return;
    }
    auto mesh = *target->revision;
    mesh.materials[target->slot].baseColorTexture.assign(bytes.begin(),bytes.end());
    QString error;
    message_->setText(store_.updateMesh(target->id,std::move(mesh),tr("Paint Texture Stroke"),error,target->revision.data())
        ? tr("Base color stroke saved.") : error);
}

void SurfaceWorkspace::changeTexture(bool remove)
{
    const auto *asset = store_.selectedMesh();
    const int slot = slot_->currentIndex(), channel = channel_;
    if (!asset || slot < 0)
        return;
    const auto revision = asset->mesh;
    const auto id = asset->id;
    auto mesh = *revision;
    auto &material = mesh.materials[slot];
    auto &bytes = *material.previewTextures()[channel];
    if (remove) {
        bytes.clear();
    } else {
        const auto path = QFileDialog::getOpenFileName(this,tr("Import Texture Map"),{},tr("Images (*.png *.jpg *.jpeg)"));
        if (path.isEmpty())
            return;
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly) || file.size() > qsizetype(wf::MeshMaterial::maxTextureBytes)) {
            message_->setText(tr("Choose a readable PNG or JPEG up to 16 MiB."));
            return;
        }
        QImageReader reader(&file);
        if (decode(reader,128).isNull() || !file.seek(0)) {
            message_->setText(tr("Choose a valid PNG or JPEG within the 32-megapixel limit."));
            return;
        }
        const auto encoded = file.readAll();
        bytes.assign(encoded.begin(),encoded.end());
        if (channel == 4 && material.emissive == wf::Point3{})
            material.emissive = {1,1,1};
    }
    QString error;
    const auto label = remove ? tr("Remove Texture Map") : tr("Import Texture Map");
    message_->setText(store_.updateMesh(id,std::move(mesh),label,error,revision.data())
        ? (remove ? tr("Texture map removed.") : tr("Texture map assigned.")) : error);
}

void SurfaceWorkspace::exportTexture()
{
    const auto *asset = store_.selectedMesh();
    const int index = slot_->currentIndex();
    if (!asset || index < 0 || std::size_t(index) >= asset->mesh->materials.size())
        return;
    const auto &texture = *asset->mesh->materials[index].previewTextures()[channel_];
    if (texture.empty())
        return;
    const QByteArray bytes(reinterpret_cast<const char *>(texture.data()),qsizetype(texture.size()));
    const bool png = bytes.startsWith("\x89PNG\r\n\x1a\n");
    const auto extension = png ? QStringLiteral(".png") : QStringLiteral(".jpg");
    QFileDialog dialog(this,tr("Export Texture Map"));
    dialog.setAcceptMode(QFileDialog::AcceptSave);
    dialog.setNameFilter(png ? tr("PNG (*.png)") : tr("JPEG (*.jpg *.jpeg)"));
    dialog.setDefaultSuffix(extension.mid(1));
    dialog.selectFile(png ? "Texture.png" : "Texture.jpg");
    if (dialog.exec() != QDialog::Accepted)
        return;
    const auto path = dialog.selectedFiles().value(0);
    const auto suffix = QFileInfo(path).suffix().toLower();
    if ((png && suffix != "png") || (!png && suffix != "jpg" && suffix != "jpeg")) {
        message_->setText(tr("Use a %1 extension for this map.").arg(extension));
        return;
    }
    QSaveFile output(path);
    const bool saved = output.open(QIODevice::WriteOnly) && output.write(bytes) == bytes.size() && output.commit();
    message_->setText(saved ? tr("Original texture map exported.") : tr("Could not export the texture map."));
}

void SurfaceWorkspace::exportUvLayout()
{
    const auto *asset = store_.selectedMesh();
    if (!asset || slot_->currentIndex() < 0)
        return;
    const auto mesh = asset->mesh;
    const int slot = allSlots_->isChecked() ? -1 : slot_->currentIndex();
    if (slot >= 0 && std::ranges::none_of(mesh->faces,[slot](const auto &face) {
        return face.material == std::uint32_t(slot);
    })) {
        message_->setText(tr("The selected slot has no faces. Choose another slot or enable All Slots."));
        return;
    }
    QFileDialog dialog(this,tr("Export UV Layout"));
    dialog.setAcceptMode(QFileDialog::AcceptSave);
    dialog.setNameFilter(tr("PNG (*.png)"));
    dialog.setDefaultSuffix(QStringLiteral("png"));
    dialog.selectFile(QStringLiteral("UV Layout.png"));
    if (dialog.exec() != QDialog::Accepted)
        return;
    const auto path = dialog.selectedFiles().value(0);
    if (!path.endsWith(".png",Qt::CaseInsensitive)) {
        message_->setText(tr("Use a .png extension for the UV layout."));
        return;
    }
    QImage layout(2048,2048,QImage::Format_ARGB32_Premultiplied);
    layout.fill(Qt::transparent);
    {
        QPainter painter(&layout);
        drawUvLayout(painter,*mesh,slot,QRectF(1.5,1.5,2045,2045));
    }
    QSaveFile output(path);
    const bool saved = output.open(QIODevice::WriteOnly) && layout.save(&output,"PNG") && output.commit();
    message_->setText(saved ? tr("UV layout exported at 2048 × 2048.") : tr("Could not export the UV layout."));
}

void SurfaceWorkspace::inspectUvLayout()
{
    const auto *asset = store_.selectedMesh();
    if (!asset)
        return;
    const auto report = wf::inspectUvs(*asset->mesh,asset->uvSeams);
    message_->setText(tr("Active UV set %1 · %2 islands · %3 outside tile · %4 degenerate · %5 overlaps")
        .arg(asset->activeUvSet).arg(report.islands).arg(report.outside).arg(report.degenerate).arg(report.overlaps));
}

void SurfaceWorkspace::applyMaterial()
{
    const auto *asset = store_.selectedMesh();
    const int index = slot_->currentIndex();
    const auto name = name_->text().trimmed();
    if (!asset || index < 0 || name.isEmpty()) {
        message_->setText(tr("Enter a material name."));
        return;
    }
    auto mesh = *asset->mesh;
    auto &material = mesh.materials[index];
    material.name = name.toStdString();
    material.color[0] = float(tint_.redF());
    material.color[1] = float(tint_.greenF());
    material.color[2] = float(tint_.blueF());
    material.metallic = float(metallic_->value());
    material.roughness = float(roughness_->value());
    material.clearCoat = float(clearCoat_->value());
    material.clearCoatRoughness = float(coatRoughness_->value());
    material.layers = layers_->layers();
    const auto previous = *std::ranges::max_element(material.emissive);
    const auto emission = float(emission_->value());
    for (auto &value : material.emissive)
        value = previous > 0 ? emission*value/previous : emission;
    QString error;
    message_->setText(store_.updateMesh(asset->id,std::move(mesh),tr("Edit Material"),error,asset->mesh.data())
        ? tr("Material updated.") : error);
}

void SurfaceWorkspace::assignMaterial()
{
    const auto *asset = store_.selectedMesh();
    const int index = slot_->currentIndex();
    if (!asset || index < 0 || std::size_t(index) >= asset->mesh->materials.size())
        return;
    auto mesh = *asset->mesh;
    for (auto &face : mesh.faces)
        face.material = std::uint32_t(index);
    QString error;
    message_->setText(store_.updateMesh(asset->id,std::move(mesh),tr("Assign Material to All Faces"),error,asset->mesh.data())
        ? tr("Material slot assigned to all faces.") : error);
}

void SurfaceWorkspace::linkMaterial()
{
    const auto *asset = store_.selectedMesh();
    const int slot = slot_->currentIndex();
    if (!asset || slot < 0)
        return;
    const auto target = asset->id;
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Link Existing Material"));
    auto *form = new QFormLayout(&dialog);
    auto *sources = new QComboBox(&dialog);
    sources->setAccessibleName(tr("Source material slot"));
    for (const auto &source : store_.project()->assets) {
        if (source.id == target)
            continue;
        for (std::size_t i = 0; i < source.mesh->materials.size(); ++i)
            sources->addItem(tr("%1 · %2").arg(source.name,QString::fromStdString(source.mesh->materials[i].name)),
                QVariantList{source.id,int(i)});
    }
    if (!sources->count()) {
        message_->setText(tr("Add another mesh asset to link one of its material slots."));
        return;
    }
    form->addRow(tr("Material"),sources);
    auto *destination = new QComboBox(&dialog);
    destination->setAccessibleName(tr("Material link destination"));
    destination->addItem(tr("Replace current slot"),slot);
    destination->addItem(tr("Add a new slot"),int(asset->mesh->materials.size()));
    form->addRow(tr("Destination"),destination);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel,&dialog);
    form->addRow(buttons);
    connect(buttons,&QDialogButtonBox::accepted,&dialog,&QDialog::accept);
    connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);
    if (dialog.exec() != QDialog::Accepted)
        return;
    const auto source = sources->currentData().toList();
    QString error;
    const auto targetSlot = destination->currentData().toInt();
    if (store_.linkMaterialSlot(target,targetSlot,source[0].toString(),source[1].toInt(),error)) {
        slot_->setCurrentIndex(targetSlot);
        message_->setText(tr("Material linked."));
    } else
        message_->setText(error);
}
