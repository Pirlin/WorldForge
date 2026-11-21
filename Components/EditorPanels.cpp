#include "Components/BehaviorEditor.h"
#include "Components/NativeGameplayEditor.h"
#include "Components/EditorPanels.h"
#include "Components/ProjectSurfaceDetails.h"
#include "Components/CommandTerminal.h"
#include "State/WorldScene.h"

#include "DesignSystem/Icons.h"
#include "Workspaces/WorkspaceCatalog.h"

#include <QApplication>
#include <QActionGroup>
#include <QCheckBox>
#include <QClipboard>
#include <QColorDialog>
#include <QComboBox>
#include <QDateTime>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSet>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QSplitter>
#include <QStandardItemModel>
#include <QStyledItemDelegate>
#include <QTabBar>
#include <QTabWidget>
#include <QToolButton>
#include <QTreeView>

#include <algorithm>

namespace {
constexpr int IdRole = Qt::UserRole + 1;
constexpr int AssetNameRole = Qt::UserRole + 2;
constexpr int AssetTypeRole = Qt::UserRole + 3;
constexpr int RowHeightRole = Qt::UserRole + 5;
const QStringList Groups{"Environment", "Lighting", "Ruins", "Props", "Gameplay"};

QLineEdit *searchBox(const QString &hint, QWidget *parent)
{
    auto *field = new QLineEdit(parent);
    field->setAccessibleName(hint);
    field->setPlaceholderText(hint + "…");
    field->setClearButtonEnabled(true);
    field->addAction(editorIcon("search", QColor("#d9dce6")), QLineEdit::LeadingPosition);
    return field;
}

QStandardItem *item(const QString &name, const QIcon &icon = {})
{
    auto *row = new QStandardItem(icon, name);
    row->setEditable(false);
    return row;
}

QString objectIcon(const SceneObject &object)
{
    if (object.type == "Folder")
        return "folder";
    if (object.type == "Character")
        return "person";
    if (object.group == "Lighting")
        return "spark";
    if (object.type == "Landscape")
        return "world";
    return "box";
}

QString selectedGroup(QTreeView *tree)
{
    const auto current = tree->currentIndex();
    for (const auto &index : {current, current.parent()}) {
        const auto group = index.siblingAtColumn(1).data().toString();
        if (Groups.contains(group))
            return group;
    }
    return "Props";
}

QString sceneGroup(const SceneObject &object)
{
    const bool supported = !object.meshAsset.isEmpty() || object.type == "Empty" || object.type == "Folder"
        || object.type == "Directional Light" || object.type == "Point Light" || object.type == "Terrain"
        || object.type == "World Atmosphere" || object.type == "Player Start";
    return supported ? (object.group.isEmpty() ? QObject::tr("Ungrouped") : object.group)
                     : QObject::tr("Unrendered Scene Data");
}

QLabel *mutedLabel(const QString &text, QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    label->setProperty("muted", true);
    return label;
}

QToolButton *iconButton(const QString &icon, const QString &label, QWidget *parent)
{
    auto *button = new QToolButton(parent);
    button->setProperty("flatIcon", true);
    button->setIcon(editorIcon(icon));
    button->setIconSize({15, 15});
    button->setAccessibleName(label);
    button->setToolTip(label);
    return button;
}

QWidget *section(QVBoxLayout *layout, const QString &title, bool expanded = true)
{
    auto *button = new QToolButton;
    button->setObjectName("sectionTitle");
    button->setText(title);
    button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    button->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
    button->setCheckable(true);
    button->setChecked(expanded);
    button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    layout->addWidget(button);
    auto *body = new QWidget;
    body->setObjectName("sectionBody");
    layout->addWidget(body);
    body->setVisible(expanded);
    QObject::connect(button, &QToolButton::toggled, body, [button, body](bool open) {
        button->setArrowType(open ? Qt::DownArrow : Qt::RightArrow);
        body->setVisible(open);
    });
    return body;
}

class RowHeightDelegate : public QStyledItemDelegate {
public:
    RowHeightDelegate(int height, QWidget *parent) : QStyledItemDelegate(parent), height_(height) {}

    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        auto size = QStyledItemDelegate::sizeHint(option, index);
        const int height = index.data(RowHeightRole).toInt();
        size.setHeight(height > 0 ? height : height_);
        return size;
    }

private:
    int height_;
};
}

Outliner::Outliner(ProjectStore &store, QWidget *parent)
    : QWidget(parent), store_(store), search_(searchBox(tr("Search scene"), this)),
      tree_(new QTreeView(this)), model_(new QStandardItemModel(this))
{
    setObjectName("panelBody");
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(3, 8, 3, 3);
    layout->setSpacing(6);
    auto *row = new QHBoxLayout;
    row->setContentsMargins(6, 0, 6, 0);
    row->setSpacing(8);
    search_->setFixedHeight(24);
    search_->setPlaceholderText(tr("Search Scene…"));
    row->addWidget(search_);
    auto *filter = iconButton("sort", tr("Scene filters"), this);
    filter->setPopupMode(QToolButton::InstantPopup);
    auto *filters = new QMenu(filter);
    filters->addAction(tr("Expand all"), tree_, &QTreeView::expandAll);
    filters->addAction(tr("Collapse folders"), this, [this] {
        tree_->collapseAll();
        tree_->expand(model_->index(0, 0));
    });
    auto *demo = filters->addAction(tr("Show legacy demo placeholders"));
    demo->setCheckable(true);
    connect(demo, &QAction::toggled, this, [this](bool show) {
        showDemo_ = show;
        rebuild();
    });
    filter->setMenu(filters);
    row->addWidget(filter);
    auto *add = iconButton("list", tr("Add scene object"), this);
    add->setPopupMode(QToolButton::InstantPopup);
    auto *addMenu = new QMenu(add);
    addMenu->addAction(tr("Cube"), this, [this] { store_.addCube(); });
    for (const QString &type : QStringList{"Directional Light", "Point Light", "Terrain", "Player Start", "Empty", "Folder"}) {
        addMenu->addAction(type, this, [this, type] {
            store_.addObject(type, type.endsWith("Light") ? "Lighting" : type == "Terrain" ? "Environment"
                : type == "Player Start" ? "Gameplay" : selectedGroup(tree_));
        });
    }
    add->setMenu(addMenu);
    row->addWidget(add);
    layout->addLayout(row);
    layout->addWidget(tree_, 1);

    tree_->setAccessibleName(tr("Scene hierarchy"));
    tree_->setRootIsDecorated(false);
    tree_->setIndentation(17);
    tree_->setIconSize({15, 15});
    tree_->setTreePosition(1);
    tree_->setModel(model_);
    tree_->setItemDelegate(new RowHeightDelegate(21, tree_));
    tree_->header()->setStretchLastSection(false);
    tree_->setSelectionBehavior(QAbstractItemView::SelectRows);
    tree_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(tree_, &QTreeView::customContextMenuRequested, this, [this](const QPoint &point) {
        const auto row = tree_->indexAt(point).siblingAtColumn(0);
        if (row.data(IdRole).toString().isEmpty())
            return;
        store_.select(row.data(IdRole).toString());
        QMenu menu(this);
        menu.addAction(editorIcon("trash"), tr("Delete object"), &store_, &ProjectStore::deleteObject);
        menu.exec(tree_->viewport()->mapToGlobal(point));
    });
    connect(search_, &QLineEdit::textChanged, this, &Outliner::rebuild);
    connect(tree_->selectionModel(), &QItemSelectionModel::currentChanged, this, [this](const QModelIndex &index) {
        const auto id = index.siblingAtColumn(0).data(IdRole).toString();
        if (!id.isEmpty())
            store_.select(id);
    });
    connect(tree_, &QTreeView::clicked, this, [this](const QModelIndex &index) {
        if (index.column() != 0)
            return;
        const auto *current = store_.object(index.data(IdRole).toString());
        if (!current)
            return;
        auto object = *current;
        object.visible = !object.visible;
        store_.updateObject(object, tr("Change visibility"));
    });
    connect(&store_, &ProjectStore::sceneChanged, this, &Outliner::rebuild);
    connect(&store_, &ProjectStore::selectionChanged, this, &Outliner::syncSelection);
    connect(&store_, &ProjectStore::projectsChanged, add, [this, add] { add->setEnabled(store_.project() != nullptr); });
    add->setEnabled(store_.project() != nullptr);
    rebuild();
}

