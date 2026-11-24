#include "Components/AssetHub.h"
#include "Components/LiveViewport.h"
#include "State/ProjectStore.h"
#include "State/StudioPublishReview.h"

#include <QBoxLayout>
#include <QBuffer>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QImageReader>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPixmap>
#include <QPushButton>
#include <QSet>

#include <algorithm>

namespace {
QStringList usage(const Project &project,const QString &id)
{
    QSet<QString> materials;
    if (project.surfaces.materials.contains(id))
        materials.insert(id);
    QStringList lines;
    for (auto it = project.surfaces.materials.cbegin(); it != project.surfaces.materials.cend(); ++it)
        if (it->textureId == id || std::ranges::find(it->maps,id) != it->maps.end()
            || std::ranges::find(it->layerMaps,id) != it->layerMaps.end()) {
            lines << QObject::tr("Material · %1").arg(it->name);
            materials.insert(it.key());
        }
    QSet<QString> meshes;
    if (project.assets.contains(id))
        meshes.insert(id);
    for (const auto &asset : project.assets)
        for (std::size_t slot = 0; slot < asset.mesh->materials.size(); ++slot)
            if (materials.contains(QString::fromStdString(asset.mesh->materials[slot].sharedMaterialId))) {
                meshes.insert(asset.id);
                lines << QObject::tr("Mesh · %1 · slot %2").arg(asset.name).arg(slot+1);
            }
    for (const auto &instance : project.objects)
        if (meshes.contains(instance.meshAsset))
            lines << QObject::tr("Level · %1 / %2").arg(instance.group,instance.name);
    return lines;
}

QPixmap thumbnail(const ProjectTexture &texture)
{
    QBuffer buffer;
    buffer.setData(texture.image);
    buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer);
    reader.setScaledSize(QSize(texture.width,texture.height).scaled({360,200},Qt::KeepAspectRatio));
    return QPixmap::fromImage(reader.read());
}

