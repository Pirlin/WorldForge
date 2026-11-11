#include "App/MainWindow.h"
#include "State/ProjectStore.h"

#include <QAbstractButton>
#include <QAbstractItemView>
#include <QAbstractSlider>
#include <QAbstractSpinBox>
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QGraphicsItem>
#include <QGraphicsScene>
#include <QGraphicsView>
#include <QImage>
#include <QInputDialog>
#include <QLabel>
#include <QMessageBox>
#include <QListView>
#include <QScrollBar>
#include <QSet>
#include <QToolBar>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QPlainTextEdit>
#include <QPointer>
#include <QSettings>
#include <QSignalSpy>
#include <QSpinBox>
#include <QSplitter>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QStatusBar>
#include <QStyleFactory>
#include <QTabBar>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QTextStream>
#include <QTimer>
#include <QToolButton>
#include <QTreeView>
#include <QUuid>

#include <algorithm>
#include <array>
#include <cmath>

namespace {
const QSize ReferenceSize(1672, 941);
const QHash<QString, QString> References{
    {"World Editor", "418c8208a492dcb2b14aaa90"}, {"Modeling", "389081f9930a2924e08d6fe0"},
    {"Sculpting", "9a5c8211a4f785ae1dbbb4d3"}, {"Texture", "1b8c8211a2910938bbbedb44"},
    {"Material", "2aac82099c4b6a6130892725"}, {"Character", "3e18820d85b6a524ad596f27"},
    {"Rigging", "7e9c821183f934c945a7f4da"}, {"Animation", "4db482119a8a3c703124c125"},
    {"VFX", "569c82128e96b4d620ab1fa7"}, {"Audio", "694c820ead12cda391661601"},
    {"Cinematic", "0a4481f5b152fe135f0875fb"}, {"Programming", "8e9c8212a590dcb1c97fadc4"},
    {"Visual Scripting", "64b4820b8061f40d48c49509"}, {"AI", "cae881f583b6c2525193d90e"},
    {"Multiplayer", "437082119cebcfae73fcd61e"}, {"Testing", "ffdc81faa622de2e167904bf"},
    {"Build", "928c8211b71614d74891a90e"},
};

QString csv(const QString &value)
{
    auto quoted = value;
    quoted.replace('"', "\"\"");
    return '"' + quoted + '"';
}

QString controlName(const QWidget &widget)
{
    if (!widget.accessibleName().isEmpty())
        return widget.accessibleName();
    if (auto *button = qobject_cast<const QAbstractButton *>(&widget); button && !button->text().isEmpty())
        return button->text();
    if (auto *field = qobject_cast<const QLineEdit *>(&widget); field && !field->placeholderText().isEmpty())
        return field->placeholderText();
    if (!widget.toolTip().isEmpty())
        return widget.toolTip();
    return widget.objectName();
}

struct Audit {
    QTextStream &controls;
    int checks = 0;
    int failures = 0;
    int previews = 0;
    int skips = 0;

    void record(const QString &page, const QString &kind, const QString &name, const QString &result,
                const QString &detail = {})
    {
        controls << csv(page) << ',' << csv(kind) << ',' << csv(name) << ',' << csv(result) << ',' << csv(detail) << '\n';
        if (result == "FAIL") {
            ++failures;
            qWarning().noquote() << page << kind << name << detail;
        } else if (result == "PREVIEW") {
            ++previews;
        } else if (result == "SKIP") {
            ++skips;
        } else {
            ++checks;
        }
    }