void Outliner::focusSearch()
{
    search_->setFocus();
    search_->selectAll();
}

void Outliner::rebuild()
{
    const QSignalBlocker block(tree_->selectionModel());
    QSet<QString> collapsed{tr("Unrendered Scene Data")};
    const auto previousRoot = model_->index(0, 0);
    for (int row = 0; row < model_->rowCount(previousRoot); ++row) {
        const auto index = model_->index(row, 0, previousRoot);
        const auto group = index.siblingAtColumn(1).data().toString();
        if (!tree_->isExpanded(index))
            collapsed.insert(group);
        else
            collapsed.remove(group);
    }
    model_->clear();
    model_->setHorizontalHeaderLabels({{}, tr("Name"), tr("Type")});
    tree_->header()->setMinimumSectionSize(16);
    tree_->header()->setSectionResizeMode(0, QHeaderView::Fixed);
    tree_->header()->resizeSection(0, 25);
    tree_->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    tree_->header()->setSectionResizeMode(2, QHeaderView::Fixed);
    tree_->header()->resizeSection(2, 91);

    const auto *project = store_.project();
    if (!project)
        return;

    const auto query = search_->text().trimmed();
    int rowNumber = 0;
    const auto sizedRow = [&rowNumber](std::initializer_list<QStandardItem *> items) {
        const int height = 22 - (rowNumber++ % 2);
        const QList<QStandardItem *> row(items);
        for (auto *entry : row)
            entry->setData(height, RowHeightRole);
        return row;
    };
    auto *root = item({});
    model_->appendRow(sizedRow({root, item(tr("World (%1)").arg(project->name), editorIcon("globe")), item(tr("World"))}));
    QStringList groups;
    const auto listed = [&](const SceneObject &object) {
        return showDemo_ || !query.isEmpty() || !store_.isLegacyPlaceholder(object);
    };
    for (const auto &object : project->objects) {
        if (!listed(object))
            continue;
        if (const auto group = sceneGroup(object); !groups.contains(group))
            groups.append(group);
    }
    groups.sort(Qt::CaseInsensitive);
    for (const auto &group : groups) {
        auto *folder = item({});
        const auto folderRow = sizedRow({folder, item(group, editorIcon("folder", QColor("#e3e5ed"))), item(tr("Folder"))});
        for (const auto &object : project->objects) {
            if (!listed(object) || sceneGroup(object) != group || (!object.name.contains(query, Qt::CaseInsensitive)
                && !object.type.contains(query, Qt::CaseInsensitive) && !group.contains(query, Qt::CaseInsensitive)))
                continue;
            auto *row = item(object.name, editorIcon(objectIcon(object)));
            auto *eye = item({}, editorIcon("eye", object.visible ? QColor("#e8ebf3") : QColor("#666b79")));
            eye->setData(object.id, IdRole);
            eye->setToolTip(tr("Toggle visibility"));
            row->setToolTip(object.name);
            if (!object.visible)
                row->setForeground(QColor("#748199"));
            const auto type = object.type == "Static Mesh" && object.meshAsset.isEmpty()
                ? tr("No mesh") : object.type;
            folder->appendRow(sizedRow({eye, row, item(type)}));
        }
        if (folder->rowCount() == 0) {
            qDeleteAll(folderRow);
            continue;
        }
        root->appendRow(folderRow);
        tree_->setExpanded(folder->index(), !query.isEmpty() || !collapsed.contains(group));
    }
    tree_->expand(root->index());
    syncSelection();
}

void Outliner::syncSelection()
{
    const QSignalBlocker block(tree_->selectionModel());
    const auto selection = store_.selection();
    const auto matches = selection.isEmpty() ? QModelIndexList{}
        : model_->match(model_->index(0, 0), IdRole, selection, 1, Qt::MatchExactly | Qt::MatchRecursive);
    if (!matches.isEmpty()) {
        const auto current = tree_->currentIndex();
        if (current.siblingAtColumn(0) != matches.first())
            tree_->setCurrentIndex(matches.first());
    } else if (tree_->currentIndex().siblingAtColumn(0).data(IdRole).isValid()) {
        tree_->selectionModel()->clear();
    }
}