void reviewReimport(ProjectStore &store,const QString &id,QWidget *parent)
{
    const auto *project = store.project();
    if (!project)
        return;
    const auto projectId = project->id;
    const auto *existing = store.meshAsset(id);
    const bool mesh = existing != nullptr;
    if (!mesh && !project->surfaces.textures.contains(id))
        return;
    const auto source = mesh ? existing->sourcePath : project->surfaces.textures.value(id).sourcePath;
    const auto path = QFileDialog::getOpenFileName(parent,QObject::tr("Choose Reimport Source"),source,
        mesh ? QObject::tr("Binary glTF (*.glb)") : QObject::tr("Images (*.png *.jpg *.jpeg)"));
    if (path.isEmpty() || !store.project() || store.project()->id != projectId)
        return;
    QString error;
    const auto meshPreview = mesh ? store.previewMeshReimport(id,path,error) : std::nullopt;
    const auto imagePreview = mesh ? std::nullopt : store.previewTextureReimport(id,path,error);
    if (!meshPreview && !imagePreview) {
        QMessageBox::warning(parent,QObject::tr("Reimport Source"),error);
        return;
    }
    QDialog dialog(parent);
    dialog.setObjectName("assetHubReimportPreview");
    dialog.setWindowTitle(QObject::tr("Review Reimport"));
    dialog.resize(760,530);
    auto *layout = new QVBoxLayout(&dialog);
    auto *paths = new QLabel(QObject::tr("Saved source  %1\nSelected source  %2")
        .arg(source.isEmpty() ? QObject::tr("Unavailable") : source,path),&dialog);
    paths->setTextFormat(Qt::PlainText);
    paths->setWordWrap(true);
    layout->addWidget(paths);
    QStringList warnings;
    if (meshPreview) {
        auto *counts = new QLabel(QObject::tr("%1 → %2 vertices · %3 → %4 faces")
            .arg(meshPreview->before.mesh->vertices.size()).arg(meshPreview->after.mesh->vertices.size())
            .arg(meshPreview->before.mesh->faces.size()).arg(meshPreview->after.mesh->faces.size()),&dialog);
        layout->addWidget(counts);
        const auto review = reviewStudioPublish(*store.project(),&meshPreview->before,{},meshPreview->after.evaluated());
        warnings = review.warnings;
        auto *changes = new QLabel(QObject::tr("UV set · %1   |   Retained modifiers · %2   |   %3")
            .arg(meshPreview->after.activeUvSet).arg(meshPreview->after.modifiers.size())
            .arg(meshPreview->after.lodMesh
                ? QObject::tr("LOD · %1 faces").arg(meshPreview->after.lodMesh->faces.size())
                : QObject::tr("No generated LOD")),&dialog);
        layout->addWidget(changes);
        auto *view = new LiveViewport(store,true,&dialog);
        view->setModelShading(ModelShading::Material);
        const auto evaluated = [](const MeshAsset &asset) { return asset.evaluatedMesh ? asset.evaluatedMesh : asset.mesh; };
        view->setAssetPreview(evaluated(meshPreview->after));
        layout->addWidget(view,1);
        auto *toggle = new QPushButton(QObject::tr("Show Current Mesh"),&dialog);
        toggle->setCheckable(true);
        QObject::connect(toggle,&QPushButton::toggled,view,[view,toggle,evaluated,&meshPreview](bool current) {
            view->setAssetPreview(evaluated(current ? meshPreview->before : meshPreview->after));
            toggle->setText(current ? QObject::tr("Show Proposed Mesh") : QObject::tr("Show Current Mesh"));
        });
        layout->addWidget(toggle);
    } else {
        auto *images = new QHBoxLayout;
        for (const auto *texture : {&imagePreview->before,&imagePreview->after}) {
            auto *column = new QVBoxLayout;
            auto *image = new QLabel(&dialog);
            image->setPixmap(thumbnail(*texture));
            column->addWidget(image,0,Qt::AlignHCenter);
            auto *size = new QLabel(QObject::tr("%1 × %2").arg(texture->width).arg(texture->height),&dialog);
            column->addWidget(size,0,Qt::AlignHCenter);
            images->addLayout(column);
        }
        layout->addLayout(images);
    }
    auto *references = new QPlainTextEdit(&dialog);
    references->setReadOnly(true);
    const auto users = usage(*store.project(),id);
    if (!warnings.isEmpty()) {
        warnings.prepend(QObject::tr("Review before applying"));
        warnings << QString{};
    }
    warnings << (users.isEmpty() ? QStringList{QObject::tr("No current references")} : users);
    references->setPlainText(warnings.join('\n'));
    references->setMaximumHeight(150);
    layout->addWidget(references);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel,&dialog);
    const bool local = meshPreview && meshPreview->before.locallyEdited;
    if (local)
        layout->addWidget(new QLabel(QObject::tr("Applying replaces local mesh edits. Project material links and level placements are retained."),&dialog));
    auto *apply = buttons->addButton(local ? QObject::tr("Replace Local Mesh Edits") : QObject::tr("Apply Reimport"),QDialogButtonBox::AcceptRole);
    apply->setAutoDefault(false);
    buttons->button(QDialogButtonBox::Cancel)->setDefault(true);
    QObject::connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);
    QObject::connect(buttons,&QDialogButtonBox::accepted,&dialog,[&] {
        const bool ok = meshPreview ? store.applyMeshReimport(*meshPreview,local,error)
            : store.applyTextureReimport(*imagePreview,error);
        if (ok)
            dialog.accept();
        else
            QMessageBox::warning(&dialog,QObject::tr("Reimport Source"),error);
    });
    layout->addWidget(buttons);
    dialog.exec();
}
}