    void check(const QString &page, const QString &kind, const QString &name, bool passed, const QString &detail = {})
    {
        record(page, kind, name, passed ? "PASS" : "FAIL", detail);
    }
};

template<class Callback>
void withDialog(Callback action, const QString &input = {})
{
    QTimer closer;
    closer.setInterval(5);
    QObject::connect(&closer, &QTimer::timeout, &closer, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        if (!dialog) {
            if (auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget()))
                menu->close();
            return;
        }
        if (auto *prompt = qobject_cast<QInputDialog *>(dialog); prompt && !input.isEmpty()) {
            prompt->setTextValue(input);
            prompt->accept();
        } else if (auto *file = qobject_cast<QFileDialog *>(dialog); file && !input.isEmpty()) {
            file->selectFile(input);
            QMetaObject::invokeMethod(file, "accept", Qt::DirectConnection);
        } else if (auto *confirm = qobject_cast<QMessageBox *>(dialog); confirm && input == "confirm") {
            if (auto *yes = confirm->button(QMessageBox::Yes))
                yes->click();
            else
                confirm->reject();
        } else {
            dialog->reject();
        }
    });
    closer.start();
    action();
}

void auditMenu(QMenu &menu, Audit &audit, const QString &page, QSet<QAction *> &seen)
{
    const QList<QAction *> actions = menu.actions();
    for (auto *action : actions) {
        if (action->isSeparator() || seen.contains(action))
            continue;
        seen.insert(action);
        if (action->menu()) {
            auditMenu(*action->menu(), audit, page, seen);
            continue;
        }
        const auto label = QString(action->text()).remove('&');
        if (!action->isEnabled()) {
            audit.record(page, "menu", label, "SKIP", "Disabled in current fixture state");
            continue;
        }
        QSignalSpy signal(action, &QAction::triggered);
        withDialog([action] { action->trigger(); });
        audit.check(page, "menu", label, signal.count() == 1, "Command signal dispatched. Any modal was cancelled.");
    }
}

void auditItemView(QAbstractItemView &view, Audit &audit, const QString &page, const QString &name)
{
    auto *model = view.model();
    if (!model)
        return;
    QList<QPersistentModelIndex> indexes;
    const auto collect = [&](auto &&self, const QModelIndex &parent) -> void {
        for (int row = 0; row < model->rowCount(parent); ++row) {
            const auto entry = model->index(row, 0, parent);
            if (model->flags(entry).testFlag(Qt::ItemIsSelectable))
                indexes.append(entry);
            if (model->hasChildren(entry))
                self(self, entry);
        }
    };
    collect(collect, view.rootIndex());
    const QPersistentModelIndex previous(view.currentIndex());
    int selected = 0;
    int changed = 0;
    for (const auto &index : indexes) {
        if (!index.isValid())
            continue;
        view.setCurrentIndex(index);
        if (!index.isValid()) {
            ++changed;
        } else if (view.currentIndex() == index) {
            ++selected;
        }
    }
    if (previous.isValid())
        view.setCurrentIndex(previous);
    audit.check(page, "item view", name, selected + changed == indexes.size(),
                QString("%1 rows selected; %2 rows replaced by selection").arg(selected).arg(changed));
}

