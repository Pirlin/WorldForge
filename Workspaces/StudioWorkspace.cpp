#include "Workspaces/StudioWorkspace.h"

#include "Components/Viewport.h"
#include "DesignSystem/Icons.h"
#include "State/ProjectStore.h"
#include "Workspaces/StudioViews.h"
#include "Workspaces/WorkspaceCatalog.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFormLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QPlainTextEdit>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSlider>
#include <QSplitter>
#include <QTabBar>
#include <QTabWidget>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>

namespace {
constexpr int ObjectRole = Qt::UserRole;
constexpr int KeyRole = Qt::UserRole + 1;

const QJsonObject &studioConfigurations()
{
    static const QJsonObject configurations = [] {
        QFile file(":/Data/studios.json");
        if (!file.open(QIODevice::ReadOnly))
            qFatal("WorldForge studio configurations are missing");
        return QJsonDocument::fromJson(file.readAll()).object();
    }();
    return configurations;
}

QLineEdit *searchBox(QWidget *parent, const QString &label)
{
    auto *search = new QLineEdit(parent);
    search->setPlaceholderText(label + "…");
    search->setAccessibleName(label);
    search->setClearButtonEnabled(true);
    search->addAction(editorIcon("search"), QLineEdit::LeadingPosition);
    return search;
}

void configureTree(QTreeWidget *tree, const QStringList &headers)
{
    tree->setHeaderLabels(headers);
    tree->setRootIsDecorated(false);
    tree->setUniformRowHeights(true);
    tree->setIconSize({15, 15});
    tree->header()->setStretchLastSection(false);
    tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    for (int column = 1; column < headers.size(); ++column)
        tree->header()->setSectionResizeMode(column, QHeaderView::ResizeToContents);
}

QLabel *heading(const QString &title, QWidget *parent)
{
    auto *label = new QLabel(title, parent);
    label->setTextFormat(Qt::PlainText);
    label->setProperty("heading", true);
    return label;
}

QWidget *labelledView(const QJsonArray &description, QWidget *contents, QWidget *parent,
                     const QList<QWidget *> &commands = {})
{
    auto *pane = new QWidget(parent);
    auto *layout = new QVBoxLayout(pane);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(1);
    auto *header = new QWidget(pane);
    header->setObjectName("viewportHeader");
    header->setFixedHeight(26);
    auto *row = new QHBoxLayout(header);
    row->setContentsMargins(8, 0, 8, 0);
    auto *icon = new QLabel(header);
    icon->setPixmap(editorIcon("forge", QColor("#a95cff")).pixmap(14));
    row->addWidget(icon);
    row->addWidget(heading(studioViewName(description), header), 1);
    auto *preview = new QLabel(QObject::tr("Preview"), header);
    preview->setProperty("muted", true);
    row->addWidget(preview);
    layout->addWidget(header);
    for (auto *command : commands)
        layout->addWidget(command);
    layout->addWidget(contents, 1);
    pane->setAccessibleName(studioViewName(description));
    return pane;
}

QString codeSample(const QString &file)
{
    if (file.endsWith(".json"))
        return "{\n    \"moveForward\": \"W\",\n    \"moveBackward\": \"S\",\n    \"jump\": \"Space\"\n}\n";
    if (file.endsWith(".h"))
        return QString("#pragma once\n\nclass %1 {\npublic:\n    void update(float deltaSeconds);\n    void setInput(float input);\n\nprivate:\n    float movementSpeed = 4.5f;\n    float moveInput = 0.0f;\n};\n").arg(file.chopped(2));
    if (file == "HeroCharacter.cpp")
        return "#include \"HeroCharacter.h\"\n#include \"CombatComponent.h\"\n#include \"HealthComponent.h\"\n#include \"HeroAbility.h\"\n\n"
               "AHeroCharacter::AHeroCharacter()\n{\n    // Demo actor initialization\n"
               "    CombatComp = CreateDefaultSubobject<UCombatComponent>(TEXT(\"CombatComp\"));\n"
               "    HealthComp = CreateDefaultSubobject<UHealthComponent>(TEXT(\"HealthComp\"));\n"
               "    AbilityComp = CreateDefaultSubobject<UHeroAbility>(TEXT(\"AbilityComp\"));\n"
               "    PrimaryActorTick.bCanEverTick = true;\n}\n\n"
               "void AHeroCharacter::BeginPlay()\n{\n    Super::BeginPlay();\n"
               "    AbilityComp->InitializeAbilities();\n"
               "    HealthComp->OnHealthChanged.AddDynamic(this, &AHeroCharacter::HandleHealthChanged);\n}\n\n"
               "void AHeroCharacter::Tick(float DeltaTime)\n{\n    Super::Tick(DeltaTime);\n}\n";
    if (file == "CombatComponent.cpp" || file == "HealthComponent.cpp") {
        const auto type = file.chopped(4);
        return QString("#include \"%1.h\"\n\nvoid U%1::BeginPlay()\n{\n    Super::BeginPlay();\n    // Sample source buffer\n}\n").arg(type);
    }
    if (file.startsWith("WorldModule"))
        return "// Frontend sample source\n\nstruct WorldModule {\n    bool initialized = false;\n\n    void initialize() {\n        initialized = true;\n    }\n};\n";
    return "#include \"HeroController.h\"\n\n// Frontend sample source\nvoid HeroController::update(float deltaSeconds)\n{\n    const float distance = movementSpeed * moveInput * deltaSeconds;\n    // The engine will consume distance after integration.\n}\n\nvoid HeroController::setInput(float input)\n{\n    moveInput = input;\n}\n";
}
}

