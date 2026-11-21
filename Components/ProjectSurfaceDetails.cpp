#include "Components/ProjectSurfaceDetails.h"
#include "State/ProjectStore.h"

#include <QBuffer>
#include <QColorDialog>
#include <QComboBox>
#include <QCoreApplication>
#include <QDoubleSpinBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QImageReader>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPixmap>
#include <QPushButton>
#include <QVBoxLayout>
#include <algorithm>

namespace {
QString tr(const char *message) { return QCoreApplication::translate("ProjectSurfaceDetails",message); }

QPixmap preview(const ProjectTexture &texture,QSize bounds)
{
    QBuffer input;
    input.setData(texture.image);
    input.open(QIODevice::ReadOnly);
    QImageReader reader(&input);
    reader.setScaledSize(QSize(texture.width,texture.height).scaled(bounds,Qt::KeepAspectRatio));
    return QPixmap::fromImage(reader.read());
}

void assign(ProjectStore &store,const QString &projectId,const QString &materialId,const QString &preferredMesh,QWidget *parent)
{
    const auto *project = store.project();
    if (!project || project->id != projectId || project->assets.isEmpty())
        return;
    QDialog dialog(parent);
    dialog.setObjectName("assignProjectMaterial");
    dialog.setWindowTitle(tr("Assign Material to Mesh"));
    auto *form = new QFormLayout(&dialog);
    auto *meshes = new QComboBox(&dialog);
    meshes->setObjectName("surfaceMesh");
    for (const auto &asset : project->assets)
        meshes->addItem(asset.name,asset.id);
    if (const int index = meshes->findData(preferredMesh); index >= 0)
        meshes->setCurrentIndex(index);
    auto *slotsBox = new QComboBox(&dialog);
    slotsBox->setObjectName("surfaceSlot");
    form->addRow(tr("Mesh"),meshes);
    form->addRow(tr("Material slot"),slotsBox);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel,&dialog);
    form->addRow(buttons);
    const auto update = [&] {
        slotsBox->clear();
        if (const auto *asset = store.meshAsset(meshes->currentData().toString()))
            for (std::size_t slot = 0; slot < asset->mesh->materials.size(); ++slot)
                slotsBox->addItem(tr("%1. %2").arg(slot+1).arg(QString::fromStdString(asset->mesh->materials[slot].name)),int(slot));
        buttons->button(QDialogButtonBox::Ok)->setEnabled(slotsBox->count() > 0);
    };
    QObject::connect(meshes,&QComboBox::currentIndexChanged,&dialog,update);
    QObject::connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);
    QObject::connect(buttons,&QDialogButtonBox::accepted,&dialog,[&] {
        QString error;
        if (!store.project() || store.project()->id != projectId || slotsBox->currentIndex() < 0)
            return;
        if (!store.assignProjectMaterial(materialId,meshes->currentData().toString(),slotsBox->currentData().toInt(),error)) {
            QMessageBox::warning(&dialog,tr("Assign Material"),error);
            return;
        }
        dialog.accept();
    });
    update();
    dialog.exec();
}
}

QString projectSurfaces::importTexture(ProjectStore &store,QWidget *parent)
{
    if (!store.project())
        return {};
    const auto projectId = store.project()->id;
    const auto path = QFileDialog::getOpenFileName(parent,tr("Import Texture"),{},tr("Images (*.png *.jpg *.jpeg)"));
    if (path.isEmpty() || !store.project() || store.project()->id != projectId)
        return {};
    QString error;
    const auto id = store.importProjectTexture(path,error);
    if (id.isEmpty())
        QMessageBox::warning(parent,tr("Import Texture"),error);
    return id;
}

void projectSurfaces::showTexture(ProjectStore &store,const QString &id,QWidget *parent)
{
    const auto *project = store.project();
    if (!project || !project->surfaces.textures.contains(id))
        return;
    const auto &texture = project->surfaces.textures.value(id);
    QDialog dialog(parent);
    dialog.setObjectName("projectTextureDetails");
    dialog.setWindowTitle(tr("%1 — Texture Details").arg(texture.name));
    auto *layout = new QVBoxLayout(&dialog);
    auto *image = new QLabel(&dialog);
    image->setPixmap(preview(texture,{640,420}));
    image->setAlignment(Qt::AlignCenter);
    layout->addWidget(image);
    auto *metadata = new QLabel(tr("%1 × %2 pixels\n%3").arg(texture.width).arg(texture.height).arg(texture.fileName),&dialog);
    metadata->setTextFormat(Qt::PlainText);
    layout->addWidget(metadata);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close,&dialog);
    layout->addWidget(buttons);
    QObject::connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);
    dialog.exec();
}