Inspector::Inspector(ProjectStore &store, QWidget *parent)
    : QWidget(parent), store_(store), type_(mutedLabel({}, this)), identity_(mutedLabel({}, this)),
      form_(new QWidget(this)), name_(new QLineEdit(this)), group_(new QComboBox(form_)),
      mesh_(new QComboBox(form_)), materialSlot_(new QComboBox(form_)), meshInfo_(new QLabel(form_)),
      geometryState_(mutedLabel({}, form_)), materialColor_(new QPushButton(form_)), intensity_(new QDoubleSpinBox(form_)),
      bodyMode_(new QComboBox(form_)), bounce_(new QDoubleSpinBox(form_)),
      terrainSize_(new QDoubleSpinBox(form_)),
      visible_(new QCheckBox(tr("Visible in viewport"), form_)), components_(new QTreeView(this)),
      componentModel_(new QStandardItemModel(this))
{
    setObjectName("panelBody");
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    auto *tabs = new QTabWidget(this);
    tabs->setObjectName("detailsTabs");
    tabs->setIconSize({14, 14});
    layout->addWidget(tabs);
    auto *details = new QWidget(tabs);
    details->setObjectName("panelBody");
    tabs->addTab(details, editorIcon("settings"), tr("Details"));
    auto *settings = new QWidget(tabs);
    tabs->addTab(settings, editorIcon("box"), tr("World Settings"));
    auto *close = iconButton("close", tr("Close Details"), tabs);
    connect(close, &QToolButton::clicked, this, &Inspector::closeRequested);
    tabs->setCornerWidget(close);
    tabs->tabBar()->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(tabs->tabBar(), &QTabBar::customContextMenuRequested, this, [this, tabs](const QPoint &point) {
        QMenu menu(tabs);
        menu.addAction(tr("Float or dock panel"), this, &Inspector::floatRequested);
        menu.exec(tabs->tabBar()->mapToGlobal(point));
    });

    auto *worldSettings = new QFormLayout(settings);
    auto *projectName = new QLabel(settings);
    auto *worldObjects = new QLabel(settings);
    auto *worldMeshes = new QLabel(settings);
    auto *worldRevision = new QLabel(settings);
    auto *unusedDemo = new QPushButton(settings);
    unusedDemo->setToolTip(tr("Removes unchanged objects from the original demo. You can undo this operation."));
    worldSettings->addRow(tr("Project"), projectName);
    worldSettings->addRow(tr("Scene objects"), worldObjects);
    worldSettings->addRow(tr("Mesh assets"), worldMeshes);
    worldSettings->addRow(tr("Scene revision"), worldRevision);
    worldSettings->addRow(unusedDemo);
    connect(unusedDemo, &QPushButton::clicked, this, [this] {
        if (QMessageBox::question(this, tr("Remove demo objects"),
                tr("Remove the unchanged objects from the original demo? This operation can be undone."),
                QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) == QMessageBox::Yes)
            store_.removeUnusedDemoObjects();
    });
    const auto updateWorld = [this, projectName, worldObjects, worldMeshes, worldRevision, unusedDemo] {
        const auto *project = store_.project();
        projectName->setText(project ? project->name : tr("No project"));
        worldObjects->setNum(project ? int(project->objects.size()) : 0);
        worldMeshes->setNum(project ? int(project->assets.size()) : 0);
        worldRevision->setText(project && project->scene ? QString::number(project->scene->revision()) : tr("—"));
        const int demoCount = int(store_.unusedDemoCount());
        unusedDemo->setText(tr("Remove demo objects (%1)").arg(demoCount));
        unusedDemo->setVisible(demoCount > 0);
    };
    connect(&store_, &ProjectStore::projectsChanged, this, updateWorld);
    connect(&store_, &ProjectStore::sceneChanged, this, updateWorld);
    updateWorld();

    auto *panel = new QVBoxLayout(details);
    panel->setContentsMargins(0, 0, 0, 0);
    panel->setSpacing(4);
    auto *objectHeader = new QWidget(details);
    objectHeader->setObjectName("detailActions");
    auto *header = new QVBoxLayout(objectHeader);
    header->setContentsMargins(7, 6, 7, 4);
    header->setSpacing(5);
    auto *summary = new QHBoxLayout;
    auto *icon = new QLabel(objectHeader);
    icon->setPixmap(editorIcon("box").pixmap(21));
    summary->addWidget(icon);
    name_->setObjectName("objectTitle");
    name_->setMaxLength(64);
    name_->setAccessibleName(tr("Object name"));
    summary->addWidget(name_, 1);
    auto *lock = iconButton("lock", tr("Lock object name"), objectHeader);
    lock->setCheckable(true);
    connect(lock, &QToolButton::toggled, name_, &QLineEdit::setReadOnly);
    summary->addWidget(lock);
    auto *copyId = iconButton("pin", tr("Copy stable object ID"), objectHeader);
    connect(copyId, &QToolButton::clicked, this, [this] {
        if (const auto *object = store_.selectedObject())
            QApplication::clipboard()->setText(object->id);
    });
    summary->addWidget(copyId);
    header->addLayout(summary);
    header->addWidget(type_);

    auto *reset = new QToolButton(objectHeader);
    reset->setText(tr("Reset Transform"));
    reset->setIcon(editorIcon("rotate"));
    reset->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    connect(reset, &QToolButton::clicked, this, [this] {
        if (const auto *current = store_.selectedObject()) {
            auto next = *current;
            next.position = {};
            next.rotation = {};
            next.scale = {1, 1, 1};
            store_.updateObject(next, tr("Reset transform"));
        }
    });
    header->addWidget(reset);
    const auto updateReset = [this, reset] { reset->setEnabled(store_.selectedObject()); };
    connect(&store_, &ProjectStore::selectionChanged, reset, updateReset);
    updateReset();
    components_->setObjectName("componentsTree");
    components_->setAccessibleName(tr("Object components"));
    components_->setModel(componentModel_);
    components_->setHeaderHidden(true);
    components_->setRootIsDecorated(true);
    components_->setIndentation(15);
    components_->setFixedHeight(94);
    components_->setIconSize({14, 14});
    header->addWidget(components_);
    panel->addWidget(objectHeader);

    auto *search = searchBox(tr("Search Details"), details);
    search->setFixedHeight(24);
    auto *searchRow = new QHBoxLayout;
    searchRow->setContentsMargins(7, 2, 6, 2);
    searchRow->setSpacing(7);
    searchRow->addWidget(search, 1);
    auto *filter = iconButton("sort", tr("Details filters"), details);
    auto *options = iconButton("settings", tr("Details settings"), details);
    filter->setPopupMode(QToolButton::InstantPopup);
    options->setPopupMode(QToolButton::InstantPopup);
    auto *filterMenu = new QMenu(filter);
    filterMenu->addAction(tr("Show all properties"), search, &QLineEdit::clear);
    filter->setMenu(filterMenu);
    auto *optionsMenu = new QMenu(options);
    optionsMenu->addAction(tr("World settings"), tabs, [tabs] { tabs->setCurrentIndex(1); });
    options->setMenu(optionsMenu);
    searchRow->addWidget(filter);
    searchRow->addWidget(options);
    panel->addLayout(searchRow);

    auto *scroll = new QScrollArea(details);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    form_->setObjectName("panelBody");
    scroll->setWidget(form_);
    panel->addWidget(scroll, 1);
    auto *sections = new QVBoxLayout(form_);
    sections->setContentsMargins(0, 0, 0, 0);
    sections->setSpacing(1);

    auto *transform = section(sections, tr("Transform"));
    auto *grid = new QGridLayout(transform);
    grid->setContentsMargins(19, 5, 7, 8);
    grid->setHorizontalSpacing(5);
    grid->setVerticalSpacing(5);
    const QStringList axisNames{"X", "Y", "Z"};
    const QStringList vectors{tr("Location"), tr("Rotation"), tr("Scale")};
    const QStringList colors{"#fa8b76", "#acd769", "#7abaed"};
    for (int vector = 0; vector < 3; ++vector) {
        grid->addWidget(new QLabel(vectors[vector], transform), vector, 0);
        for (int axis = 0; axis < 3; ++axis) {
            auto *field = new QWidget(transform);
            field->setObjectName("transformField");
            auto *fieldRow = new QHBoxLayout(field);
            fieldRow->setContentsMargins(3, 0, 2, 0);
            fieldRow->setSpacing(3);
            auto *axisLabel = new QLabel(axisNames[axis], field);
            axisLabel->setStyleSheet("color: " + colors[axis]);
            fieldRow->addWidget(axisLabel);
            auto *spin = new QDoubleSpinBox(field);
            spin->setAccessibleName(vectors[vector] + ' ' + axisNames[axis]);
            spin->setRange(-1000000, 1000000);
            spin->setDecimals(vector == 2 ? 3 : 1);
            spin->setSingleStep(vector == 2 ? 0.1 : 1.0);
            spin->setButtonSymbols(QAbstractSpinBox::NoButtons);
            spin->setKeyboardTracking(false);
            spin->setFixedHeight(19);
            spin->setMinimumWidth(37);
            spin->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
            spin->setStyleSheet("QDoubleSpinBox { border: 0; background: transparent; padding: 0; }");
            if (vector == 1)
                spin->setSuffix("°");
            fieldRow->addWidget(spin, 1);
            axes_[vector * 3 + axis] = spin;
            grid->addWidget(field, vector, axis + 1);
            grid->setColumnStretch(axis + 1, 1);
            connect(spin, &QDoubleSpinBox::editingFinished, this, &Inspector::apply);
        }
    }
    grid->setColumnMinimumWidth(0, 81);

    auto *geometry = section(sections, tr("Geometry"));
    auto *meshForm = new QFormLayout(geometry);
    meshForm->setContentsMargins(12, 4, 8, 5);
    mesh_->setAccessibleName(tr("Static mesh asset"));
    meshForm->addRow(tr("Mesh asset"), mesh_);
    meshInfo_->setWordWrap(true);
    meshForm->addRow(tr("Geometry"), meshInfo_);
    terrainSize_->setRange(4,1024);
    foliageDensity_ = new QDoubleSpinBox(geometry);
    foliageDensity_->setRange(0,48);
    foliageDensity_->setDecimals(0);
    foliageDensity_->setAccessibleName(tr("Foliage per chunk"));
    meshForm->addRow(tr("Trees per chunk"),foliageDensity_);
    foliageSeed_ = new QDoubleSpinBox(geometry);
    foliageSeed_->setRange(0,1000000);
    foliageSeed_->setDecimals(0);
    foliageSeed_->setAccessibleName(tr("Foliage seed"));
    meshForm->addRow(tr("Foliage seed"),foliageSeed_);
    foliageWind_ = new QDoubleSpinBox(geometry);
    foliageWind_->setRange(0,0.5);
    foliageWind_->setSingleStep(0.01);
    foliageWind_->setAccessibleName(tr("Foliage wind strength"));
    meshForm->addRow(tr("Wind strength"),foliageWind_);
    for (auto *input : {foliageDensity_,foliageSeed_,foliageWind_}) {
        input->setKeyboardTracking(false);
        connect(input,&QDoubleSpinBox::editingFinished,this,&Inspector::apply);
    }
    terrainSize_->setSuffix(tr(" m"));
    terrainSize_->setAccessibleName(tr("Terrain width"));
    meshForm->addRow(tr("Terrain width"),terrainSize_);
    connect(terrainSize_,&QDoubleSpinBox::editingFinished,this,&Inspector::apply);
    geometryState_->setWordWrap(true);
    meshForm->addRow(geometryState_);

    auto *logic = section(sections,tr("Behavior"));
    auto *logicForm = new QFormLayout(logic);
    behavior_ = new QComboBox(logic);
    behavior_->setObjectName("objectBehavior");
    logicForm->addRow(tr("First behavior"),behavior_);
    behaviorComponents_ = new QPushButton(tr("Manage Components…"),logic);
    behaviorComponents_->setObjectName("detailsManageBehaviorComponents");
    logicForm->addRow(behaviorComponents_);
    connect(behaviorComponents_,&QPushButton::clicked,this,[this] {
        editBehaviorComponents(store_,store_.selection(),this);
    });
    auto *nativeComponents = new QPushButton(tr("Native Components…"),logic);
    nativeComponents->setObjectName("detailsNativeComponents");
    logicForm->addRow(nativeComponents);
    connect(nativeComponents,&QPushButton::clicked,this,[this] { editNativeComponents(store_,this); });
    connect(behavior_,&QComboBox::activated,this,[this] {
        QString error;
        if (!store_.attachBehavior(store_.selection(),behavior_->currentData().toString(),error)) {
            QMessageBox::warning(this,tr("Attach Behavior"),error);
            refresh();
        }
    });
    auto *surface = section(sections, tr("Material"));
    auto *materialForm = new QFormLayout(surface);
    materialForm->setContentsMargins(12, 4, 8, 5);
    materialForm->addRow(tr("Material slot"), materialSlot_);
    materialForm->addRow(tr("Base color"), materialColor_);
    materialColor_->setToolTip(tr("Edit the material on this mesh asset and its instances"));
    connect(materialSlot_, &QComboBox::currentIndexChanged, this, [this] {
        const auto *asset = store_.selectedMesh();
        const int index = materialSlot_->currentIndex();
        const bool valid = asset && index >= 0 && std::size_t(index) < asset->mesh->materials.size();
        materialColor_->setEnabled(valid);
        if (!valid) {
            materialColor_->setText(tr("No material"));
            return;
        }
        const auto &value = asset->mesh->materials[index].color;
        materialColor_->setText(QColor::fromRgbF(value[0], value[1], value[2]).name().toUpper());
    });
    connect(materialColor_, &QPushButton::clicked, this, [this] {
        const auto *asset = store_.selectedMesh();
        const int index = materialSlot_->currentIndex();
        if (!asset || index < 0 || std::size_t(index) >= asset->mesh->materials.size())
            return;
        const auto id = asset->id;
        const auto original = asset->mesh;
        const auto &value = original->materials[index].color;
        const auto color = QColorDialog::getColor(QColor::fromRgbF(value[0], value[1], value[2]), this, tr("Material Base Color"));
        asset = store_.meshAsset(id);
        if (!color.isValid() || !asset || asset->mesh != original)
            return;
        auto candidate = *original;
        candidate.materials[index].color = {float(color.redF()), float(color.greenF()), float(color.blueF()), value[3]};
        QString error;
        if (!store_.updateMesh(id, std::move(candidate), tr("Set Material Base Color"), error))
            QMessageBox::warning(this, tr("Material"), error);
    });
    auto *light = section(sections, tr("Light"));
    auto *lightForm = new QFormLayout(light);
    // Photometric units: lux for directional lights, candela for point lights (see Engine/Render/ColorPipeline.h).
    intensity_->setRange(0, maximumLuminousIntensity);
    intensity_->setDecimals(0);
    intensity_->setSingleStep(1000);
    intensity_->setAccessibleName(tr("Light intensity"));
    lightForm->addRow(tr("Intensity"), intensity_);
    connect(intensity_, &QDoubleSpinBox::editingFinished, this, &Inspector::apply);

    auto *physics = section(sections, tr("Physics"), false);
    auto *physicsForm = new QFormLayout(physics);
    physicsForm->setContentsMargins(12,4,8,5);
    bodyMode_->setAccessibleName(tr("Physics body"));
    bodyMode_->addItem(tr("None"), QString());
    bodyMode_->addItem(tr("Static"), "Static");
    bodyMode_->addItem(tr("Dynamic"), "Dynamic");
    physicsForm->addRow(tr("Collider"), bodyMode_);
    colliderShape_ = new QComboBox(physics);
    colliderShape_->setAccessibleName(tr("Collider shape"));
    colliderShape_->addItems({tr("Box"),tr("Sphere"),tr("Capsule")});
    physicsForm->addRow(tr("Shape"),colliderShape_);
    connect(colliderShape_,&QComboBox::activated,this,&Inspector::apply);
    bounce_->setAccessibleName(tr("Restitution"));
    bounce_->setRange(0,1);
    bounce_->setSingleStep(0.05);
    physicsForm->addRow(tr("Bounce"), bounce_);
    mass_ = new QDoubleSpinBox(physics);
    mass_->setRange(0,100000);
    mass_->setSpecialValueText(tr("Automatic"));
    mass_->setSuffix(tr(" kg"));
    mass_->setAccessibleName(tr("Rigid body mass"));
    friction_ = new QDoubleSpinBox(physics);
    friction_->setRange(0,1);
    friction_->setSingleStep(0.05);
    friction_->setAccessibleName(tr("Rigid body friction"));
    physicsForm->addRow(tr("Mass"),mass_);
    physicsForm->addRow(tr("Friction"),friction_);
    spin_ = new QDoubleSpinBox(physics);
    spin_->setRange(-720,720);
    spin_->setSuffix(tr(" °/s"));
    spin_->setAccessibleName(tr("Initial spin about world Z"));
    physicsForm->addRow(tr("Initial Z spin"),spin_);
    for (auto *input : {mass_,friction_,spin_}) {
        input->setKeyboardTracking(false);
        connect(input,&QDoubleSpinBox::editingFinished,this,&Inspector::apply);
    }
    jointType_ = new QComboBox(physics);
    jointType_->setAccessibleName(tr("Joint type"));
    jointType_->addItem(tr("None"),QString());
    for (const auto &type : {QString("Fixed"),QString("Hinge"),QString("Distance")})
        jointType_->addItem(type,type);
    jointTarget_ = new QComboBox(physics);
    jointTarget_->setAccessibleName(tr("Joint target"));
    jointAxis_ = new QComboBox(physics);
    jointAxis_->setAccessibleName(tr("Joint world axis"));
    jointAxis_->addItems({"X","Y","Z"});
    physicsForm->addRow(tr("Joint"),jointType_);
    physicsForm->addRow(tr("Connect to"),jointTarget_);
    physicsForm->addRow(tr("Hinge world axis"),jointAxis_);
    auto *pivot = new QWidget(physics);
    auto *pivotFields = new QHBoxLayout(pivot);
    pivotFields->setContentsMargins(0,0,0,0);
    pivotFields->setSpacing(3);
    for (int axis = 0; axis < int(jointOffset_.size()); ++axis) {
        auto *input = new QDoubleSpinBox(pivot);
        input->setRange(-10000,10000);
        input->setButtonSymbols(QAbstractSpinBox::NoButtons);
        input->setKeyboardTracking(false);
        input->setAccessibleName(tr("Hinge pivot offset %1").arg(QString("XYZ").at(axis)));
        pivotFields->addWidget(input);
        jointOffset_[axis] = input;
        connect(input,&QDoubleSpinBox::editingFinished,this,&Inspector::apply);
    }
    physicsForm->addRow(tr("Pivot offset XYZ"),pivot);
    pivot->setToolTip(tr("Offset in world axes from the selected object's origin."));
    for (auto *input : {jointType_,jointTarget_,jointAxis_})
        connect(input,&QComboBox::activated,this,&Inspector::apply);
    connect(bodyMode_, &QComboBox::activated, this, &Inspector::apply);
    connect(bounce_, &QDoubleSpinBox::editingFinished, this, &Inspector::apply);

    auto *object = section(sections, tr("Object"), false);
    auto *objectTitle = qobject_cast<QToolButton *>(sections->itemAt(sections->count() - 2)->widget());
    objectTitle->setProperty("hiddenByDefault", true);
    objectTitle->hide();
    optionsMenu->addAction(tr("Object properties"), this, [objectTitle, scroll, object] {
        objectTitle->show();
        objectTitle->setChecked(true);
        scroll->ensureWidgetVisible(object);
    });
    auto *properties = new QFormLayout(object);
    group_->addItems(Groups);
    group_->setAccessibleName(tr("Object folder"));
    properties->addRow(tr("Folder"), group_);
    properties->addRow(visible_);
    identity_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    properties->addRow(tr("Object ID"), identity_);
    sections->addStretch();

    connect(search, &QLineEdit::textChanged, this, [sections](const QString &query) {
        for (int index = 0; index + 1 < sections->count(); index += 2) {
            auto *title = qobject_cast<QToolButton *>(sections->itemAt(index)->widget());
            auto *body = sections->itemAt(index + 1)->widget();
            const auto labels = body->findChildren<QLabel *>();
            const bool visible = !title->property("hiddenByDefault").toBool() || !query.isEmpty() || title->isChecked();
            const bool matches = visible && (title->text().contains(query, Qt::CaseInsensitive)
                || std::ranges::any_of(labels, [&](QLabel *label) { return label->text().contains(query, Qt::CaseInsensitive); }));
            title->setVisible(matches);
            body->setVisible(matches && (title->isChecked() || !query.isEmpty()));
        }
    });

    connect(&store_, &ProjectStore::selectionChanged, this, &Inspector::refresh);
    connect(name_, &QLineEdit::editingFinished, this, &Inspector::apply);
    for (auto *combo : {group_, mesh_})
        connect(combo, &QComboBox::activated, this, &Inspector::apply);
    connect(visible_, &QCheckBox::clicked, this, &Inspector::apply);
    refresh();
}