StudioWorkspace::StudioWorkspace(const Workspace &workspace, ProjectStore &store, QObject *parent)
    : QObject(parent), store_(store), config_(studioConfigurations().value(workspace.name).toObject()),
      reference_(workspace.reference),
      navigator_(new QTreeWidget), navSearch_(searchBox(navigator_, tr("Search %1").arg(navigatorTitle()))),
      inspector_(createTabs(1)), tools_(createTabs(2)), properties_(new QScrollArea(inspector_))
{
    Q_ASSERT(!config_.isEmpty());
    surface_ = createSurface(workspace);
    auto *navigation = new QWidget;
    auto *column = new QVBoxLayout(navigation);
    column->setContentsMargins(5, 6, 5, 5);
    column->setSpacing(5);
    column->addWidget(navSearch_);
    column->addWidget(navigator_, 1);
    configureTree(navigator_, {tr("Name"), tr("Type")});
    navigator_->setRootIsDecorated(true);
    navigator_->setIndentation(13);
    const auto library = config_.value("library").toArray();
    if (!library.isEmpty()) {
        auto *gallery = createStudioView(library, navigation, reference_);
        column->addWidget(heading(studioViewName(library), navigation));
        column->addWidget(gallery, 1);
        if (auto *tiles = gallery->findChild<QListWidget *>())
            connect(tiles, &QListWidget::itemClicked, this, [this](const QListWidgetItem *item) { selectNamedItem(item->text()); });
    }
    navigation->setAccessibleName(navigatorTitle());
    navigator_->setAccessibleName(navigatorTitle());
    inspector_->setAccessibleName(inspectorTitle());
    tools_->setAccessibleName(toolsTitle());

    properties_->setWidgetResizable(true);
    properties_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    inspector_->addTab(properties_, editorIcon(workspace.icon), inspectorTitle());
    const auto settings = config_.value("settings").toArray();
    if (settings.size() == 2) {
        auto *scroll = new QScrollArea(inspector_);
        scroll->setWidgetResizable(true);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        auto *page = new QWidget(scroll);
        auto *settingsLayout = new QVBoxLayout(page);
        settingsLayout->setContentsMargins(6, 6, 6, 6);
        settingsLayout->setSpacing(2);
        buildProperties(settingsLayout, settings.at(1).toArray(), "studio-settings");
        settingsLayout->addStretch();
        scroll->setWidget(page);
        inspector_->addTab(scroll, editorIcon("settings"), settings.at(0).toString());
    }
    const auto descriptions = config_.value("tools").toArray();
    for (const auto &description : descriptions) {
        const auto view = description.toArray();
        const auto kind = view.at(1).toString();
        const auto icon = kind == "timeline" ? "film" : kind == "waveform" ? "audio" : kind == "graph" ? "graph"
            : kind == "metrics" ? "pulse" : kind == "log" ? "log" : "list";
        auto *page = createStudioView(view, tools_, reference_);
        tools_->addTab(page, editorIcon(icon), studioViewName(view));
        for (auto *gallery : page->findChildren<QListWidget *>()) {
            if (gallery->objectName() == "studioGallery") {
                connect(gallery, &QListWidget::itemClicked, this, [this](const QListWidgetItem *item) {
                    selectNamedItem(item->text());
                });
            }
        }
    }

    connect(navSearch_, &QLineEdit::textChanged, this, &StudioWorkspace::filterNavigator);
    connect(navigator_, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem *row) {
        if (row) {
            const auto id = row->data(0, ObjectRole).toString();
            if (!id.isEmpty())
                store_.select(id);
            if (code_ && !row->childCount())
                openCodeFile(row->text(0));
        }
        populateAssets();
        refreshInspector();
    });
    connect(&store_, &ProjectStore::projectsChanged, this, [this] {
        inspectorKey_.clear();
        rebuildNavigator();
    });
    connect(&store_, &ProjectStore::sceneChanged, this, &StudioWorkspace::rebuildNavigator);
    connect(&store_, &ProjectStore::selectionChanged, this, &StudioWorkspace::syncSelection);
    rebuildNavigator();
}

