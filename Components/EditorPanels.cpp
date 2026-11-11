#include "Components/EditorPanels.h"

#include "DesignSystem/Icons.h"
#include "Workspaces/WorkspaceCatalog.h"

#include <QApplication>
#include <QActionGroup>
#include <QCheckBox>
#include <QClipboard>
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
#include <QPainter>
#include <QPlainTextEdit>
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
constexpr int FolderRole = Qt::UserRole + 4;
constexpr int RowHeightRole = Qt::UserRole + 5;
const QStringList Groups{"Environment", "Lighting", "Ruins", "Props", "Gameplay"};
const QStringList Materials{"", "M_Castle_Stone", "M_Castle_Moss", "M_Valley_Grass",
    "M_Silverwater", "M_Knight_Armor", "M_Default", "M_Castle_Banner"};

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

QIcon thumbnail(const SceneObject &object)
{
    static const QHash<QString, QRect> assetCrops{
        {"castle-main", {795, 737, 65, 69}}, {"castle-tower", {876, 736, 65, 69}},
        {"castle-tower-b", {957, 736, 65, 69}}, {"castle-wall", {1038, 736, 65, 69}},
        {"archway", {1119, 736, 65, 69}}, {"statue-left", {1229, 736, 65, 69}},
        {"statue-right", {1229, 736, 65, 69}}};
    if (assetCrops.contains(object.id)) {
        static const QPixmap sheet(":/Docs/file_00000000418c8208a492dcb2b14aaa90.png");
        return QIcon(sheet.copy(assetCrops[object.id]));
    }
    if (object.id == "hero-knight") {
        static const QPixmap sheet(":/Docs/file_000000003e18820d85b6a524ad596f27.png");
        return QIcon(sheet.copy(465, 176, 275, 275));
    }
    return editorIcon(objectIcon(object), QColor("#a78be6"));
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

QPixmap worldCrop(const QRect &rect)
{
    static const QPixmap screenshot(":/Docs/file_00000000418c8208a492dcb2b14aaa90.png");
    return screenshot.copy(rect);
}

QString assetName(const SceneObject &object)
{
    if (object.id == "statue-left" || object.id == "statue-right")
        return "SM_Statue_Knight";
    return object.type == "Static Mesh" ? "SM_" + object.name : object.name;
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
    filter->setMenu(filters);
    row->addWidget(filter);
    auto *add = iconButton("list", tr("Add scene object"), this);
    add->setPopupMode(QToolButton::InstantPopup);
    auto *addMenu = new QMenu(add);
    for (const QString &type : QStringList{"Static Mesh", "Character", "Point Light", "Empty"}) {
        addMenu->addAction(type, this, [this, type] { store_.addObject(type, selectedGroup(tree_)); });
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
    QSet<QString> collapsed = model_->rowCount() == 0 ? QSet<QString>{"Props"} : QSet<QString>{};
    const auto previousRoot = model_->index(0, 0);
    for (int row = 0; row < model_->rowCount(previousRoot); ++row) {
        const auto index = model_->index(row, 0, previousRoot);
        if (!tree_->isExpanded(index))
            collapsed.insert(index.siblingAtColumn(1).data().toString());
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
    for (const auto &group : Groups) {
        auto *folder = item({}, editorIcon("eye"));
        const auto folderRow = sizedRow({folder, item(group, editorIcon("folder", QColor("#e3e5ed"))), item(tr("Folder"))});
        for (const auto &object : project->objects) {
            if (object.group != group || (!object.name.contains(query, Qt::CaseInsensitive)
                && !object.type.contains(query, Qt::CaseInsensitive) && !group.contains(query, Qt::CaseInsensitive)))
                continue;
            auto *row = item(object.name, editorIcon(objectIcon(object)));
            auto *eye = item({}, editorIcon("eye", object.visible ? QColor("#e8ebf3") : QColor("#666b79")));
            eye->setData(object.id, IdRole);
            eye->setToolTip(tr("Toggle visibility"));
            row->setToolTip(object.name);
            if (!object.visible)
                row->setForeground(QColor("#748199"));
            const auto type = object.type == "Directional Light" ? "DirectionalLight"
                : QString(object.type).remove(' ');
            folder->appendRow(sizedRow({eye, row, item(type)}));
        }
        if (folder->rowCount() == 0 && !query.isEmpty()) {
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
      mesh_(new QComboBox(form_)), lodGroup_(new QComboBox(form_)), lods_(new QSpinBox(form_)),
      autoLod_(new QCheckBox(form_)), collision_(new QCheckBox(tr("Enabled"), form_)),
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
    worldSettings->addRow(tr("Project"), projectName);
    worldSettings->addRow(tr("Scene objects"), worldObjects);
    const auto updateWorld = [this, projectName, worldObjects] {
        const auto *project = store_.project();
        projectName->setText(project ? project->name : tr("No project"));
        worldObjects->setNum(project ? int(project->objects.size()) : 0);
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

    auto *commands = new QHBoxLayout;
    auto *add = new QToolButton(objectHeader);
    add->setObjectName("primaryTool");
    add->setIcon(editorIcon("plus"));
    add->setText(tr("Add Component"));
    add->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    add->setPopupMode(QToolButton::InstantPopup);
    auto *addMenu = new QMenu(add);
    for (const auto &component : QStringList{"PointLightComponent", "AudioComponent", "PhysicsComponent", "ScriptComponent"}) {
        addMenu->addAction(component, this, [this, component] {
            const auto *current = store_.selectedObject();
            if (!current)
                return;
            auto next = *current;
            auto custom = next.details.value("components").toArray();
            if (custom.contains(QJsonValue(component)))
                return;
            custom.append(component);
            next.details.insert("components", custom);
            store_.updateObject(next, tr("Add %1").arg(component));
        });
    }
    add->setMenu(addMenu);
    commands->addWidget(add);
    commands->addStretch();
    auto *blueprint = new QToolButton(objectHeader);
    blueprint->setIcon(editorIcon("graph"));
    blueprint->setText(tr("Blueprint"));
    blueprint->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    blueprint->setPopupMode(QToolButton::InstantPopup);
    auto *blueprintMenu = new QMenu(blueprint);
    blueprintMenu->addAction(tr("Copy object ID"), copyId, &QToolButton::click);
    blueprintMenu->addAction(tr("Reset transform"), this, [this] {
        if (const auto *current = store_.selectedObject()) {
            auto next = *current;
            next.position = {};
            next.rotation = {};
            next.scale = {1, 1, 1};
            store_.updateObject(next, tr("Reset transform"));
        }
    });
    blueprint->setMenu(blueprintMenu);
    commands->addWidget(blueprint);
    header->addLayout(commands);

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

    auto *staticMesh = section(sections, tr("Static Mesh"));
    auto *meshRow = new QHBoxLayout(staticMesh);
    meshRow->setContentsMargins(19, 4, 7, 2);
    auto *meshLabel = new QLabel(tr("Static Mesh"), staticMesh);
    meshLabel->setMinimumWidth(91);
    meshRow->addWidget(meshLabel);
    auto *meshPreview = new QLabel(staticMesh);
    meshPreview->setFixedSize(40, 40);
    meshPreview->setPixmap(worldCrop({1448, 514, 38, 38}));
    meshRow->addWidget(meshPreview);
    mesh_->addItems({"SM_Castle_Main", "SM_Castle_Tower_A", "SM_Castle_Tower_B", "SM_Castle_Wall_A", "SM_Archway", "SM_Statue_Knight"});
    mesh_->setAccessibleName(tr("Static mesh asset"));
    meshRow->addWidget(mesh_, 1);
    auto *resetMesh = iconButton("rotate", tr("Reset mesh"), staticMesh);
    connect(resetMesh, &QToolButton::clicked, this, [this] {
        if (const auto *object = store_.selectedObject()) {
            auto next = *object;
            next.details.remove("mesh");
            store_.updateObject(next, tr("Reset mesh"));
        }
    });
    meshRow->addWidget(resetMesh);

    auto *surface = section(sections, tr("Materials"));
    auto *materialGrid = new QGridLayout(surface);
    materialGrid->setContentsMargins(19, 4, 15, 3);
    materialGrid->setHorizontalSpacing(8);
    materialGrid->setVerticalSpacing(2);
    for (int index = 0; index < int(materials_.size()); ++index) {
        auto *label = new QLabel(tr("Element %1").arg(index), surface);
        label->setMinimumWidth(88);
        materialGrid->addWidget(label, index, 0);
        auto *preview = new QLabel(surface);
        preview->setFixedSize(40, 34);
        preview->setPixmap(worldCrop({1448, 583 + 38 * index, 37, 35}));
        materialGrid->addWidget(preview, index, 1);
        auto *combo = new QComboBox(surface);
        for (const auto &material : Materials)
            combo->addItem(material.isEmpty() ? tr("Unassigned") : material, material);
        combo->setAccessibleName(index == 0 ? tr("Surface material") : tr("Material element %1").arg(index));
        materials_[index] = combo;
        materialGrid->addWidget(combo, index, 2);
        connect(combo, &QComboBox::activated, this, &Inspector::apply);
    }
    materialGrid->setColumnStretch(2, 1);

    auto *lodBody = section(sections, tr("LOD"));
    auto *lodForm = new QFormLayout(lodBody);
    lodForm->setContentsMargins(19, 4, 15, 6);
    lodForm->setVerticalSpacing(3);
    lodForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    lodGroup_->addItems({"LargeProp", "SmallProp", "Character", "None"});
    lodGroup_->setAccessibleName(tr("LOD Group"));
    lods_->setRange(1, 8);
    lods_->setAccessibleName(tr("Number of LODs"));
    autoLod_->setAccessibleName(tr("Auto Compute LODs"));
    lodForm->addRow(tr("LOD Group"), lodGroup_);
    lodForm->addRow(tr("Number of LODs"), lods_);
    lodForm->addRow(tr("Auto Compute LODs"), autoLod_);
    collision_->setChecked(true);
    auto *collisionBody = section(sections, tr("Collision"), false);
    auto *collisionLayout = new QHBoxLayout(collisionBody);
    collisionLayout->addWidget(collision_);

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
    for (auto *combo : {group_, mesh_, lodGroup_})
        connect(combo, &QComboBox::activated, this, &Inspector::apply);
    for (auto *check : {visible_, autoLod_, collision_})
        connect(check, &QCheckBox::clicked, this, &Inspector::apply);
    connect(lods_, &QSpinBox::editingFinished, this, &Inspector::apply);
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
    const bool castle = object->id == "castle-main";
    const bool staticMesh = object->type == "Static Mesh";
    name_->setToolTip(object->id);
    group_->setCurrentText(object->group);
    const auto selectedMesh = object->details.value("mesh").toString(staticMesh ? assetName(*object) : QString());
    int meshIndex = mesh_->findText(selectedMesh);
    if (meshIndex < 0 && !selectedMesh.isEmpty()) {
        mesh_->addItem(selectedMesh);
        meshIndex = mesh_->count() - 1;
    }
    mesh_->setCurrentIndex(meshIndex);
    materials_[0]->setCurrentIndex(materials_[0]->findData(object->material));
    materials_[1]->setCurrentIndex(materials_[1]->findData(object->details.value("material1").toString(castle ? "M_Castle_Moss" : "")));
    materials_[2]->setCurrentIndex(materials_[2]->findData(object->details.value("material2").toString(castle ? "M_Castle_Banner" : "")));
    lodGroup_->setCurrentText(object->details.value("lodGroup").toString(staticMesh ? "LargeProp" : "None"));
    lods_->setValue(object->details.value("lods").toInt(castle ? 4 : 1));
    autoLod_->setChecked(object->details.value("autoLod").toBool(staticMesh));
    collision_->setChecked(object->details.value("collision").toBool(true));
    visible_->setChecked(object->visible);
    identity_->setText(object->id);
    const std::array vectors{object->position, object->rotation, object->scale};
    for (size_t i = 0; i < axes_.size(); ++i)
        axes_[i]->setValue(vectors[i / 3][int(i % 3)]);

    const auto kind = staticMesh ? tr("StaticMeshActor") : object->type;
    auto *actor = item(tr("%1 (%2)").arg(object->name, kind), editorIcon("box"));
    componentModel_->appendRow(actor);
    if (staticMesh) {
        for (const auto &component : {tr("StaticMeshComponent (StaticMeshComponent0)"),
            tr("CollisionComponent (Collision)"), tr("LODComponent (LOD)")})
            actor->appendRow(item(component, editorIcon("box")));
    }
    for (const auto &component : object->details.value("components").toArray())
        actor->appendRow(item(component.toString(), editorIcon("box")));
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
    } else if (sender() == materials_[0]) {
        next.material = materials_[0]->currentData().toString();
    } else if (sender() == materials_[1] || sender() == materials_[2]) {
        const int index = sender() == materials_[1] ? 1 : 2;
        next.details.insert("material" + QString::number(index), materials_[index]->currentData().toString());
    } else if (sender() == visible_) {
        next.visible = visible_->isChecked();
    } else if (sender() == mesh_) {
        next.details.insert("mesh", mesh_->currentText());
    } else if (sender() == lodGroup_) {
        next.details.insert("lodGroup", lodGroup_->currentText());
    } else if (sender() == lods_) {
        next.details.insert("lods", lods_->value());
    } else if (sender() == autoLod_) {
        next.details.insert("autoLod", autoLod_->isChecked());
    } else if (sender() == collision_) {
        next.details.insert("collision", collision_->isChecked());
    } else {
        const auto axis = std::ranges::find(axes_, sender());
        if (axis == axes_.end())
            return;
        const auto index = std::distance(axes_.begin(), axis);
        const std::array vectors{&next.position, &next.rotation, &next.scale};
        (*vectors[index / 3])[int(index % 3)] = float((*axis)->value());
    }
    if (next.name != current->name || next.group != current->group || next.material != current->material
        || next.visible != current->visible || next.position != current->position || next.rotation != current->rotation
        || next.scale != current->scale || next.details != current->details)
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
        const bool folder = !index.data(FolderRole).toString().isEmpty();
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
        if (!folder) {
            painter->setPen(QColor("#b2b3c0"));
            painter->drawText(QRect(tile.left(), image.bottom() + 17, tile.width(), 14),
                Qt::AlignCenter, index.data(AssetTypeRole).toString());
        }
        painter->restore();
    }

    QSize sizeHint(const QStyleOptionViewItem &, const QModelIndex &) const override { return {80, 115}; }
};
}

ContentBrowser::ContentBrowser(ProjectStore &store, QWidget *parent)
    : QWidget(parent), store_(store), search_(searchBox(tr("Search Content"), this)),
      folders_(new QTreeView(this)), tiles_(new QListView(this)), folderModel_(new QStandardItemModel(this)),
      assetModel_(new QStandardItemModel(this)), tabs_(new QTabWidget(this)), path_(new QLabel(this)),
      count_(mutedLabel({}, this)), log_(new QPlainTextEdit(this))
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
    auto *messages = new QLabel(tr("No project messages"), tabs_);
    messages->setAlignment(Qt::AlignCenter);
    tabs_->addTab(messages, editorIcon("log"), tr("Message Log"));
    auto *profiler = new QLabel(tr("Profiler preview · sample readings appear in the status bar"), tabs_);
    profiler->setAlignment(Qt::AlignCenter);
    tabs_->addTab(profiler, editorIcon("pulse"), tr("Profiler"));
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
    appendLog(tr("WorldForge frontend initialized."));

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
    for (const auto &kind : QStringList{"Static Mesh", "Character", "Empty"}) {
        addMenu->addAction(kind, this, [this, kind] {
            const auto group = folders_->currentIndex().data(IdRole).toString();
            store_.addObject(kind, Groups.contains(group) ? group : "Props");
        });
    }
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
            appendLog(tr("Import failed: %1").arg(error));
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
        if (const auto *object = store_.selectedObject())
            QApplication::clipboard()->setText(assetName(*object));
    });
    connect(link, &QToolButton::clicked, this, [this] {
        if (const auto *object = store_.selectedObject())
            QApplication::clipboard()->setText(object->id);
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
    auto *project = item("MythicValley", editorIcon("box"));
    auto *root = item(tr("Content"), editorIcon("folderFill", QColor("#f3ce63")));
    folderModel_->appendRow(project);
    project->appendRow(root);
    for (const auto &group : QStringList{"Environment", "Megascans", "Props", "Ruins", "Characters",
        "Materials", "Textures", "Blueprints", "Maps", "Lighting", "Gameplay"}) {
        auto *row = item(group, editorIcon("folderFill", QColor("#f3ce63")));
        row->setData(group == "Characters" ? "Props" : group, IdRole);
        root->appendRow(row);
    }
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
        if (!id.isEmpty())
            store_.select(id);
    });
    connect(tiles_, &QListView::activated, this, [this, root](const QModelIndex &index) {
        const auto folder = index.data(FolderRole).toString();
        if (folder.isEmpty())
            return;
        for (int row = 0; row < root->rowCount(); ++row) {
            if (root->child(row)->text() == folder) {
                folders_->setCurrentIndex(root->child(row)->index());
                return;
            }
        }
        search_->setText(folder == "Meshes" ? "Static Mesh" : folder);
    });
    connect(&store_, &ProjectStore::sceneChanged, this, &ContentBrowser::rebuild);
    connect(&store_, &ProjectStore::selectionChanged, this, &ContentBrowser::syncSelection);
    rebuild();
}