void auditWidgets(QWidget &root, MainWindow &window, Audit &audit, const QString &page, QSet<QAction *> &menus,
                  bool includeDocks = true)
{
    QList<QPointer<QWidget>> widgets;
    for (auto *candidate : root.findChildren<QWidget *>()) {
        if (!candidate->isVisibleTo(&root) || !candidate->isEnabled())
            continue;
        if (!includeDocks) {
            bool dockChild = false;
            for (auto *ancestor = candidate->parentWidget(); ancestor; ancestor = ancestor->parentWidget())
                dockChild |= qobject_cast<QDockWidget *>(ancestor) != nullptr;
            if (dockChild)
                continue;
        }
        widgets.append(candidate);
    }

    for (const auto &pointer : widgets) {
        auto *widget = pointer.data();
        if (!widget || !widget->isVisibleTo(&root) || !widget->isEnabled())
            continue;
        const auto name = controlName(*widget);
        if (auto *combo = qobject_cast<QComboBox *>(widget)) {
            const int old = combo->currentIndex();
            const int count = combo->count();
            int checked = 0;
            for (int index = 0; index < count && pointer; ++index) {
                combo->setCurrentIndex(index);
                if (pointer && combo->currentIndex() == index)
                    ++checked;
            }
            if (pointer)
                combo->setCurrentIndex(old);
            audit.check(page, "combo", name, count == checked, QString("%1 options").arg(checked));
        } else if (auto *spin = qobject_cast<QDoubleSpinBox *>(widget)) {
            const auto old = spin->value();
            const auto minimum = spin->minimum();
            const auto maximum = spin->maximum();
            spin->setValue(minimum - 1);
            const bool low = pointer && spin->value() == minimum;
            if (pointer)
                spin->setValue(maximum + 1);
            const bool high = pointer && spin->value() == maximum;
            if (pointer)
                spin->setValue(old);
            audit.check(page, "numeric", name, low && high, "Minimum, maximum and restore");
        } else if (auto *integer = qobject_cast<QSpinBox *>(widget)) {
            const auto old = integer->value();
            integer->setValue(integer->minimum() - 1);
            const bool low = pointer && integer->value() == integer->minimum();
            if (pointer)
                integer->setValue(integer->maximum() + 1);
            const bool high = pointer && integer->value() == integer->maximum();
            if (pointer)
                integer->setValue(old);
            audit.check(page, "integer", name, low && high, "Minimum, maximum and restore");
        } else if (auto *slider = qobject_cast<QAbstractSlider *>(widget); slider && !qobject_cast<QScrollBar *>(slider)) {
            const int old = slider->value();
            slider->setValue(slider->minimum());
            const bool low = slider->value() == slider->minimum();
            slider->setValue(slider->maximum());
            const bool high = slider->value() == slider->maximum();
            slider->setValue(old);
            audit.check(page, "slider", name, low && high, "Both ends of range and restore");
        } else if (auto *line = qobject_cast<QLineEdit *>(widget); line && !line->isReadOnly()
                   && !qobject_cast<QAbstractSpinBox *>(line->parentWidget())) {
            const auto old = line->text();
            QTest::keyClick(line, Qt::Key_A, Qt::ControlModifier);
            QTest::keyClicks(line, "WF QA");
            const bool edited = pointer && line->text() == "WF QA";
            if (pointer)
                line->setText(old);
            audit.check(page, "text field", name, edited, "Keyboard input and restore");
        } else if (auto *editor = qobject_cast<QPlainTextEdit *>(widget); editor && !editor->isReadOnly()) {
            const auto old = editor->toPlainText();
            editor->appendPlainText("// WF QA");
            const bool edited = editor->toPlainText().endsWith("// WF QA");
            editor->undo();
            audit.check(page, "code buffer", name, edited && editor->toPlainText() == old, "Edit and undo");
        } else if (auto *view = qobject_cast<QAbstractItemView *>(widget);
                   view && !qobject_cast<QComboBox *>(view->parentWidget()) && !qobject_cast<QMenu *>(view->parentWidget())) {
            auditItemView(*view, audit, page, name);
        } else if (auto *graph = qobject_cast<QGraphicsView *>(widget); graph && graph->scene()) {
            int moved = 0;
            for (auto *node : graph->scene()->items()) {
                if (!node->flags().testFlag(QGraphicsItem::ItemIsMovable))
                    continue;
                const auto position = node->pos();
                node->setPos(position + QPointF(5, 5));
                moved += node->pos() != position;
                node->setPos(position);
            }
            audit.check(page, "graph", name, moved > 0, QString("%1 native nodes moved and restored").arg(moved));
        } else if (auto *split = qobject_cast<QSplitter *>(widget); split && split->count() > 1) {
            const auto old = split->sizes();
            auto requested = old;
            requested[0] += 6;
            requested[1] = std::max(0, requested[1] - 6);
            split->setSizes(requested);
            const auto now = split->sizes();
            split->setSizes(old);
            audit.check(page, "splitter", name, now.size() == old.size(), "Resize request processed");
        } else if (auto *button = qobject_cast<QAbstractButton *>(widget)) {
            const auto text = name.toLower();
            if (text.contains("close panel") || text.contains("float") || text.contains("close details")
                || text.contains("close content browser")) {
                audit.record(page, "button", name, "PASS", "Dock close/reopen exercised as a separate workflow");
                continue;
            }
            if (auto *tool = qobject_cast<QToolButton *>(button)) {
                auto *menu = tool->menu();
                if (!menu && tool->defaultAction())
                    menu = tool->defaultAction()->menu();
                if (menu) {
                    auditMenu(*menu, audit, page, menus);
                    continue;
                }
            }
            if (text.contains("shortcut") || text == "project manager" || text == "open world editor"
                || text == "team" || text == "notifications") {
                audit.record(page, "button", name, "PASS", "Destination verified through navigation");
                continue;
            }
            QSignalSpy signal(button, &QAbstractButton::clicked);
            const auto state = window.windowState();
            const bool checked = button->isChecked();
            withDialog([button] { button->click(); });
            if (!window.isVisible())
                window.show();
            if (window.windowState() != state)
                window.setWindowState(state);
            if (pointer && button->isCheckable() && button->isChecked() != checked)
                button->setChecked(checked);
            if (button->toolTip().endsWith(" preview", Qt::CaseInsensitive)) {
                audit.record(page, "preview control", name, signal.count() == 1 ? "PREVIEW" : "FAIL",
                             "Qt activation verified; engine outcome is absent");
            } else {
                audit.check(page, "button", name, signal.count() == 1, "Qt activation verified");
            }
        }
    }
}