QWidget *StudioWorkspace::navigator() const { return navigator_->parentWidget(); }
QWidget *StudioWorkspace::inspector() const { return inspector_; }
QWidget *StudioWorkspace::tools() const { return tools_; }
QString StudioWorkspace::navigatorTitle() const { return config_.value("navigator").toString(); }
QString StudioWorkspace::inspectorTitle() const { return config_.value("inspector").toString(); }
QString StudioWorkspace::toolsTitle() const { return studioViewName(config_.value("tools").toArray().at(0).toArray()); }

QList<int> StudioWorkspace::dockSizes() const
{
    const auto sizes = config_.value("dockSizes").toArray();
    return sizes.size() == 3 ? QList<int>{sizes.at(0).toInt(), sizes.at(1).toInt(), sizes.at(2).toInt()}
                             : QList<int>{265, 322, 242};
}

QTabWidget *StudioWorkspace::createTabs(int dock)
{
    auto *tabs = new QTabWidget;
    tabs->setIconSize({14, 14});
    auto *close = new QToolButton(tabs);
    close->setIcon(editorIcon("close"));
    close->setProperty("flatIcon", true);
    close->setToolTip(tr("Close panel"));
    close->setAccessibleName(tr("Close panel"));
    connect(close, &QToolButton::clicked, this, [this, dock] { emit closeRequested(dock); });
    tabs->setCornerWidget(close);
    tabs->tabBar()->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(tabs->tabBar(), &QTabBar::customContextMenuRequested, this, [this, tabs, dock](const QPoint &point) {
        QMenu menu(tabs);
        menu.addAction(tr("Float or dock panel"), this, [this, dock] { emit floatRequested(dock); });
        menu.exec(tabs->tabBar()->mapToGlobal(point));
    });
    return tabs;
}

QWidget *StudioWorkspace::createSurface(const Workspace &workspace)
{
    const auto description = config_.value("surface").toArray();
    const auto kind = description.at(1).toString();
    auto *pane = new QWidget;
    QWidget *contents = nullptr;
    if (kind == "art") {
        auto *viewport = new Viewport(store_, pane);
        viewport->setWorkspace(workspace);
        viewport->setMinimumSize(205, 150);
        const auto overlay = config_.value("viewportControls").toArray();
        if (!overlay.isEmpty()) {
            auto *controls = new QVBoxLayout(viewport);
            controls->setContentsMargins(6, 5, 6, 0);
            auto *row = createCommandRow(overlay, viewport);
            row->setObjectName("studioViewportBar");
            controls->addWidget(row);
            controls->addStretch();
        }
        contents = viewport;
    } else if (kind == "assets") {
        auto *inventory = new QWidget(pane);
        auto *layout = new QVBoxLayout(inventory);
        layout->setContentsMargins(5, 5, 5, 5);
        assetSearch_ = searchBox(inventory, tr("Search Project Assets"));
        layout->addWidget(assetSearch_);
        assets_ = new QTreeWidget(inventory);
        configureTree(assets_, {tr("Asset"), tr("Type"), tr("Collection")});
        assets_->setAccessibleName(tr("Asset Inventory"));
        layout->addWidget(assets_, 1);
        connect(assetSearch_, &QLineEdit::textChanged, this, &StudioWorkspace::populateAssets);
        connect(assets_, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem *item) {
            if (item)
                store_.select(item->data(0, ObjectRole).toString());
        });
        contents = inventory;
    } else if (kind == "build") {
        auto *configuration = new QWidget(pane);
        auto *column = new QVBoxLayout(configuration);
        column->setContentsMargins(5, 4, 5, 4);
        const auto form = config_.value("configuration").toArray();
        auto *scroll = new QScrollArea(configuration);
        scroll->setWidgetResizable(true);
        auto *formBody = new QWidget(scroll);
        auto *formLayout = new QVBoxLayout(formBody);
        formLayout->setContentsMargins(5, 5, 5, 5);
        buildProperties(formLayout, form, "build-settings");
        formLayout->addStretch();
        scroll->setWidget(formBody);
        column->addWidget(scroll, 3);
        const auto cooking = config_.value("cooking").toArray();
        column->addWidget(labelledView(cooking, createStudioView(cooking, configuration, reference_), configuration), 2);
        contents = configuration;
    } else {
        contents = createStudioView(description, pane, reference_);
        if (kind == "code")
            code_ = qobject_cast<QPlainTextEdit *>(contents);
    }

    auto *layout = new QVBoxLayout(pane);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(3);
    QList<QWidget *> commands;
    for (const auto &row : config_.value("commandRows").toArray())
        commands.append(createCommandRow(row.toArray(), pane));
    if (!config_.value("commandsInViewport").toBool()) {
        for (auto *command : commands)
            layout->addWidget(command);
        commands.clear();
    }
    if (code_) {
        codeTabs_ = new QTabBar(pane);
        codeTabs_->setAccessibleName(tr("Source Files"));
        codeTabs_->setExpanding(false);
        const auto sourceFiles = config_.value("sourceTabs").toArray();
        for (const auto &file : sourceFiles)
            codeTabs_->addTab(editorIcon("code"), file.toString());
        connect(codeTabs_, &QTabBar::currentChanged, this, [this](int tab) { selectNamedItem(codeTabs_->tabText(tab)); });
        layout->addWidget(codeTabs_);
    }
    const auto companion = config_.value("companion").toArray();
    if (companion.isEmpty()) {
        layout->addWidget(labelledView(description, contents, pane, commands));
    } else {
        auto *split = new QSplitter(Qt::Horizontal, pane);
        split->addWidget(labelledView(description, contents, split, commands));
        split->addWidget(labelledView(companion, createStudioView(companion, split, reference_), split));
        split->setChildrenCollapsible(false);
        split->setStretchFactor(0, 3);
        split->setStretchFactor(1, 2);
        split->setSizes({650, 350});
        layout->addWidget(split);
    }
    return pane;
}