void Inspector::refresh()
{
    const auto *object = store_.selectedObject();
    type_->setText(tr("Select an object in the Scene Outliner or Content Browser."));
    type_->setVisible(object == nullptr);
    form_->setVisible(object != nullptr);
    name_->setEnabled(object != nullptr);
    name_->setText(object ? object->name : tr("No object selected"));
    components_->setVisible(object != nullptr);
    componentModel_->clear();
    if (!object)
        return;
    const auto *asset = store_.selectedMesh();
    const bool terrain = object->type == "Terrain";
    const bool atmosphere = object->type == "World Atmosphere";
    name_->setToolTip(object->id);
    if (group_->findText(object->group) < 0)
        group_->addItem(object->group);
    group_->setCurrentText(object->group);
    const QSignalBlocker meshBlock(mesh_);
    mesh_->clear();
    mesh_->addItem(tr("None"), QString());
    for (const auto &candidate : store_.project()->assets)
        mesh_->addItem(editorIcon("box"), candidate.name, candidate.id);
    mesh_->setCurrentIndex(std::max(0, mesh_->findData(object->meshAsset)));
    mesh_->setEnabled(object->type == "Static Mesh" || object->type == "Empty");
    meshInfo_->setText(terrain ? tr("Heightfield · Static terrain collider")
        : asset ? tr("%1 vertices · %2 faces").arg(asset->evaluated().vertices.size())
            .arg(asset->evaluated().faces.size()) : tr("No mesh assigned"));
    auto *meshForm = qobject_cast<QFormLayout *>(terrainSize_->parentWidget()->layout());
    meshForm->setRowVisible(terrainSize_,terrain);
    meshForm->setRowVisible(foliageDensity_,terrain);
    meshForm->setRowVisible(foliageSeed_,terrain);
    meshForm->setRowVisible(foliageWind_,terrain);
    if (terrain) {
        terrainSize_->setValue(readHeightfield(*object).size);
        foliageDensity_->setValue(object->details.value("foliageDensity").toInt());
        foliageSeed_->setValue(object->details.value("foliageSeed").toInt(42));
        foliageWind_->setValue(object->details.value("foliageWind").toDouble(0.09));
    }
    geometryState_->setText(asset ? (asset->sourcePath.isEmpty() ? tr("Native mesh") : QFileInfo(asset->sourcePath).fileName())
        : (object->type == "Static Mesh" ? tr("This object has no renderable asset. Assign a project mesh.") : QString()));
    const auto *selected = store_.selectedObject();
    const auto components = selected ? behaviorComponents(*selected) : QList<BehaviorComponent>{};
    const auto behaviorId = components.isEmpty() ? QString{} : components.front().assetId;
    const QSignalBlocker behaviorSignals(behavior_);
    behavior_->clear();
    behavior_->addItem(tr("None"),QString{});
    if (const auto *project = store_.project())
        for (auto it = project->behaviors.cbegin(); it != project->behaviors.cend(); ++it)
            behavior_->addItem(it->name,it.key());
    if (!behaviorId.isEmpty() && behavior_->findData(behaviorId) < 0)
        behavior_->addItem(tr("Missing Behavior"),behaviorId);
    behavior_->setCurrentIndex(behavior_->findData(behaviorId));
    behavior_->setEnabled(selected && components.size() <= 1);
    behaviorComponents_->setEnabled(selected);
    behaviorComponents_->setText(tr("Manage Components (%1)…").arg(components.size()));
    const int materialIndex = materialSlot_->currentIndex();
    materialSlot_->clear();
    if (asset)
        for (const auto &material : asset->mesh->materials)
            materialSlot_->addItem(QString::fromStdString(material.name));
    if (materialIndex >= 0 && materialIndex < materialSlot_->count())
        materialSlot_->setCurrentIndex(materialIndex);
    const bool light = object->type == "Directional Light" || object->type == "Point Light";
    intensity_->setEnabled(light);
    const bool point = object->type == "Point Light";
    intensity_->setSuffix(point ? tr(" cd") : tr(" lx"));
    intensity_->setMaximum(point ? maximumLuminousIntensity : maximumIlluminance);
    intensity_->setToolTip(point ? tr("Luminous intensity in candela. A bright street lamp is a few thousand candela.")
                                 : tr("Illuminance in lux. A clear midday sun is about 100,000 lux, an overcast sky about 10,000."));
    intensity_->setValue(light ? lightPhotometric(*object) : 0);
    bodyMode_->setEnabled(asset != nullptr);
    bodyMode_->setCurrentIndex(std::max(0,bodyMode_->findData(object->details.value("physics").toString())));
    colliderShape_->setEnabled(asset && bodyMode_->currentIndex() > 0);
    colliderShape_->setCurrentIndex(std::clamp(object->details.value("colliderShape").toInt(),0,2));
    bounce_->setEnabled(asset && bodyMode_->currentIndex() == 2);
    bounce_->setValue(object->details.value("bounce").toDouble());
    mass_->setEnabled(asset && bodyMode_->currentIndex() == 2);
    mass_->setValue(object->details.value("mass").toDouble());
    friction_->setEnabled(asset && bodyMode_->currentIndex() > 0);
    friction_->setValue(object->details.value("friction").toDouble(0.55));
    spin_->setEnabled(asset && bodyMode_->currentIndex() == 2);
    spin_->setValue(object->details.value("spin").toDouble());
    jointType_->setEnabled(asset && bodyMode_->currentIndex() > 0);
    jointType_->setCurrentIndex(std::max(0,jointType_->findData(object->details.value("jointType").toString())));
    jointTarget_->clear();
    jointTarget_->addItem(tr("Choose a body"),QString());
    if (const auto *project = store_.project())
        for (const auto &candidate : project->objects)
            if (candidate.id != object->id && candidate.visible && store_.meshAsset(candidate.meshAsset)
                && !candidate.details.value("physics").toString().isEmpty())
                jointTarget_->addItem(candidate.name,candidate.id);
    jointTarget_->setCurrentIndex(std::max(0,jointTarget_->findData(object->details.value("jointTarget").toString())));
    jointTarget_->setEnabled(jointType_->isEnabled() && jointType_->currentIndex() > 0);
    jointAxis_->setCurrentIndex(std::clamp(object->details.value("jointAxis").toInt(2),0,2));
    jointAxis_->setEnabled(jointType_->isEnabled() && jointType_->currentData().toString() == "Hinge");
    const auto offset = object->details.value("jointOffset").toArray();
    for (int axis = 0; axis < int(jointOffset_.size()); ++axis) {
        jointOffset_[axis]->setEnabled(jointAxis_->isEnabled());
        jointOffset_[axis]->setValue(axis < offset.size() ? offset.at(axis).toDouble() : 0);
    }
    visible_->setChecked(object->visible);
    identity_->setText(object->id);
    const std::array vectors{object->position, object->rotation, object->scale};
    for (size_t i = 0; i < axes_.size(); ++i) {
        axes_[i]->setEnabled(!atmosphere && (!terrain || i < 3));
        axes_[i]->setValue(vectors[i / 3][int(i % 3)]);
    }

    auto *actor = item(tr("%1 (%2)").arg(object->name, object->type), editorIcon(objectIcon(*object)));
    componentModel_->appendRow(actor);
    actor->appendRow(item(tr("Transform"), editorIcon("move")));
    if (asset)
        actor->appendRow(item(tr("Mesh · %1").arg(asset->name), editorIcon("box")));
    if (light)
        actor->appendRow(item(tr("Light"), editorIcon("spark")));
    if (terrain || bodyMode_->currentIndex() > 0)
        actor->appendRow(item(terrain ? tr("Terrain collider") : bodyMode_->currentText(), editorIcon("box")));
    components_->expandAll();
    components_->setCurrentIndex(actor->rowCount() ? actor->child(0)->index() : actor->index());
}