void ContentBrowser::rebuild()
{
    const QSignalBlocker block(tiles_->selectionModel());
    assetModel_->clear();
    const auto *project = store_.project();
    const auto query = search_->text().trimmed();
    auto folder = folders_->currentIndex().data(IdRole).toString();
    const bool referenceProject = project && project->id == "mythic-valley";
    if (folder.isEmpty() && referenceProject && query.isEmpty())
        folder = "Ruins";
    path_->setText(folder == "Ruins" ? tr("Content   ›   Environment   ›   Ruins   ›")
        : (folder.isEmpty() ? tr("Content   ›") : tr("Content   ›   %1   ›").arg(folder)));

    if (project) {
        if (referenceProject && query.isEmpty() && folder == "Ruins") {
            const QStringList names{tr("Meshes"), tr("Materials"), tr("Textures")};
            for (const auto &name : names) {
                auto *tile = item(name, QIcon(worldCrop({553, 741, 65, 63})));
                tile->setData(name, AssetNameRole);
                tile->setData(name, FolderRole);
                assetModel_->appendRow(tile);
            }
        }
        QList<const SceneObject *> assets;
        for (const auto &object : project->objects) {
            if ((folder.isEmpty() || object.group == folder)
                && (object.name.contains(query, Qt::CaseInsensitive) || object.type.contains(query, Qt::CaseInsensitive)
                    || assetName(object).contains(query, Qt::CaseInsensitive)))
                assets.append(&object);
        }
        if (sort_ != 0) {
            std::ranges::sort(assets, [this](const SceneObject *a, const SceneObject *b) {
                if (sort_ == 3 && a->type != b->type)
                    return a->type.localeAwareCompare(b->type) < 0;
                const int compare = a->name.localeAwareCompare(b->name);
                return sort_ == 2 ? compare > 0 : compare < 0;
            });
        }
        const auto statue = std::ranges::find_if(assets, [](const SceneObject *object) {
            return object->id == "statue-left";
        });
        const bool pairedStatue = statue != assets.end() && query.isEmpty() && folder == "Ruins";
        for (const auto *object : assets) {
            if (object->id == "statue-right" && pairedStatue)
                continue;
            auto *tile = item(object->name, thumbnail(*object));
            tile->setData(object->id, IdRole);
            tile->setData(assetName(*object), AssetNameRole);
            tile->setData(object->type, AssetTypeRole);
            tile->setToolTip(assetName(*object) + '\n' + object->type + "  /  " + object->group);
            assetModel_->appendRow(tile);
        }
    }
    syncSelection();
}

void ContentBrowser::syncSelection()
{
    const QSignalBlocker block(tiles_->selectionModel());
    const auto matches = store_.selection().isEmpty() ? QModelIndexList{}
        : assetModel_->match(assetModel_->index(0, 0), IdRole, store_.selection(), 1, Qt::MatchExactly);
    if (matches.isEmpty())
        tiles_->selectionModel()->clear();
    else
        tiles_->setCurrentIndex(matches.first());
    const int count = assetModel_->rowCount();
    count_->setText((count == 1 ? tr("1 item") : tr("%1 items").arg(count))
        + (matches.isEmpty() ? QString() : tr(" (1 selected)")));
}

void ContentBrowser::appendLog(const QString &message)
{
    log_->appendPlainText(QDateTime::currentDateTime().toString("HH:mm:ss  ") + message);
}

void ContentBrowser::showMessages()
{
    tabs_->setCurrentIndex(2);
}
