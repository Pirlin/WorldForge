#include "Components/BehaviorEditor.h"
#include "Components/BehaviorCode.h"
#include "State/ProjectStore.h"

#include <QComboBox>
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSplitter>
#include <QVBoxLayout>
#include <algorithm>

namespace {
using Rule = worldforge::BehaviorRule;
QString tr(const char *message) { return QObject::tr(message); }

void fillMissions(QComboBox &picker,const Project &project,const QString &id,const QString &empty)
{
    picker.addItem(empty,QString{});
    for (const auto &object : project.objects)
        if (object.visible && object.details.value("gameplay").toString() == "missionStart")
            picker.addItem(object.details.value("mission").toString(),object.id);
    if (!id.isEmpty() && picker.findData(id) < 0)
        picker.addItem(tr("Missing mission"),id);
    picker.setCurrentIndex(picker.findData(id));
}
}

void editBehaviorAsset(ProjectStore &store,const QString &id,QWidget *parent,bool graph)
{
    const auto *project = store.project();
    if (!project || (!id.isEmpty() && !project->behaviors.contains(id)))
        return;
    const auto projectId = project->id;
    const auto behavior = project->behaviors.value(id);
    QDialog dialog(parent);
    dialog.setObjectName(graph ? "behaviorGraphEditor" : "behaviorAssetEditor");
    dialog.setWindowTitle(graph ? tr("Behavior Graph") : tr("Behavior Asset"));
    dialog.resize(graph ? 850 : 470,graph ? 265 : 280);
    auto *layout = new QVBoxLayout(&dialog);
    auto *form = new QFormLayout;
    layout->addLayout(form);
    auto *name = new QLineEdit(behavior.name,&dialog);
    name->setObjectName("behaviorName");
    name->setMaxLength(120);
    auto *event = new QComboBox(&dialog);
    event->setObjectName("behaviorEvent");
    event->addItems({tr("Play started"),tr("Mission completed"),tr("Interact nearby")});
    event->setCurrentIndex(int(behavior.event));
    auto *mission = new QComboBox(&dialog);
    mission->setObjectName("behaviorMission");
    fillMissions(*mission,*project,behavior.mission,tr("Choose a mission"));
    auto *condition = new QComboBox(&dialog);
    condition->setObjectName("behaviorCondition");
    fillMissions(*condition,*project,behavior.requiredMission,tr("Always"));
    auto *action = new QComboBox(&dialog);
    action->setObjectName("behaviorAction");
    action->addItems({tr("Pulse beacon"),tr("Raise object"),tr("Launch upward")});
    action->setCurrentIndex(int(behavior.action));
    auto *amount = new QDoubleSpinBox(&dialog);
    amount->setObjectName("behaviorAmount");
    amount->setRange(0.1,15);
    amount->setSingleStep(0.25);
    amount->setValue(behavior.amount);
    auto *amountLabel = new QLabel(&dialog);
    form->addRow(tr("Name"),name);
    if (graph) {
        auto *nodes = new QHBoxLayout;
        const auto node = [&](const QString &title) {
            auto *box = new QGroupBox(title,&dialog);
            auto *fields = new QFormLayout(box);
            nodes->addWidget(box,1);
            return fields;
        };
        auto *trigger = node(tr("Event"));
        trigger->addRow(event);
        trigger->addRow(tr("Mission"),mission);
        nodes->addWidget(new QLabel(QStringLiteral("→"),&dialog));
        auto *guard = node(tr("Condition"));
        guard->addRow(tr("Completed mission"),condition);
        nodes->addWidget(new QLabel(QStringLiteral("→"),&dialog));
        auto *effect = node(tr("Action"));
        effect->addRow(action);
        effect->addRow(amountLabel,amount);
        layout->addLayout(nodes,1);
    } else {
        form->addRow(tr("Event"),event);
        form->addRow(tr("Event mission"),mission);
        form->addRow(tr("Require completed mission"),condition);
        form->addRow(tr("Action"),action);
        form->addRow(amountLabel,amount);
    }
    auto *message = new QLabel(&dialog);
    message->setWordWrap(true);
    layout->addWidget(message);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save|QDialogButtonBox::Cancel,&dialog);
    layout->addWidget(buttons);
    const auto refresh = [&] {
        mission->setEnabled(event->currentIndex() == int(Rule::Event::MissionCompleted));
        switch (Rule::Action(action->currentIndex())) {
        case Rule::Action::PulseBeacon:
            amountLabel->setText(tr("Duration"));
            amount->setSuffix(tr(" s"));
            message->setText(tr("The pulse appears at the attached object's position."));
            break;
        case Rule::Action::RaiseObject:
            amountLabel->setText(tr("Height"));
            amount->setSuffix(tr(" m"));
            message->setText(tr("Attach to an unparented mesh with Static physics. It rises once during Play."));
            break;
        case Rule::Action::LaunchUpward:
            amountLabel->setText(tr("Launch speed"));
            amount->setSuffix(tr(" m/s"));
            message->setText(tr("Attach to an unparented mesh with Dynamic physics. Its upward speed changes once during Play."));
            break;
        }
    };
    QObject::connect(event,&QComboBox::currentIndexChanged,&dialog,refresh);
    QObject::connect(action,&QComboBox::currentIndexChanged,&dialog,refresh);
    QObject::connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);
    QObject::connect(buttons,&QDialogButtonBox::accepted,&dialog,[&] {
        if (!store.project() || store.project()->id != projectId)
            return;
        ProjectBehavior draft{name->text().trimmed(),Rule::Event(event->currentIndex()),Rule::Action(action->currentIndex()),
            mission->isEnabled() ? mission->currentData().toString() : QString{},condition->currentData().toString(),
            float(amount->value())};
        QString error;
        if (!store.saveBehavior(id,draft,error).isEmpty())
            dialog.accept();
        else
            message->setText(error);
    });
    refresh();
    dialog.exec();
}