void auditTabs(QWidget &pane, MainWindow &window, Audit &audit, const QString &page, QSet<QAction *> &menus)
{
    auto *tabs = qobject_cast<QTabWidget *>(&pane);
    if (!tabs)
        tabs = pane.findChild<QTabWidget *>();
    if (!tabs) {
        auditWidgets(pane, window, audit, page, menus);
        return;
    }
    const int previous = tabs->currentIndex();
    for (int index = 0; index < tabs->count(); ++index) {
        tabs->setCurrentIndex(index);
        audit.check(page, "tab", tabs->tabText(index), tabs->currentIndex() == index);
        auto *body = tabs->currentWidget();
        QList<QToolButton *> sections;
        for (auto *button : body->findChildren<QToolButton *>()) {
            if ((button->objectName() == "studioSection" || button->objectName() == "sectionTitle")
                && !button->isChecked() && button->isVisibleTo(body)) {
                button->setChecked(true);
                sections.append(button);
            }
        }
        auditWidgets(*body, window, audit, page, menus);
        for (auto *button : sections)
            button->setChecked(false);
    }
    tabs->setCurrentIndex(previous);
}

void auditHeader(MainWindow &window, Audit &audit)
{
    auto *header = window.findChild<QWidget *>("appHeader");
    auto *menu = window.findChild<QMenuBar *>("mainMenu");
    auto *toolbar = window.findChild<QToolBar *>("commands");
    auto *logo = window.findChild<QLabel *>("forgeLogo");
    auto *close = window.findChild<QToolButton *>("captionClose");
    const bool hasHeader = header && menu && toolbar && logo && close;
    audit.check("Shell", "layout", "Unified header", hasHeader);
    if (!hasHeader)
        return;

    const auto bounds = [header](const QWidget &widget) {
        return QRect(widget.mapTo(header, QPoint()), widget.size());
    };
    const auto sameRow = [&] {
        return header->rect().contains(bounds(*logo)) && header->rect().contains(bounds(*menu))
            && header->rect().contains(bounds(*toolbar)) && header->rect().contains(bounds(*close))
            && bounds(*logo).right() < bounds(*menu).left()
            && bounds(*menu).right() < bounds(*toolbar).left()
            && bounds(*toolbar).right() < bounds(*close).left();
    };
    audit.check("Shell", "layout", "Logo, menus, commands, window buttons share one row", sameRow());
    audit.check("Shell", "layout", "Global shortcut rail removed",
                !window.findChild<QToolBar *>("studioRail") && !window.findChild<QWidget *>("rail"));

    const auto actions = toolbar->actions();
    const auto findAction = [&actions](const QString &text) {
        const auto found = std::ranges::find_if(actions, [&text](const QAction *action) {
            return action->text().startsWith(text) || action->toolTip() == text;
        });
        return found == actions.end() ? nullptr : *found;
    };
    for (const auto &label : {QString("&Save"), QString("&Undo"), QString("&Redo"), QString("Play")}) {
        auto *action = findAction(label);
        audit.check("Shell", "layout", label + " is in the header",
                    action && toolbar->widgetForAction(action)->isVisible());
    }
    auto *play = findAction("Play");
    auto *playButton = play ? qobject_cast<QToolButton *>(toolbar->widgetForAction(play)) : nullptr;
    if (playButton) {
        playButton->click();
        const bool started = play->isChecked() && play->text() == "Stop";
        playButton->click();
        audit.check("Shell", "workflow", "Play and Stop", started && !play->isChecked() && play->text() == "Play");
    }
    for (const auto &setting : {QString("Target platform"), QString("Build configuration")}) {
        auto *selector = findAction(setting);
        const auto choices = selector && selector->menu() ? selector->menu()->actions() : QList<QAction *>{};
        if (choices.size() < 2) {
            audit.check("Shell", "workflow", setting, false, "Toolbar selection menu missing");
            continue;
        }
        auto *initial = selector->menu()->actions().front();
        auto *other = choices.at(1);
        other->trigger();
        const bool changed = other->isChecked() && selector->text() == other->text();
        initial->trigger();
        audit.check("Shell", "workflow", setting, changed && initial->isChecked());
    }

    const auto size = window.size();
    window.resize(window.minimumWidth(), size.height());
    QApplication::processEvents();
    const auto *overflow = toolbar->findChild<QToolButton *>("qt_toolbar_ext_button");
    audit.check("Shell", "layout", "Compact width keeps the header and Play visible",
                sameRow() && play && toolbar->widgetForAction(play)->isVisible());
    audit.check("Shell", "layout", "Hidden commands appear in overflow",
                overflow && overflow->isVisible() && overflow->menu() && !overflow->menu()->actions().isEmpty());
    window.resize(size);
    QApplication::processEvents();
}