QWidget *StudioWorkspace::createCommandRow(const QJsonArray &commands, QWidget *parent)
{
    auto *scroll = new QScrollArea(parent);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setFixedHeight(33);
    auto *bar = new QWidget(scroll);
    bar->setObjectName("studioCommandBar");
    auto *row = new QHBoxLayout(bar);
    row->setContentsMargins(6, 2, 6, 2);
    row->setSpacing(5);
    auto *modes = new QButtonGroup(bar);
    modes->setExclusive(true);
    for (const auto &value : commands) {
        if (value.toObject().contains("check")) {
            const auto state = value.toObject();
            auto *check = new QCheckBox(state.value("check").toString(), bar);
            check->setChecked(state.value("checked").toBool());
            check->setToolTip(tr("%1 preview").arg(check->text()));
            row->addWidget(check);
            continue;
        }
        if (value.toObject().contains("field")) {
            const auto field = value.toObject();
            const auto label = field.value("field").toString();
            row->addWidget(new QLabel(label, bar));
            auto *spin = new QDoubleSpinBox(bar);
            spin->setRange(field.value("min").toDouble(), field.value("max").toDouble(1));
            spin->setDecimals(field.value("decimals").toInt(2));
            spin->setButtonSymbols(QAbstractSpinBox::NoButtons);
            spin->setSuffix(field.value("suffix").toString());
            spin->setValue(field.value("value").toDouble());
            spin->setFixedWidth(58);
            spin->setAccessibleName(label + tr(" toolbar"));
            commandFields_.insert(label, spin);
            row->addWidget(spin);
            connect(spin, &QDoubleSpinBox::valueChanged, this, [this, label](double number) {
                for (auto *field : properties_->findChildren<QDoubleSpinBox *>()) {
                    if (field->accessibleName() == label) {
                        field->setValue(number);
                        break;
                    }
                }
            });
            continue;
        }
        if (value.isArray()) {
            const auto choices = value.toArray();
            auto *combo = new QComboBox(bar);
            for (const auto &choice : choices)
                combo->addItem(choice.toString());
            combo->setAccessibleName(combo->currentText());
            row->addWidget(combo);
            continue;
        }
        const auto label = value.isObject() ? value.toObject().value("mode").toString() : value.toString();
        if (label == "|") {
            row->addSpacing(9);
            continue;
        }
        auto *button = new QToolButton(bar);
        button->setText(label);
        button->setCheckable(true);
        button->setToolTip(tr("%1 preview").arg(label));
        modes->addButton(button);
        if (modes->buttons().size() == 1 || value.toObject().value("selected").toBool())
            button->setChecked(true);
        row->addWidget(button);
        connect(button, &QToolButton::clicked, this, [this, label] {
            for (auto *combo : inspector_->findChildren<QComboBox *>()) {
                if (combo->findText(label) >= 0) {
                    combo->setCurrentText(label);
                    break;
                }
            }
        });
    }
    row->addStretch();
    scroll->setWidget(bar);
    return scroll;
}