QString projectSurfaces::editMaterial(ProjectStore &store,const QString &id,const QString &preferredMesh,QWidget *parent)
{
    const auto *project = store.project();
    if (!project || (!id.isEmpty() && !project->surfaces.materials.contains(id)))
        return {};
    const auto projectId = project->id;
    auto material = project->surfaces.materials.value(id);
    QString savedId = id;
    QDialog dialog(parent);
    dialog.setObjectName("projectMaterialDetails");
    dialog.setWindowTitle(id.isEmpty() ? tr("New Material") : tr("%1 — Material Details").arg(material.name));
    auto *layout = new QVBoxLayout(&dialog);
    auto *form = new QFormLayout;
    layout->addLayout(form);
    auto *name = new QLineEdit(material.name,&dialog);
    name->setObjectName("projectMaterialName");
    name->setMaxLength(120);
    auto *texture = new QComboBox(&dialog);
    texture->setObjectName("projectMaterialTexture");
    texture->addItem(tr("None"),QString{});
    for (auto it = project->surfaces.textures.cbegin(); it != project->surfaces.textures.cend(); ++it)
        texture->addItem(it->name,it.key());
    texture->setCurrentIndex(std::max(0,texture->findData(material.textureId)));
    auto *tint = new QPushButton(&dialog);
    auto *image = new QLabel(&dialog);
    image->setAlignment(Qt::AlignCenter);
    form->addRow(tr("Name"),name);
    form->addRow(tr("Base-color texture"),texture);
    form->addRow(tr("Base-color tint"),tint);
    const std::array mapLabels{tr("Normal"),tr("Metallic (B) / Roughness (G)"),tr("Occlusion (R)"),tr("Emissive")};
    std::array<QComboBox *,4> maps{};
    for (std::size_t i = 0; i < maps.size(); ++i) {
        auto *map = new QComboBox(&dialog);
        map->setObjectName(QStringLiteral("projectMaterialMap%1").arg(i));
        map->addItem(tr("None"),QString{});
        for (auto it = project->surfaces.textures.cbegin(); it != project->surfaces.textures.cend(); ++it)
            map->addItem(it->name,it.key());
        map->setCurrentIndex(std::max(0,map->findData(material.maps[i])));
        form->addRow(mapLabels[i],map);
        maps[i] = map;
    }
    const auto addFactor = [&](const QString &label,float value) {
        auto *box = new QDoubleSpinBox(&dialog);
        box->setRange(0,1);
        box->setDecimals(3);
        box->setSingleStep(0.05);
        box->setValue(value);
        form->addRow(label,box);
        return box;
    };
    const auto *metallic = addFactor(tr("Metallic factor"),material.metallic);
    const auto *roughness = addFactor(tr("Roughness factor"),material.roughness);
    const auto *emission = addFactor(tr("Emission strength"),material.emission);
    layout->addWidget(image);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save|QDialogButtonBox::Close,&dialog);
    auto *assignButton = buttons->addButton(tr("Assign to Mesh…"),QDialogButtonBox::ActionRole);
    assignButton->setObjectName("assignSavedMaterial");
    layout->addWidget(buttons);
    const auto draftMaterial = [&] {
        auto draft = material;
        draft.name = name->text().trimmed();
        draft.textureId = texture->currentData().toString();
        for (std::size_t i = 0; i < maps.size(); ++i)
            draft.maps[i] = maps[i]->currentData().toString();
        draft.metallic = float(metallic->value());
        draft.roughness = float(roughness->value());
        draft.emission = float(emission->value());
        return draft;
    };
    const auto refresh = [&] {
        const auto color = QColor::fromRgbF(material.tint[0],material.tint[1],material.tint[2],material.tint[3]);
        tint->setText(color.name());
        image->clear();
        const auto *active = store.project();
        if (active && active->id == projectId && active->surfaces.textures.contains(texture->currentData().toString()))
            image->setPixmap(preview(active->surfaces.textures.value(texture->currentData().toString()),{320,160}));
        const auto draft = draftMaterial();
        const bool existing = active && active->id == projectId && !savedId.isEmpty() && active->surfaces.materials.contains(savedId);
        assignButton->setEnabled(existing && !active->assets.isEmpty() && draft == active->surfaces.materials.value(savedId));
        assignButton->setToolTip(tr("Save any changes before assigning this material."));
        buttons->button(QDialogButtonBox::Save)->setEnabled(!draft.name.isEmpty() && active && active->id == projectId);
    };
    QObject::connect(name,&QLineEdit::textChanged,&dialog,refresh);
    QObject::connect(texture,&QComboBox::currentIndexChanged,&dialog,refresh);
    for (const auto *map : maps)
        QObject::connect(map,&QComboBox::currentIndexChanged,&dialog,refresh);
    for (const auto *factor : {metallic,roughness,emission})
        QObject::connect(factor,&QDoubleSpinBox::valueChanged,&dialog,refresh);
    QObject::connect(tint,&QPushButton::clicked,&dialog,[&] {
        const auto picked = QColorDialog::getColor(QColor::fromRgbF(material.tint[0],material.tint[1],material.tint[2]),
            &dialog,tr("Base-color Tint"));
        if (picked.isValid()) {
            material.tint = {picked.redF(),picked.greenF(),picked.blueF(),material.tint[3]};
            refresh();
        }
    });
    QObject::connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);
    QObject::connect(buttons,&QDialogButtonBox::accepted,&dialog,[&] {
        if (!store.project() || store.project()->id != projectId)
            return;
        QString error;
        const auto updated = store.saveProjectMaterial(savedId,draftMaterial(),error);
        if (updated.isEmpty()) {
            QMessageBox::warning(&dialog,tr("Save Material"),error);
            return;
        }
        savedId = updated;
        dialog.setWindowTitle(tr("%1 — Material Details").arg(name->text().trimmed()));
        refresh();
    });
    QObject::connect(assignButton,&QPushButton::clicked,&dialog,[&] { assign(store,projectId,savedId,preferredMesh,&dialog); });
    refresh();
    dialog.exec();
    return savedId;
}