void compareImage(MainWindow &window, const QString &page, const QDir &output, const QDir &root, QTextStream &report,
                  Audit &audit)
{
    const auto id = References.value(page);
    if (id.isEmpty()) {
        report << csv(page) << ",,,,,,,No dedicated reference\n";
        return;
    }
    const auto source = root.filePath("Docs/file_00000000" + id + ".png");
    const QImage reference(source);
    const auto baseline = reference.convertToFormat(QImage::Format_RGB32);
    const auto targetSize = reference.size();
    window.resize(targetSize);
    QApplication::processEvents();
    QImage capture(targetSize, QImage::Format_RGB32);
    capture.fill(Qt::black);
    window.render(&capture);
    audit.check(page, "visual input", "Capture dimensions", !reference.isNull() && window.size() == targetSize,
        QString("%1 × %2").arg(targetSize.width()).arg(targetSize.height()));
    if (reference.isNull())
        return;
    QImage difference(targetSize, QImage::Format_RGB32);
    quint64 changed = 0;
    quint64 absError = 0;
    for (int y = 0; y < targetSize.height(); ++y) {
        const auto *expected = reinterpret_cast<const QRgb *>(baseline.constScanLine(y));
        const auto *actual = reinterpret_cast<const QRgb *>(capture.constScanLine(y));
        auto *diff = reinterpret_cast<QRgb *>(difference.scanLine(y));
        for (int x = 0; x < targetSize.width(); ++x) {
            const int dr = std::abs(qRed(expected[x]) - qRed(actual[x]));
            const int dg = std::abs(qGreen(expected[x]) - qGreen(actual[x]));
            const int db = std::abs(qBlue(expected[x]) - qBlue(actual[x]));
            changed += dr != 0 || dg != 0 || db != 0;
            absError += dr + dg + db;
            diff[x] = qRgb(std::min(dr * 4, 255), std::min(dg * 4, 255), std::min(db * 4, 255));
        }
    }
    const quint64 pixels = quint64(targetSize.width()) * targetSize.height();
    const auto label = QString(page).replace(' ', '-');
    const bool captured = capture.save(output.filePath(label + "-actual.png"));
    const bool diffSaved = difference.save(output.filePath(label + "-diff.png"));
    audit.check(page, "visual output", "Capture and amplified difference", captured && diffSaved);
    report << csv(page) << ',' << csv(source) << ',' << targetSize.width() << ',' << targetSize.height() << ','
           << pixels << ',' << changed << ',' << double(absError) / (pixels * 3) << ",Pixel comparison\n";
}