void StudioWorkspace::rebuildNavigator()
{
    const QSignalBlocker block(navigator_);
    const auto old = navigator_->currentItem();
    const auto previousKey = old ? old->data(0, KeyRole).toString() : QString();
    navigator_->clear();
    QTreeWidgetItem *previous = nullptr;
    const auto add = [&](QTreeWidgetItem *parent, const QString &name, const QString &type, const QString &key, const QString &object) {
        auto *row = new QTreeWidgetItem(parent, {name, type});
        row->setIcon(0, editorIcon(assets_ || type == "Folder" ? "folder" : type == "Bone" ? "graph"
            : (object.isEmpty() ? "layers" : "box")));
        row->setData(0, KeyRole, key);
        row->setData(0, ObjectRole, object);
        if (key == previousKey)
            previous = row;
        return row;
    };
    auto *root = navigator_->invisibleRootItem();
    if (assets_) {
        add(root, tr("All Project Assets"), tr("Collection"), {}, {});
        if (const auto *project = store_.project()) {
            QStringList groups;
            for (const auto &object : project->objects) {
                if (!groups.contains(object.group))
                    groups.append(object.group);
            }
            groups.sort(Qt::CaseInsensitive);
            for (const auto &group : groups)
                add(root, group, tr("Collection"), group, {});
        }
    } else {
        const auto addRows = [&](auto &&self, QTreeWidgetItem *parent, const QJsonArray &rows, const QString &path) -> void {
            for (const auto &value : rows) {
                const auto row = value.toArray();
                const auto id = row.size() > 2 ? row.at(2).toString() : QString();
                const auto *record = store_.object(id);
                if (!id.isEmpty() && !record)
                    continue;
                const auto label = row.at(0).toString();
                const auto key = path + '/' + label;
                auto *item = add(parent, record ? record->name : label, row.at(1).toString(), record ? id : key, id);
                if (row.size() > 3)
                    self(self, item, row.at(3).toArray(), key);
            }
        };
        addRows(addRows, root, config_.value("items").toArray(), {});
    }

    navigator_->setCurrentItem(previous ? previous : navigator_->topLevelItem(0));
    if (!previous)
        selectNamedItem(config_.value("selection").toString());
    navigator_->expandAll();
    filterNavigator();
    syncSelection();
    if (code_ && navigator_->currentItem() && !navigator_->currentItem()->childCount())
        openCodeFile(navigator_->currentItem()->text(0));
    populateAssets();
}

void StudioWorkspace::filterNavigator()
{
    const auto query = navSearch_->text().trimmed();
    const auto filter = [&](auto &&self, QTreeWidgetItem *item, bool ancestorMatches) -> bool {
        const bool match = ancestorMatches || item->text(0).contains(query, Qt::CaseInsensitive)
            || item->text(1).contains(query, Qt::CaseInsensitive);
        bool childMatches = false;
        for (int index = 0; index < item->childCount(); ++index)
            childMatches = self(self, item->child(index), match) || childMatches;
        item->setHidden(!match && !childMatches);
        if (childMatches && !query.isEmpty())
            item->setExpanded(true);
        return match || childMatches;
    };
    for (int index = 0; index < navigator_->topLevelItemCount(); ++index)
        filter(filter, navigator_->topLevelItem(index), false);
}

void StudioWorkspace::selectNamedItem(const QString &name)
{
    if (name.isEmpty())
        return;
    const auto unprefixed = name.startsWith("SM_") ? name.mid(3) : name;
    for (QTreeWidgetItemIterator row(navigator_); *row; ++row) {
        if ((*row)->text(0) == name || (*row)->text(0) == unprefixed || (*row)->data(0, KeyRole).toString() == name) {
            navigator_->setCurrentItem(*row);
            return;
        }
    }
}

void StudioWorkspace::syncSelection()
{
    const QSignalBlocker block(navigator_);
    const auto selection = store_.selection();
    if (!selection.isEmpty()) {
        for (QTreeWidgetItemIterator row(navigator_); *row; ++row) {
            if ((*row)->data(0, ObjectRole).toString() == selection) {
                navigator_->setCurrentItem(*row);
                break;
            }
        }
    }
    if (assets_) {
        const QSignalBlocker assetBlock(assets_);
        QTreeWidgetItem *selected = nullptr;
        if (!selection.isEmpty()) {
            for (int index = 0; index < assets_->topLevelItemCount(); ++index) {
                auto *row = assets_->topLevelItem(index);
                if (row->data(0, ObjectRole).toString() == selection) {
                    selected = row;
                    break;
                }
            }
        }
        assets_->setCurrentItem(selected);
    }
    refreshInspector();
}