void Inspector::apply()
{
    const auto *current = store_.selectedObject();
    if (!current)
        return;
    auto next = *current;
    QString action = tr("Edit %1").arg(current->name);
    if (sender() == name_) {
        next.name = name_->text().trimmed();
        if (next.name.isEmpty()) {
            name_->setText(current->name);
            return;
        }
        action = tr("Rename %1").arg(current->name);
    } else if (sender() == group_) {
        next.group = group_->currentText();
    } else if (sender() == visible_) {
        next.visible = visible_->isChecked();
    } else if (sender() == mesh_) {
        next.meshAsset = mesh_->currentData().toString();
        if (!next.meshAsset.isEmpty() && next.type == "Empty")
            next.type = "Static Mesh";
    } else if (sender() == intensity_) {
        // Leaving the field without editing keeps an older project's unitless value as it is.
        if (std::abs(intensity_->value() - std::round(lightPhotometric(*current))) < 0.5)
            return;
        setLightPhotometric(next.details, next.type, intensity_->value());
    } else if (sender() == bodyMode_) {
        next.details.insert("physics", bodyMode_->currentData().toString());
    } else if (sender() == colliderShape_) {
        next.details.insert("colliderShape",colliderShape_->currentIndex());
    } else if (sender() == bounce_) {
        next.details.insert("bounce", bounce_->value());
    } else if (sender() == mass_) {
        next.details.insert("mass",mass_->value());
    } else if (sender() == friction_) {
        next.details.insert("friction",friction_->value());
    } else if (sender() == spin_) {
        next.details.insert("spin",spin_->value());
    } else if (sender() == jointType_) {
        next.details.insert("jointType",jointType_->currentData().toString());
    } else if (sender() == jointTarget_) {
        next.details.insert("jointTarget",jointTarget_->currentData().toString());
    } else if (sender() == jointAxis_) {
        next.details.insert("jointAxis",jointAxis_->currentIndex());
    } else if (std::ranges::find(jointOffset_,sender()) != jointOffset_.end()) {
        QJsonArray offset;
        for (const auto *input : jointOffset_)
            offset.append(input->value());
        next.details.insert("jointOffset",offset);
    } else if (sender() == foliageDensity_ && next.type == "Terrain") {
        next.details.insert("foliageDensity",int(foliageDensity_->value()));
    } else if (sender() == foliageSeed_ && next.type == "Terrain") {
        next.details.insert("foliageSeed",int(foliageSeed_->value()));
    } else if (sender() == foliageWind_ && next.type == "Terrain") {
        next.details.insert("foliageWind",foliageWind_->value());
    } else if (sender() == terrainSize_ && next.type == "Terrain") {
        next.details.insert("size",terrainSize_->value());
    } else {
        const auto axis = std::ranges::find(axes_, sender());
        if (axis == axes_.end())
            return;
        const auto index = std::distance(axes_.begin(), axis);
        const std::array vectors{&next.position, &next.rotation, &next.scale};
        (*vectors[index / 3])[int(index % 3)] = float((*axis)->value());
    }
    if (next != *current)
        store_.updateObject(next, action);
}