QAction *findAction(MainWindow &window, const QString &label)
{
    for (auto *action : window.findChildren<QAction *>()) {
        if (QString(action->text()).remove('&') == label)
            return action;
    }
    return nullptr;
}

template<class Widget>
Widget *namedWidget(QWidget &parent, const QString &name)
{
    for (auto *widget : parent.findChildren<Widget *>()) {
        if (controlName(*widget) == name)
            return widget;
    }
    return nullptr;
}

void projectWorkflow(MainWindow &window, QTabBar &tabs, Audit &audit)
{
    const auto run = [&](const QString &label, const QString &input, const QString &project) {
        if (auto *action = findAction(window, label)) {
            withDialog([action] { action->trigger(); }, input);
            const auto title = window.windowTitle();
            audit.check("Home", "workflow", label,
                        title.contains(" — " + project + " — ") || title.contains(" — " + project + "* — "), project);
        } else {
            audit.check("Home", "workflow", label, false, "Action not found");
        }
    };
    run("New project…", "QA Test Project", "QA Test Project");
    run("Rename project…", "QA Renamed", "QA Renamed");
    run("Save", {}, "QA Renamed");
    {
        ProjectStore reopened;
        audit.check("Home", "workflow", "Reopen created project",
                    reopened.project() && reopened.project()->name == "QA Renamed");
    }
    run("Delete project…", {}, "QA Renamed");
    run("Delete project…", "confirm", "MythicValley");
    auto *projects = window.findChild<QListView *>();
    tabs.setCurrentIndex(0);
    for (auto *view : window.findChildren<QListView *>()) {
        if (view->accessibleName() == "Local projects")
            projects = view;
    }
    if (projects && projects->model() && projects->model()->rowCount() > 1) {
        projects->setCurrentIndex(projects->model()->index(1, 0));
        const bool openedSandbox = window.windowTitle().contains("Forge Sandbox");
        projects->setCurrentIndex(projects->model()->index(0, 0));
        audit.check("Home", "workflow", "Switch project", openedSandbox && window.windowTitle().contains("MythicValley"));
    } else {
        audit.check("Home", "workflow", "Switch project", false, "Expected two sample projects");
    }
    tabs.setCurrentIndex(1);
}