void editBehaviorComponents(ProjectStore &store,const QString &objectId,QWidget *parent)
{
    const auto *owner = store.object(objectId);
    if (!owner)
        return;
    const auto projectId = store.project()->id;
    const auto assets = store.project()->behaviors;
    const auto baseline = behaviorComponents(*owner);
    auto draft = baseline;
    QDialog dialog(parent);
    dialog.setObjectName("behaviorComponentsEditor");
    dialog.setWindowTitle(tr("Behavior Components · %1").arg(owner->name));
    dialog.resize(540,440);
    auto *layout = new QVBoxLayout(&dialog);
    auto *addRow = new QHBoxLayout;
    auto *asset = new QComboBox(&dialog);
    asset->setObjectName("componentAsset");
    for (auto it = assets.cbegin(); it != assets.cend(); ++it)
        asset->addItem(it->name,it.key());
    auto *add = new QPushButton(tr("Add Component"),&dialog);
    add->setObjectName("addBehaviorComponent");
    addRow->addWidget(asset,1);
    addRow->addWidget(add);
    layout->addLayout(addRow);
    auto *items = new QListWidget(&dialog);
    items->setObjectName("behaviorComponentList");
    layout->addWidget(items,1);
    auto *order = new QHBoxLayout;
    auto *up = new QPushButton(tr("Move Up"),&dialog);
    auto *down = new QPushButton(tr("Move Down"),&dialog);
    auto *remove = new QPushButton(tr("Remove"),&dialog);
    order->addWidget(up);
    order->addWidget(down);
    order->addStretch();
    order->addWidget(remove);
    layout->addLayout(order);
    auto *form = new QFormLayout;
    auto *enabled = new QCheckBox(tr("Enabled"),&dialog);
    enabled->setObjectName("behaviorComponentEnabled");
    auto *target = new QComboBox(&dialog);
    target->setObjectName("behaviorComponentTarget");
    target->addItem(tr("This object"),QString{});
    for (const auto &object : store.project()->objects)
        if (object.id != objectId && object.visible)
            target->addItem(tr("%1 · %2").arg(object.name,object.type),object.id);
    const int targetCount = target->count();
    auto *override = new QCheckBox(tr("Override asset value"),&dialog);
    override->setObjectName("behaviorComponentOverride");
    auto *amount = new QDoubleSpinBox(&dialog);
    amount->setObjectName("behaviorComponentAmount");
    amount->setRange(0.1,15);
    amount->setDecimals(3);
    form->addRow(enabled);
    form->addRow(tr("Action target"),target);
    form->addRow(override);
    form->addRow(tr("Value"),amount);
    layout->addLayout(form);
    auto *message = new QLabel(&dialog);
    message->setWordWrap(true);
    message->setTextFormat(Qt::PlainText);
    layout->addWidget(message);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save|QDialogButtonBox::Cancel,&dialog);
    layout->addWidget(buttons);
    const auto sync = [&] {
        const int index = items->currentRow();
        const auto *component = index < 0 ? nullptr : &draft[index];
        const QSignalBlocker enableGuard(enabled), targetGuard(target), overrideGuard(override), amountGuard(amount);
        enabled->setEnabled(component);
        target->setEnabled(component);
        override->setEnabled(component);
        enabled->setChecked(component && component->enabled);
        override->setChecked(component && component->amount.has_value());
        amount->setEnabled(override->isChecked());
        while (target->count() > targetCount)
            target->removeItem(target->count()-1);
        const auto targetId = component ? component->targetId : QString{};
        if (target->findData(targetId) < 0)
            target->addItem(tr("Missing or hidden object"),targetId);
        target->setCurrentIndex(target->findData(targetId));
        const auto rule = assets.value(component ? component->assetId : QString{});
        const auto suffix = rule.action == Rule::Action::PulseBeacon ? tr(" s")
            : rule.action == Rule::Action::RaiseObject ? tr(" m") : tr(" m/s");
        amount->setSuffix(suffix);
        amount->setValue(component ? component->amount.value_or(rule.amount) : rule.amount);
        up->setEnabled(index > 0);
        down->setEnabled(index >= 0 && index+1 < draft.size());
        remove->setEnabled(component);
    };
    const auto refresh = [&](int row) {
        const QSignalBlocker guard(items);
        items->clear();
        for (const auto &component : draft) {
            const auto name = assets.value(component.assetId).name;
            const auto label = name.isEmpty() ? tr("Missing Behavior") : name;
            items->addItem(component.enabled ? label : tr("%1 (disabled)").arg(label));
        }
        items->setCurrentRow(row);
        sync();
    };
    QObject::connect(items,&QListWidget::currentRowChanged,&dialog,sync);
    QObject::connect(add,&QPushButton::clicked,&dialog,[&] {
        const auto id = asset->currentData().toString();
        if (id.isEmpty() || draft.size() >= 16 || std::ranges::any_of(draft,[&](const auto &row) { return row.assetId == id; })) {
            message->setText(tr("Choose an unused behavior asset. Each object supports up to 16 components."));
            return;
        }
        draft.append({id,true,{},{}});
        message->clear();
        refresh(int(draft.size())-1);
    });
    const auto move = [&](int direction) {
        const int index = items->currentRow(), destination = index+direction;
        draft.swapItemsAt(index,destination);
        refresh(destination);
    };
    QObject::connect(up,&QPushButton::clicked,&dialog,[&] { move(-1); });
    QObject::connect(down,&QPushButton::clicked,&dialog,[&] { move(1); });
    QObject::connect(remove,&QPushButton::clicked,&dialog,[&] {
        const int index = items->currentRow();
        draft.removeAt(index);
        refresh(std::min(index,int(draft.size())-1));
    });
    QObject::connect(enabled,&QCheckBox::toggled,&dialog,[&](bool checked) {
        const int index = items->currentRow();
        draft[index].enabled = checked;
        refresh(index);
    });
    QObject::connect(target,&QComboBox::activated,&dialog,[&] {
        draft[items->currentRow()].targetId = target->currentData().toString();
    });
    QObject::connect(override,&QCheckBox::toggled,&dialog,[&](bool checked) {
        draft[items->currentRow()].amount = checked ? std::optional(float(amount->value())) : std::nullopt;
        amount->setEnabled(checked);
        if (!checked)
            sync();
    });
    QObject::connect(amount,&QDoubleSpinBox::valueChanged,&dialog,[&](double value) {
        draft[items->currentRow()].amount = float(value);
    });
    QObject::connect(buttons,&QDialogButtonBox::accepted,&dialog,[&] {
        const auto *current = store.object(objectId);
        if (!current || store.project()->id != projectId || behaviorComponents(*current) != baseline) {
            message->setText(tr("The object's components changed. Reopen this panel."));
            return;
        }
        QString error;
        if (store.setBehaviorComponents(objectId,draft,error))
            dialog.accept();
        else
            message->setText(error);
    });
    QObject::connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);
    add->setEnabled(!assets.isEmpty());
    refresh(draft.isEmpty() ? -1 : 0);
    dialog.exec();
}