void StudioWorkspace::populateAssets()
{
    if (!assets_)
        return;
    const QSignalBlocker block(assets_);
    assets_->clear();
    if (const auto *project = store_.project()) {
        const auto *current = navigator_->currentItem();
        const auto group = current ? current->data(0, KeyRole).toString() : QString();
        const auto query = assetSearch_->text().trimmed();
        for (const auto &object : project->objects) {
            if ((!group.isEmpty() && group != object.group)
                || (!object.name.contains(query, Qt::CaseInsensitive) && !object.type.contains(query, Qt::CaseInsensitive)))
                continue;
            auto *row = new QTreeWidgetItem(assets_, {object.name, object.type, object.group});
            row->setIcon(0, editorIcon("box"));
            row->setData(0, ObjectRole, object.id);
            if (object.id == store_.selection())
                assets_->setCurrentItem(row);
        }
    }
    refreshInspector();
}

void StudioWorkspace::openCodeFile(const QString &file)
{
    const auto *project = store_.project();
    const auto key = (project ? project->id : QString()) + '/' + file;
    if (activeFile_ == key)
        return;
    if (!activeFile_.isEmpty())
        buffers_.insert(activeFile_, code_->toPlainText());
    activeFile_ = key;
    code_->setPlainText(buffers_.value(key, codeSample(file)));
    code_->setAccessibleName(file);
    if (codeTabs_) {
        const QSignalBlocker block(codeTabs_);
        for (int index = 0; index < codeTabs_->count(); ++index) {
            if (codeTabs_->tabText(index) == file) {
                codeTabs_->setCurrentIndex(index);
                break;
            }
        }
    }
}

QString StudioWorkspace::currentKey() const
{
    const auto *project = store_.project();
    const auto *row = navigator_->currentItem();
    const auto *object = inspectedObject();
    const auto key = object ? object->id : row ? row->data(0, KeyRole).toString() : QString();
    return (project ? project->id : QString()) + QChar(0x1f) + key;
}

const SceneObject *StudioWorkspace::inspectedObject() const
{
    const auto *row = assets_ ? assets_->currentItem() : navigator_->currentItem();
    return row ? store_.object(row->data(0, ObjectRole).toString()) : nullptr;
}

void StudioWorkspace::refreshInspector()
{
    const auto key = currentKey();
    const auto *row = navigator_->currentItem();
    const auto *object = inspectedObject();
    if (key == inspectorKey_) {
        if (boundName_ && object && boundName_->text() != object->name)
            boundName_->setText(object->name);
        return;
    }
    inspectorKey_ = key;
    boundName_ = nullptr;
    auto *content = new QWidget;
    content->setObjectName("panelBody");
    auto *column = new QVBoxLayout(content);
    column->setContentsMargins(7, 7, 7, 7);
    column->setSpacing(5);
    if (object) {
        const auto id = object->id;
        auto *name = new QLineEdit(object->name, content);
        name->setAccessibleName(tr("Shared object name"));
        name->setMaxLength(64);
        name->setObjectName("objectTitle");
        boundName_ = name;
        column->addWidget(name);
        auto *identity = new QLabel(tr("Object ID  ·  %1").arg(id), content);
        identity->setProperty("muted", true);
        identity->setTextInteractionFlags(Qt::TextSelectableByMouse);
        column->addWidget(identity);
        connect(name, &QLineEdit::editingFinished, this, [this, name, id] {
            const auto *object = store_.object(id);
            if (!object)
                return;
            const auto text = name->text().trimmed();
            if (text.isEmpty() || text == object->name) {
                name->setText(object->name);
                return;
            }
            auto next = *object;
            next.name = text;
            store_.updateObject(next, tr("Rename %1").arg(object->name));
        });
    } else {
        column->addWidget(heading(row ? row->text(0) : tr("No selection"), content));
        if (row) {
            auto *type = new QLabel(row->text(1), content);
            type->setProperty("muted", true);
            column->addWidget(type);
        }
    }
    buildProperties(column, config_.value("properties").toArray(), key);
    column->addStretch();
    if (auto *previous = properties_->takeWidget()) {
        previous->hide();
        previous->deleteLater();
    }
    properties_->setWidget(content);
    for (auto *number : content->findChildren<QDoubleSpinBox *>()) {
        if (auto *toolbar = commandFields_.value(number->accessibleName())) {
            const QSignalBlocker block(toolbar);
            toolbar->setValue(number->value());
            connect(number, &QDoubleSpinBox::valueChanged, toolbar, [toolbar](double value) {
                const QSignalBlocker blocker(toolbar);
                toolbar->setValue(value);
            });
        }
    }
}