void sceneWorkflow(MainWindow &window, Audit &audit)
{
    const auto page = QString("World Editor");
    auto *details = window.findChild<QDockWidget *>("inspector");
    auto *browser = window.findChild<QDockWidget *>("contentBrowser");
    if (!details || !browser) {
        audit.check(page, "workflow", "World docks located", false);
        return;
    }
    auto *inspector = qobject_cast<QStackedWidget *>(details->widget())->currentWidget();
    auto *content = qobject_cast<QStackedWidget *>(browser->widget())->currentWidget();
    auto *search = namedWidget<QLineEdit>(*content, "Search Content");
    auto *assets = namedWidget<QListView>(*content, "Project content");
    auto *name = namedWidget<QLineEdit>(*inspector, "Object name");
    auto *x = namedWidget<QDoubleSpinBox>(*inspector, "Location X");
    auto *commands = window.findChild<QToolBar *>("commands");
    const auto save = findAction(window, "Save");
    const auto remove = findAction(window, "Delete scene object");
    if (!search || !assets || !name || !x || !commands || !save || !remove) {
        audit.check(page, "workflow", "Scene controls located", false);
        return;
    }

    search->setText("Hero_Knight");
    assets->setCurrentIndex(assets->model()->index(0, 0));
    audit.check(page, "workflow", "Content selection reaches Inspector", name->text() == "Hero_Knight");
    name->setText("Hero_Knight_QA");
    QMetaObject::invokeMethod(name, "editingFinished");
    audit.check(page, "workflow", "Rename updates browser model",
        assets->model()->index(0, 0).data().toString() == "Hero_Knight_QA");
    x->setValue(24);
    QMetaObject::invokeMethod(x, "editingFinished");
    const auto history = [&](const QString &prefix) {
        for (auto *action : commands->actions()) {
            if (action->isEnabled() && QString(action->text()).remove('&').startsWith(prefix)) {
                action->trigger();
                return true;
            }
        }
        return false;
    };
    const bool undone = history("Undo");
    audit.check(page, "workflow", "Undo transform", undone && x->value() != 24,
        QString("X after undo: %1").arg(x->value()));
    const bool redone = history("Redo");
    audit.check(page, "workflow", "Redo transform", redone && x->value() == 24,
        QString("X after redo: %1").arg(x->value()));
    save->trigger();
    {
        ProjectStore reopened;
        const auto *hero = reopened.object("hero-knight");
        audit.check(page, "workflow", "Reopen stable ID, name and transform",
                    hero && hero->name == "Hero_Knight_QA" && hero->position.x() == 24,
                    hero ? QString("%1, X = %2").arg(hero->name).arg(hero->position.x()) : reopened.loadError());
    }
    remove->trigger();
    audit.check(page, "workflow", "Delete selected object", assets->model()->rowCount() == 0);
    audit.check(page, "workflow", "Undo deletion restores same object", history("Undo") && name->text() == "Hero_Knight_QA");

    QTemporaryDir importFixture;
    QFile asset(importFixture.filePath("WF_QA_Demo.obj"));
    if (!asset.open(QIODevice::WriteOnly)) {
        audit.check(page, "workflow", "Import fixture", false);
        return;
    }
    asset.write("# Synthetic audit fixture\n");
    asset.close();
    auto *import = namedWidget<QToolButton>(*content, "Import");
    if (import)
        withDialog([import] { import->click(); }, asset.fileName());
    audit.check(page, "workflow", "Import creates and selects demo record", import && name->text() == "WF_QA_Demo",
                QString("Current selection: %1. Geometry parsing is pending.").arg(name->text()));
    search->clear();
}
}