namespace {
class AssetTileDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        const bool selected = option.state.testFlag(QStyle::State_Selected);
        const bool hovered = option.state.testFlag(QStyle::State_MouseOver);
        painter->save();
        painter->setRenderHints(QPainter::Antialiasing | QPainter::SmoothPixmapTransform);
        QRect tile = option.rect.adjusted(2, 2, -2, -3);
        painter->setPen(selected ? QPen(QColor("#b366ff")) : QPen(Qt::NoPen));
        painter->setBrush(selected ? QColor("#38254e") : (hovered ? QColor("#252937") : Qt::transparent));
        painter->drawRoundedRect(tile, 5, 5);
        const QRect image(tile.left() + 3, tile.top() + 3, tile.width() - 6, 70);
        painter->setBrush(QColor("#242832"));
        painter->setPen(Qt::NoPen);
        painter->drawRoundedRect(image, 4, 4);
        qvariant_cast<QIcon>(index.data(Qt::DecorationRole)).paint(painter, image.adjusted(2, 2, -2, -2));

        auto font = option.font;
        font.setPixelSize(9);
        font.setLetterSpacing(QFont::AbsoluteSpacing, -0.18);
        painter->setFont(font);
        const auto name = index.data(AssetNameRole).toString();
        const auto label = painter->fontMetrics().elidedText(name, Qt::ElideMiddle, tile.width() - 5);
        painter->setPen(QColor("#eff0f5"));
        painter->drawText(QRect(tile.left(), image.bottom() + 3, tile.width(), 15), Qt::AlignCenter, label);
        painter->setPen(QColor("#b2b3c0"));
        painter->drawText(QRect(tile.left(), image.bottom() + 17, tile.width(), 14),
            Qt::AlignCenter, index.data(AssetTypeRole).toString());
        painter->restore();
    }

    QSize sizeHint(const QStyleOptionViewItem &, const QModelIndex &) const override { return {80, 115}; }
};
}