QWidget *createBehaviorWorkspace(ProjectStore &store,QWidget *parent)
{
    auto *panel = new QWidget(parent);
    auto *layout = new QVBoxLayout(panel);
    auto *split = new QSplitter(Qt::Horizontal,panel);
    layout->addWidget(split,1);
    auto *scene = new QWidget(split);
    auto *sceneLayout = new QVBoxLayout(scene);
    sceneLayout->setContentsMargins(0,0,0,0);
    auto *findObject = new QLineEdit(scene);
    findObject->setObjectName("programmingObjectSearch");
    findObject->setPlaceholderText(tr("Search scene objects…"));
    findObject->setClearButtonEnabled(true);
    auto *objects = new QListWidget(scene);
    objects->setObjectName("programmingObjects");
    sceneLayout->addWidget(findObject);
    sceneLayout->addWidget(objects,1);
    auto *inspector = new QWidget(split);
    auto *inspectorLayout = new QVBoxLayout(inspector);
    inspectorLayout->setContentsMargins(0,0,0,0);
    split->addWidget(scene);
    split->addWidget(inspector);
    split->setSizes({240,700});
    split->setStretchFactor(1,1);

    auto *objectName = new QLabel(inspector);
    objectName->setObjectName("programmingSelectedObject");
    objectName->setTextFormat(Qt::PlainText);
    auto *attachment = new QComboBox(inspector);
    attachment->setObjectName("programmingObjectBehavior");
    auto *editAttached = new QPushButton(tr("Open Attached Behavior…"),inspector);
    editAttached->setObjectName("programmingOpenAttached");
    auto *componentsButton = new QPushButton(tr("Manage Components…"),inspector);
    componentsButton->setObjectName("programmingManageComponents");
    auto *selection = new QFormLayout;
    selection->addRow(tr("Selected object"),objectName);
    auto *attachmentRow = new QHBoxLayout;
    attachmentRow->addWidget(attachment,1);
    attachmentRow->addWidget(editAttached);
    selection->addRow(tr("Attached behavior"),attachmentRow);
    selection->addRow(tr("Behavior components"),componentsButton);
    inspectorLayout->addLayout(selection);

    auto *commands = new QHBoxLayout;
    auto *create = new QPushButton(tr("New Behavior…"),panel);
    auto *edit = new QPushButton(tr("Open Behavior…"),panel);
    auto *graph = new QPushButton(tr("Open Graph…"),panel);
    graph->setObjectName("openBehaviorGraph");
    auto *code = new QPushButton(tr("C++ Behavior Project…"),panel);
    commands->addWidget(create);
    commands->addWidget(edit);
    commands->addWidget(graph);
    commands->addWidget(code);
    commands->addStretch();
    inspectorLayout->addLayout(commands);
    auto *assets = new QListWidget(panel);
    assets->setObjectName("programmingBehaviors");
    inspectorLayout->addWidget(assets,1);
    auto *status = new QLabel(panel);
    status->setObjectName("programmingBehaviorStatus");
    status->setWordWrap(true);
    status->setTextFormat(Qt::PlainText);
    inspectorLayout->addWidget(status);
    const auto syncSelection = [&store,objects,objectName,attachment,editAttached,componentsButton,status] {
        const QSignalBlocker blockList(objects);
        const auto *selected = store.selectedObject();
        objects->setCurrentRow(-1);
        for (int index = 0; selected && index < objects->count(); ++index)
            if (objects->item(index)->data(Qt::UserRole).toString() == selected->id) {
                objects->setCurrentRow(index);
                break;
            }
        const QSignalBlocker blockAttachment(attachment);
        attachment->clear();
        attachment->addItem(tr("None"),QString{});
        const auto components = selected ? behaviorComponents(*selected) : QList<BehaviorComponent>{};
        const auto id = components.isEmpty() ? QString{} : components.front().assetId;
        componentsButton->setEnabled(selected);
        componentsButton->setText(tr("Manage Components (%1)…").arg(components.size()));
        if (selected)
            for (auto it = store.project()->behaviors.cbegin(); it != store.project()->behaviors.cend(); ++it)
                attachment->addItem(it->name,it.key());
        const auto index = attachment->findData(id);
        if (index < 0)
            attachment->addItem(tr("Missing Behavior"),id);
        attachment->setCurrentIndex(index < 0 ? attachment->count()-1 : index);
        attachment->setEnabled(selected && components.size() <= 1);
        editAttached->setEnabled(index > 0);
        objectName->setText(selected ? tr("%1 · %2").arg(selected->name,selected->type) : tr("Select a scene object"));
        status->setText(selected && !selected->visible
            ? tr("This object is hidden. Its behavior runs when it is visible at the start of Play.")
            : tr("Run World Editor Play and open Output Log to trace behaviors."));
    };
    const auto reloadObjects = [&store,objects,findObject,syncSelection] {
        const QSignalBlocker block(objects);
        objects->clear();
        const auto filter = findObject->text().trimmed();
        if (const auto *project = store.project())
            for (const auto &object : project->objects) {
                const auto description = tr("%1 · %2").arg(object.name,object.type);
                if (!description.contains(filter,Qt::CaseInsensitive) && !object.group.contains(filter,Qt::CaseInsensitive))
                    continue;
                auto *row = new QListWidgetItem(description,objects);
                row->setData(Qt::UserRole,object.id);
                row->setToolTip(object.group);
            }
        syncSelection();
    };
    const auto reload = [&store,assets,create,edit,graph,code] {
        const auto current = assets->currentItem() ? assets->currentItem()->data(Qt::UserRole).toString() : QString{};
        assets->clear();
        if (const auto *project = store.project()) {
            QHash<QString,int> uses;
            for (const auto &object : project->objects)
                for (const auto &component : behaviorComponents(object))
                    ++uses[component.assetId];
            for (auto it = project->behaviors.cbegin(); it != project->behaviors.cend(); ++it) {
                auto *row = new QListWidgetItem(tr("%1 · %2 attachment(s)").arg(it->name).arg(uses.value(it.key())),assets);
                row->setData(Qt::UserRole,it.key());
                if (current == it.key())
                    assets->setCurrentItem(row);
            }
        }
        if (!assets->currentItem() && assets->count())
            assets->setCurrentRow(0);
        create->setEnabled(store.project());
        code->setEnabled(store.project());
        edit->setEnabled(assets->currentItem());
        graph->setEnabled(assets->currentItem());
    };
    const auto open = [&store,assets,panel] {
        if (const auto *row = assets->currentItem())
            editBehaviorAsset(store,row->data(Qt::UserRole).toString(),panel);
    };
    QObject::connect(create,&QPushButton::clicked,panel,[&store,panel] { editBehaviorAsset(store,{},panel); });
    QObject::connect(code,&QPushButton::clicked,panel,[&store,panel] { showBehaviorCode(store,panel); });
    QObject::connect(edit,&QPushButton::clicked,panel,open);
    QObject::connect(graph,&QPushButton::clicked,panel,[&store,assets,panel] {
        if (const auto *row = assets->currentItem())
            editBehaviorAsset(store,row->data(Qt::UserRole).toString(),panel,true);
    });
    QObject::connect(assets,&QListWidget::itemActivated,panel,open);
    QObject::connect(assets,&QListWidget::currentItemChanged,panel,[edit,graph](QListWidgetItem *item) {
        edit->setEnabled(item);
        graph->setEnabled(item);
    });
    QObject::connect(findObject,&QLineEdit::textChanged,panel,reloadObjects);
    QObject::connect(objects,&QListWidget::currentItemChanged,panel,[&store](QListWidgetItem *item) {
        if (item)
            store.select(item->data(Qt::UserRole).toString());
    });
    QObject::connect(attachment,&QComboBox::activated,panel,[&store,attachment,status,syncSelection] {
        QString error;
        if (!store.attachBehavior(store.selection(),attachment->currentData().toString(),error)) {
            syncSelection();
            status->setText(error);
        }
    });
    QObject::connect(editAttached,&QPushButton::clicked,panel,[&store,attachment,panel] {
        editBehaviorAsset(store,attachment->currentData().toString(),panel);
    });
    QObject::connect(componentsButton,&QPushButton::clicked,panel,[&store,panel] {
        editBehaviorComponents(store,store.selection(),panel);
    });
    QObject::connect(&store,&ProjectStore::selectionChanged,panel,syncSelection);
    QObject::connect(&store,&ProjectStore::sceneChanged,panel,reload);
    QObject::connect(&store,&ProjectStore::sceneChanged,panel,reloadObjects);
    QObject::connect(&store,&ProjectStore::projectsChanged,panel,reloadObjects);
    QObject::connect(&store,&ProjectStore::projectsChanged,panel,reload);
    reload();
    reloadObjects();
    return panel;
}