QWidget *createAssetHub(ProjectStore &store,std::function<void(const QString &)> open,QWidget *parent)
{
    auto *panel = new QWidget(parent);
    panel->setObjectName("assetHub");
    auto *layout = new QVBoxLayout(panel);
    auto *search = new QLineEdit(panel);
    search->setObjectName("assetHubSearch");
    search->setPlaceholderText(QObject::tr("Search assets by name, type, source, or usage…"));
    search->setClearButtonEnabled(true);
    layout->addWidget(search);
    auto *body = new QHBoxLayout;
    layout->addLayout(body,1);
    auto *list = new QListWidget(panel);
    list->setObjectName("assetHubAssets");
    auto *details = new QPlainTextEdit(panel);
    details->setObjectName("assetHubUsage");
    details->setReadOnly(true);
    body->addWidget(list,1);
    body->addWidget(details,1);
    auto *actions = new QHBoxLayout;
    auto *inspect = new QPushButton(QObject::tr("Open Asset"),panel);
    auto *reimport = new QPushButton(QObject::tr("Preview Reimport…"),panel);
    reimport->setObjectName("assetHubReimport");
    actions->addWidget(inspect);
    actions->addWidget(reimport);
    actions->addStretch(1);
    layout->addLayout(actions);
    const auto selection = [list] {
        return list->currentItem() ? list->currentItem()->data(Qt::UserRole).toString() : QString{};
    };
    const auto update = [&,selection,list,inspect,reimport,details] {
        const auto *project = store.project();
        const auto id = selection();
        inspect->setEnabled(project && !id.isEmpty());
        reimport->setEnabled(false);
        if (!project || id.isEmpty()) {
            details->setPlainText(QObject::tr("Select an asset to see where it is used."));
            return;
        }
        QStringList lines;
        const auto source = list->currentItem()->data(Qt::UserRole+1).toString();
        if (!source.isEmpty())
            lines << QObject::tr("Source · %1").arg(source)
                  << (QFileInfo::exists(source) ? QObject::tr("Source available") : QObject::tr("Source missing. Choose a replacement to relink it."));
        const auto refs = usage(*project,id);
        lines << (refs.isEmpty() ? QStringList{QObject::tr("No current references")} : refs);
        details->setPlainText(lines.join('\n'));
        reimport->setEnabled(project->surfaces.textures.contains(id) || (store.meshAsset(id) && !source.isEmpty()));
    };
    const auto rebuild = [&,selection,list,search,update] {
        const auto selected = selection();
        list->clear();
        const auto *project = store.project();
        if (project) {
            const auto query = search->text().trimmed();
            const auto add = [&](const QString &id,const QString &name,const QString &type,const QString &path) {
                const auto title = QObject::tr("%1 · %2").arg(name,type);
                if (!(title+' '+path).contains(query,Qt::CaseInsensitive)
                    && !usage(*project,id).join(' ').contains(query,Qt::CaseInsensitive))
                    return;
                auto *item = new QListWidgetItem(title,list);
                item->setData(Qt::UserRole,id);
                item->setData(Qt::UserRole+1,path);
                if (id == selected)
                    list->setCurrentItem(item);
            };
            for (const auto &item : project->assets)
                add(item.id,item.name,QObject::tr("Mesh"),item.sourcePath);
            for (auto it = project->surfaces.textures.cbegin(); it != project->surfaces.textures.cend(); ++it)
                add(it.key(),it->name,QObject::tr("Texture"),it->sourcePath);
            for (auto it = project->surfaces.materials.cbegin(); it != project->surfaces.materials.cend(); ++it)
                add(it.key(),it->name,QObject::tr("Material"),{});
        }
        if (!list->currentItem() && list->count())
            list->setCurrentRow(0);
        update();
    };
    QObject::connect(search,&QLineEdit::textChanged,panel,rebuild);
    QObject::connect(&store,&ProjectStore::sceneChanged,panel,rebuild);
    QObject::connect(&store,&ProjectStore::projectsChanged,panel,rebuild);
    QObject::connect(list,&QListWidget::currentItemChanged,panel,update);
    const auto show = [open = std::move(open),selection] { if (!selection().isEmpty()) open(selection()); };
    QObject::connect(list,&QListWidget::itemActivated,panel,show);
    QObject::connect(inspect,&QPushButton::clicked,panel,show);
    QObject::connect(reimport,&QPushButton::clicked,panel,[&store,selection,panel] { reviewReimport(store,selection(),panel); });
    rebuild();
    return panel;
}