ContentBrowser::ContentBrowser(ProjectStore &store, QWidget *parent)
    : QWidget(parent), store_(store), search_(searchBox(tr("Search Content"), this)),
      folders_(new QTreeView(this)), tiles_(new QListView(this)), folderModel_(new QStandardItemModel(this)),
      assetModel_(new QStandardItemModel(this)), tabs_(new QTabWidget(this)), path_(new QLabel(this)),
      count_(mutedLabel({}, this)), log_(new QPlainTextEdit(this)), messages_(new QPlainTextEdit(this)),
      profiler_(new QLabel(this))
{
    setObjectName("panelBody");
    setMinimumHeight(219);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    tabs_->setIconSize({15, 15});
    tabs_->setObjectName("contentTabs");
    layout->addWidget(tabs_);
    auto *assets = new QWidget(tabs_);
    assets->setObjectName("panelBody");
    tabs_->addTab(assets, editorIcon("box"), tr("Content Browser"));
    tabs_->addTab(log_, editorIcon("log"), tr("Output Log"));
    messages_->setReadOnly(true);
    messages_->setMaximumBlockCount(500);
    messages_->setAccessibleName(tr("Project messages"));
    messages_->setPlaceholderText(tr("No project messages"));
    tabs_->addTab(messages_, editorIcon("log"), tr("Message Log"));
    profiler_->setAlignment(Qt::AlignCenter);
    tabs_->addTab(profiler_, editorIcon("pulse"), tr("Profiler"));
    tabs_->addTab(new CommandTerminal(store_,tabs_),editorIcon("code"),tr("Terminal"));
    auto *close = iconButton("close", tr("Close Content Browser"), tabs_);
    connect(close, &QToolButton::clicked, this, &ContentBrowser::closeRequested);
    tabs_->setCornerWidget(close);
    tabs_->tabBar()->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(tabs_->tabBar(), &QTabBar::customContextMenuRequested, this, [this](const QPoint &point) {
        QMenu menu(tabs_);
        menu.addAction(tr("Float or dock panel"), this, &ContentBrowser::floatRequested);
        menu.exec(tabs_->tabBar()->mapToGlobal(point));
    });
    log_->setReadOnly(true);
    log_->setMaximumBlockCount(500);
    log_->setAccessibleName(tr("Output log"));
    log_->setPlaceholderText(tr("Editor activity, rendering, and runtime output appear here."));

    auto *content = new QVBoxLayout(assets);
    content->setContentsMargins(0, 0, 0, 0);
    content->setSpacing(3);
    auto *commands = new QWidget(assets);
    commands->setObjectName("contentActions");
    auto *bar = new QHBoxLayout(commands);
    bar->setContentsMargins(7, 3, 10, 3);
    bar->setSpacing(6);
    const auto textButton = [commands, bar](const QString &icon, const QString &text) {
        auto *button = new QToolButton(commands);
        button->setIcon(editorIcon(icon));
        button->setText(text);
        button->setIconSize({15, 15});
        button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        bar->addWidget(button);
        return button;
    };
    auto *add = textButton("plus", tr("Add"));
    add->setPopupMode(QToolButton::InstantPopup);
    auto *addMenu = new QMenu(add);
    addMenu->addAction(tr("Cube"), this, [this] { store_.addCube(); });
    addMenu->addAction(tr("Place Selected Mesh"), this, [this] {
        store_.placeMesh(tiles_->currentIndex().data(IdRole).toString());
    });
    addMenu->addAction(tr("New Studio Asset…"), this, &ContentBrowser::newStudioAssetRequested);
    addMenu->addAction(tr("New Behavior…"),this,[this] { editBehaviorAsset(store_,{},this); });
    addMenu->addAction(tr("Import Texture…"),this,[this] { projectSurfaces::importTexture(store_,this); });
    addMenu->addAction(tr("New Material…"),this,[this] {
        const auto *mesh = store_.selectedMesh();
        projectSurfaces::editMaterial(store_,{},mesh ? mesh->id : QString{},this);
    });
    add->setMenu(addMenu);
    auto *import = textButton("import", tr("Import"));
    connect(import, &QToolButton::clicked, this, [this] {
        if (!store_.project())
            return;
        const auto file = QFileDialog::getOpenFileName(this, tr("Import GLB"), {}, tr("Binary glTF (*.glb)"));
        if (file.isEmpty())
            return;
        QString error;
        if (!store_.importMesh(file, error)) {
            appendMessage(tr("Import failed: %1").arg(error));
            showMessages();
            return;
        }
        appendLog(tr("Imported %1.").arg(QFileInfo(file).fileName()));
        emit meshImported();
    });
    auto *save = textButton("save", tr("Save All"));
    connect(save, &QToolButton::clicked, this, &ContentBrowser::saveRequested);
    auto *copy = iconButton("copy", tr("Copy asset name"), commands);
    auto *link = iconButton("link", tr("Copy asset ID"), commands);
    connect(copy, &QToolButton::clicked, this, [this] {
        const auto index = tiles_->currentIndex();
        if (index.isValid())
            QApplication::clipboard()->setText(index.data(AssetNameRole).toString());
    });
    connect(link, &QToolButton::clicked, this, [this] {
        const auto index = tiles_->currentIndex();
        if (index.isValid())
            QApplication::clipboard()->setText(index.data(IdRole).toString());
    });
    bar->addWidget(copy);
    bar->addWidget(link);
    auto *folderIcon = new QLabel(commands);
    folderIcon->setPixmap(editorIcon("folder").pixmap(15));
    bar->addWidget(folderIcon);
    bar->addWidget(path_);
    bar->addStretch(1);
    search_->setFixedHeight(24);
    search_->setMaximumWidth(216);
    search_->setMinimumWidth(136);
    bar->addWidget(search_, 2);
    auto *view = iconButton("list", tr("Toggle list view"), commands);
    view->setCheckable(true);
    bar->addWidget(view);
    bar->addSpacing(3);
    auto *sortButton = iconButton("sort", tr("Sort content"), commands);
    sortButton->setPopupMode(QToolButton::InstantPopup);
    auto *sortMenu = new QMenu(sortButton);
    const QStringList sorts{tr("Default"), tr("Name A–Z"), tr("Name Z–A"), tr("Type")};
    auto *sortActions = new QActionGroup(sortMenu);
    for (int i = 0; i < sorts.size(); ++i) {
        auto *action = sortMenu->addAction(sorts[i], this, [this, i] { sort_ = i; rebuild(); });
        action->setCheckable(true);
        action->setChecked(i == sort_);
        sortActions->addAction(action);
    }
    sortButton->setMenu(sortMenu);
    bar->addWidget(sortButton);
    bar->addSpacing(3);
    auto *options = iconButton("settings", tr("Content Browser settings"), commands);
    options->setPopupMode(QToolButton::InstantPopup);
    auto *optionsMenu = new QMenu(options);
    optionsMenu->addAction(tr("Clear search"), search_, &QLineEdit::clear);
    optionsMenu->addAction(tr("Float or dock panel"), this, &ContentBrowser::floatRequested);
    options->setMenu(optionsMenu);
    bar->addWidget(options);
    content->addWidget(commands);

    auto *split = new QSplitter(Qt::Horizontal, assets);
    folders_->setObjectName("folderTree");
    folders_->setAccessibleName(tr("Content folders"));
    folders_->setModel(folderModel_);
    folders_->setHeaderHidden(true);
    folders_->setIndentation(13);
    folders_->setIconSize({13, 13});
    folders_->setItemDelegate(new RowHeightDelegate(15, folders_));
    folders_->setUniformRowHeights(true);
    auto *project = item({}, editorIcon("box"));
    auto *root = item(tr("Content"), editorIcon("folderFill", QColor("#f3ce63")));
    folderModel_->appendRow(project);
    project->appendRow(root);
    root->appendRow(item(tr("Meshes"), editorIcon("folderFill", QColor("#f3ce63"))));
    folders_->expandAll();
    folders_->setCurrentIndex(root->index());
    const auto updateProject = [this, project] {
        const auto *active = store_.project();
        project->setText(active ? active->name : tr("Project"));
    };
    connect(&store_, &ProjectStore::projectsChanged, this, updateProject);
    updateProject();
    split->addWidget(folders_);

    auto *assetsPane = new QWidget(split);
    assetsPane->setObjectName("panelBody");
    auto *tileLayout = new QVBoxLayout(assetsPane);
    tileLayout->setContentsMargins(3, 7, 6, 4);
    tileLayout->setSpacing(1);
    tiles_->setObjectName("assetTiles");
    tiles_->setAccessibleName(tr("Project content"));
    tiles_->setModel(assetModel_);
    tiles_->setViewMode(QListView::IconMode);
    tiles_->setResizeMode(QListView::Adjust);
    tiles_->setMovement(QListView::Static);
    tiles_->setGridSize({81, 116});
    tiles_->setIconSize({65, 69});
    tiles_->setMouseTracking(true);
    tiles_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(tiles_, &QListView::customContextMenuRequested, this, [this](const QPoint &point) {
        const auto id = tiles_->indexAt(point).data(IdRole).toString();
        if (id.isEmpty())
            return;
        QMenu menu(tiles_);
        menu.addAction(tr("Open Asset"), this, [this, id] { emit openAssetRequested(id); });
        if (store_.meshAsset(id)) {
            menu.addAction(tr("Edit in Studio"), this, [this, id] { emit editStudioAssetRequested(id); });
            menu.addAction(tr("Add to Level"), this, [this, id] { store_.placeMesh(id); });
        }
        menu.exec(tiles_->viewport()->mapToGlobal(point));
    });
    auto *tileDelegate = new AssetTileDelegate(tiles_);
    auto *listDelegate = tiles_->itemDelegate();
    tiles_->setItemDelegate(tileDelegate);
    connect(view, &QToolButton::toggled, this, [this, tileDelegate, listDelegate](bool list) {
        tiles_->setViewMode(list ? QListView::ListMode : QListView::IconMode);
        tiles_->setGridSize(list ? QSize() : QSize(81, 116));
        tiles_->setIconSize(list ? QSize(19, 19) : QSize(65, 69));
        tiles_->setItemDelegate(list ? listDelegate : tileDelegate);
    });
    tileLayout->addWidget(tiles_, 1);
    auto *footer = new QHBoxLayout;
    footer->setContentsMargins(4, 0, 5, 0);
    footer->addWidget(count_);
    footer->addStretch();
    auto *zoom = new QSlider(Qt::Horizontal, assetsPane);
    zoom->setRange(80, 115);
    zoom->setValue(81);
    zoom->setMaximumWidth(86);
    zoom->setAccessibleName(tr("Asset thumbnail size"));
    connect(zoom, &QSlider::valueChanged, this, [this](int size) {
        if (tiles_->viewMode() == QListView::IconMode)
            tiles_->setGridSize({size, 116});
    });
    footer->addWidget(zoom);
    tileLayout->addLayout(footer);
    split->addWidget(assetsPane);
    split->setCollapsible(0, false);
    split->setCollapsible(1, false);
    split->setStretchFactor(1, 1);
    split->setSizes({187, 770});
    content->addWidget(split, 1);

    connect(search_, &QLineEdit::textChanged, this, &ContentBrowser::rebuild);
    connect(folders_->selectionModel(), &QItemSelectionModel::currentChanged, this, &ContentBrowser::rebuild);
    connect(tiles_->selectionModel(), &QItemSelectionModel::currentChanged, this, [this](const QModelIndex &index) {
        const auto id = index.data(IdRole).toString();
        const auto *project = store_.project();
        if (!project || id.isEmpty())
            return;
        const auto *current = store_.selectedObject();
        if (current && current->meshAsset == id)
            return;
        const auto instance = std::ranges::find(project->objects, id, &SceneObject::meshAsset);
        if (instance != project->objects.end())
            store_.select(instance->id);
    });
    connect(tiles_, &QListView::activated, this, [this](const QModelIndex &index) {
        const auto id = index.data(IdRole).toString();
        if (!id.isEmpty())
            emit openAssetRequested(id);
    });
    connect(&store_, &ProjectStore::sceneChanged, this, &ContentBrowser::rebuild);
    connect(&store_, &ProjectStore::selectionChanged, this, &ContentBrowser::syncSelection);
    rebuild();
}