void StudioWorkspace::buildProperties(QVBoxLayout *layout, const QJsonArray &sections, const QString &key)
{
    for (const auto &sectionValue : sections) {
        const auto section = sectionValue.toArray();
        const auto title = section.at(0).toString();
        auto *header = new QToolButton;
        header->setObjectName("studioSection");
        header->setText(title);
        header->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        header->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        header->setCheckable(true);
        const auto sectionKey = key + QChar(0x1f) + title;
        const bool expanded = values_.value(sectionKey, section.size() < 3 || section.at(2).toBool()).toBool();
        header->setChecked(expanded);
        header->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
        layout->addWidget(header);
        auto *body = new QWidget;
        body->setObjectName("studioSectionBody");
        body->setVisible(expanded);
        auto *form = new QFormLayout(body);
        form->setContentsMargins(7, 3, 7, 5);
        form->setVerticalSpacing(3);
        form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        layout->addWidget(body);
        connect(header, &QToolButton::toggled, body, [this, header, body, sectionKey](bool open) {
            header->setArrowType(open ? Qt::DownArrow : Qt::RightArrow);
            body->setVisible(open);
            values_.insert(sectionKey, open);
        });
        for (const auto &fieldValue : section.at(1).toArray()) {
            const auto field = fieldValue.toArray();
            const auto label = field.at(0).toString();
            const auto initial = field.at(1);
            const auto fieldKey = key + QChar(0x1f) + title + QChar(0x1f) + label;
            const auto remember = [this, fieldKey](const QVariant &value) { values_.insert(fieldKey, value); };
            QWidget *control = nullptr;
            const auto spec = initial.toObject();
            if (spec.contains("vector")) {
                auto *row = new QWidget;
                auto *axes = new QHBoxLayout(row);
                axes->setContentsMargins(0, 0, 0, 0);
                axes->setSpacing(4);
                const auto coords = spec.value("vector").toArray();
                const auto saved = values_.value(fieldKey).toList();
                const QStringList names{"X", "Y", "Z"};
                const QStringList colors{"#ff9779", "#9ed875", "#83beff"};
                QList<QDoubleSpinBox *> spins;
                for (int axis = 0; axis < 3; ++axis) {
                    auto *axisLabel = new QLabel(names[axis], row);
                    axisLabel->setStyleSheet("color:" + colors[axis]);
                    axes->addWidget(axisLabel);
                    auto *spin = new QDoubleSpinBox(row);
                    spin->setRange(-100000, 100000);
                    spin->setDecimals(3);
                    spin->setButtonSymbols(QAbstractSpinBox::NoButtons);
                    spin->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
                    spin->setMinimumWidth(35);
                    spin->setValue(saved.size() == 3 ? saved.at(axis).toDouble() : coords.at(axis).toDouble());
                    spin->setAccessibleName(label + " " + names[axis]);
                    axes->addWidget(spin, 1);
                    spins.append(spin);
                }
                for (auto *spin : spins)
                    connect(spin, &QDoubleSpinBox::valueChanged, this, [spins, remember] {
                        remember(QVariantList{spins[0]->value(), spins[1]->value(), spins[2]->value()});
                    });
                control = row;
            } else if (spec.contains("view")) {
                control = createStudioView(spec.value("view").toArray(), body, reference_);
                control->setFixedHeight(spec.value("height").toInt(150));
            } else if (spec.contains("stack")) {
                auto *stack = new QListWidget;
                stack->setDragDropMode(QAbstractItemView::InternalMove);
                stack->setUniformItemSizes(true);
                stack->setIconSize({14, 14});
                const auto order = values_.value(fieldKey).toList();
                const auto addModifier = [stack](const QString &name, bool visible) {
                    auto *item = new QListWidgetItem(editorIcon("box"), name, stack);
                    item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
                    item->setCheckState(visible ? Qt::Checked : Qt::Unchecked);
                };
                if (order.isEmpty()) {
                    for (const auto &entry : spec.value("stack").toArray())
                        addModifier(entry.toString(), true);
                } else {
                    for (const auto &entry : order) {
                        const auto modifier = entry.toMap();
                        addModifier(modifier.value("name").toString(), modifier.value("visible").toBool());
                    }
                }
                stack->setFixedHeight(std::min(stack->count(), 5) * 21 + 4);
                stack->setCurrentRow(0);
                const auto rememberStack = [stack, remember] {
                    QVariantList modifiers;
                    for (int i = 0; i < stack->count(); ++i) {
                        const auto *item = stack->item(i);
                        modifiers.append(QVariantMap{{"name", item->text()}, {"visible", item->checkState() == Qt::Checked}});
                    }
                    remember(modifiers);
                };
                connect(stack, &QListWidget::itemChanged, this, rememberStack);
                connect(stack->model(), &QAbstractItemModel::rowsMoved, this, rememberStack);
                if (spec.contains("add")) {
                    auto *pane = new QWidget;
                    auto *column = new QVBoxLayout(pane);
                    column->setContentsMargins(0, 0, 0, 0);
                    column->setSpacing(3);
                    auto *add = new QToolButton(pane);
                    add->setText(tr("Add Modifier"));
                    add->setIcon(editorIcon("plus"));
                    add->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
                    add->setPopupMode(QToolButton::InstantPopup);
                    auto *menu = new QMenu(add);
                    for (const auto &entry : spec.value("add").toArray()) {
                        const auto modifier = entry.toString();
                        menu->addAction(modifier, stack, [addModifier, rememberStack, stack, modifier] {
                            addModifier(modifier, true);
                            stack->setCurrentRow(stack->count() - 1);
                            rememberStack();
                        });
                    }
                    add->setMenu(menu);
                    column->addWidget(add, 0, Qt::AlignLeft);
                    column->addWidget(stack);
                    control = pane;
                } else {
                    control = stack;
                }
            } else if (spec.contains("value")) {
                auto *row = new QWidget;
                auto *controls = new QHBoxLayout(row);
                controls->setContentsMargins(0, 0, 0, 0);
                controls->setSpacing(6);
                const double lower = spec.value("min").toDouble();
                const double upper = spec.value("max").toDouble(1);
                auto *slider = new QSlider(Qt::Horizontal, row);
                slider->setRange(0, 1000);
                slider->setAccessibleName(label + tr(" slider"));
                auto *spin = new QDoubleSpinBox(row);
                spin->setRange(lower, upper);
                spin->setDecimals(spec.value("decimals").toInt(2));
                spin->setSingleStep(spec.value("step").toDouble(0.01));
                spin->setSuffix(spec.value("suffix").toString());
                spin->setButtonSymbols(QAbstractSpinBox::NoButtons);
                spin->setAccessibleName(label);
                spin->setFixedWidth(55);
                spin->setValue(values_.value(fieldKey, spec.value("value").toDouble()).toDouble());
                const auto slide = [slider, lower, upper](double number) {
                    const QSignalBlocker blocker(slider);
                    slider->setValue(upper > lower ? qRound((number - lower) * 1000 / (upper - lower)) : 0);
                };
                slide(spin->value());
                connect(spin, &QDoubleSpinBox::valueChanged, this, [slide, remember](double number) {
                    slide(number);
                    remember(number);
                });
                connect(slider, &QSlider::valueChanged, spin, [spin, lower, upper](int value) {
                    spin->setValue(lower + (upper - lower) * value / 1000.0);
                });
                controls->addWidget(slider, 1);
                controls->addWidget(spin);
                control = row;
            } else if (initial.isBool()) {
                auto *check = new QCheckBox;
                check->setChecked(values_.value(fieldKey, initial.toBool()).toBool());
                connect(check, &QCheckBox::toggled, this, remember);
                control = check;
            } else if (initial.isDouble()) {
                auto *spin = new QDoubleSpinBox;
                spin->setRange(-100000, 100000);
                spin->setDecimals(2);
                spin->setSingleStep(initial.toDouble() < 2 && initial.toDouble() >= 0 ? 0.05 : 1);
                spin->setValue(values_.value(fieldKey, initial.toDouble()).toDouble());
                connect(spin, &QDoubleSpinBox::valueChanged, this, remember);
                control = spin;
            } else if (initial.isArray()) {
                auto *combo = new QComboBox;
                for (const auto &option : initial.toArray())
                    combo->addItem(option.toString());
                combo->setCurrentText(values_.value(fieldKey, combo->currentText()).toString());
                connect(combo, &QComboBox::currentTextChanged, this, remember);
                control = combo;
            } else {
                auto *edit = new QLineEdit(values_.value(fieldKey, initial.toString()).toString());
                connect(edit, &QLineEdit::textEdited, this, remember);
                control = edit;
            }
            control->setAccessibleName(label);
            if (label.isEmpty())
                form->addRow(control);
            else
                form->addRow(label, control);
        }
    }
}