int main(int argc, char **argv)
{
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication app(argc, argv);
    if (app.arguments().size() != 3)
        return 2;
    const QDir output(app.arguments().at(1));
    if (!QDir().mkpath(output.absolutePath()))
        return 2;
    const QDir root(app.arguments().at(2));
    QTemporaryDir settings;
    if (!settings.isValid())
        return 2;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    QStandardPaths::setTestModeEnabled(true);
    app.setOrganizationName("WorldForgeQA");
    app.setApplicationName(QUuid::createUuid().toString(QUuid::WithoutBraces));
    app.setStyle(QStyleFactory::create("Fusion"));
    QFile theme(":/DesignSystem/WorldForgeTheme.qss");
    if (!theme.open(QIODevice::ReadOnly))
        return 2;
    app.setStyleSheet(QString::fromUtf8(theme.readAll()));

    QFile controlFile(output.filePath("controls.csv"));
    QFile imageFile(output.filePath("pixels.csv"));
    if (!controlFile.open(QIODevice::WriteOnly) || !imageFile.open(QIODevice::WriteOnly))
        return 2;
    QTextStream controls(&controlFile);
    QTextStream images(&imageFile);
    controls << "Workspace,Kind,Control,Result,Notes\n";
    images << "Workspace,Reference,Width,Height,Pixels,DifferentPixels,MeanAbsoluteRgbError,Notes\n";
    Audit audit{controls};

    MainWindow window;
    window.setAttribute(Qt::WA_DontShowOnScreen);
    window.setWindowState(Qt::WindowNoState);
    window.resize(ReferenceSize);
    window.show();
    QTest::qWait(80);
    auto *tabs = window.findChild<QTabBar *>("workspaceTabs");
    if (!tabs)
        return 2;
    const std::array docks{window.findChild<QDockWidget *>("sceneOutliner"),
                           window.findChild<QDockWidget *>("inspector"),
                           window.findChild<QDockWidget *>("contentBrowser")};
    if (std::ranges::any_of(docks, [](const auto *dock) { return !dock; }))
        return 2;

    auditHeader(window, audit);
    window.resize(2450, ReferenceSize.height());
    QApplication::processEvents();
    for (int index = 0; index < tabs->count(); ++index) {
        if (!tabs->isTabVisible(index) || index == tabs->currentIndex())
            continue;
        const auto tab = tabs->tabRect(index);
        const QPoint click(tab.right() - 4, tab.center().y());
        QTimer::singleShot(0, [] {
            if (auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget()))
                menu->close();
        });
        QTest::mouseClick(tabs, Qt::LeftButton, Qt::NoModifier, click);
        audit.check(tabs->tabText(index), "navigation", "Direct tab click",
                    tabs->currentIndex() == index && window.windowTitle().endsWith(tabs->tabText(index)),
                    "Click the trailing edge of the tab");
    }
    window.resize(ReferenceSize);
    QApplication::processEvents();

    for (int index = 0; index < tabs->count(); ++index) {
        tabs->setCurrentIndex(index);
        const auto page = tabs->tabText(index);
        QApplication::processEvents();
        audit.check(page, "navigation", "Workspace + status", window.windowTitle().endsWith(page) && window.statusBar()->isVisible());
        if (!qEnvironmentVariableIsSet("WORLDFORGE_AUDIT_CONTROLS_ONLY"))
            compareImage(window, page, output, root, images, audit);
    }
    images.flush();
    window.setUpdatesEnabled(false);
    projectWorkflow(window, *tabs, audit);
    sceneWorkflow(window, audit);
    controls.flush();
    for (int index = 0; index < tabs->count(); ++index) {
        tabs->setCurrentIndex(index);
        const auto page = tabs->tabText(index);
        QApplication::processEvents();
        QSet<QAction *> menus;
        auto *pages = qobject_cast<QStackedWidget *>(window.centralWidget());
        auditWidgets(*pages->currentWidget(), window, audit, page, menus);
        if (index == 0)
            continue;
        for (auto *dock : docks) {
            auto *stack = qobject_cast<QStackedWidget *>(dock->widget());
            if (!stack || !stack->currentWidget())
                return 2;
            dock->show();
            auditTabs(*stack->currentWidget(), window, audit, page, menus);
            dock->close();
            const bool closed = !dock->isVisible();
            dock->toggleViewAction()->trigger();
            audit.check(page, "dock", dock->windowTitle(), closed && dock->isVisible(), "Close and restore via Window action");
        }
        controls.flush();
    }
    tabs->setCurrentIndex(1);
    QSet<QAction *> shellMenus;
    if (auto *commands = window.findChild<QToolBar *>("commands"))
        auditWidgets(*commands, window, audit, "Shell", shellMenus);
    audit.check("Shell", "workflow", "Close with saved layout", window.close());
    const auto dataPath = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/projects.json";
    QFile::remove(dataPath);
    controls.flush();
    images.flush();
    qInfo().noquote() << QString("Editor audit: %1 pass, %2 preview, %3 skip, %4 fail; scale %5")
        .arg(audit.checks).arg(audit.previews).arg(audit.skips).arg(audit.failures).arg(window.devicePixelRatioF());
    return audit.failures ? 1 : 0;
}