void ContentBrowser::rebuild()
{
    const QSignalBlocker block(tiles_->selectionModel());
    const auto previous = tiles_->currentIndex().data(IdRole).toString();
    assetModel_->clear();
    const auto *project = store_.project();
    const auto query = search_->text().trimmed();
    path_->setText(project && (!project->surfaces.empty() || !project->behaviors.isEmpty())
        ? tr("Content") : tr("Content  ›  Meshes"));
    QList<const MeshAsset *> assets;
    if (project) {
        for (const auto &asset : project->assets) {
            if (asset.name.contains(query, Qt::CaseInsensitive)
                || QFileInfo(asset.sourcePath).fileName().contains(query, Qt::CaseInsensitive))
                assets.append(&asset);
        }
    }
    if (sort_ != 0) {
        std::ranges::sort(assets, [this](const MeshAsset *a, const MeshAsset *b) {
            const auto comparison = a->name.localeAwareCompare(b->name);
            return sort_ == 2 ? comparison > 0 : comparison < 0;
        });
    }
    for (const auto *asset : assets) {
        auto *tile = item(asset->name, editorIcon("box", QColor("#a78be6")));
        tile->setData(asset->id, IdRole);
        tile->setData(asset->name, AssetNameRole);
        tile->setData(tr("Static Mesh"), AssetTypeRole);
        tile->setToolTip(tr("%1\n%2 vertices · %3 faces").arg(asset->name)
            .arg(asset->evaluated().vertices.size()).arg(asset->evaluated().faces.size()));
        assetModel_->appendRow(tile);
    }
    if (project) {
        const auto surfaceTile = [&](const QString &id,const QString &name,const QString &type,const QString &icon) {
            auto *tile = item(name,editorIcon(icon,QColor("#a78be6")));
            tile->setData(id,IdRole);
            tile->setData(name,AssetNameRole);
            tile->setData(type,AssetTypeRole);
            assetModel_->appendRow(tile);
        };
        for (auto it = project->surfaces.textures.cbegin(); it != project->surfaces.textures.cend(); ++it)
            if (it->name.contains(query,Qt::CaseInsensitive) || it->fileName.contains(query,Qt::CaseInsensitive))
                surfaceTile(it.key(),it->name,tr("Texture"),"layers");
        for (auto it = project->surfaces.materials.cbegin(); it != project->surfaces.materials.cend(); ++it)
            if (it->name.contains(query,Qt::CaseInsensitive))
                surfaceTile(it.key(),it->name,tr("Material"),"box");
        for (auto it = project->behaviors.cbegin(); it != project->behaviors.cend(); ++it)
            if (it->name.contains(query,Qt::CaseInsensitive))
                surfaceTile(it.key(),it->name,tr("Behavior"),"code");
    }
    if (!previous.isEmpty()) {
        const auto matches = assetModel_->match(assetModel_->index(0, 0), IdRole, previous);
        if (!matches.isEmpty())
            tiles_->setCurrentIndex(matches.first());
    }
    syncSelection();
}

void ContentBrowser::syncSelection()
{
    const QSignalBlocker block(tiles_->selectionModel());
    const auto *asset = store_.selectedMesh();
    if (asset && (tiles_->currentIndex().data(IdRole).toString().isEmpty()
        || store_.meshAsset(tiles_->currentIndex().data(IdRole).toString()))) {
        const auto matches = assetModel_->match(assetModel_->index(0, 0), IdRole, asset->id, 1, Qt::MatchExactly);
        if (matches.isEmpty())
            tiles_->selectionModel()->clear();
        else
            tiles_->setCurrentIndex(matches.first());
    }
    const int count = assetModel_->rowCount();
    count_->setText(tr("%n asset(s)", nullptr, count));
}
void ContentBrowser::appendLog(const QString &message)
{
    log_->appendPlainText(QDateTime::currentDateTime().toString("HH:mm:ss  ") + message);
}

void ContentBrowser::appendMessage(const QString &message)
{
    const auto time = QDateTime::currentDateTime().toString("HH:mm:ss  ");
    messages_->appendPlainText(time + message);
    appendLog(message);
}

void ContentBrowser::setMetrics(const QString &report)
{
    profiler_->setText(report);
}

void ContentBrowser::showMessages()
{
    tabs_->setCurrentWidget(messages_);
}

void ContentBrowser::showLog() { tabs_->setCurrentWidget(log_); }

void ContentBrowser::showTerminal()
{
    tabs_->setCurrentIndex(tabs_->count()-1);
    if (auto *field = tabs_->currentWidget()->findChild<QLineEdit *>())
        field->setFocus();
}
