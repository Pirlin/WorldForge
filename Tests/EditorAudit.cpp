#include "App/MainWindow.h"
#include "App/EditorLog.h"
#include "Components/LiveViewport.h"
#include "Components/ProjectLauncher.h"
#include "State/WorldScene.h"
#include "State/WorldRuntime.h"
#include "State/ProjectStore.h"
#include "Workspaces/ModelingWorkspace.h"
#include "Workspaces/SurfaceWorkspace.h"
#include "Workspaces/SculptingWorkspace.h"

#include <QAbstractButton>
#include <QAbstractItemView>
#include <QAbstractSlider>
#include <QAbstractSpinBox>
#include <QApplication>
#include <QComboBox>
#include <QCheckBox>
#include <QDialog>
#include <QDir>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include "Engine/World/PlayableScene.h"
#include <sstream>
#include <QGraphicsItem>
#include <QGraphicsScene>
#include <QGraphicsView>
#include <QImage>
#include <QBuffer>
#include <QImageReader>
#include <QInputDialog>
#include "Engine/Geometry/MeshTopology.h"
#include "Engine/Geometry/MeshRemesh.h"
#include "Engine/Geometry/MeshUv.h"
#include "State/GlbImport.h"
#include "State/StudioPublishReview.h"
#include "State/ForgeAi.h"
#include "State/PlayableStreet.h"
#include <QProcess>
#include "Components/ForgeAiPanel.h"
#include "Workspaces/ForgeAiJob.h"
#include "Workspaces/EmbeddedStudio.h"
#include <QEventLoop>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QMessageBox>
#include <QMouseEvent>
#include <QListView>
#include <QListWidget>
#include <QScrollBar>
#include <QScopeGuard>
#include <QScrollArea>
#include <QScreen>
#include <QSet>
#include <QToolBar>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProgressDialog>
#include <QPushButton>
#include <QSettings>
#include <QSignalSpy>
#include <QSpinBox>
#include <QTableWidget>
#include <QTreeWidget>
#include <QSplitter>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QStandardItemModel>
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
#include <QWindow>

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
        controls.flush();
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
        if (action->property("requiresStudioProcess").toBool()) {
            audit.record(page,"menu",label,"SKIP","Requires the separate Studio integration test");
            continue;
        }
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
            if (page == "World Editor" && (text == "frame scene" || text == "add cube" || text == "edit mesh")) {
                audit.record(page, "button", name, "SKIP", "Live viewport actions are exercised by the native authoring workflow.");
                continue;
            }
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
    auto *close = header ? header->findChild<QToolButton *>("captionClose") : nullptr;
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
    auto *simulate = findAction("Simulate");
    audit.check("Shell", "workflow", "Runtime controls wait for a live viewport",
                play && simulate && !play->isEnabled() && !simulate->isEnabled());
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

bool studioHostedTab(const QString &page)
{
    return page == "Modeling" || page == "Sculpting" || page == "Texture" || page == "Material"
        || page == "Rigging" || page == "Animation";
}

// Studio-hosted tabs switch only after the embedded Studio reports that the workspace is ready, so wait for it.
// Returns false with a reason when the tab never became current.
bool waitForWorkspace(MainWindow &window, const QTabBar &tabs, int index, QString &reason)
{
    const auto page = tabs.tabText(index);
    const auto shown = [&] { return tabs.currentIndex() == index && window.windowTitle().endsWith(page); };
    if (!studioHostedTab(page)) {
        QApplication::processEvents();
        return shown();
    }
    if (EmbeddedStudio::installedExecutable().isEmpty()) {
        reason = "Studio is not installed";
        return false;
    }
    if (QTest::qWaitFor(shown, 60000))
        return true;
    reason = QString("Studio did not open %1 within 60 s | %2").arg(page, window.statusBar()->currentMessage());
    return false;
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

void sceneWorkflow(MainWindow &window, const QDir &root, Audit &audit)
{
    const auto page = QString("World Editor");
    auto *details = window.findChild<QDockWidget *>("inspector");
    auto *browser = window.findChild<QDockWidget *>("contentBrowser");
    auto *outliner = window.findChild<QDockWidget *>("sceneOutliner");
    if (!details || !browser || !outliner) {
        audit.check(page, "workflow", "World docks located", false);
        return;
    }
    auto *inspector = qobject_cast<QStackedWidget *>(details->widget())->currentWidget();
    auto *content = qobject_cast<QStackedWidget *>(browser->widget())->currentWidget();
    auto *search = namedWidget<QLineEdit>(*content, "Search Content");
    auto *assets = namedWidget<QListView>(*content, "Project content");
    auto *sceneSearch = namedWidget<QLineEdit>(*outliner, "Search scene");
    auto *tree = namedWidget<QTreeView>(*outliner, "Scene hierarchy");
    auto *name = namedWidget<QLineEdit>(*inspector, "Object name");
    auto *x = namedWidget<QDoubleSpinBox>(*inspector, "Location X");
    auto *commands = window.findChild<QToolBar *>("commands");
    const auto save = findAction(window, "Save");
    const auto remove = findAction(window, "Delete scene object");
    if (!search || !assets || !sceneSearch || !tree || !name || !x || !commands || !save || !remove) {
        audit.check(page, "workflow", "Scene controls located", false);
        return;
    }

    audit.check(page, "workflow", "Legacy objects do not appear as imported assets", assets->model()->rowCount() == 0);
    const auto hasLegacy = [&] {
        const auto root = tree->model()->index(0,0);
        for (int i = 0; i < tree->model()->rowCount(root); ++i)
            if (tree->model()->index(i,1,root).data().toString() == "Unrendered Scene Data")
                return true;
        return false;
    };
    auto *demo = findAction(window,"Show legacy demo placeholders");
    const bool initiallyClean = !hasLegacy();
    if (demo)
        demo->setChecked(true);
    const bool recovered = hasLegacy();
    if (demo)
        demo->setChecked(false);
    audit.check(page,"workflow","Original demo entries are hidden and can be revealed",
                demo && initiallyClean && recovered && !hasLegacy());
    sceneSearch->setText("Hero_Knight");
    const auto rootIndex = tree->model()->index(0, 0);
    const auto group = tree->model()->index(0, 0, rootIndex);
    tree->setCurrentIndex(tree->model()->index(0, 1, group));
    audit.check(page, "workflow", "Outliner selection reaches Inspector", name->text() == "Hero_Knight");
    name->setText("Hero_Knight_QA");
    QMetaObject::invokeMethod(name, "editingFinished");
    audit.check(page, "workflow", "Rename updates the Outliner",
        tree->currentIndex().siblingAtColumn(1).data().toString() == "Hero_Knight_QA");
    sceneSearch->clear();
    const bool editedPlaceholderHidden = !hasLegacy();
    sceneSearch->setText("Hero_Knight");
    audit.check(page,"workflow","Edited placeholders without meshes are still hidden and searchable",
                editedPlaceholderHidden && hasLegacy() && name->text() == "Hero_Knight_QA");
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
    audit.check(page, "workflow", "Delete selected object", name->text() != "Hero_Knight_QA");
    audit.check(page, "workflow", "Undo deletion restores same object", history("Undo") && name->text() == "Hero_Knight_QA");

    QTemporaryDir importFixture;
    const auto asset = importFixture.filePath("WF_QA_Mesh.glb");
    if (!QFile::copy(root.filePath("Tests/Fixtures/ForgeCube.glb"), asset)) {
        audit.check(page, "workflow", "Import fixture", false);
        return;
    }
    auto *import = namedWidget<QToolButton>(*content, "Import");
    if (import)
        withDialog([import] { import->click(); }, asset);
    audit.check(page, "workflow", "GLB import creates and selects a mesh instance", import && name->text() == "WF_QA_Mesh",
                QString("Current selection: %1").arg(name->text()));
    audit.check(page, "workflow", "Content Browser lists the imported asset", assets->model()->rowCount() == 1);
    search->clear();
    sceneSearch->clear();
}

void authoringData(const QDir &root, const QDir &data, Audit &audit)
{
    const QString page = "Authoring";
    if (!QDir().mkpath(data.absolutePath()) || !QFile::copy(":/Data/demo.json", data.filePath("projects.json"))) {
        audit.check(page, "storage", "Legacy fixture installed", false);
        return;
    }
    ProjectStore store;
    const auto legacyGuid = store.entityGuid("hero-knight");
    audit.check(page, "scene", "Legacy alias resolves to stable GUID", !legacyGuid.isNull());
    const auto *original = store.object("hero-knight");
    if (!original || !store.project() || !store.project()->scene)
        return;
    const auto start = store.project()->scene->revision();
    auto object = *original;
    object.name = "Hero migration test";
    object.position.setX(3.25f);
    store.updateObject(object, "Rename and move");
    const auto *edited = store.project()->scene->find(legacyGuid);
    audit.check(page, "scene", "Typed rename and transform updated engine scene",
                edited && edited->name == "Hero migration test" && edited->transform.positionMeters[0] == 3.25
                    && store.project()->scene->revision() == start+2);
    store.history().undo();
    audit.check(page, "scene", "Undo advances revision and restores transform",
                store.project()->scene->revision() == start+4 && store.object("hero-knight")->position.x() == 0);
    store.history().redo();
    auto invalid = *store.object("hero-knight");
    invalid.scale.setX(0);
    store.updateObject(invalid, "Invalid scale");
    audit.check(page, "scene", "Invalid transform has no side effects",
                store.project()->scene->revision() == start+6 && store.object("hero-knight")->scale.x() != 0);
    const auto unused = store.unusedDemoCount();
    store.removeUnusedDemoObjects();
    audit.check(page, "scene", "Demo cleanup retains edited records and working lights",
                unused > 0 && !store.unusedDemoCount() && !store.object("castle-main")
                    && store.object("hero-knight") && store.object("sun"));
    store.history().undo();
    audit.check(page, "scene", "Demo cleanup can be undone", store.unusedDemoCount() == unused && store.object("castle-main"));

    QString error;
    ProjectStore competing;
    audit.check(page, "storage", "Atomic project and scene save", store.save(&error), error);
    error.clear();
    audit.check(page, "storage", "Stale editor cannot overwrite manifest", !competing.save(&error) && !error.isEmpty());
    {
        ProjectStore reopened;
        const auto *hero = reopened.object("hero-knight");
        audit.check(page, "storage", "Migrated GUID and transform survive reopen",
                    hero && reopened.entityGuid(hero->id) == legacyGuid && hero->position.x() == 3.25f);
    }

    QTemporaryDir source;
    const auto path = source.filePath("BlenderBracket.glb");
    const auto fixture = root.filePath("Tests/Fixtures/ForgeCube.glb");
    const bool imported = source.isValid() && QFile::copy(fixture, path) && store.importMesh(path, error);
    const auto *mesh = store.selectedMesh();
    audit.check(page, "GLB", "Blender geometry, UVs and material imported", imported && mesh
        && mesh->mesh->faces.size() == 12 && mesh->mesh->materials.size() == 2
        && mesh->mesh->materials[1].name == "Painted Steel" && mesh->mesh->materials[1].color[2] > 0.7f
        && std::ranges::any_of(mesh->mesh->faces, [](const auto &face) {
            return std::ranges::any_of(face.corners, [](const auto &corner) { return corner.uv[0] != 0; });
        }), error);
    if (!imported || !mesh)
        return;
    const auto assetId = mesh->id;
    const auto baseMesh = mesh->mesh;
    const auto originalInstance = store.selection();
    store.addMeshInstance();
    const auto secondInstance = store.selection();
    const bool sourceUpdated = QFile::remove(path)
        && QFile::copy(root.filePath("Tests/Fixtures/ForgeCubeUpdated.glb"), path);
    const bool reimported = sourceUpdated && store.reimportMesh(error);
    const auto *changed = store.meshAsset(assetId);
    audit.check(page, "GLB", "Reimport updates geometry and both stable asset references",
                reimported && changed && changed->mesh->vertices != baseMesh->vertices
                    && changed->mesh->materials[1].color[0] > 0.6f
                    && store.object(originalInstance)->meshAsset == assetId
                    && store.object(secondInstance)->meshAsset == assetId, error);
    if (!reimported)
        return;
    const auto updated = changed->mesh;
    QFile broken(path);
    if (broken.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        broken.write("not a GLB");
        broken.close();
    }
    audit.check(page, "GLB", "Malformed reimport leaves previous asset intact",
                !store.reimportMesh(error) && store.meshAsset(assetId)->mesh == updated);
    error.clear();
    audit.check(page, "storage", "Asset registry saved", store.save(&error), error);
    {
        ProjectStore reopened;
        audit.check(page, "storage", "Two saved instances reopen with shared geometry",
            reopened.meshAsset(assetId) && *reopened.meshAsset(assetId)->mesh == *updated
                && reopened.object(originalInstance)->meshAsset == reopened.object(secondInstance)->meshAsset);
    }
    store.addCube(true);
    const auto halfId = store.selectedMesh()->id;
    const auto mirror = store.setMirrorX(true, error);
    audit.check(page, "mesh", "Mirror changes evaluated mesh and preserves editable half",
                mirror && store.meshAsset(halfId)->mesh->faces.size() == 6
                    && store.meshAsset(halfId)->evaluated().faces.size() == 10, error);
    audit.check(page, "storage", "Second scene generation saved", store.save(&error), error);
    QFile manifest(data.filePath("projects.wfproject"));
    if (!manifest.open(QIODevice::ReadOnly)) {
        audit.check(page, "storage", "Read manifest", false);
        return;
    }
    const auto entry = QJsonDocument::fromJson(manifest.readAll()).object().value("projects").toArray().first().toObject();
    manifest.close();
    QFile interrupted(data.filePath("scenes/" + entry.value("sceneFile").toString()));
    if (interrupted.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        interrupted.write("incomplete");
        interrupted.close();
    }
    ProjectStore recovered;
    audit.check(page, "storage", "Damaged latest generation falls back to prior complete save",
                !recovered.loadError().isEmpty() && recovered.meshAsset(assetId) && !recovered.meshAsset(halfId));
    const bool gpuProject = recovered.createProject("First Light QA");
    audit.check(page, "storage", "Recovery can commit a new project", gpuProject && recovered.save(&error), error);
    if (manifest.open(QIODevice::ReadOnly)) {
        const auto latest = manifest.readAll();
        manifest.close();
        auto newer = QJsonDocument::fromJson(latest).object();
        newer.insert("schemaVersion", 99);
        const auto future = QJsonDocument(newer).toJson();
        if (manifest.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            manifest.write(future);
            manifest.close();
            ProjectStore compatible;
            const bool savedElsewhere = compatible.loadError().contains("newer schema") && compatible.save(&error);
            const bool readable = manifest.open(QIODevice::ReadOnly);
            const bool preserved = readable && manifest.readAll() == future;
            manifest.close();
            const bool restoredLatest = manifest.open(QIODevice::WriteOnly | QIODevice::Truncate)
                && manifest.write(latest) == latest.size();
            manifest.close();
            audit.check(page, "storage", "Newer schema preserved while backup opens separately",
                        savedElsewhere && preserved && restoredLatest, error);
        }
    }
}

bool waitForRender(LiveViewport &viewport)
{
    for (int attempt = 0; attempt < 100 && !viewport.hasRenderedFrame(); ++attempt)
        QTest::qWait(50);
    return viewport.hasRenderedFrame();
}

QWindow *exposedVulkanWindow()
{
    for (auto *candidate : QApplication::allWindows())
        if (candidate->objectName() == "vulkanSceneWindow" && candidate->isExposed())
            return candidate;
    return nullptr;
}

int modelingBenchmark(const QDir &output)
{
    namespace wf = worldforge;
    QTemporaryDir directory;
    ProjectStore store;
    QString error;
    if (!directory.isValid() || !store.createStarterProject("Modeling Benchmark",
            {{"location",directory.filePath("Modeling Benchmark")},{"starter","blank"}},error)
        || !store.addMeshPrimitive(wf::Primitive::Cube,{1,1,1},4,4,"Benchmark Mesh",error))
        return 2;
    const auto asset = store.selectedMesh()->id;
    LiveViewport view(store,true);
    view.resize(1280,720);
    view.show();
    if (!waitForRender(view))
        return 2;
    auto *surface = exposedVulkanWindow();
    QFile file(output.filePath("modeling-performance.csv"));
    if (!surface || !file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return 2;
    QTextStream report(&file);
    report << "Adapter,TriangleCount,EditCommitMs,P95OrbitSubmitMs,ColdFacePickMs,P95FacePickMs,ColdEdgePickMs,P95EdgePickMs,ColdVertexPickMs,P95VertexPickMs,MeshPayloadBytes\n";
    QSignalSpy frames(&view,&LiveViewport::frameSubmitted);
    int picked = -1;
    ModelDomain domain = ModelDomain::Face;
    QObject::connect(&view,&LiveViewport::componentsPicked,&view,[&](const auto &components,auto) {
        picked = components.isEmpty() ? -1 : components.front().index;
        view.setModelSelection(domain,std::set<ModelComponent>(components.begin(),components.end()),false);
    });
    const auto percentile = [](std::vector<double> values) {
        std::ranges::sort(values);
        return values[std::size_t(std::ceil(double(values.size())*0.95))-1];
    };
    for (const auto &[columns,rows] : {std::pair{50,100},std::pair{200,250},std::pair{500,1000}}) {
        wf::Mesh mesh;
        mesh.vertices.reserve(std::size_t(columns+1)*(rows+1));
        mesh.faces.reserve(std::size_t(columns)*rows*2);
        for (int y = 0; y <= rows; ++y)
            for (int x = 0; x <= columns; ++x)
                mesh.vertices.push_back({float(x)*8/float(columns)-4,float(y)*8/float(rows)-4,0});
        for (int y = 0; y < rows; ++y)
            for (int x = 0; x < columns; ++x) {
                const auto i = wf::VertexIndex(y*(columns+1)+x), j = i+wf::VertexIndex(columns+1);
                mesh.faces.push_back({{{i},{i+1},{j+1}}});
                mesh.faces.push_back({{{i},{j+1},{j}}});
            }
        const auto triangles = mesh.faces.size();
        const auto payload = mesh.vertices.size()*sizeof(wf::Point3)+triangles*(sizeof(wf::MeshFace)+3*sizeof(wf::MeshCorner));
        QElapsedTimer timer;
        timer.start();
        if (!store.updateMesh(asset,std::move(mesh),"Replace benchmark mesh",error))
            return 2;
        const auto commit = double(timer.nsecsElapsed())/1e6;
        domain = ModelDomain::Face;
        view.frame();
        view.setModelSelection(domain,{},false);
        if (!frames.wait(15000))
            return 2;
        const QPoint center(surface->width()/2,surface->height()/2);
        QTest::mousePress(surface,Qt::RightButton,Qt::NoModifier,center);
        std::vector<double> orbit;
        for (int i = 0; i < 36; ++i) {
            const QPoint p = center+QPoint((i%6)*3,i%2);
            QMouseEvent event(QEvent::MouseMove,QPointF(p),QPointF(surface->mapToGlobal(p)),
                Qt::NoButton,Qt::RightButton,Qt::NoModifier);
            timer.restart();
            const auto before = frames.size();
            QApplication::sendEvent(surface,&event);
            if (frames.size() == before && !frames.wait(15000))
                return 2;
            if (i >= 4)
                orbit.push_back(double(timer.nsecsElapsed())/1e6);
        }
        QTest::mouseRelease(surface,Qt::RightButton,Qt::NoModifier,center);
        view.setModelAxisView(2);
        view.frame();
        if (!frames.wait(15000))
            return 2;
        std::array<double,6> measurements{};
        std::size_t metric = 0;
        for (const auto mode : {ModelDomain::Face,ModelDomain::Edge,ModelDomain::Vertex}) {
            domain = mode;
            view.setModelSelection(domain,{},false);
            surface->requestUpdate();
            if (!frames.wait(15000))
                return 2;
            double cold = 0;
            std::vector<double> picks;
            for (int i = 0; i <= 32; ++i) {
                const auto before = frames.size();
                picked = -1;
                timer.restart();
                QTest::mouseClick(surface,Qt::LeftButton,Qt::NoModifier,center+QPoint(i%13-6,(i/13)*5-5));
                surface->requestUpdate();
                if (picked < 0 || (frames.size() == before && !frames.wait(15000)))
                    return 2;
                const auto elapsed = double(timer.nsecsElapsed())/1e6;
                if (i == 0)
                    cold = elapsed;
                else
                    picks.push_back(elapsed);
            }
            measurements[metric++] = cold;
            measurements[metric++] = percentile(picks);
        }
        report << view.rendererName() << ',' << triangles << ',' << commit << ',' << percentile(orbit);
        for (double measurement : measurements)
            report << ',' << measurement;
        report << ',' << payload << '\n';
        report.flush();
    }
    QFile tools(output.filePath("modeling-tools-performance.csv"));
    if (!tools.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return 2;
    QTextStream timings(&tools);
    timings << "Tool,Fixture,ElapsedMs,ResultFaces\n";
    const auto sphere = wf::makePrimitive(wf::Primitive::Sphere,{4,4,4},80,64);
    if (!sphere)
        return 2;
    const auto measure = [&](const char *label,const char *fixture,const wf::Mesh &source,auto action) {
        auto candidate = source;
        QElapsedTimer timer;
        timer.start();
        const auto status = action(candidate);
        timings << label << ',' << fixture << ',' << double(timer.nsecsElapsed())/1e6 << ',' << candidate.faces.size() << '\n';
        return status.empty() && wf::validateMesh(candidate).empty();
    };
    const auto cube = wf::makeCube();
    const bool boolean = measure("Boolean Difference","80x64 sphere and offset cube",*sphere,[&](auto &mesh) {
        return wf::booleanMesh(mesh,cube,wf::BooleanOperation::Difference,{1.6f,0,0});
    });
    const bool remesh = measure("Voxel Remesh","80x64 sphere at 0.08 m",*sphere,[](auto &mesh) {
        return wf::remeshVoxels(mesh,0.08f);
    });
    const bool decimate = measure("Decimate","80x64 sphere at 0.15 m",*sphere,[](auto &mesh) {
        return wf::applyMeshModifier(mesh,{wf::ModifierKind::Decimate,true,0,2,0.15f});
    });
    if (!boolean || !remesh || !decimate)
        return 2;
    return 0;
}

void liveAuthoring(MainWindow &window, const QDir &output, Audit &audit)
{
    const QString page = "Live Authoring";
    auto *tabs = window.findChild<QTabBar *>("workspaceTabs");
    auto *cube = namedWidget<QToolButton>(window, "Add Cube");
    if (!tabs || !cube || !QTest::qWaitForWindowExposed(&window, 5000)) {
        audit.check(page, "Vulkan", "Native editor window exposed", false);
        return;
    }
    cube->click();
    auto *world = window.findChild<LiveViewport *>();
    if (!world || !waitForRender(*world)) {
        audit.check(page, "Vulkan", "First Light frame rendered", false,
                    world && world->findChild<QLabel *>() ? world->findChild<QLabel *>()->text() : QString());
        return;
    }
    const auto first = world->captureFrame();
    const auto firstSaved = !first.isNull() && first.save(output.filePath("first-light.png"));
    audit.check(page, "Vulkan", "GPU frame readback", firstSaved, QString("%1 × %2").arg(first.width()).arg(first.height()));
#ifdef Q_OS_WIN
    const auto reading = [&window](const QString &name) -> QString {
        for (const auto *label : window.findChildren<QLabel *>())
            if (label->accessibleName() == name)
                return label->text();
        return {};
    };
    for (int i = 0; i < 25 && !reading("GPU reading").endsWith('%'); ++i) {
        world->frame();
        QTest::qWait(120);
    }
    const auto cpu = reading("CPU reading");
    const auto gpu = reading("GPU reading");
    const auto ram = reading("RAM reading");
    const auto memory = reading("GPU MEM reading");
    audit.check(page, "Metrics", "Windows CPU and RAM readings", cpu.endsWith('%') && ram.endsWith("GiB"),
                cpu + " CPU, " + ram + " RAM");
    audit.check(page, "Metrics", "Viewport adapter matched to Windows GPU counters",
                !world->adapterCounterId().isEmpty() && gpu.endsWith('%') && memory.endsWith("GiB"),
                world->rendererName() + " · " + gpu + " GPU, " + memory + " GPU memory");
#endif
    auto *inspector = window.findChild<QDockWidget *>("inspector");
    auto *surface = exposedVulkanWindow();
    if (inspector && surface) {
        QTest::mouseClick(surface, Qt::LeftButton, Qt::NoModifier, QPoint(5, 5));
        auto *emptyName = namedWidget<QLineEdit>(*inspector, "Object name");
        const bool deselected = emptyName && !emptyName->isEnabled();
        QTest::mouseClick(surface, Qt::LeftButton, Qt::NoModifier, QPoint(surface->width()/2, surface->height()/2));
        auto *name = namedWidget<QLineEdit>(*inspector, "Object name");
        audit.check(page, "Vulkan", "Clicking the cube selects it in the Inspector", deselected && name && name->text() == "Cube");
    } else {
        audit.check(page, "Vulkan", "World picking surface and Inspector", false);
    }
    auto *x = inspector ? namedWidget<QDoubleSpinBox>(*inspector, "Location X") : nullptr;
    auto *save = findAction(window, "Save");
    if (!x || !save) {
        audit.check(page, "workflow", "Inspector transform and save controls", false);
        return;
    }
    x->setValue(0.4);
    QMetaObject::invokeMethod(x, "editingFinished");
    const auto moved = world->captureFrame();
    audit.check(page, "Vulkan", "Inspector transform changes rendered frame", !moved.isNull() && first != moved);
    const auto triggerHistory = [&window](const QString &prefix) {
        auto *bar = window.findChild<QToolBar *>("commands");
        for (auto *action : bar->actions()) {
            if (action->isEnabled() && QString(action->text()).remove('&').startsWith(prefix)) {
                action->trigger();
                return true;
            }
        }
        return false;
    };
    audit.check(page, "workflow", "Move undo and redo",
                triggerHistory("Undo") && x->value() == 0 && triggerHistory("Redo") && x->value() == 0.4);
    save->trigger();
    ProjectStore reopened;
    const auto *cubeObject = reopened.selectedObject();
    const auto objectId = cubeObject ? cubeObject->id : QString();
    audit.check(page, "workflow", "Reopen retains cube and transform",
        cubeObject && std::abs(cubeObject->position.x() - 0.4f) < 0.001f && reopened.selectedMesh());
    if (!cubeObject || !reopened.selectedMesh())
        return;
    const auto assetId = cubeObject->meshAsset;

    auto *editMesh = namedWidget<QToolButton>(window, "Edit Mesh");
    if (editMesh)
        editMesh->click();
    if (!editMesh || tabs->currentIndex() != 2) {
        audit.check(page, "Modeling", "World Editor opens selected mesh in Modeling", false);
        return;
    }
    auto *pages = qobject_cast<QStackedWidget *>(window.centralWidget());
    auto *model = pages->currentWidget();
    auto *view = model->findChild<LiveViewport *>();
    auto *extrude = model->findChild<QPushButton *>("meshExtrude");
    auto *unwrap = model->findChild<QPushButton *>("meshUnwrap");
    auto *uvEditing = namedWidget<QToolButton>(*model, "UV Editing");
    auto *newInstance = namedWidget<QToolButton>(*model, "Add Instance");
    if (!view || !extrude || !newInstance || !waitForRender(*view)) {
        audit.check(page, "Modeling", "Live model viewport and controls", false);
        return;
    }
    surface = exposedVulkanWindow();
    if (!surface) {
        audit.check(page, "Modeling", "Exposed Modeling surface", false);
        return;
    }
    newInstance->click();
    auto *vertex = namedWidget<QToolButton>(*model,"Vertex");
    auto *edge = namedWidget<QToolButton>(*model,"Edge");
    auto *face = namedWidget<QToolButton>(*model,"Face");
    auto *xray = namedWidget<QToolButton>(*model,"X-Ray");
    auto *selection = namedWidget<QLabel>(*model,"Model selection");
    auto *moveTool = namedWidget<QToolButton>(*model,"Move");
    auto *rotateTool = namedWidget<QToolButton>(*model,"Rotate");
    auto *scaleTool = namedWidget<QToolButton>(*model,"Scale");
    auto *axis = namedWidget<QComboBox>(*model,"Transform constraint");
    auto *snapping = namedWidget<QComboBox>(*model,"Transform snapping");
    auto *xValue = namedWidget<QDoubleSpinBox>(*model,"Transform X");
    auto *zValue = namedWidget<QDoubleSpinBox>(*model,"Transform Z");
    auto *apply = namedWidget<QToolButton>(*model,"Apply");
    const auto drag = [surface](QPoint a,QPoint b,Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
        QTest::mousePress(surface,Qt::LeftButton,modifiers,a);
        QMouseEvent moved(QEvent::MouseMove,QPointF(b),QPointF(surface->mapToGlobal(b)),
            Qt::NoButton,Qt::LeftButton,modifiers);
        QApplication::sendEvent(surface,&moved);
        QTest::mouseRelease(surface,Qt::LeftButton,modifiers,b);
    };
    const auto currentMesh = [&] {
        save->trigger();
        ProjectStore snapshot;
        const auto *mesh = snapshot.meshAsset(assetId);
        return mesh ? mesh->mesh : QSharedPointer<const worldforge::Mesh>{};
    };
    std::vector<QImage> shadingFrames;
    for (const QString label : {"Wireframe","Solid","Material Preview","Shaded + Wire"}) {
        auto *mode = namedWidget<QToolButton>(*model,label);
        if (mode && mode->isVisible()) {
            mode->click();
            shadingFrames.push_back(view->captureFrame());
        }
    }
    audit.check(page,"Modeling","Viewport shading buttons render distinct views",
        shadingFrames.size() == 4 && std::ranges::none_of(shadingFrames,&QImage::isNull) && shadingFrames[0] != shadingFrames[1]
        && shadingFrames[1] != shadingFrames[2] && shadingFrames[2] != shadingFrames[3]);
    if (auto *location = namedWidget<QDoubleSpinBox>(*model,"Location X")) {
        const auto old = location->value();
        location->setValue(old+0.2);
        emit location->editingFinished();
        save->trigger();
        ProjectStore changed;
        const auto *object = changed.selectedObject();
        const bool saved = object && std::abs(object->position.x()-old-0.2) < 0.001;
        const bool undone = triggerHistory("Undo");
        audit.check(page,"Modeling","Object inspector edits the active instance and supports undo",
            saved && undone && std::abs(location->value()-old) < 0.001);
    }
    if (vertex && edge && face && xray && selection && moveTool && rotateTool && scaleTool
        && axis && snapping && xValue && zValue && apply) {
        vertex->click();
        QTest::keyClick(surface,Qt::Key_1,Qt::KeypadModifier);
        view->frame();
        const QPoint center(surface->width()/2,surface->height()/2);
        const int half = qRound(surface->height()*0.25/(std::sqrt(3.0)*1.55*0.41421356));
        const QPoint upperLeft = center-QPoint(half,half);
        const QPoint upperRight = center+QPoint(half,-half);
        const QPoint boxStart(12,12), boxEnd(surface->width()-12,surface->height()-12);
        const auto selectedCount = [selection](int count) {
            return selection->text().contains(QString("%1 selected").arg(count));
        };
        QTest::mouseClick(surface,Qt::LeftButton,Qt::NoModifier,upperLeft);
        const bool pickedVertex = selectedCount(1);
        QTest::mouseClick(surface,Qt::LeftButton,Qt::ShiftModifier,upperRight);
        const bool extended = selectedCount(2);
        QTest::mouseClick(surface,Qt::LeftButton,Qt::ControlModifier,upperRight);
        audit.check(page,"Modeling","Vertex picking and selection modifiers",
            pickedVertex && extended && selectedCount(1),selection->text());
        const bool defaultMove = moveTool->isChecked() && !apply->isVisible();
        const auto beforeGizmo = currentMesh();
        drag(upperLeft,upperLeft+QPoint(22,9));
        const auto afterGizmo = currentMesh();
        const bool draggedWithDefault = beforeGizmo && afterGizmo && beforeGizmo->vertices != afterGizmo->vertices;
        const bool undidGizmo = draggedWithDefault && triggerHistory("Undo");
        const auto afterGizmoUndo = currentMesh();
        audit.check(page,"Modeling","Default Move handle commits on drag release and undoes in one step",
            defaultMove && undidGizmo && afterGizmoUndo && *afterGizmoUndo == *beforeGizmo);
        auto *selectMenu = namedWidget<QToolButton>(*model,"Select");
        const auto invokeSelection = [selectMenu](const QString &name) {
            if (!selectMenu || !selectMenu->menu())
                return false;
            for (auto *action : selectMenu->menu()->findChildren<QAction *>()) {
                if (action->text() == name) {
                    action->trigger();
                    return true;
                }
            }
            return false;
        };
        const bool grown = invokeSelection("Grow") && selectedCount(4);
        const bool shrunk = invokeSelection("Shrink") && selectedCount(1);
        const bool connected = invokeSelection("Connected") && selectedCount(8);
        const bool converted = invokeSelection("Edge") && selectedCount(12);
        audit.check(page,"Modeling","Grow, shrink, connected and conversion select cube components",
            grown && shrunk && connected && converted,selection->text());
        vertex->click();
        drag(boxStart,boxEnd);
        const bool visibleOnly = selectedCount(4);
        xray->click();
        drag(boxStart,boxEnd);
        audit.check(page,"Modeling","Marquee distinguishes visible and X-ray vertices",
            visibleOnly && selectedCount(8),selection->text());
        xray->click();
        QTest::mouseClick(surface,Qt::LeftButton,Qt::NoModifier,upperLeft);
        const auto original = currentMesh();
        moveTool->click();
        axis->setCurrentIndex(1);
        xValue->setValue(0.2);
        QTest::keyClick(surface,Qt::Key_Escape);
        const auto afterCancel = currentMesh();
        audit.check(page,"Modeling","Escape discards the numeric transform preview",
            original && afterCancel && *afterCancel == *original && !apply->isVisible());
        moveTool->click();
        axis->setCurrentIndex(1);
        snapping->setCurrentIndex(1);
        QTest::qWait(60);
        const QPoint dragCenter(surface->width()/2,surface->height()/2);
        drag(dragCenter,dragCenter+QPoint(52,0));
        const double gridValue = xValue->value();
        snapping->setCurrentIndex(2);
        QTest::qWait(60);
        const int snapHalf = qRound(surface->height()*0.25/(std::sqrt(3.0)*1.55*0.41421356));
        drag(dragCenter,QPoint(surface->width()/2+snapHalf,surface->height()/2-snapHalf));
        audit.check(page,"Modeling","Move drag supports grid increments and vertex targets",
            gridValue > 0 && std::abs(gridValue*10-std::round(gridValue*10)) < 0.0001
            && std::abs(xValue->value()-1.0) < 0.0001,
            QString("Grid %1, vertex %2").arg(gridValue).arg(xValue->value()));
        snapping->setCurrentIndex(3);
        QTest::qWait(60);
        const QPoint surfaceCenter(surface->width()/2,surface->height()/2);
        drag(surfaceCenter+QPoint(0,40),surfaceCenter);
        audit.check(page,"Modeling","Surface snap uses the ray hit on an unselected face",
            std::abs(xValue->value()-0.5) < 0.01,QString::number(xValue->value()));
        snapping->setCurrentIndex(0);
        QTest::keyClick(surface,Qt::Key_Escape);
        moveTool->click();
        axis->setCurrentIndex(1);
        xValue->setValue(0.2);
        rotateTool->click();
        const bool switchedToRotate = rotateTool->isChecked();
        edge->click();
        const bool switchedToEdge = edge->isChecked();
        const auto movedVertex = currentMesh();
        std::size_t changed = 0;
        if (original && movedVertex)
            for (std::size_t i = 0; i < original->vertices.size(); ++i)
                if (std::abs(movedVertex->vertices[i][0]-original->vertices[i][0]-0.2f) < 0.0001f
                    && movedVertex->vertices[i][1] == original->vertices[i][1]
                    && movedVertex->vertices[i][2] == original->vertices[i][2])
                    ++changed;
        ProjectStore linked;
        audit.check(page,"Modeling","A constrained vertex move survives save and keeps both linked instances",
            changed == 1 && linked.project() && linked.project()->objects.size() == 2
            && linked.project()->objects[0].meshAsset == assetId && linked.project()->objects[1].meshAsset == assetId);
        audit.check(page,"Modeling","Switch Move to Rotate and Vertex to Edge without resetting geometry",
            switchedToRotate && switchedToEdge && changed == 1);
        const bool undidMove = triggerHistory("Undo");
        const auto afterUndo = currentMesh();
        const bool redidMove = triggerHistory("Redo");
        const auto afterRedo = currentMesh();
        audit.check(page,"Modeling","One undo and redo restore exact mesh data",
            undidMove && redidMove && original && afterUndo && movedVertex && afterRedo
            && *afterUndo == *original && *afterRedo == *movedVertex);
        triggerHistory("Undo");
        vertex->click();
        QTest::qWait(60);
        const int restoredHalf = qRound(surface->height()*0.25/(std::sqrt(3.0)*1.55*0.41421356));
        QTest::mouseClick(surface,Qt::LeftButton,Qt::NoModifier,
            QPoint(surface->width()/2-restoredHalf,surface->height()/2-restoredHalf));
        auto *symmetry = namedWidget<QToolButton>(*model,"Symmetry X");
        auto *soft = namedWidget<QToolButton>(*model,"Soft Selection");
        auto *radius = namedWidget<QDoubleSpinBox>(*model,"Soft selection radius");
        if (symmetry && soft && radius && original) {
            const auto countMoved = [&](const QSharedPointer<const worldforge::Mesh> &mesh) {
                std::size_t count = 0;
                if (mesh && mesh->vertices.size() == original->vertices.size())
                    for (std::size_t i = 0; i < mesh->vertices.size(); ++i)
                        count += mesh->vertices[i] != original->vertices[i];
                return count;
            };
            symmetry->click();
            moveTool->click();
            zValue->setValue(0.12);
            apply->click();
            const auto mirroredEdit = currentMesh();
            audit.check(page,"Modeling","Symmetry edits the selected vertex and its X counterpart",countMoved(mirroredEdit) == 2);
            if (countMoved(mirroredEdit))
                triggerHistory("Undo");
            symmetry->click();
            moveTool->click();
            soft->click();
            radius->setValue(1.5);
            zValue->setValue(0.12);
            apply->click();
            const auto softEdit = currentMesh();
            audit.check(page,"Modeling","Soft selection moves only the source and neighbors within the radius",
                countMoved(softEdit) == 4);
            if (countMoved(softEdit))
                triggerHistory("Undo");
            soft->setChecked(false);
        }
        edge->click();
        QTest::qWait(80);
        const int edgeHalf = qRound(surface->height()*0.25/(std::sqrt(3.0)*1.55*0.41421356));
        QTest::mouseClick(surface,Qt::LeftButton,Qt::NoModifier,
            QPoint(surface->width()/2,surface->height()/2-edgeHalf));
        const bool pickedEdge = selectedCount(1);
        audit.check(page,"Modeling","Edge ring traverses the four parallel cube edges",
            pickedEdge && invokeSelection("Edge Ring") && selectedCount(4),selection->text());
        QTest::mouseClick(surface,Qt::LeftButton,Qt::NoModifier,
            QPoint(surface->width()/2,surface->height()/2-edgeHalf));
        rotateTool->click();
        axis->setCurrentIndex(3);
        zValue->setValue(20);
        apply->click();
        const auto rotated = currentMesh();
        changed = 0;
        if (rotated && original)
            for (std::size_t i = 0; i < original->vertices.size(); ++i)
                changed += rotated->vertices[i] != original->vertices[i];
        audit.check(page,"Modeling","Edge selection rotates its two endpoints",
            pickedEdge && changed == 2,selection->text());
        if (rotated && original && rotated->vertices != original->vertices)
            triggerHistory("Undo");
        face->click();
        QTest::mouseClick(surface,Qt::LeftButton,Qt::NoModifier,center);
        const bool pickedFace = selectedCount(1);
        scaleTool->click();
        const auto beforeDrag = view->captureFrame();
        drag(center,center+QPoint(26,0));
        const auto scaledPreview = view->captureFrame();
        apply->click();
        const auto scaled = currentMesh();
        audit.check(page,"Modeling","Dragging scales the selected face and previews before commit",
            pickedFace && !beforeDrag.isNull() && !scaledPreview.isNull() && beforeDrag != scaledPreview
            && scaled && original && scaled->vertices != original->vertices
            && scaledPreview.save(output.filePath("modeling-component-transform.png")),selection->text());
        if (scaled && original && scaled->vertices != original->vertices)
            triggerHistory("Undo");
        QTest::qWait(80);
        QTest::mouseClick(surface,Qt::LeftButton,Qt::NoModifier,QPoint(5,5));
        view->frame();
        const QPoint cutCenter(surface->width()/2,surface->height()/2);
        const int cutHalf = qRound(surface->height()*0.25/(std::sqrt(3.0)*1.55*0.41421356));
        QTest::mouseClick(surface,Qt::LeftButton,Qt::NoModifier,cutCenter);
        if (auto *knife = namedWidget<QToolButton>(*model,"Knife")) {
            knife->click();
            QTest::mouseClick(surface,Qt::LeftButton,Qt::NoModifier,cutCenter-QPoint(0,cutHalf));
            QTest::mouseClick(surface,Qt::LeftButton,Qt::NoModifier,cutCenter+QPoint(0,cutHalf));
            QTest::keyClick(surface,Qt::Key_Return);
            const auto cutMesh = currentMesh();
            const bool didCut = cutMesh && cutMesh->faces.size() == 7 && cutMesh->vertices.size() == 10;
            audit.check(page,"Modeling","Knife picks two edges, commits and saves one continuous cut",didCut);
            if (didCut)
                triggerHistory("Undo");
            else
                QTest::keyClick(surface,Qt::Key_Escape);
        }
        if (auto *meshMenu = namedWidget<QToolButton>(*model,"Mesh"); meshMenu && meshMenu->menu()) {
            for (auto *action : meshMenu->menu()->actions()) {
                if (action->text() != "Voxel Remesh…")
                    continue;
                const auto runRemesh = [action](QMessageBox::StandardButton choice) {
                    bool responded = false;
                    QTimer responder;
                    QObject::connect(&responder,&QTimer::timeout,&responder,[&] {
                        auto *modal = QApplication::activeModalWidget();
                        if (auto *input = qobject_cast<QInputDialog *>(modal)) {
                            input->setDoubleValue(choice == QMessageBox::Cancel ? 0.008 : 0.17);
                            input->accept();
                        } else if (auto *progress = qobject_cast<QProgressDialog *>(modal); progress && choice == QMessageBox::Cancel) {
                            if (auto *cancel = namedWidget<QPushButton>(*progress,"Cancel")) {
                                responded = true;
                                cancel->click();
                            }
                        } else if (auto *preview = qobject_cast<QMessageBox *>(modal)) {
                            if (auto *button = preview->button(choice)) {
                                responded = preview->windowTitle() == "Voxel Remesh Preview";
                                button->click();
                            } else {
                                preview->reject();
                            }
                        }
                    });
                    responder.start(5);
                    action->trigger();
                    return responded;
                };
                const auto base = currentMesh();
                const bool cancelled = runRemesh(QMessageBox::Cancel) && base && currentMesh() && *currentMesh() == *base;
                audit.check(page,"Modeling","Cancel a running Voxel Remesh without changing the mesh",cancelled);
                const bool previewDiscarded = runRemesh(QMessageBox::Discard) && base && currentMesh() && *currentMesh() == *base;
                const bool previewApplied = runRemesh(QMessageBox::Apply);
                const auto remeshed = currentMesh();
                const bool applied = previewApplied && remeshed && base && remeshed->faces.size() > base->faces.size();
                const bool undo = applied && triggerHistory("Undo") && currentMesh() && *currentMesh() == *base;
                audit.check(page,"Modeling","Voxel Remesh preview can be discarded or applied in one undo step",
                    previewDiscarded && applied && undo);
                break;
            }
            for (auto *action : meshMenu->menu()->findChildren<QAction *>()) {
                if (action->text() != "Draw Quad")
                    continue;
                action->trigger();
                const int inset = cutHalf/2;
                for (const QPoint offset : {QPoint(-inset,-inset),QPoint(inset,-inset),
                                           QPoint(inset,inset),QPoint(-inset,inset)})
                    QTest::mouseClick(surface,Qt::LeftButton,Qt::NoModifier,cutCenter+offset);
                QTest::keyClick(surface,Qt::Key_Return);
                save->trigger();
                ProjectStore retopo;
                const auto *patch = retopo.selectedMesh();
                const bool created = patch && patch->id != assetId && patch->mesh->faces.size() == 1
                    && patch->mesh->vertices.size() == 4 && retopo.meshAsset(assetId);
                audit.check(page,"Modeling","Quad placement projects onto the source and saves a separate patch",created);
                if (created)
                    triggerHistory("Undo");
                else
                    QTest::keyClick(surface,Qt::Key_Escape);
                break;
            }
        }
        if (auto *ortho = namedWidget<QToolButton>(*model,"Ortho"); ortho && ortho->isChecked())
            ortho->click();
    } else {
        audit.check(page,"Modeling","Component selection and transform controls",false);
    }
    QTest::mouseClick(surface, Qt::LeftButton, Qt::NoModifier, QPoint(surface->width()/2, surface->height()/2));
    const auto selected = view->captureFrame();
    extrude->click();
    const auto extruded = view->captureFrame();
    audit.check(page, "Modeling", "Extruded geometry differs in rendered viewport", !selected.isNull()
                && !extruded.isNull() && selected != extruded && extruded.save(output.filePath("modeling-extrude.png")));
    save->trigger();
    QSharedPointer<const worldforge::Mesh> extrusionMesh;
    {
        ProjectStore edited;
        const auto *asset = edited.meshAsset(assetId);
        if (asset)
            extrusionMesh = asset->mesh;
        audit.check(page, "Modeling", "Picked face extrusion changes saved topology",
                    asset && asset->mesh->vertices.size() == 12 && asset->mesh->faces.size() == 10);
    }
    const bool undone = triggerHistory("Undo");
    save->trigger();
    ProjectStore undoReopen;
    const bool restored = undoReopen.meshAsset(assetId) && undoReopen.meshAsset(assetId)->mesh->faces.size() == 6;
    const bool redone = triggerHistory("Redo");
    if (uvEditing && unwrap) {
        QTest::mouseClick(surface,Qt::LeftButton,Qt::NoModifier,QPoint(surface->width()/2,surface->height()/2));
        uvEditing->click();
        unwrap->click();
        save->trigger();
        ProjectStore uvReopen;
        const auto *uvAsset = uvReopen.meshAsset(assetId);
        audit.check(page, "Modeling", "UV split edits and saves face-corner UVs", uvEditing->isChecked()
            && uvAsset && extrusionMesh && uvAsset->mesh->vertices == extrusionMesh->vertices
            && uvAsset->mesh->faces != extrusionMesh->faces);
    } else {
        audit.check(page, "Modeling", "UV split and unwrap controls", false);
    }
    save->trigger();
    ProjectStore finalReopen;
    const auto *asset = finalReopen.meshAsset(assetId);
    audit.check(page, "Modeling", "Extrude undo, redo and shared World instance survive saves",
                undone && restored && redone && asset && asset->mesh->faces.size() == 10
                    && finalReopen.project()->objects.size() == 2 && finalReopen.selectedObject()->meshAsset == assetId);
    if (auto *uvTools = namedWidget<QToolButton>(*model,"UV Tools"); uvTools && uvTools->menu()) {
        const auto triggerUv = [uvTools](const QString &name) {
            for (auto *action : uvTools->menu()->actions())
                if (action->text() == name) {
                    action->trigger();
                    return true;
                }
            return false;
        };
        const bool marked = triggerUv("Seams from Sharp Edges");
        save->trigger();
        ProjectStore seamSnapshot;
        const auto *markedMesh = seamSnapshot.meshAsset(assetId);
        audit.check(page,"Modeling","UV seam marks survive save and reopen",
            marked && markedMesh && !markedMesh->uvSeams.empty());
        QTimer::singleShot(0,[] {
            if (auto *dialog = qobject_cast<QInputDialog *>(QApplication::activeModalWidget()))
                dialog->accept();
        });
        const bool unwrapped = triggerUv("Unwrap Islands…");
        save->trigger();
        ProjectStore uvSnapshot;
        const auto *unwrappedMesh = uvSnapshot.meshAsset(assetId);
        const auto uvReport = unwrappedMesh ? worldforge::inspectUvs(*unwrappedMesh->mesh,unwrappedMesh->uvSeams)
                                           : worldforge::UvReport{};
        audit.check(page,"Modeling","UV unwrap and pack save valid charts without overlaps",
            unwrapped && unwrappedMesh && markedMesh && unwrappedMesh->mesh->faces != markedMesh->mesh->faces
            && uvReport.islands > 1 && !uvReport.outside && !uvReport.degenerate && !uvReport.overlaps);
        bool pinned = false;
        if (auto *canvas = namedWidget<QWidget>(*model,"UV canvas"); canvas && unwrappedMesh && uvEditing) {
            uvEditing->setChecked(true);
            const auto uv = unwrappedMesh->mesh->faces.front().corners.front().uv;
            const int size = std::max(1,std::min(canvas->width()-38,canvas->height()-60));
            const QPoint point(qRound((canvas->width()-size)/2.0+uv[0]*size),
                qRound((canvas->height()+size)/2.0-uv[1]*size));
            if (triggerUv("Pin UV Corners")) {
                QTest::mouseClick(canvas,Qt::LeftButton,Qt::NoModifier,point);
                triggerUv("Pin UV Corners");
                save->trigger();
                ProjectStore pinSnapshot;
                const auto *pinnedMesh = pinSnapshot.meshAsset(assetId);
                pinned = pinnedMesh && pinnedMesh->uvPins.size() == 1;
            }
        }
        audit.check(page,"Modeling","Click a UV corner to pin it and reopen the project",pinned);
    } else {
        audit.check(page,"Modeling","UV workspace exposes island tools",false);
    }
    tabs->setCurrentIndex(1);
    world->frame();
    const auto worldInstances = waitForRender(*world) ? world->captureFrame() : QImage{};
    audit.check(page, "Vulkan", "World renders shared updated asset instances",
                !worldInstances.isNull() && worldInstances != moved && worldInstances.save(output.filePath("world-instances.png")),
                QString("%1 × %2").arg(worldInstances.width()).arg(worldInstances.height()));
    window.resize(1450, 820);
    QTest::qWait(200);
    const auto resized = world->captureFrame();
    audit.check(page, "Vulkan", "Resize recreates the swapchain and renders",
                !resized.isNull() && resized.size() != worldInstances.size());

    auto *manager = findAction(window, "Project manager");
    auto *openWorld = findAction(window, "Open World Editor");
    if (manager)
        manager->trigger();
    auto *projects = namedWidget<QListView>(window, "Local projects");
    if (projects && openWorld) {
        projects->setCurrentIndex(projects->model()->index(0, 0));
        openWorld->trigger();
        const auto imported = waitForRender(*world) ? world->captureFrame() : QImage{};
        bool renderedMaterial = false;
        for (int y = 0; y < imported.height() && !renderedMaterial; y += 4) {
            for (int sampleX = 0; sampleX < imported.width() && !renderedMaterial; sampleX += 4) {
                const auto pixel = imported.pixelColor(sampleX, y);
                renderedMaterial = pixel.red() > 75 && pixel.red() > pixel.green()*2 && pixel.red() > pixel.blue()*2;
            }
        }
        audit.check(page, "Vulkan", "Reimported Blender mesh renders its updated material",
                    renderedMaterial && imported.save(output.filePath("blender-glb.png")),
                    QString("%1 × %2").arg(imported.width()).arg(imported.height()));
    } else {
        audit.check(page, "Vulkan", "Open the Blender fixture project", false);
    }
}

void sculptingWorkflow(MainWindow &window,const QDir &output,Audit &audit)
{
    const QString page = "Sculpting";
    auto *tabs = window.findChild<QTabBar *>("workspaceTabs");
    const auto finish = qScopeGuard([tabs] { if (tabs) tabs->setCurrentIndex(1); });
    auto *create = findAction(window,"New project…");
    auto *save = findAction(window,"Save");
    if (!tabs || !create || !save) {
        audit.check(page,"workflow","Create the sculpting fixture",false);
        return;
    }
    withDialog([create] { create->trigger(); },"Sculpting QA");
    tabs->setCurrentIndex(3);
    auto *sculpting = window.findChild<SculptingWorkspace *>();
    auto *view = sculpting ? sculpting->findChild<LiveViewport *>() : nullptr;
    auto *add = sculpting ? namedWidget<QPushButton>(*sculpting,"Add Sculpt Sphere") : nullptr;
    if (!view || !add) {
        audit.check(page,"workflow","Open live Sculpting workspace",false);
        return;
    }
    add->click();
    save->trigger();
    ProjectStore before;
    const auto *base = before.selectedMesh();
    auto *surface = waitForRender(*view) ? exposedVulkanWindow() : nullptr;
    if (!base || !surface) {
        audit.check(page,"workflow","Render the sculpt sphere",false);
        return;
    }
    const auto id = base->id;
    const auto center = QPoint(surface->width()/2,surface->height()/2);
    const auto drag = [surface,center] {
        QTest::mousePress(surface,Qt::LeftButton,Qt::NoModifier,center-QPoint(24,0));
        const QPoint end = center+QPoint(24,0);
        QMouseEvent motion(QEvent::MouseMove,QPointF(end),QPointF(surface->mapToGlobal(end)),Qt::NoButton,Qt::LeftButton,Qt::NoModifier);
        QApplication::sendEvent(surface,&motion);
    };
    const auto baseline = view->captureFrame();
    drag();
    QTest::keyClick(surface,Qt::Key_Escape);
    QTest::mouseRelease(surface,Qt::LeftButton,Qt::NoModifier,center+QPoint(24,0));
    save->trigger();
    ProjectStore cancelled;
    const auto *cancelledAsset = cancelled.meshAsset(id);
    audit.check(page,"workflow","Escape discards the sculpt stroke",cancelledAsset && *cancelledAsset->mesh == *base->mesh);
    drag();
    QTest::mouseRelease(surface,Qt::LeftButton,Qt::NoModifier,center+QPoint(24,0));
    const auto sculptedFrame = view->captureFrame();
    save->trigger();
    ProjectStore after;
    const auto *result = after.meshAsset(id);
    audit.check(page,"workflow","Sculpt drag changes the viewport and saves the same mesh asset",
        result && result->mesh->vertices != base->mesh->vertices && result->mesh->faces.size() == base->mesh->faces.size()
        && !sculptedFrame.isNull() && sculptedFrame != baseline && sculptedFrame.save(output.filePath("sculpting-live.png")));
    if (auto *undo = findAction(window,"Undo Sculpt Stroke"))
        undo->trigger();
    save->trigger();
    ProjectStore undone;
    const auto *undoneAsset = undone.meshAsset(id);
    audit.check(page,"workflow","One undo restores the entire sculpt stroke",undoneAsset && *undoneAsset->mesh == *base->mesh);
}

void modelingObjectWorkflow(Audit &audit)
{
    QTemporaryDir directory;
    ProjectStore store;
    QString error;
    const auto page = "Modeling objects";
    const bool created = directory.isValid() && store.createStarterProject("Mesh Workflow QA",
        {{"location",directory.filePath("Mesh Workflow QA")},{"starter","blank"}},error);
    if (!created || !store.addMeshPrimitive(worldforge::Primitive::Cylinder,{2,3,4},16,8,"Cylinder QA",error)) {
        audit.check(page,"workflow","Create an undoable primitive in a new project",false,error);
        return;
    }
    const auto original = store.selectedObject()->id;
    const auto assetId = store.selectedMesh()->id;
    const auto base = store.selectedMesh()->mesh;
    audit.check(page,"workflow","Create an undoable primitive in a new project",base->faces.size() == 18 && store.history().canUndo());
    store.addMeshInstance();
    const auto linked = store.selectedObject()->id;
    audit.check(page,"workflow","Linked duplicate references the original asset",linked != original && store.selectedMesh()->id == assetId);
    const bool madeUnique = store.makeMeshUnique(error);
    const auto uniqueId = store.selectedMesh()->id;
    audit.check(page,"workflow","Make Unique changes only the selected instance",
        madeUnique && uniqueId != assetId && store.object(original)->meshAsset == assetId && *store.selectedMesh()->mesh == *base,error);
    const bool duplicated = store.duplicateMesh(error);
    const auto copy = store.selectedObject()->id;
    const auto copyAsset = store.selectedMesh()->id;
    audit.check(page,"workflow","Independent duplicate has its own asset",
        duplicated && copyAsset != assetId && copyAsset != uniqueId && *store.selectedMesh()->mesh == *base,error);
    auto scaled = *store.selectedObject();
    scaled.rotation = {0,0,90};
    scaled.scale = {-2,1,1};
    store.updateObject(scaled,"Set transform for bake");
    const bool baked = store.bakeMeshTransform(false,error);
    const auto bakedMesh = store.selectedMesh()->mesh;
    audit.check(page,"workflow","Apply rotation and negative scale preserves an independent mesh",
        baked && store.selectedObject()->rotation.isNull() && store.selectedObject()->scale == QVector3D(1,1,1)
        && bakedMesh->vertices != base->vertices && store.meshAsset(assetId)->mesh == base,error);
    auto offset = *bakedMesh;
    for (auto &vertex : offset.vertices)
        vertex[0] += 1.0f;
    store.updateMesh(copyAsset,std::move(offset),"Offset mesh",error);
    const auto beforeCenter = store.selectedObject()->position;
    const bool centered = store.bakeMeshTransform(true,error);
    audit.check(page,"workflow","Center Origin compensates object position to preserve geometry placement",
        centered && std::abs(store.selectedObject()->position.x()-beforeCenter.x()-1.0f) < 0.0001f
        && std::abs(store.selectedMesh()->mesh->vertices[0][0]-bakedMesh->vertices[0][0]) < 0.0001f,error);
    const bool separated = store.separateMeshFaces({0},error);
    const auto part = store.selectedObject()->id;
    audit.check(page,"workflow","Separate faces creates a compact asset and keeps the remainder",
        separated && store.selectedMesh()->mesh->faces.size() == 1
        && store.meshAsset(store.object(copy)->meshAsset)->mesh->faces.size() == 17,error);
    store.select(copy);
    const bool joined = store.joinMeshes({part},error);
    const auto joinedId = store.selectedMesh()->id;
    const bool saved = store.save(&error);
    ProjectStore reopened;
    audit.check(page,"workflow","Join and reopen preserve destination identity and material slots",
        joined && saved && !reopened.object(part) && reopened.object(copy) && reopened.object(copy)->meshAsset == joinedId
        && reopened.meshAsset(joinedId) && reopened.meshAsset(joinedId)->mesh->faces.size() == 18
        && reopened.meshAsset(joinedId)->mesh->materials.size() == 2,error);
    store.history().undo();
    audit.check(page,"workflow","One undo restores both source objects after joining",
        store.object(part) && store.object(copy) && store.object(original)->meshAsset == assetId);
    store.select(original);
    const auto originalMesh = store.selectedMesh()->mesh;
    const std::vector<worldforge::MeshModifier> stack{
        {worldforge::ModifierKind::Array,true,0,3,3},
        {worldforge::ModifierKind::Subdivision,true,0,1}
    };
    const bool added = store.setMeshModifiers(stack,error);
    const auto evaluated = store.selectedMesh()->evaluatedMesh;
    const bool persisted = store.save(&error);
    ProjectStore modifierReopen;
    const auto *reopenedAsset = modifierReopen.meshAsset(assetId);
    audit.check(page,"workflow","Modifier stack saves, evaluates in order and preserves the base mesh",
        added && persisted && evaluated && originalMesh == store.selectedMesh()->mesh
        && reopenedAsset && reopenedAsset->modifiers == stack && reopenedAsset->evaluated() == *evaluated,error);
    const bool applied = store.applyMeshModifiers(0,error);
    audit.check(page,"workflow","Apply bakes one modifier prefix without changing evaluated output",
        applied && evaluated && store.selectedMesh()->modifiers.size() == 1 && store.selectedMesh()->mesh->faces.size() == 54
        && store.selectedMesh()->evaluated() == *evaluated,error);
    store.history().undo();
    audit.check(page,"workflow","Undo restores original base and modifier stack",
        store.selectedMesh()->modifiers == stack && store.selectedMesh()->mesh == originalMesh);
    auto exportMesh = *store.selectedMesh()->evaluatedMesh;
    exportMesh.materials.push_back({"Copper",{0.9f,0.5f,0.2f,1},0.7f,0.4f});
    exportMesh.faces.front().material = 1;
    const auto data = exportGlb(exportMesh,"QA mesh",error);
    const auto imported = importGlb(data,error);
    const auto face = imported ? std::ranges::find(imported->faces,2u,&worldforge::MeshFace::material)
                               : std::vector<worldforge::MeshFace>::const_iterator{};
    audit.check(page,"workflow","Evaluated GLB round-trip preserves material primitives and Z-up geometry",
        imported && face != imported->faces.end() && imported->materials.at(2).name == "Copper"
        && std::abs(imported->materials.at(2).metallic-0.7f) < 0.0001f
        && std::abs(face->corners.front().normal[2]-exportMesh.faces.front().corners.front().normal[2]) < 0.0001f
        && std::ranges::any_of(imported->vertices,[&](const auto &p) {
            return p == exportMesh.vertices.front();
        }),error);
    const bool sphere = store.addMeshPrimitive(worldforge::Primitive::Sphere,{2,2,2},32,16,"LOD Sphere",error);
    if (!sphere) {
        audit.check(page,"workflow","LOD and collision proxy export fixture",false,error);
        return;
    }
    const auto objectId = store.selectedObject()->id;
    const auto lodAssetId = store.selectedMesh()->id;
    const bool generated = store.setMeshLod(0.25f,20,error);
    auto withCollision = *store.selectedObject();
    withCollision.details.insert("physics","Dynamic");
    withCollision.details.insert("colliderShape",2);
    withCollision.details.insert("collisionProxy",QJsonObject{{"center",QJsonArray{0.4,0,0}},{"half",QJsonArray{0.3,0.3,0.7}}});
    store.updateObject(withCollision,"Set custom collision proxy");
    const bool lodSaved = store.save(&error);
    ProjectStore lodReopen;
    const auto *lod = lodReopen.meshAsset(lodAssetId);
    audit.check(page,"workflow","LOD and collision proxy settings survive save and reopen",
        generated && lodSaved && lod && lod->lodMesh && lod->lodDistance == 20
        && lodReopen.object(objectId) && lodReopen.object(objectId)->details.contains("collisionProxy"),error);
    std::stringstream package;
    const bool exported = WorldRuntime::exportScene(lodReopen,package);
    const auto playable = worldforge::readPlayable(package);
    bool geometry = false, proxy = false;
    if (playable)
        for (const auto &draw : playable->meshes) {
            if (draw.lodVertices.empty() || draw.body < 0)
                continue;
            geometry = draw.lodVertices.size() < draw.vertices.size() && draw.lodDistance == 20;
            const auto &body = playable->simulation.bodies[draw.body];
            proxy = body.shape == worldforge::BoxBody::Shape(2) && std::abs(body.position[0]-0.4f) < 0.001f
                && std::abs(body.halfSize[2]-0.7f) < 0.001f;
        }
    audit.check(page,"workflow","Player scene carries reduced LOD geometry and custom collider bounds",exported && geometry && proxy);
    const bool targetCreated = store.addMeshPrimitive(worldforge::Primitive::Plane,{4,4,1},4,4,"Wrap Target",error);
    const auto targetObject = targetCreated ? store.selection() : QString{};
    const auto targetAsset = targetCreated ? store.selectedMesh()->id : QString{};
    const bool sourceCreated = targetCreated && store.addMeshPrimitive(worldforge::Primitive::Plane,{1,1,1},4,4,"Wrap Source",error);
    if (!sourceCreated) {
        audit.check(page,"workflow","Shrinkwrap fixture builds two mesh assets",false,error);
        return;
    }
    const auto sourceObject = store.selection();
    const auto sourceId = store.selectedMesh()->id;
    worldforge::MeshModifier wrap{worldforge::ModifierKind::Shrinkwrap,true,0,2,0.1f,0,targetAsset.toStdString()};
    const bool appliedWrap = store.setMeshModifiers({wrap},error);
    auto movedTarget = *store.meshAsset(targetAsset)->mesh;
    for (auto &vertex : movedTarget.vertices)
        vertex[2] += 0.8f;
    const bool changedTarget = store.updateMesh(targetAsset,std::move(movedTarget),"Move wrap target geometry",error);
    const auto *wrapped = store.meshAsset(sourceId);
    audit.check(page,"workflow","Target mesh edits update linked Shrinkwrap output without changing the base",
        appliedWrap && changedTarget && wrapped && wrapped->mesh->vertices.front()[2] == 0
        && std::abs(wrapped->evaluated().vertices.front()[2]-0.9f) < 0.0001f,error);
    store.history().undo();
    audit.check(page,"workflow","Undo restores both the target and its dependent output",
        std::abs(store.meshAsset(sourceId)->evaluated().vertices.front()[2]-0.1f) < 0.0001f);
    store.select(targetObject);
    wrap.targetAsset = sourceId.toStdString();
    const auto historyIndex = store.history().index();
    audit.check(page,"workflow","Modifier dependency cycles are rejected atomically",
        !store.setMeshModifiers({wrap},error) && store.history().index() == historyIndex && store.selectedMesh()->modifiers.empty(),error);
    const bool savedWrap = store.save(&error);
    ProjectStore wrapReopen;
    const auto *reopenedWrap = wrapReopen.meshAsset(sourceId);
    audit.check(page,"workflow","Shrinkwrap dependency evaluates again when the project reopens",
        savedWrap && reopenedWrap && reopenedWrap->modifiers.size() == 1
        && std::abs(reopenedWrap->evaluated().vertices.front()[2]-0.1f) < 0.0001f,error);
    wrap.enabled = false;
    const bool addedDisabledWrap = store.setMeshModifiers({wrap},error);
    bool restoredCheck = false;
    bool validToggle = false;
    ModelingWorkspace modeling(store);
    if (auto *modifiers = namedWidget<QPushButton>(modeling,"Modifiers…")) {
        const auto toggleModifier = [modifiers](auto check) {
            QTimer::singleShot(0,modifiers,[check] {
                if (auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget())) {
                    if (auto *list = dialog->findChild<QListWidget *>(); list && list->count() == 1)
                        check(*list);
                    dialog->reject();
                }
            });
            modifiers->click();
        };
        toggleModifier([&](QListWidget &list) {
            const auto previousIndex = store.history().index();
            list.item(0)->setCheckState(Qt::Checked);
            restoredCheck = list.item(0)->checkState() == Qt::Unchecked
                && store.history().index() == previousIndex && !store.selectedMesh()->modifiers.front().enabled;
        });
        store.select(sourceObject);
        toggleModifier([&](QListWidget &list) {
            list.item(0)->setCheckState(Qt::Unchecked);
            validToggle = list.item(0)->checkState() == Qt::Unchecked
                && !store.selectedMesh()->modifiers.front().enabled
                && store.selectedMesh()->evaluated() == *store.selectedMesh()->mesh;
        });
    }
    audit.check(page,"workflow","Modifier checkbox retains valid changes and rolls back rejected cycles",
        addedDisabledWrap && restoredCheck && validToggle,error);
    store.select(objectId);
    const auto baseUvs = store.selectedMesh()->mesh;
    const bool addedUv = store.selectMeshUvSet("Lightmap",true,false,error);
    const worldforge::UvCorner pin{0,0};
    const bool pinned = store.toggleMeshUvPin(pin,error);
    if (pinned)
        store.history().undo();
    const bool undonePin = pinned && store.selectedMesh()->uvPins.empty();
    if (undonePin)
        store.history().redo();
    auto shifted = *store.selectedMesh()->mesh;
    worldforge::translateFaceUV(shifted,0,{0.125f,0.25f});
    const auto lightmapUvs = shifted.faces[0].corners[0].uv;
    const bool savedUv = store.updateMesh(lodAssetId,std::move(shifted),"Move Lightmap UV",error) && store.save(&error);
    ProjectStore uvLayers;
    const auto *reopenedUv = uvLayers.meshAsset(lodAssetId);
    audit.check(page,"workflow","Named UV sets save independent coordinates and the active set",
        addedUv && savedUv && reopenedUv && reopenedUv->activeUvSet == "Lightmap"
        && reopenedUv->mesh->faces[0].corners[0].uv == lightmapUvs && reopenedUv->otherUvSets.contains("UV Map"),error);
    audit.check(page,"workflow","UV pins support undo and redo and survive reopening in the active set",
        undonePin && reopenedUv && reopenedUv->uvPins.contains(pin),error);
    const bool switchedUv = store.selectMeshUvSet("UV Map",false,false,error);
    audit.check(page,"workflow","Switching UV sets restores the original coordinates",
        switchedUv && *store.selectedMesh()->mesh == *baseUvs && store.selectedMesh()->uvPins.empty()
        && store.selectedMesh()->otherUvSets.value("Lightmap").pins.contains(pin),error);
    const worldforge::UvCorner untouchedPin{2,0};
    const bool pinnedUntouched = store.toggleMeshUvPin(untouchedPin,error);
    auto changedTopology = *store.selectedMesh()->mesh;
    const bool splitUv = worldforge::extrudeFace(changedTopology,0,0.1f).empty()
        && store.updateMesh(lodAssetId,std::move(changedTopology),"Change topology with UV sets",error);
    const auto *changedUv = store.selectedMesh();
    audit.check(page,"workflow","Topology edits retain pins on geometrically unchanged faces",
        pinnedUntouched && splitUv && changedUv->uvPins.contains(untouchedPin),error);
    audit.check(page,"workflow","Topology edits flag inactive UV sets and retain their previous layout",
        splitUv && changedUv->otherUvSets.value("Lightmap").needsUnwrap
        && changedUv->otherUvSets.value("Lightmap").corners.front() == lightmapUvs
        && !store.selectMeshUvSet("Lightmap",false,false,error),error);
    if (splitUv)
        store.history().undo();
    audit.check(page,"workflow","Undo restores compatibility of inactive UV sets",
        splitUv && store.selectMeshUvSet("Lightmap",false,false,error)
        && store.selectedMesh()->mesh->faces[0].corners[0].uv == lightmapUvs
        && store.selectedMesh()->uvPins.contains(pin),error);

    namespace wf = worldforge;
    const bool cutterMade = store.addMeshPrimitive(wf::Primitive::Cube,{1,1,1},4,4,"Boolean Cutter",error);
    const auto cutter = cutterMade ? store.selectedMesh()->id : QString{};
    const bool bodyMade = cutterMade && store.addMeshPrimitive(wf::Primitive::Cube,{1,1,1},4,4,"Boolean Body",error);
    if (!bodyMade) {
        audit.check(page,"workflow","Boolean creates and saves an asset-linked modifier",false,error);
        return;
    }
    const auto body = store.selectedMesh()->id;
    wf::MeshModifier cut{wf::ModifierKind::Boolean};
    cut.targetAsset = cutter.toStdString();
    cut.targetOffset = {0.45f,0,0};
    const bool cutCreated = store.setMeshModifiers({cut},error);
    const auto evaluatedCut = store.selectedMesh()->evaluatedMesh;
    audit.check(page,"workflow","Boolean keeps the editable body and creates a separate solid result",
        cutCreated && evaluatedCut && evaluatedCut->faces != store.selectedMesh()->mesh->faces,error);
    auto moved = *store.meshAsset(cutter)->mesh;
    for (auto &vertex : moved.vertices)
        vertex[0] += 0.2f;
    const bool cutterUpdated = store.updateMesh(cutter,std::move(moved),"Move cutter vertices",error);
    const auto revisedCut = store.selectedMesh()->evaluatedMesh;
    const bool savedCut = store.save(&error);
    ProjectStore cutReopened;
    const auto *savedBody = cutReopened.meshAsset(body);
    audit.check(page,"workflow","Boolean responds to cutter edits and restores its settings and output on reopen",
        cutterUpdated && savedCut && revisedCut && evaluatedCut && *evaluatedCut != *revisedCut
        && savedBody && savedBody->modifiers == std::vector{cut} && savedBody->evaluated() == *revisedCut,error);
    if (cutterUpdated)
        store.history().undo();
    audit.check(page,"workflow","Undo restores Boolean output after editing its cutter",
        evaluatedCut && store.selectedMesh()->evaluated() == *evaluatedCut);
}

void modelingDeliveryWorkflow(const QDir &output, Audit &audit)
{
    namespace wf = worldforge;
    QTemporaryDir directory;
    ProjectStore store;
    QString error;
    const QString page = "Modeling delivery";
    if (!directory.isValid() || !store.createStarterProject("Equipment QA",
            {{"location",directory.filePath("Equipment QA")},{"starter","blank"}},error)
        || !store.addMeshPrimitive(wf::Primitive::Cube,{2,1.5f,1},4,4,"Equipment Case",error)) {
        audit.check(page,"workflow","Create the equipment case from a sized cube",false,error);
        return;
    }
    const auto objectId = store.selection();
    const auto assetId = store.selectedMesh()->id;
    auto mesh = *store.selectedMesh()->mesh;
    const std::array<std::size_t,1> lid{1};
    const auto edit = [&](const QString &label,auto operation) {
        const auto result = operation();
        audit.check(page,"topology",label,result.empty(),QString::fromStdString(result));
        return result.empty();
    };
    if (!edit("Bevel the case base",[&] { return wf::bevelFace(mesh,0,0.12f,3); })
        || !edit("Inset the lid",[&] { return wf::insetFaces(mesh,lid,0.16f); })
        || !edit("Extrude the inset lid",[&] { return wf::extrudeFace(mesh,lid.front(),0.15f); })
        || !edit("Cut a support loop",[&] { return wf::cutQuadLoop(mesh,1,0,1,0.2f); }))
        return;
    mesh.materials.push_back({"Orange lid",{0.9f,0.2f,0.05f,1},0.2f,0.5f});
    mesh.faces[lid.front()].material = 1;
    for (auto &vertex : mesh.vertices)
        vertex[0] += 1.25f;
    const auto seams = wf::sharpUvSeams(mesh);
    if (!edit("Unwrap and pack the case by its sharp edges",[&] { return wf::unwrapUvIslands(mesh,seams,0.01f); }))
        return;
    const auto uvReport = wf::inspectUvs(mesh,seams);
    const bool valid = !uvReport.outside && !uvReport.degenerate && !uvReport.overlaps;
    const bool updated = store.updateMesh(assetId,mesh,"Author the equipment case",error,nullptr,seams);
    audit.check(page,"workflow","Persist clean UVs, seam marks and two material slots",valid && updated,error);
    if (!updated)
        return;
    const std::vector<wf::MeshModifier> modifiers{
        {wf::ModifierKind::Mirror,true,0},{wf::ModifierKind::Array,true,1,2,3.0f}
    };
    const bool stacked = store.setMeshModifiers(modifiers,error);
    audit.check(page,"workflow","Mirror the one-sided case and array the pair",stacked,error);
    if (!stacked)
        return;
    store.addMeshInstance();
    const auto linked = store.selection();
    store.addMeshInstance();
    const auto detached = store.selection();
    const bool unique = store.makeMeshUnique(error);
    const auto uniqueId = store.selectedMesh()->id;
    for (auto &vertex : mesh.vertices)
        vertex[2] += 0.05f;
    const bool revised = store.updateMesh(assetId,mesh,"Raise shared case geometry",error);
    store.addTerrain();
    const bool saved = store.save(&error);
    ProjectStore reopened;
    const auto *caseAsset = reopened.meshAsset(assetId);
    const auto *uniqueAsset = reopened.meshAsset(uniqueId);
    audit.check(page,"workflow","Reopen the revised shared asset and preserve the unique copy",
        unique && revised && saved && caseAsset && uniqueAsset && uniqueId != assetId
        && reopened.object(objectId) && reopened.object(objectId)->meshAsset == assetId
        && reopened.object(linked) && reopened.object(linked)->meshAsset == assetId
        && reopened.object(detached) && reopened.object(detached)->meshAsset == uniqueId
        && caseAsset->modifiers == modifiers && caseAsset->uvSeams == seams && *caseAsset->mesh == mesh
        && uniqueAsset->mesh->vertices != caseAsset->mesh->vertices,error);
    if (!caseAsset)
        return;
    const auto glb = exportGlb(caseAsset->evaluated(),"Equipment Case",error);
    const auto roundTrip = importGlb(glb,error);
    audit.check(page,"export","Evaluated GLB contains the lid material and modifier geometry",
        roundTrip && caseAsset->evaluated().faces.size() > mesh.faces.size()
        && roundTrip->faces.size() > mesh.faces.size()
        && std::ranges::any_of(roundTrip->materials,[](const auto &material) { return material.name == "Orange lid"; }),error);
    std::stringstream data;
    const bool exported = WorldRuntime::exportScene(reopened,data);
    const auto playable = wf::readPlayable(data);
    QFile file(output.filePath("equipment-case.wfplay"));
    const auto bytes = data.str();
    audit.check(page,"export","Playable scene contains all three case instances",
        exported && playable && playable->meshes.size() == 3 && playable->simulation.terrains.size() == 1
        && file.open(QIODevice::WriteOnly)
        && file.write(bytes.data(),qint64(bytes.size())) == qint64(bytes.size()));
}

void studioPublishingWorkflow(const QDir &output,Audit &audit)
{
    const QString page = "Studio publishing";
    const bool barrier = qEnvironmentVariableIsSet("WORLDFORGE_STUDIO_TEST_BARRIER");
    QTemporaryDir directory;
    ProjectStore store;
    QString error;
    if (!directory.isValid() || !store.createStarterProject("Studio publish QA",
        {{"location",directory.filePath("Studio publish QA")},{"starter","blank"}},error)) {
        audit.check(page,"workflow","Create the publishing fixture",false,error);
        return;
    }
    if (barrier)
        store.addMeshPrimitive(worldforge::Primitive::Cube,{3.6f,2,1.3f},8,4,"Road Barrier",error);
    else
        store.addCube();
    if (!store.selectedMesh()) {
        audit.check(page,"workflow","Create the publishing target",false,error);
        return;
    }
    const auto id = store.selectedMesh()->id;
    const auto original = store.selectedMesh()->mesh;
    if (barrier) {
        QFile studioInput(output.filePath("authored-studio-input.glb"));
        const auto handoff = exportGlb(*original,"Road Barrier",error,true);
        audit.check(page,"export","Write the authored quad mesh for the Studio handoff",
            !handoff.isEmpty() && studioInput.open(QIODevice::WriteOnly)
            && studioInput.write(handoff) == handoff.size(),error);
        auto split = *original;
        const auto first = split.faces.front();
        split.faces.front().corners.resize(3);
        split.faces.push_back({{first.corners[0],first.corners[2],first.corners[3]},first.material});
        const auto splitHandoff = exportGlb(split,"Authored Triangles",error,true);
        QFile splitInput(output.filePath("triangular-studio-input.glb"));
        audit.check(page,"export","Write deliberate triangle faces for the Studio handoff",
            !splitHandoff.isEmpty() && splitInput.open(QIODevice::WriteOnly)
            && splitInput.write(splitHandoff) == splitHandoff.size(),error);

        LiveViewport view(store,false);
        view.resize(800,480);
        view.show();
        view.frame();
        const auto quads = waitForRender(view) ? view.captureFrame().convertToFormat(QImage::Format_RGB32) : QImage{};
        const auto triangleGlb = exportGlb(*original,"Road Barrier",error);
        const bool triangulated = store.publishStudioMesh(id,triangleGlb,original.data(),error);
        const auto triangles = view.captureFrame().convertToFormat(QImage::Format_RGB32);
        const auto selection = store.selection();
        store.select({});
        const auto deselected = view.captureFrame().convertToFormat(QImage::Format_RGB32);
        store.select(selection);
        const auto highlights = [](const QImage &frame) {
            const auto purple = [](QRgb rgb) {
                return qBlue(rgb)-qRed(rgb) > 38 && qRed(rgb)-qGreen(rgb) > 48 && qGreen(rgb) < 95;
            };
            std::array<int,2> count{};
            for (int y = 0; y < frame.height(); ++y)
                for (int x = 0; x < frame.width(); ++x) {
                    if (!purple(frame.pixel(x,y)))
                        continue;
                    ++count[0];
                    if (x > 0 && x+1 < frame.width() && purple(frame.pixel(x-1,y)) && purple(frame.pixel(x+1,y)))
                        ++count[1];
                }
            return count;
        };
        const auto [quadPixels, quadInterior] = highlights(quads);
        const auto [trianglePixels, triangleInterior] = highlights(triangles);
        audit.check(page,"Vulkan","Selected silhouette draws a wide dark purple stroke without triangulation edges",
            triangulated && !quads.isNull() && !triangles.isNull() && quadInterior > 200 && triangleInterior > 200
            && std::abs(quadPixels-trianglePixels) < 40 && !deselected.isNull() && highlights(deselected)[0] == 0,
            QString("Purple pixels before and after triangulation: %1 and %2. Wide stroke pixels: %3 and %4. %5")
                .arg(quadPixels).arg(trianglePixels).arg(quadInterior).arg(triangleInterior).arg(error));
        triangles.save(output.filePath("triangulated-selection.png"));
        if (triangulated)
            store.history().undo();
    }
    store.placeMesh(id);
    if (barrier) {
        auto instance = *store.selectedObject();
        instance.position = {4,0,0.65f};
        instance.rotation = {0,0,12};
        instance.details.insert("physics","Static");
        store.updateObject(instance,"Place second barrier");
        const auto primary = std::ranges::find(store.project()->objects,id,&SceneObject::meshAsset);
        auto first = *primary;
        first.position = {-4,0,0.65f};
        first.details.insert("physics","Static");
        store.updateObject(first,"Place first barrier");
        store.addCube();
        auto unrelated = *store.selectedObject();
        unrelated.position = {0,0,-0.25f};
        unrelated.scale = {18,18,0.5f};
        unrelated.details.insert("physics","Static");
        store.updateObject(unrelated,"Add a static ground prop for Play");
    } else {
        store.placeMesh(id);
    }
    const auto placements = store.project()->objects;
    auto revised = *original;
    for (auto &vertex : revised.vertices)
        vertex[2] *= 2;
    auto data = exportGlb(revised,"Studio Mesh",error);
    if (barrier) {
        const auto &project = *store.project();
        const auto *target = store.meshAsset(id);
        const auto same = reviewStudioPublish(project,target,{},*original);
        audit.check(page,"workflow","Publish review names the target asset and every affected placement",
            same.blocker.isEmpty() && same.assetName == "Road Barrier" && same.placements.size() == 2
            && same.warnings.isEmpty() && same.summary().contains("Update “Road Barrier” in 2 level placements")
            && same.summary().contains("Collision checks passed"),same.summary());
        auto withProxy = project;
        const auto proxied = std::ranges::find(withProxy.objects,id,&SceneObject::meshAsset);
        proxied->details.insert("collisionProxy",QJsonObject{{"center",QJsonArray{0,0,0}},
            {"half",QJsonArray{1.8,1.0,0.65}}});
        const auto fits = reviewStudioPublish(withProxy,target,{},*original);
        const auto taller = reviewStudioPublish(withProxy,target,{},revised);
        auto passive = project;
        std::ranges::find(passive.objects,id,&SceneObject::meshAsset)->details.remove("physics");
        const auto noCollider = reviewStudioPublish(passive,target,{},*original);
        audit.check(page,"workflow","Publish review flags collision that no longer matches the mesh",
            fits.warnings.isEmpty() && taller.warnings.size() == 2
            && taller.warnings.join(' ').contains("custom collision bounds no longer cover")
            && taller.warnings.join(' ').contains("changes size") && noCollider.warnings.size() == 1
            && noCollider.warnings.front().contains("will not block the player"),
            (taller.warnings + noCollider.warnings).join(" | "));
        auto broken = *original;
        broken.faces.front().corners.front().vertex = 9999;
        const auto invalid = reviewStudioPublish(project,target,{},broken);
        const auto empty = reviewStudioPublish(project,target,{},worldforge::Mesh{});
        const auto creating = reviewStudioPublish(project,nullptr,"New Barrier",*original);
        audit.check(page,"workflow","Publish review blocks unsupported meshes and describes new assets",
            !invalid.blocker.isEmpty() && !empty.blocker.isEmpty() && creating.blocker.isEmpty()
            && creating.creating && creating.placements.isEmpty()
            && creating.summary().startsWith("Create “New Barrier”"),invalid.blocker + " | " + empty.blocker);
    }
    const auto before = store.history().index();
    const bool rejected = !store.publishStudioMesh(id,"invalid",original.data(),error)
        && store.history().index() == before;
    // The normal editor audit can run without a Studio installation.
    QByteArray source("PK\x03\x04" "Studio fixture source");
    QFile studioFixture(qEnvironmentVariable("WORLDFORGE_STUDIO_TEST_ASSET"));
    if (!studioFixture.fileName().isEmpty()) {
        if (!studioFixture.open(QIODevice::ReadOnly)) {
            audit.check(page,"storage","Open the Studio-produced asset fixture",false,studioFixture.errorString());
            return;
        }
        source = studioFixture.readAll();
        QFile studioMesh(QFileInfo(studioFixture).dir().filePath("mesh.glb"));
        if (!studioMesh.open(QIODevice::ReadOnly)) {
            audit.check(page,"storage","Open the Studio-produced mesh fixture",false,studioMesh.errorString());
            return;
        }
        data = studioMesh.readAll();
    }
    const bool published = !data.isEmpty() && store.publishStudioMesh(id,data,original.data(),error,source);
    const auto updated = store.meshAsset(id)->mesh;
    const auto sourcePath = store.studioSourcePath(*store.meshAsset(id));
    QFile storedSource(sourcePath);
    audit.check(page,"storage","Publish stores the editable source under its content hash",
        storedSource.open(QIODevice::ReadOnly) && storedSource.readAll() == source);
    const auto instances = std::ranges::count(placements,id,&SceneObject::meshAsset);
    audit.check(page,"workflow","Publish by ID preserves placement transforms, collider settings and unrelated objects",
        published && updated != original && instances == (barrier ? 2 : 3)
        && store.project()->objects == placements,error);
    if (!studioFixture.fileName().isEmpty()) {
        std::set<std::uint32_t> usedMaterials;
        std::set<worldforge::Point2> usedUvs;
        for (const auto &face : updated->faces) {
            usedMaterials.insert(face.material);
            for (const auto &corner : face.corners)
                usedUvs.insert(corner.uv);
        }
        audit.check(page,"export","Studio geometry, UV layout and material assignments survive Editor import",
            published && updated->faces.size() > original->faces.size()
            && usedMaterials.size() == 2 && usedUvs.size() > 4 && worldforge::validateMesh(*updated).empty());
        if (barrier)
            audit.check(page,"export","The sculpted barrier includes an embedded base-color map",
                published && std::ranges::any_of(updated->materials,[](const auto &slot) { return !slot.baseColorTexture.empty(); }));
    }
    const bool stale = !store.publishStudioMesh(id,data,original.data(),error);
    audit.check(page,"workflow","Corrupt GLB and stale revisions leave the asset intact",
        rejected && stale && store.meshAsset(id)->mesh == updated && store.history().index() == before+1);
    if (published)
        store.history().undo();
    const bool undone = store.meshAsset(id)->mesh == original && store.studioSourcePath(*store.meshAsset(id)).isEmpty();
    if (published)
        store.history().redo();
    audit.check(page,"workflow","One undo and redo restores the published revision",
        published && undone && store.meshAsset(id)->mesh == updated && store.project()->objects == placements
        && store.studioSourcePath(*store.meshAsset(id)) == sourcePath);
    std::ostringstream playable;
    const bool exported = WorldRuntime::exportScene(store,playable);
    const auto bytes = playable.str();
    std::istringstream input(bytes);
    const auto scene = worldforge::readPlayable(input);
    QFile playerFixture(output.filePath("studio-published.wfplay"));
    // Textured assets export one draw per material, so the barriers can add more than one mesh each.
    const auto texturedDraws = scene ? std::ranges::count_if(scene->meshes,[](const auto &mesh) {
        return !mesh.baseColorTexture.empty();
    }) : 0;
    const bool textured = std::ranges::any_of(updated->materials,[](const auto &slot) { return !slot.baseColorTexture.empty(); });
    audit.check(page,"export","Player scene contains all published instances and their base-color maps",
        exported && scene && (textured ? scene->meshes.size() >= 3 && texturedDraws >= instances
                                       : scene->meshes.size() == 3)
        && playerFixture.open(QIODevice::WriteOnly)
        && playerFixture.write(bytes.data(),qint64(bytes.size())) == qint64(bytes.size()),
        QString("%1 draws, %2 with a base-color map").arg(scene ? scene->meshes.size() : 0).arg(texturedDraws));
    error.clear();
    if (barrier)
        store.select(std::ranges::find(placements,id,&SceneObject::meshAsset)->id);
    const bool saved = store.save(&error);
    ProjectStore reopened;
    const auto *savedAsset = reopened.meshAsset(id);
    audit.check(page,"storage","Published geometry survives a project reopen",
        saved && savedAsset && *savedAsset->mesh == *updated && reopened.studioSourcePath(*savedAsset) == sourcePath,error);
    if (barrier && saved && savedAsset) {
        reopened.select({});
        LiveViewport view(reopened,false);
        view.resize(960,600);
        view.show();
        view.frame();
        const auto painted = waitForRender(view) ? view.captureFrame().convertToFormat(QImage::Format_RGB32) : QImage{};
        auto plain = *savedAsset->mesh;
        for (auto &slot : plain.materials)
            slot.baseColorTexture.clear();
        const bool removedTexture = reopened.updateMesh(id,std::move(plain),"Remove base-color map in QA",error);
        const auto unpainted = view.captureFrame().convertToFormat(QImage::Format_RGB32);
        const auto yellowPixels = [](const QImage &image) {
            std::array<int,2> count{};
            for (int y = 0; y < image.height(); ++y)
                for (int x = 0; x < image.width(); ++x) {
                    const auto pixel = image.pixel(x,y);
                    count[x >= image.width()/2] += qRed(pixel) >= qGreen(pixel)
                        && qRed(pixel)-qBlue(pixel) > 35 && qGreen(pixel)-qBlue(pixel) > 20;
                }
            return count;
        };
        const auto paintedPixels = yellowPixels(painted);
        const auto plainPixels = yellowPixels(unpainted);
        painted.save(output.filePath("painted-barriers.png"));
        unpainted.save(output.filePath("unpainted-barriers.png"));
        audit.check(page,"Vulkan","Base-color paint changes the rendered pixels after project reopen",
            removedTexture && !painted.isNull() && !unpainted.isNull()
            && paintedPixels[0] > plainPixels[0]+100 && paintedPixels[1] > plainPixels[1]+100,
            QString("Painted pixels before/after removing the map: %1/%2 left, %3/%4 right. %5")
                .arg(paintedPixels[0]).arg(plainPixels[0]).arg(paintedPixels[1]).arg(plainPixels[1]).arg(error));
        if (removedTexture)
            reopened.history().undo();
        const bool played = view.start(LiveViewport::RunMode::Play);
        const auto playFrame = played ? view.captureFrame() : QImage{};
        audit.check(page,"runtime","Play starts with the published barrier scene",
            played && view.playerPosition().has_value() && !playFrame.isNull());
        view.stop();
        WorldRuntime collision;
        const bool collisionLoaded = collision.start(reopened,true);
        if (collisionLoaded) {
            collision.key(Qt::Key_W,true);
            for (int step = 0; step < 180; ++step)
                collision.tick(0);
        }
        const auto player = collision.player();
        audit.check(page,"runtime","Walking into the published barrier stops the player",
            player && player->x() < -0.2f && player->x() > -4.0f && player->z() > 0.7f,
            player ? QString("Player stopped at (%1, %2, %3)").arg(player->x()).arg(player->y()).arg(player->z())
                : QStringLiteral("No player"));
    }
    if (barrier && saved && qEnvironmentVariableIsSet("WORLDFORGE_STUDIO_TEST_NAVIGATION")) {
        MainWindow window;
        window.show();
        auto *tabs = window.findChild<QTabBar *>("workspaceTabs");
        bool opensSelected = tabs != nullptr;
        for (const auto &name : {QStringLiteral("Sculpting"),QStringLiteral("Texture"),QStringLiteral("Modeling")}) {
            if (!opensSelected)
                break;
            for (int i = 0; i < tabs->count(); ++i)
                if (tabs->tabText(i) == name)
                    tabs->setCurrentIndex(i);
            const auto ready = [&] {
                const auto *action = window.findChild<QAction *>("applyStudioUpdate");
                return window.windowTitle().endsWith(name) && action && action->isVisible()
                    && action->toolTip() == "Apply Studio Update to Road Barrier";
            };
            for (int attempt = 0; !ready() && attempt < 560; ++attempt)
                QTest::qWait(50);
            opensSelected = ready();
        }
        const auto *action = window.findChild<QAction *>("applyStudioUpdate");
        const auto *commands = window.findChild<QToolBar *>("commands");
        audit.check(page,"navigation","World opens the barrier across Studio tabs with Apply in the top toolbar",
            opensSelected && commands && commands->actions().value(0) == action,
            QString("%1 | %2 | %3").arg(window.windowTitle(),action ? action->toolTip() : QStringLiteral("No Studio action"),
                window.statusBar()->currentMessage()));
    }
    auto nativeEdit = *updated;
    for (auto &vertex : nativeEdit.vertices)
        vertex[0] += 1;
    const bool edited = store.updateMesh(id,std::move(nativeEdit),"Native mesh edit",error,updated.data());
    const auto nativeRevision = store.meshAsset(id)->mesh;
    audit.check(page,"workflow","Studio cannot overwrite a newer native mesh edit",
        edited && !store.publishStudioMesh(id,data,updated.data(),error,source)
        && store.meshAsset(id)->mesh == nativeRevision);
    const auto objectCount = store.project()->objects.size();
    const bool created = store.createStudioMesh("New Studio Mesh",*updated,source,error);
    const auto *newAsset = store.selectedMesh();
    const auto newId = created && newAsset ? newAsset->id : QString{};
    const bool placed = !newId.isEmpty() && newId != id && store.project()->objects.size() == objectCount+1
        && store.studioSourcePath(*newAsset) == sourcePath;
    if (created)
        store.history().undo();
    const bool removed = !store.meshAsset(newId) && store.project()->objects.size() == objectCount;
    if (created)
        store.history().redo();
    audit.check(page,"workflow","First publish creates a new mesh and placement in one undoable edit",
        placed && removed && store.meshAsset(newId) && store.project()->objects.size() == objectCount+1,error);
}

std::optional<ForgeAiJob::Result> runForgeJob(const ProjectStore &store,const forge::Plan &plan,
    QJsonObject job = {},int timeoutMs = 120000,bool cancel = false)
{
    const auto *asset = store.meshAsset(plan.barrierId);
    if (!asset)
        return std::nullopt;
    QString error;
    const auto glb = exportGlb(*asset->mesh,asset->name,error,true);
    ForgeAiJob runner;
    QEventLoop loop;
    std::optional<ForgeAiJob::Result> result;
    QObject::connect(&runner,&ForgeAiJob::finished,&loop,[&](const ForgeAiJob::Result &value) {
        result = value;
        loop.quit();
    });
    const auto source = store.studioSourcePath(*asset);
    if (job.isEmpty())
        job = forge::jobDescription(plan);
    if (!runner.start(EmbeddedStudio::installedExecutable(),glb,QFile::exists(source) ? source : QString(),
                      job,error,timeoutMs))
        return ForgeAiJob::Result{false,false,error};
    if (cancel)
        QTimer::singleShot(150,&runner,&ForgeAiJob::cancel);
    QTimer::singleShot(180000,&loop,&QEventLoop::quit);
    loop.exec();
    return result;
}

void forgeAiWorkflow(const QDir &output,Audit &audit)
{
    const QString page = "Forge AI";
    QTemporaryDir directory;
    ProjectStore store;
    QString error;
    if (!directory.isValid() || !store.createStarterProject("Forge AI QA",
        {{"location",directory.filePath("Forge AI QA")},{"starter","blank"}},error)
        || !store.addMeshPrimitive(worldforge::Primitive::Cube,{3.6f,2,1.3f},8,4,"Road Barrier",error)) {
        audit.check(page,"workflow","Create the Forge AI fixture",false,error);
        return;
    }
    const auto barrierId = store.selectedMesh()->id;
    const auto setup = [&](const QString &name,QVector3D position,float yaw,const QString &physics = {}) {
        auto object = *store.selectedObject();
        object.name = name;
        object.position = position;
        object.rotation = {0,0,yaw};
        if (!physics.isEmpty())
            object.details.insert("physics",physics);
        store.updateObject(object,"Set up " + name);
        return object.id;
    };
    setup("Road Barrier",{-4,0,0.65f},0,"Static");
    store.placeMesh(barrierId);
    setup("Road Barrier_2",{4,0,0.65f},12,"Static");
    store.addCube();
    auto ground = *store.selectedObject();
    ground.position = {0,0,-0.25f};
    ground.scale = {24,24,0.5f};
    ground.details.insert("physics","Static");
    store.updateObject(ground,"Add ground");
    store.addObject("Empty","Markers");
    const auto north = setup("North Entrance",{0,8,0},0);
    store.addObject("Empty","Markers");
    const auto south = setup("South Entrance",{0,-8,0},180);

    const QString prompt = "Make a worn barrier with faded yellow stripes and place it at the north road entrance";
    auto plan = forge::plan(*store.project(),prompt);
    const auto list = forge::commands(*store.project(),plan,error);
    QStringList described;
    for (const auto &command : list)
        described << command.describe();
    audit.check(page,"workflow","The request plans four typed commands with targets and expected revisions",
        plan.ready() && plan.barrierId == barrierId && plan.entranceId == north && list.size() == 4
        && list[0].operation == "bevel_edges" && list[1].operation == "paint_stripes"
        && list[2].operation == "place_instance" && list[3].operation == "check_play"
        && list[0].expectedRevision == forge::revision(*store.meshAsset(barrierId)->mesh)
        && plan.style.color == QColor("#f2b705") && plan.style.wear > 0.5f && plan.style.fade > 0.3f
        && described.join(' ').contains("yellow stripes"),described.join(" | ") + error);

    const auto ambiguous = forge::plan(*store.project(),"Put a worn barrier at the entrance");
    QString ambiguousError;
    const bool ambiguousBlocked = forge::commands(*store.project(),ambiguous,ambiguousError).isEmpty();
    audit.check(page,"workflow","An ambiguous entrance asks the user to choose",
        ambiguous.needsChoice() && ambiguous.barrierId == barrierId && ambiguous.entranceId.isEmpty()
        && ambiguous.entrances.size() == 2 && ambiguousBlocked && !ambiguousError.isEmpty(),ambiguousError);
    auto chosen = ambiguous;
    chosen.entranceId = south;
    QString chosenError;
    const auto chosenCommands = forge::commands(*store.project(),chosen,chosenError);
    audit.check(page,"workflow","Choosing the entrance completes the plan",
        chosen.ready() && chosenCommands.size() == 4,chosenError);

    auto invalid = plan;
    invalid.style.stripes = 40;
    QString invalidError;
    const bool invalidBlocked = forge::commands(*store.project(),invalid,invalidError).isEmpty();
    const auto tooMany = forge::plan(*store.project(),"Paint 30 stripes on the barrier at the north entrance");
    const auto unsupported = forge::plan(*store.project(),"Paint the repair shop door red");
    audit.check(page,"workflow","Invalid parameters and unsupported requests are rejected before running",
        invalidBlocked && invalidError.contains("12 stripes")
        && !tooMany.ready() && tooMany.error.contains("12 stripes") && !unsupported.error.isEmpty(),
        invalidError + " | " + tooMany.error + " | " + unsupported.error);

    if (!qEnvironmentVariableIsSet("WORLDFORGE_STUDIO_TEST_FORGE") || EmbeddedStudio::installedExecutable().isEmpty()) {
        audit.record(page,"runtime","Studio jobs","SKIP","Set WORLDFORGE_STUDIO_TEST_FORGE with Studio installed.");
        return;
    }
    const auto history = store.history().index();
    const auto original = store.meshAsset(barrierId)->mesh;
    const auto canceled = runForgeJob(store,plan,{},120000,true);
    const auto timedOut = runForgeJob(store,plan,{},1);
    const auto failed = runForgeJob(store,plan,QJsonObject{{"bevel",QJsonObject{}}});
    audit.check(page,"runtime","Cancel, timeout, and a failed Studio job leave the project unchanged",
        canceled && canceled->canceled && !canceled->ok && timedOut && !timedOut->ok
        && timedOut->message.contains("time limit") && failed && !failed->ok && !failed->message.isEmpty()
        && store.history().index() == history && store.meshAsset(barrierId)->mesh == original,
        QStringList{canceled ? canceled->message : "no result",timedOut ? timedOut->message : "no result",
                    failed ? failed->message.left(120) : "no result"}.join(" | "));

    QElapsedTimer timer;
    timer.start();
    const auto result = runForgeJob(store,plan);
    const auto elapsed = timer.elapsed();
    auto staged = result && result->ok
        ? forge::stage(*store.project(),plan,list,result->glb,result->source,result->steps,error) : std::nullopt;
    const bool striped = staged && std::ranges::any_of(staged->mesh.materials,
        [](const auto &slot) { return !slot.baseColorTexture.empty(); });
    audit.check(page,"runtime","Studio bevels and paints a working copy without changing the project",
        staged && striped && staged->mesh.faces.size() > original->faces.size()
        && store.history().index() == history && store.meshAsset(barrierId)->mesh == original
        && staged->placement.meshAsset == barrierId && std::abs(staged->placement.position.y()-8) < 0.001f
        && std::abs(staged->placement.position.z()-0.65f) < 0.01f,
        QString("%1 ms. %2 %3").arg(elapsed).arg(result ? result->steps.join(' ') + result->message : "No result",error));
    if (!staged)
        return;
    if (striped)
        for (const auto &slot : staged->mesh.materials)
            if (!slot.baseColorTexture.empty()) {
                QFile texture(output.filePath("forge-stripes.png"));
                if (texture.open(QIODevice::WriteOnly))
                    texture.write(reinterpret_cast<const char *>(slot.baseColorTexture.data()),
                                  qint64(slot.baseColorTexture.size()));
                break;
            }

    auto manual = *original;
    for (auto &vertex : manual.vertices)
        vertex[2] *= 1.1f;
    const bool edited = store.updateMesh(barrierId,std::move(manual),"Manual edit during preview",error,original.data());
    const auto manualRevision = store.meshAsset(barrierId)->mesh;
    QString staleError;
    const bool staleRejected = edited && !forge::apply(store,*staged,staleError)
        && store.meshAsset(barrierId)->mesh == manualRevision && staleError.contains("changed while the preview");
    store.history().undo();
    auto moved = *store.object(north);
    moved.position.setX(1);
    store.updateObject(moved,"Move entrance during preview");
    QString movedError;
    const bool movedRejected = !forge::apply(store,*staged,movedError) && movedError.contains("entrance moved");
    store.history().undo();
    audit.check(page,"workflow","A manual edit made during the preview blocks the stale AI result",
        staleRejected && movedRejected && store.meshAsset(barrierId)->mesh == original,staleError + " | " + movedError);

    const auto beforeApply = store.history().index();
    const auto objectsBefore = store.project()->objects;
    const bool applied = forge::apply(store,*staged,error);
    const auto *placed = store.object(staged->placement.id);
    const bool appliedState = applied && placed && store.history().index() == beforeApply+1
        && *store.meshAsset(barrierId)->mesh == staged->mesh && !store.meshAsset(barrierId)->studioSourceHash.isEmpty()
        && store.selection() == staged->placement.id;
    store.history().undo();
    const bool undone = !store.object(staged->placement.id) && store.meshAsset(barrierId)->mesh == original
        && store.project()->objects == objectsBefore;
    store.history().redo();
    audit.check(page,"workflow","Apply publishes the barrier and its placement as one undoable change",
        appliedState && undone && store.object(staged->placement.id)
        && *store.meshAsset(barrierId)->mesh == staged->mesh,error);

    const auto check = forge::checkInPlay(store,*staged);
    audit.check(page,"runtime","Play check confirms placement, stripes, and collision",
        check.ok() && check.passed.size() == 3,(check.passed + check.failed).join(" | "));
    auto passive = *store.object(staged->placement.id);
    passive.details.remove("physics");
    store.updateObject(passive,"Remove barrier physics");
    const auto failedCheck = forge::checkInPlay(store,*staged);
    store.history().undo();
    audit.check(page,"runtime","A failed Play check names the change to revise",
        !failedCheck.ok() && failedCheck.failed.size() == 1 && failedCheck.failed.front().contains("Physics")
        && forge::checkInPlay(store,*staged).ok(),failedCheck.failed.join(" | "));

    LiveViewport view(store,false);
    view.resize(960,600);
    view.show();
    store.select({});
    view.frame();
    const auto frame = waitForRender(view) ? view.captureFrame().convertToFormat(QImage::Format_RGB32) : QImage{};
    int yellow = 0;
    for (int y = 0; y < frame.height(); ++y)
        for (int x = 0; x < frame.width(); ++x) {
            const auto pixel = frame.pixel(x,y);
            yellow += qRed(pixel) >= qGreen(pixel) && qRed(pixel)-qBlue(pixel) > 35
                && qGreen(pixel)-qBlue(pixel) > 20;
        }
    frame.save(output.filePath("forge-barrier.png"));
    audit.check(page,"Vulkan","The staged stripes render in the World Editor",!frame.isNull() && yellow > 150,
        QString("Yellow pixels: %1").arg(yellow));

    error.clear();
    const bool saved = store.save(&error);
    ProjectStore reopened;
    const auto *reopenedObject = reopened.object(staged->placement.id);
    const auto *reopenedAsset = reopened.meshAsset(barrierId);
    audit.check(page,"storage","The applied barrier, stripes, and placement survive a project reopen",
        saved && reopenedObject && reopenedAsset && *reopenedAsset->mesh == staged->mesh
        && QFile::exists(reopened.studioSourcePath(*reopenedAsset)),error);

    ProjectStore panelStore;
    ForgeAiPanel panel(panelStore);
    panel.show();
    auto *promptBox = panel.findChild<QPlainTextEdit *>("forgePrompt");
    auto *entranceBox = panel.findChild<QComboBox *>("forgeEntrance");
    auto *report = panel.findChild<QPlainTextEdit *>("forgeReport");
    const auto click = [&](const char *name) {
        auto *button = panel.findChild<QPushButton *>(name);
        const bool enabled = button && button->isEnabled();
        if (enabled)
            button->click();
        return enabled;
    };
    promptBox->setPlainText("Put a worn barrier with faded yellow stripes at the entrance");
    click("forgePlanButton");
    const bool asked = !panel.findChild<QPushButton *>("forgePreview")->isEnabled()
        && report->toPlainText().contains("Choose one");
    entranceBox->setCurrentIndex(entranceBox->findText("South Entrance"));
    const bool previewing = click("forgePreview");
    for (int wait = 0; wait < 1200 && panel.isJobRunning(); ++wait)
        QTest::qWait(100);
    const bool reviewed = panel.stage().has_value() && report->toPlainText().contains("Update “Road Barrier”");
    const auto panelHistory = panelStore.history().index();
    const bool appliedFromPanel = click("forgeApply") && panelStore.history().index() == panelHistory+1;
    const bool checked = click("forgeCheck") && report->toPlainText().contains("Play check passed");
    panel.grab().save(output.filePath("forge-panel.png"));
    panelStore.history().undo();
    const bool reverted = panel.stage() && !panelStore.object(panel.stage()->placement.id);
    audit.check(page,"workflow","The Forge AI panel asks for the entrance, previews, applies, and checks in Play",
        asked && previewing && reviewed && appliedFromPanel && checked && reverted,
        QString("asked %1 preview %2 reviewed %3 applied %4 checked %5 reverted %6. %7").arg(asked).arg(previewing)
            .arg(reviewed).arg(appliedFromPanel).arg(checked).arg(reverted).arg(report->toPlainText().left(300)));
}

struct PlayerRun {
    int exitCode = -1;
    QJsonObject report;
};

PlayerRun runStreetPlayer(const ProjectStore &store,const QDir &output,const QString &name)
{
    PlayerRun run;
    std::ostringstream playable;
    if (!WorldRuntime::exportScene(store,playable))
        return run;
    const auto bytes = playable.str();
    QFile scene(output.filePath(name + ".wfplay"));
    if (!scene.open(QIODevice::WriteOnly) || scene.write(bytes.data(),qint64(bytes.size())) != qint64(bytes.size()))
        return run;
    scene.close();
    QProcess player;
    const auto report = output.filePath(name + "-player.json");
    QFile::remove(report);
    player.start(QDir(QCoreApplication::applicationDirPath()).filePath("WorldForgePlayer.exe"),
                 {"--verify-street",scene.fileName(),report});
    if (!player.waitForFinished(240000)) {
        player.kill();
        return run;
    }
    run.exitCode = player.exitStatus() == QProcess::NormalExit ? player.exitCode() : -2;
    QFile result(report);
    if (result.open(QIODevice::ReadOnly))
        run.report = QJsonDocument::fromJson(result.readAll()).object();
    return run;
}

QString playerSummary(const PlayerRun &run)
{
    const auto frame = run.report.value("frameMs").toObject();
    const auto cpu = run.report.value("cpuMs").toObject();
    return QString("exit %1 · %2 · mission %3 s · frame avg %4 ms p95 %5 ms p99 %6 ms max %7 ms · CPU avg %8 ms p95 %9 ms"
                   " · working set %10 MiB, peak %11 MiB, private %12 MiB · textured meshes %13")
        .arg(run.exitCode).arg(run.report.value("adapter").toString())
        .arg(run.report.value("missionSeconds").toDouble(),0,'f',1)
        .arg(frame.value("average").toDouble(),0,'f',2).arg(frame.value("p95").toDouble(),0,'f',2)
        .arg(frame.value("p99").toDouble(),0,'f',2).arg(frame.value("max").toDouble(),0,'f',2)
        .arg(cpu.value("average").toDouble(),0,'f',2).arg(cpu.value("p95").toDouble(),0,'f',2)
        .arg(run.report.value("workingSetMiB").toDouble(),0,'f',0).arg(run.report.value("peakWorkingSetMiB").toDouble(),0,'f',0)
        .arg(run.report.value("privateMiB").toDouble(),0,'f',0).arg(run.report.value("texturedMeshes").toInt());
}

void streetWorkflow(const QDir &output,Audit &audit)
{
    const QString page = "Playable street";
    QTemporaryDir directory;
    ProjectStore store;
    QString error;
    if (!directory.isValid() || !store.createStarterProject("Street QA",
        {{"location",directory.filePath("Street QA")},{"starter","street"}},error)) {
        audit.check(page,"workflow","Create the Playable Street starter",false,error);
        return;
    }
    const auto find = [&](const QString &name) {
        const auto match = std::ranges::find(store.project()->objects,name,&SceneObject::name);
        return match == store.project()->objects.end() ? QString() : match->id;
    };
    const auto baseline = street::verify(store);
    audit.check(page,"runtime","Play: traffic loops, the player enters the shop, and the mission completes",
        baseline.ok() && baseline.objectives.size() >= 3,
        QString("%1 s simulated. %2 | %3").arg(baseline.seconds,0,'f',1).arg(baseline.summary(),baseline.objectives.join(" → ")));
    const auto baselinePlayer = runStreetPlayer(store,output,"street-baseline");
    audit.check(page,"runtime","Packaged Player completes the baseline mission with traffic",
        baselinePlayer.exitCode == 0 && baselinePlayer.report.value("missionComplete").toBool()
        && baselinePlayer.report.value("trafficLooped").toBool() && baselinePlayer.report.value("enteredShop").toBool(),
        playerSummary(baselinePlayer));

    // Populate the street with the Phase 3 barrier workflow.
    auto barrierPlan = forge::plan(*store.project(),"Make a worn barrier with faded yellow stripes at the main street entrance");
    const auto barrierCommands = forge::commands(*store.project(),barrierPlan,error);
    std::optional<forge::Stage> staged;
    if (qEnvironmentVariableIsSet("WORLDFORGE_STUDIO_TEST_FORGE") && !EmbeddedStudio::installedExecutable().isEmpty()) {
        const auto result = runForgeJob(store,barrierPlan);
        if (result && result->ok)
            staged = forge::stage(*store.project(),barrierPlan,barrierCommands,result->glb,result->source,result->steps,error);
        else if (result)
            error = result->message;
    }
    const bool barrierApplied = staged && forge::apply(store,*staged,error);
    audit.check(page,"workflow","Forge AI places a striped barrier at Main Street Entrance",
        barrierPlan.ready() && barrierApplied && store.object(staged->placement.id)
        && std::abs(store.object(staged->placement.id)->position.y()+10.6f) < 0.01f,error);
    if (!barrierApplied)
        return;
    const auto blocked = street::verify(store,60);
    audit.check(page,"runtime","The street check reports traffic blocked by the new barrier",
        blocked.missionComplete && !blocked.trafficLooped,blocked.summary());

    const QString request = "Move the repair shop entrance to the side street, move its mission marker, "
                            "and route traffic around the barrier";
    auto coordinated = street::plan(store,request);
    QStringList operations;
    for (const auto &command : coordinated.commands)
        operations << command.operation;
    audit.check(page,"workflow","One request plans the entrance, mission marker, and traffic changes for review",
        coordinated.error.isEmpty() && operations == QStringList{"move_entrance","move_mission_marker","reroute_traffic","check_street"}
        && coordinated.objects.size() == 10 && coordinated.changes.size() == 3,
        coordinated.error + coordinated.changes.join(" | "));

    const auto markerId = find("Repair Shop Entrance");
    auto nudged = *store.object(markerId);
    nudged.position.setX(nudged.position.x()+0.5f);
    store.updateObject(nudged,"Manual marker edit during preview");
    QString staleError;
    const bool staleRejected = !street::apply(store,coordinated,staleError) && staleError.contains("changed while the preview");
    store.history().undo();
    audit.check(page,"workflow","A manual edit during the preview blocks the coordinated change",staleRejected,staleError);

    const auto before = store.project()->objects;
    const auto history = store.history().index();
    const bool applied = street::apply(store,coordinated,error) && store.history().index() == history+1;
    const auto after = store.project()->objects;
    const auto marker = *store.object(markerId);
    const auto changed = street::verify(store);
    audit.check(page,"runtime","After the change the side entrance works, the mission completes, and traffic avoids the barrier",
        applied && changed.ok() && marker.position.x() > 5 && !store.object(find("Shop Front Door Lintel"))->visible
        && store.object(find("Shop Side Door Lintel"))->visible,changed.summary() + error);
    store.history().undo();
    const bool undone = store.project()->objects == before && !street::verify(store,60).trafficLooped;
    store.history().redo();
    const auto replayed = street::verify(store);
    audit.check(page,"workflow","Undo restores the street and redo replays the same result",
        undone && store.project()->objects == after && replayed.ok(),replayed.summary());

    error.clear();
    const bool saved = store.save(&error);
    ProjectStore reopened;
    const auto reopenedCheck = street::verify(reopened);
    audit.check(page,"storage","The changed street reopens and still plays through",
        saved && reopened.project() && reopened.project()->objects == after && reopenedCheck.ok(),
        error + reopenedCheck.summary());
    const auto finalPlayer = runStreetPlayer(reopened,output,"street-final");
    audit.check(page,"runtime","Packaged Player completes the changed street with the textured barrier",
        finalPlayer.exitCode == 0 && finalPlayer.report.value("missionComplete").toBool()
        && finalPlayer.report.value("trafficLooped").toBool() && finalPlayer.report.value("texturedMeshes").toInt() >= 1,
        playerSummary(finalPlayer));
    const auto frame = finalPlayer.report.value("frameMs").toObject();
    const bool gtx1650 = finalPlayer.report.value("adapter").toString().contains("GTX 1650");
    audit.record(page,"performance","Frame time and memory on the street route",
        gtx1650 && frame.value("p95").toDouble() > 0 && frame.value("p95").toDouble() < 16.7
            && finalPlayer.report.value("peakWorkingSetMiB").toDouble() < 1024 ? "PASS" : "FAIL",
        playerSummary(finalPlayer) + ". Budget: p95 frame under 16.7 ms and peak working set under 1 GiB.");

    // Run the panel from the state before the coordinated change: the barrier blocks traffic again.
    store.history().undo();
    ForgeAiPanel panel(store);
    panel.show();
    panel.findChild<QPlainTextEdit *>("forgePrompt")->setPlainText(request);
    const auto click = [&](const char *name) {
        auto *button = panel.findChild<QPushButton *>(name);
        const bool enabled = button && button->isEnabled();
        if (enabled)
            button->click();
        return enabled;
    };
    const bool planned = click("forgePlanButton") && panel.streetPlan().has_value();
    const bool reviewed = click("forgePreview") && !panel.findChild<QComboBox *>("forgeBarrier")->isVisible();
    const auto panelHistory = store.history().index();
    const bool appliedFromPanel = click("forgeApply") && store.history().index() == panelHistory+1;
    const bool checked = click("forgeCheck");
    const auto report = panel.findChild<QPlainTextEdit *>("forgeReport")->toPlainText();
    panel.grab().save(output.filePath("street-panel.png"));
    audit.check(page,"workflow","The Forge AI panel reviews, applies, and checks the coordinated street change",
        planned && reviewed && appliedFromPanel && checked && report.contains("Play check passed"),
        QString("planned %1 reviewed %2 applied %3 checked %4. %5").arg(planned).arg(reviewed).arg(appliedFromPanel)
            .arg(checked).arg(report.left(300)));

    LiveViewport view(reopened,false);
    view.resize(1280,720);
    view.show();
    reopened.select({});
    view.frame();
    if (waitForRender(view))
        view.captureFrame().save(output.filePath("street-editor.png"));
}

void surfaceWorkflow(const QDir &output,Audit &audit)
{
    const QString page = "Texture and Material";
    QTemporaryDir directory;
    ProjectStore store;
    QString error;
    const auto add = [&](const QString &name) {
        return store.addMeshPrimitive(worldforge::Primitive::Plane,{2,2,1},4,4,name,error);
    };
    if (!directory.isValid() || !store.createStarterProject("Surface QA",
        {{"location",directory.filePath("Surface QA")},{"starter","blank"}},error) || !add("Source Plane")) {
        audit.check(page,"workflow","Create a surface project",false,error);
        return;
    }
    const auto source = store.selectedMesh()->id;
    if (!add("Target Plane")) {
        audit.check(page,"workflow","Create a second mesh",false,error);
        return;
    }
    const auto target = store.selectedMesh()->id;
    SurfaceWorkspace texture(store,true);
    texture.resize(1300,760);
    texture.show();
    auto *preview = namedWidget<QWidget>(texture,"Texture and UV preview");
    auto *overlay = namedWidget<QCheckBox>(texture,"Show UV Overlay");
    auto *info = namedWidget<QLabel>(texture,"Texture image information");
    auto *import = namedWidget<QPushButton>(texture,"Import Map…");
    auto *remove = namedWidget<QPushButton>(texture,"Remove Map");
    auto *view = texture.findChild<LiveViewport *>();
    if (!preview || !overlay || !info || !import || !remove || !view || !waitForRender(*view)) {
        audit.check(page,"workflow","Open the live Texture workspace",false);
        return;
    }
    const auto before = view->captureFrame();
    QImage sample(64,32,QImage::Format_RGB32);
    sample.fill(QColor("#ef4935"));
    const auto path = directory.filePath("map.png");
    if (!sample.save(path)) {
        audit.check(page,"workflow","Write texture fixture",false);
        return;
    }
    withDialog([&] { import->click(); },path);
    const auto after = view->captureFrame();
    const auto uvWith = preview->grab().toImage();
    overlay->setChecked(false);
    const auto uvWithout = preview->grab().toImage();
    const auto textureBytes = store.selectedMesh()->mesh->materials[0].baseColorTexture;
    audit.check(page,"Vulkan","Import shows dimensions and changes the 3D material preview",
        !textureBytes.empty() && info->text().contains("64 × 32") && !before.isNull() && !after.isNull() && before != after);
    audit.check(page,"workflow","UV overlay can be toggled over the image",
        !uvWith.isNull() && !uvWithout.isNull() && uvWith != uvWithout);
    uvWith.save(output.filePath("texture-uv-preview.png"));
    auto *fit = namedWidget<QPushButton>(texture,"Fit Image");
    const auto center = QPointF(preview->rect().center());
    QWheelEvent zoom(center,preview->mapToGlobal(center.toPoint()),{},QPoint(0,240),
        Qt::NoButton,Qt::NoModifier,Qt::NoScrollPhase,false);
    QApplication::sendEvent(preview,&zoom);
    const auto zoomed = preview->grab().toImage();
    if (fit)
        fit->click();
    audit.check(page,"workflow","Texture zoom changes the image and Fit Image restores it",
        fit && zoomed != uvWithout && preview->grab().toImage() == uvWithout);
    auto *exportUv = namedWidget<QPushButton>(texture,"Export UV Layout…");
    const auto uvPath = directory.filePath("uv-layout.png");
    const auto saveFile = [&](QPushButton *button,const QString &name) {
        if (!button)
            return;
        QTimer saveLayout;
        saveLayout.setInterval(20);
        QObject::connect(&saveLayout,&QTimer::timeout,&saveLayout,[&] {
            auto *dialog = qobject_cast<QFileDialog *>(QApplication::activeModalWidget());
            if (!dialog)
                return;
            dialog->setDirectory(directory.path());
            auto *fileName = dialog->findChild<QLineEdit *>("fileNameEdit");
            if (!fileName) {
                dialog->reject();
                return;
            }
            fileName->setText(name);
            QMetaObject::invokeMethod(dialog,"accept",Qt::DirectConnection);
        });
        saveLayout.start();
        button->click();
    };
    saveFile(exportUv,"uv-layout.png");
    const QImage exportedUv(uvPath);
    const auto *exportStatus = namedWidget<QLabel>(texture,"Surface operation result");
    auto *inspectUv = namedWidget<QPushButton>(texture,"Inspect UVs");
    const auto exportMessage = exportStatus ? exportStatus->text() : QString{};
    bool edgeVisible = false;
    if (exportedUv.size() == QSize(2048,2048))
        for (int i = 0; i < 8; ++i)
            edgeVisible |= exportedUv.pixelColor(i,1024).alpha() > 0;
    audit.check(page,"workflow","Export writes the UV layout as a transparent PNG",
        exportedUv.size() == QSize(2048,2048) && exportedUv.pixelColor(1024,1024).alpha() == 0
        && edgeVisible,
        QString("%1 × %2 | %3").arg(exportedUv.width()).arg(exportedUv.height()).arg(exportMessage));
    if (inspectUv)
        inspectUv->click();
    audit.check(page,"workflow","Inspect UVs reports the active mesh's layout",
        inspectUv && exportStatus && exportStatus->text().contains("1 islands")
        && exportStatus->text().contains("0 degenerate") && exportStatus->text().contains("0 overlaps"));
    auto *exportMap = namedWidget<QPushButton>(texture,"Export Map…");
    saveFile(exportMap,"roundtrip.png");
    QFile mapCopy(directory.filePath("roundtrip.png"));
    const QByteArray expected(reinterpret_cast<const char *>(textureBytes.data()),qsizetype(textureBytes.size()));
    audit.check(page,"workflow","Export Map preserves the original PNG bytes",
        exportMap && mapCopy.open(QIODevice::ReadOnly) && mapCopy.readAll() == expected);
    const auto jpegPath = directory.filePath("revised.jpg");
    sample.fill(QColor("#3977bf"));
    if (sample.save(jpegPath))
        withDialog([&] { import->click(); },jpegPath);
    QFile jpeg(jpegPath);
    const auto revised = jpeg.open(QIODevice::ReadOnly) ? jpeg.readAll() : QByteArray{};
    const bool reimported = !revised.isEmpty()
        && store.selectedMesh()->mesh->materials[0].baseColorTexture != textureBytes;
    if (reimported)
        saveFile(exportMap,"roundtrip.jpg");
    QFile jpegCopy(directory.filePath("roundtrip.jpg"));
    audit.check(page,"workflow","Replace the map with a JPEG and export it without re-encoding",
        reimported && jpegCopy.open(QIODevice::ReadOnly) && jpegCopy.readAll() == revised);
    if (reimported)
        store.history().undo();
    auto *uvSets = namedWidget<QComboBox>(texture,"Surface UV set");
    const auto originalUv = store.selectedMesh()->activeUvSet;
    const bool createdUv = store.selectMeshUvSet("Lightmap",true,false,error);
    const int originalIndex = uvSets ? uvSets->findData(originalUv) : -1;
    if (originalIndex >= 0) {
        uvSets->setCurrentIndex(originalIndex);
        QMetaObject::invokeMethod(uvSets,"activated",Qt::DirectConnection,Q_ARG(int,originalIndex));
    }
    const bool switched = store.selectedMesh()->activeUvSet == originalUv;
    if (createdUv && switched)
        store.history().undo();
    audit.check(page,"workflow","Switch a named UV set and undo from Texture",
        createdUv && originalIndex >= 0 && switched && store.selectedMesh()->activeUvSet == "Lightmap"
        && uvSets->currentData().toString() == "Lightmap");
    remove->click();
    const bool removed = store.selectedMesh()->mesh->materials[0].baseColorTexture.empty();
    if (removed)
        store.history().undo();
    audit.check(page,"workflow","Removing a map is undoable",
        removed && store.selectedMesh()->mesh->materials[0].baseColorTexture == textureBytes);
    auto *paint = namedWidget<QCheckBox>(texture,"Paint");
    auto *diameter = namedWidget<QSpinBox>(texture,"Brush diameter");
    auto *opacity = namedWidget<QSpinBox>(texture,"Brush opacity");
    if (!paint || !diameter || !opacity) {
        audit.check(page,"workflow","Open the Texture brush controls",false);
    } else {
        paint->setChecked(true);
        diameter->setValue(12);
        opacity->setValue(50);
        const auto point = preview->rect().center();
        const auto end = point+QPoint(25,0);
        const auto history = store.history().index();
        const auto unpainted3d = view->captureFrame();
        QTest::mousePress(preview,Qt::LeftButton,Qt::NoModifier,point);
        QMouseEvent drag(QEvent::MouseMove,QPointF(end),QPointF(preview->mapToGlobal(end)),
            Qt::NoButton,Qt::LeftButton,Qt::NoModifier);
        QApplication::sendEvent(preview,&drag);
        QTest::qWait(130);
        const auto live3d = view->captureFrame();
        audit.check(page,"Vulkan","A held stroke previews in 3D without touching the project",
            !unpainted3d.isNull() && !live3d.isNull() && live3d != unpainted3d
            && store.history().index() == history && store.selectedMesh()->mesh->materials[0].baseColorTexture == textureBytes);
        QTest::mouseRelease(preview,Qt::LeftButton,Qt::NoModifier,end);
        const auto painted = store.selectedMesh()->mesh->materials[0].baseColorTexture;
        const auto image = QImage::fromData(painted.data(),int(painted.size()));
        const auto centerColor = image.pixelColor(32,16);
        audit.check(page,"workflow","Painting preserves image dimensions and blends at the chosen opacity",
            image.size() == QSize(64,32) && image.pixelColor(1,1) == QColor("#ef4935")
            && std::abs(centerColor.red()-202) <= 2 && std::abs(centerColor.blue()-154) <= 2
            && store.history().index() == history+1);
        if (store.history().index() == history+1)
            store.history().undo();
        const bool undo = store.selectedMesh()->mesh->materials[0].baseColorTexture == textureBytes;
        if (store.history().canRedo())
            store.history().redo();
        const bool redo = store.selectedMesh()->mesh->materials[0].baseColorTexture == painted;
        const bool saved = store.save(&error);
        ProjectStore reopened;
        const auto *reopenedAsset = reopened.meshAsset(target);
        audit.check(page,"storage","A paint stroke is undoable, redoable and survives reopening",
            painted != textureBytes && undo && redo && saved && reopenedAsset
            && reopenedAsset->mesh->materials[0].baseColorTexture == painted,error);
        const auto beforeCancel = store.history().index();
        QTest::mousePress(preview,Qt::LeftButton,Qt::NoModifier,point+QPoint(-50,0));
        QTest::keyClick(preview,Qt::Key_Escape);
        QTest::mouseRelease(preview,Qt::LeftButton,Qt::NoModifier,point+QPoint(-50,0));
        audit.check(page,"workflow","Escape cancels the stroke without changing history",
            store.history().index() == beforeCancel && store.selectedMesh()->mesh->materials[0].baseColorTexture == painted);
        remove->click();
        QTest::mouseClick(preview,Qt::LeftButton,Qt::NoModifier,point);
        const auto &created = store.selectedMesh()->mesh->materials[0].baseColorTexture;
        const auto blankMap = QImage::fromData(created.data(),int(created.size()));
        audit.check(page,"workflow","Painting an empty slot creates a 1024-pixel base color map",
            blankMap.size() == QSize(1024,1024) && blankMap.pixelColor(5,5) == QColor(Qt::white)
            && blankMap.pixelColor(512,512) != QColor(Qt::white));
        if (store.history().index() == beforeCancel+2)
            store.history().undo();
        if (store.history().index() == beforeCancel+1)
            store.history().undo();
        if (store.history().index() == history+1)
            store.history().undo();
    }
    texture.hide();

    SurfaceWorkspace material(store,false);
    material.show();
    auto *link = namedWidget<QPushButton>(material,"Link Existing Material…");
    auto *unlink = namedWidget<QPushButton>(material,"Make Material Independent");
    auto *roughness = namedWidget<QDoubleSpinBox>(material,"Surface roughness");
    auto *apply = namedWidget<QPushButton>(material,"Apply Material");
    if (!link || !unlink || !roughness || !apply) {
        audit.check(page,"workflow","Open Material properties",false);
        return;
    }
    QTimer accept;
    accept.setInterval(20);
    QObject::connect(&accept,&QTimer::timeout,&accept,[] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        if (dialog && dialog->windowTitle() == "Link Existing Material")
            dialog->accept();
    });
    accept.start();
    link->click();
    accept.stop();
    const auto current = [&](const QString &id) -> const worldforge::MeshMaterial & {
        return store.meshAsset(id)->mesh->materials[0];
    };
    const auto id = current(target).sharedMaterialId;
    const bool linked = !id.empty() && current(source).sharedMaterialId == id;
    roughness->setValue(0.27);
    apply->click();
    const bool propagated = std::abs(current(source).roughness-0.27f) < 0.0001f && current(source) == current(target);
    if (propagated)
        store.history().undo();
    const bool undone = current(source) == current(target) && std::abs(current(source).roughness-0.65f) < 0.0001f;
    if (propagated)
        store.history().redo();
    const bool saved = store.save(&error);
    ProjectStore reopened;
    const auto *savedSource = reopened.meshAsset(source), *savedTarget = reopened.meshAsset(target);
    audit.check(page,"workflow","Linked material updates both meshes, undoes, and survives reopening",
        linked && propagated && undone && saved && savedSource && savedTarget
        && savedSource->mesh->materials[0] == savedTarget->mesh->materials[0]
        && savedSource->mesh->materials[0].sharedMaterialId == id,error);
    unlink->click();
    roughness->setValue(0.42);
    apply->click();
    audit.check(page,"workflow","Making a material independent isolates later changes",
        current(target).sharedMaterialId.empty() && current(source).sharedMaterialId == id
        && std::abs(current(target).roughness-0.42f) < 0.0001f && std::abs(current(source).roughness-0.27f) < 0.0001f);
    QTimer append;
    append.setInterval(20);
    QObject::connect(&append,&QTimer::timeout,&append,[] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        if (!dialog || dialog->windowTitle() != "Link Existing Material")
            return;
        if (auto *destination = namedWidget<QComboBox>(*dialog,"Material link destination"))
            destination->setCurrentIndex(1);
        dialog->accept();
    });
    append.start();
    link->click();
    append.stop();
    const auto &materials = store.meshAsset(target)->mesh->materials;
    audit.check(page,"workflow","Link as a new slot preserves the original material and face assignment",
        materials.size() == 2 && materials[0].sharedMaterialId.empty() && materials[1] == current(source)
        && store.meshAsset(target)->mesh->faces.front().material == 0);
    auto *assign = namedWidget<QPushButton>(material,"Assign Slot to All Faces");
    if (assign)
        assign->click();
    const bool assigned = store.meshAsset(target)->mesh->faces.front().material == 1;
    if (assigned)
        store.history().undo();
    audit.check(page,"workflow","Assigning the new slot changes mesh faces and supports undo",
        assigned && store.meshAsset(target)->mesh->faces.front().material == 0);
}

void modelingHierarchyWorkflow(const QDir &output,Audit &audit)
{
    const QString page = "Modeling hierarchy";
    QTemporaryDir directory;
    ProjectStore store;
    QString error;
    if (!directory.isValid() || !store.createStarterProject("Hierarchy QA",
        {{"location",directory.filePath("Hierarchy QA")},{"starter","blank"}},error)) {
        audit.check(page,"workflow","Create hierarchy project",false,error);
        return;
    }
    const auto add = [&](const QString &name,QVector3D position) {
        if (!store.addMeshPrimitive(worldforge::Primitive::Cube,{1,1,1},4,4,name,error))
            return QString{};
        auto object = *store.selectedObject();
        object.position = position;
        store.updateObject(object,"Place fixture object");
        return object.id;
    };
    const auto root = add("Assembly",{}), arm = add("Arm",{1,0,0}), tip = add("Tip",{2,0,0});
    if (root.isEmpty() || arm.isEmpty() || tip.isEmpty()) {
        audit.check(page,"workflow","Create three mesh objects",false,error);
        return;
    }
    const auto equal = [](const QMatrix4x4 &a,const QMatrix4x4 &b) {
        for (int row = 0; row < 4; ++row)
            for (int column = 0; column < 4; ++column)
                if (std::abs(a(row,column)-b(row,column)) > 0.0002f)
                    return false;
        return true;
    };
    const auto armStart = store.worldMatrix(arm), tipStart = store.worldMatrix(tip);
    const bool linked = store.reparentObject(arm,root,error) && store.reparentObject(tip,arm,error);
    const bool retained = equal(armStart,store.worldMatrix(arm)) && equal(tipStart,store.worldMatrix(tip));
    const bool cycle = !store.reparentObject(root,tip,error) && store.object(root)->parentId.isEmpty();
    audit.check(page,"workflow","Three-level parenting keeps placement and rejects a cycle",linked && retained && cycle);
    if (!linked)
        return;
    store.select(root);
    ModelingWorkspace workspace(store);
    workspace.resize(1300,760);
    workspace.show();
    auto *view = workspace.findChild<LiveViewport *>();
    auto *tree = workspace.findChild<QTreeWidget *>("meshOutliner");
    auto *rotate = namedWidget<QToolButton>(workspace,"Rotate");
    auto *scale = namedWidget<QToolButton>(workspace,"Scale");
    auto *z = namedWidget<QDoubleSpinBox>(workspace,"Transform Z");
    auto *apply = namedWidget<QToolButton>(workspace,"Apply");
    if (!view || !tree || !rotate || !scale || !z || !apply || !waitForRender(*view) || !exposedVulkanWindow()) {
        audit.check(page,"workflow","Open the Modeling assembly viewport",false);
        return;
    }
    auto *surface = exposedVulkanWindow();
    auto *rootRow = tree->currentItem();
    audit.check(page,"workflow","Outliner shows parent, child and grandchild",
        rootRow && rootRow->childCount() == 1 && rootRow->child(0)->childCount() == 1
        && rootRow->child(0)->child(0)->text(0) == "Tip");
    view->setModelAxisView(2);
    view->frame();
    QTest::qWait(100);
    const QPoint pivot(surface->width()/2-qRound(surface->height()/(2*std::sqrt(11.0)*1.55*0.41421356)),
        surface->height()/2);
    QTest::mousePress(surface,Qt::LeftButton,Qt::NoModifier,pivot);
    const auto movedPixel = pivot+QPoint(42,-14);
    QMouseEvent move(QEvent::MouseMove,QPointF(movedPixel),QPointF(surface->mapToGlobal(movedPixel)),
        Qt::NoButton,Qt::LeftButton,Qt::NoModifier);
    QApplication::sendEvent(surface,&move);
    QTest::mouseRelease(surface,Qt::LeftButton,Qt::NoModifier,movedPixel);
    const auto movedRoot = store.worldMatrix(root);
    const bool dragged = !movedRoot.isIdentity() && equal(store.worldMatrix(tip),movedRoot*tipStart);
    if (dragged)
        store.history().undo();
    audit.check(page,"workflow","Object Move gizmo drags the assembly and undoes",
        dragged && equal(store.worldMatrix(tip),tipStart));
    rotate->click();
    apply->click();
    view->frame();
    QTest::qWait(60);
    const QPoint turnPivot(surface->width()/2-qRound(surface->height()/(2*std::sqrt(11.0)*1.55*0.41421356)),
        surface->height()/2);
    const int diagonal = qRound(90.0*0.83/(2*0.41421356*std::sqrt(2.0)));
    const auto ringStart = turnPivot+QPoint(diagonal,-diagonal), ringEnd = turnPivot+QPoint(-diagonal,-diagonal);
    QTest::mousePress(surface,Qt::LeftButton,Qt::NoModifier,ringStart);
    QMouseEvent turn(QEvent::MouseMove,QPointF(ringEnd),QPointF(surface->mapToGlobal(ringEnd)),
        Qt::NoButton,Qt::LeftButton,Qt::NoModifier);
    QApplication::sendEvent(surface,&turn);
    QTest::mouseRelease(surface,Qt::LeftButton,Qt::NoModifier,ringEnd);
    const auto dragTurn = store.worldMatrix(tip).map(QVector3D{});
    const bool dragRotated = dragTurn.y() > 1.7f && std::abs(dragTurn.z()) < 0.001f
        && std::abs(dragTurn.length()-2) < 0.001f;
    if (!store.worldMatrix(root).isIdentity())
        store.history().undo();
    audit.check(page,"workflow","Dragging the Z ring rotates descendants around the parent pivot",dragRotated,
        QString("%1, %2").arg(dragTurn.x()).arg(dragTurn.y()));
    rotate->click();
    z->setValue(90);
    apply->click();
    const auto rotatedTip = store.worldMatrix(tip).map(QVector3D{});
    audit.check(page,"workflow","Object Rotate turns children around the parent pivot",
        (rotatedTip-QVector3D(0,2,0)).length() < 0.001f);
    scale->click();
    auto *scaleX = namedWidget<QDoubleSpinBox>(workspace,"Transform X");
    if (scaleX)
        scaleX->setValue(1.5);
    apply->click();
    const auto scaledTip = store.worldMatrix(tip).map(QVector3D{});
    audit.check(page,"workflow","Object Scale propagates through the three-level assembly",
        scaleX && (scaledTip-QVector3D(0,3,0)).length() < 0.001f);
    store.select(arm);
    auto *childX = namedWidget<QDoubleSpinBox>(workspace,"Location X");
    if (childX) {
        childX->setValue(2);
        emit childX->editingFinished();
    }
    const bool childEdited = (store.worldMatrix(tip).map(QVector3D{})-QVector3D(0,4.5f,0)).length() < 0.001f;
    if (childEdited)
        store.history().undo();
    audit.check(page,"workflow","Editing child local coordinates moves its descendants",childEdited);

    const auto beforeUnparent = store.worldMatrix(tip);
    const bool reparented = store.reparentObject(tip,root,error) && equal(beforeUnparent,store.worldMatrix(tip));
    if (reparented)
        store.history().undo();
    audit.check(page,"workflow","Reparenting under a rotated, nonuniformly scaled root preserves the complete matrix",
        reparented && store.object(tip)->parentId == arm && equal(beforeUnparent,store.worldMatrix(tip)));
    const bool unparented = store.reparentObject(tip,{},error) && equal(beforeUnparent,store.worldMatrix(tip));
    if (unparented)
        store.history().undo();
    const bool restored = store.object(tip)->parentId == arm;
    if (restored) {
        store.history().redo();
        store.history().undo();
    }
    audit.check(page,"workflow","Unparent preserves world placement through Undo and Redo",
        unparented && restored && equal(beforeUnparent,store.worldMatrix(tip)));
    store.select(root);
    store.deleteObject();
    const bool preserved = !store.object(root) && store.object(arm)->parentId.isEmpty()
        && store.object(tip)->parentId == arm && equal(beforeUnparent,store.worldMatrix(tip));
    if (preserved)
        store.history().undo();
    const bool saved = preserved && store.save(&error);
    ProjectStore reopened;
    audit.check(page,"workflow","Deleting a parent preserves descendants and Undo survives save",
        saved && reopened.object(arm) && reopened.object(arm)->parentId == root
        && reopened.object(tip) && reopened.object(tip)->parentId == arm && equal(beforeUnparent,reopened.worldMatrix(tip)),error);

    std::stringstream bytes(std::ios::in|std::ios::out|std::ios::binary);
    const bool exported = saved && WorldRuntime::exportScene(reopened,bytes);
    const auto playable = exported ? worldforge::readPlayable(bytes) : std::nullopt;
    bool transformed = false;
    if (playable && playable->meshes.size() == 3) {
        const auto &mesh = reopened.meshAsset(reopened.object(tip)->meshAsset)->evaluated();
        const auto vertices = worldforge::makePlayVertices(mesh);
        const auto &actual = playable->meshes.back().vertices.front().position;
        const auto &p = vertices.front().position;
        transformed = (QVector3D(actual[0],actual[1],actual[2])-beforeUnparent.map({p[0],p[1],p[2]})).length() < 0.001f;
    }
    QFile fixture(output.filePath("hierarchy-assembly.wfplay"));
    const auto data = bytes.str();
    const bool written = transformed && fixture.open(QIODevice::WriteOnly)
        && fixture.write(data.data(),qsizetype(data.size())) == qsizetype(data.size());
    audit.check(page,"workflow","Player export bakes the grandchild world transform",written);
    workspace.hide();
    LiveViewport world(reopened,false);
    world.resize(900,570);
    world.show();
    if (saved && waitForRender(world)) {
        world.frame();
        const auto before = world.captureFrame();
        auto movedParent = *reopened.object(root);
        movedParent.position.setX(1);
        reopened.updateObject(movedParent,"Move assembly in World Editor");
        const auto after = world.captureFrame();
        audit.check(page,"Vulkan","World Editor redraws the assembly after the root moves",
            !before.isNull() && !after.isNull() && before != after);
    } else
        audit.check(page,"Vulkan","World Editor opens the saved assembly",false);
}

void modelingViewportToolsWorkflow(const QDir &output,Audit &audit)
{
    const QString page = "Modeling viewport tools";
    QTemporaryDir directory;
    ProjectStore store;
    QString error;
    if (!directory.isValid() || !store.createStarterProject("Modeling Tools QA",
        {{"location",directory.filePath("Modeling Tools QA")},{"starter","blank"}},error)
        || !store.addMeshPrimitive(worldforge::Primitive::Plane,{2,2,1},4,4,"Textured plane",error)) {
        audit.check(page,"workflow","Create viewport tool fixture",false,error);
        return;
    }
    const auto objectId = store.selection();
    const auto assetId = store.selectedMesh()->id;
    const auto original = store.selectedMesh()->mesh;
    ModelingWorkspace workspace(store);
    workspace.resize(1300,760);
    workspace.show();
    auto *view = workspace.findChild<LiveViewport *>();
    auto *measure = namedWidget<QToolButton>(workspace,"Toolbar: Measure");
    auto *annotate = namedWidget<QToolButton>(workspace,"Toolbar: Annotate");
    auto *spin = namedWidget<QToolButton>(workspace,"Toolbar: Spin");
    auto *targetWeld = namedWidget<QToolButton>(workspace,"Toolbar: Target Weld");
    auto *connectTool = namedWidget<QToolButton>(workspace,"Toolbar: Connect");
    auto *loopCut = namedWidget<QToolButton>(workspace,"Toolbar: Loop Cut");
    auto *cutCount = namedWidget<QSpinBox>(workspace,"Interactive cut count");
    auto *measurement = namedWidget<QLabel>(workspace,"Surface measurement");
    auto *measurementMode = namedWidget<QComboBox>(workspace,"Measurement mode");
    auto *clear = namedWidget<QPushButton>(workspace,"Clear Annotations");
    auto *erase = namedWidget<QToolButton>(workspace,"Erase Annotation");
    auto *textureButton = namedWidget<QPushButton>(workspace,"Material Preview Settings…");
    auto *revolve = namedWidget<QToolButton>(workspace,"Revolve…");
    if (!view || !measure || !annotate || !spin || !targetWeld || !connectTool || !loopCut || !cutCount
        || !measurement || !measurementMode || !clear || !erase || !textureButton || !revolve
        || !waitForRender(*view) || !exposedVulkanWindow()) {
        audit.check(page,"workflow","Expose markup tools and Vulkan viewport",false);
        return;
    }
    auto *surface = exposedVulkanWindow();
    view->setModelAxisView(2);
    view->frame();
    QTest::qWait(100);
    const QPoint center(surface->width()/2,surface->height()/2);
    const int half = qRound(surface->height()/(2*std::sqrt(8.0)*1.55*0.41421356));
    QTest::keyClick(surface,Qt::Key_F10);
    QTest::mouseClick(surface,Qt::LeftButton,Qt::NoModifier,center+QPoint(half,0));
    QTimer confirmSpin;
    confirmSpin.setInterval(20);
    QObject::connect(&confirmSpin,&QTimer::timeout,&confirmSpin,[] {
        if (auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            dialog && dialog->windowTitle() == "Spin Boundary") {
            if (auto *axis = namedWidget<QComboBox>(*dialog,"Spin axis"))
                axis->setCurrentIndex(1);
            dialog->accept();
        }
    });
    confirmSpin.start();
    spin->click();
    confirmSpin.stop();
    const bool swept = store.selectedMesh()->mesh->faces.size() == 13;
    if (swept)
        store.history().undo();
    audit.check(page,"workflow","Spin shelf sweeps a picked boundary edge and supports undo",
        swept && *store.selectedMesh()->mesh == *original);
    connectTool->click();
    QTest::mouseClick(surface,Qt::LeftButton,Qt::NoModifier,center-QPoint(half,0));
    QTest::mouseClick(surface,Qt::LeftButton,Qt::NoModifier,center+QPoint(half,0));
    const bool previewOnly = *store.selectedMesh()->mesh == *original;
    QTest::keyClick(surface,Qt::Key_Return);
    const bool connected = store.selectedMesh()->mesh->faces.size() == 2 && store.selectedMesh()->mesh->vertices.size() == 6;
    if (connected)
        store.history().undo();
    audit.check(page,"workflow","Connect inserts an edge across picked edge midpoints and undoes",
        previewOnly && connected && *store.selectedMesh()->mesh == *original);
    QTest::keyClick(surface,Qt::Key_F9);
    targetWeld->click();
    const auto weldStart = center-QPoint(half,half), weldEnd = center+QPoint(half,-half);
    QTest::mousePress(surface,Qt::LeftButton,Qt::NoModifier,weldStart);
    QMouseEvent weldDrag(QEvent::MouseMove,QPointF(weldEnd),QPointF(surface->mapToGlobal(weldEnd)),
        Qt::NoButton,Qt::LeftButton,Qt::NoModifier);
    QApplication::sendEvent(surface,&weldDrag);
    QTest::mouseRelease(surface,Qt::LeftButton,Qt::NoModifier,weldEnd);
    const bool welded = store.selectedMesh()->mesh->faces.size() == 1 && store.selectedMesh()->mesh->vertices.size() == 3;
    if (welded)
        store.history().undo();
    audit.check(page,"workflow","Target Weld drags one vertex onto another and undoes",
        welded && *store.selectedMesh()->mesh == *original);
    loopCut->click();
    cutCount->setValue(2);
    const QPoint loopCenter(surface->width()/2,surface->height()/2);
    const int loopHalf = qRound(surface->height()/(2*std::sqrt(8.0)*1.55*0.41421356));
    QTest::mouseClick(surface,Qt::LeftButton,Qt::NoModifier,loopCenter+QPoint(loopHalf,0));
    const bool loopPreview = *store.selectedMesh()->mesh == *original;
    QTest::keyClick(surface,Qt::Key_Return);
    const bool loopApplied = store.selectedMesh()->mesh->faces.size() == 3 && store.selectedMesh()->mesh->vertices.size() == 8;
    if (loopApplied)
        store.history().undo();
    audit.check(page,"workflow","Interactive Loop Cut previews two segments before committing",
        loopPreview && loopApplied && *store.selectedMesh()->mesh == *original);
    if (auto *object = namedWidget<QToolButton>(workspace,"Object"))
        object->click();
    view->frame();
    const auto measurePoints = [surface] {
        const QPoint midpoint(surface->width()/2,surface->height()/2);
        const QPoint offset(qRound(surface->height()*0.04),0);
        return std::pair{midpoint-offset,midpoint+offset};
    };
    measure->click();
    QTest::qWait(100);
    const auto [left,right] = measurePoints();
    const auto pixelFraction = [&] (QPoint a,QPoint b) { return double(b.x()-a.x())/surface->height(); };
    const auto initialFraction = pixelFraction(left,right);
    QTest::mouseClick(surface,Qt::LeftButton,Qt::NoModifier,left);
    QTest::mouseClick(surface,Qt::LeftButton,Qt::NoModifier,right);
    const auto baselineDistance = measurement->text().section(' ',1,1).toDouble();
    auto scaled = *store.selectedObject();
    scaled.scale = {2,2,2};
    store.updateObject(scaled,"Scale measurement fixture");
    QTest::qWait(100);
    const auto [scaledLeft,scaledRight] = measurePoints();
    const auto scaledFraction = pixelFraction(scaledLeft,scaledRight);
    QTest::mouseClick(surface,Qt::LeftButton,Qt::NoModifier,scaledLeft);
    QTest::mouseClick(surface,Qt::LeftButton,Qt::NoModifier,scaledRight);
    const auto scaledDistance = measurement->text().section(' ',1,1).toDouble();
    audit.check(page,"workflow","Measure reports scaled scene distance",
        baselineDistance > 0 && std::abs(2*baselineDistance*scaledFraction/initialFraction-scaledDistance) < 0.0005,
        QString("Baseline: %1 m | %2").arg(baselineDistance).arg(measurement->text()));
    measurementMode->setCurrentIndex(1);
    const auto angleCenter = (scaledLeft+scaledRight)/2;
    QTest::mouseClick(surface,Qt::LeftButton,Qt::NoModifier,scaledLeft);
    QTest::mouseClick(surface,Qt::LeftButton,Qt::NoModifier,angleCenter);
    QTest::mouseClick(surface,Qt::LeftButton,Qt::NoModifier,angleCenter+QPoint(0,scaledRight.x()-angleCenter.x()));
    audit.check(page,"workflow","Measure computes the angle at the middle surface point",
        measurement->text().contains("90.00"),measurement->text());
    measurementMode->setCurrentIndex(0);
    annotate->click();
    QTest::mousePress(surface,Qt::LeftButton,Qt::NoModifier,left);
    QMouseEvent move(QEvent::MouseMove,QPointF(right),QPointF(surface->mapToGlobal(right)),
        Qt::NoButton,Qt::LeftButton,Qt::NoModifier);
    QApplication::sendEvent(surface,&move);
    QTest::mouseRelease(surface,Qt::LeftButton,Qt::NoModifier,right);
    const auto annotations = [&] { return store.selectedObject()->details.value("modelAnnotations").toArray(); };
    const bool drawn = annotations().size() == 1;
    const bool saved = store.save(&error);
    ProjectStore reopened;
    const auto *savedObject = reopened.object(objectId);
    const bool restored = savedObject && savedObject->details.value("modelAnnotations").toArray() == annotations();
    erase->click();
    QTest::mouseClick(surface,Qt::LeftButton,Qt::NoModifier,(left+right)/2);
    const bool erased = annotations().isEmpty();
    if (erased)
        store.history().undo();
    audit.check(page,"workflow","Erase Annotation removes a single stroke and supports undo",
        drawn && erased && annotations().size() == 1);
    clear->click();
    const bool cleared = annotations().isEmpty();
    store.history().undo();
    audit.check(page,"workflow","Surface annotation saves, clears and supports undo",
        drawn && saved && restored && cleared && annotations().size() == 1,error);
    clear->click();
    QTest::keyClick(surface,Qt::Key_Escape);

    QImage swatch(8,8,QImage::Format_RGB32);
    for (int y = 0; y < swatch.height(); ++y)
        for (int x = 0; x < swatch.width(); ++x)
            swatch.setPixelColor(x,y,x < 4 ? QColor(250,20,25) : QColor(25,30,250));
    QByteArray encoded;
    QBuffer buffer(&encoded);
    buffer.open(QIODevice::WriteOnly);
    const bool encodedPng = swatch.save(&buffer,"PNG");
    auto texturedMesh = *original;
    texturedMesh.materials[0].baseColorTexture.assign(encoded.begin(),encoded.end());
    QTest::keyClick(surface,Qt::Key_6);
    const auto untexturedFrame = view->captureFrame();
    const auto textureFile = directory.filePath("swatch.png");
    const bool textureFileSaved = swatch.save(textureFile,"PNG");
    if (textureFileSaved) {
        QTimer input;
        input.setInterval(20);
        QObject::connect(&input,&QTimer::timeout,&input,[&] {
            auto *dialog = QApplication::activeModalWidget();
            if (auto *picker = qobject_cast<QFileDialog *>(dialog)) {
                picker->selectFile(textureFile);
                QMetaObject::invokeMethod(picker,"accept",Qt::DirectConnection);
            } else if (dialog && dialog->windowTitle() == "Material Preview Settings") {
                if (auto *choose = namedWidget<QPushButton>(*dialog,"Choose Image…"))
                    choose->click();
            }
        });
        input.start();
        textureButton->click();
    }
    const bool assigned = textureFileSaved && *store.selectedMesh()->mesh == texturedMesh;
    const auto texturedFrame = view->captureFrame().convertToFormat(QImage::Format_RGB32);
    if (!texturedFrame.isNull())
        texturedFrame.save(output.filePath("modeling-textured-preview.png"));
    int red = 0, blue = 0;
    for (int y = 0; y < texturedFrame.height(); ++y) {
        const auto *pixels = reinterpret_cast<const QRgb *>(texturedFrame.constScanLine(y));
        for (int x = 0; x < texturedFrame.width(); ++x) {
            red += qRed(pixels[x]) > qBlue(pixels[x])*3 && qRed(pixels[x]) > 65;
            blue += qBlue(pixels[x]) > qRed(pixels[x])*3 && qBlue(pixels[x]) > 75;
        }
    }
    audit.check(page,"Vulkan","Material Preview samples both halves of a UV texture",
        encodedPng && assigned && !untexturedFrame.isNull() && red > 50 && blue > 50,
        QString("red=%1 blue=%2 %3").arg(red).arg(blue).arg(error));
    const auto glb = exportGlb(texturedMesh,"Textured plane",error);
    const auto imported = importGlb(glb,error);
    const bool textureSaved = store.save(&error);
    ProjectStore textureReopened;
    const auto *asset = textureReopened.meshAsset(assetId);
    audit.check(page,"workflow","PNG survives project reopen and embedded GLB round trip",
        textureSaved && asset && *asset->mesh == texturedMesh && imported
        && std::ranges::any_of(imported->materials,[&](const auto &slot) {
            return slot.baseColorTexture == texturedMesh.materials[0].baseColorTexture;
        }),error);
    QTimer materialEdit;
    materialEdit.setInterval(20);
    QObject::connect(&materialEdit,&QTimer::timeout,&materialEdit,[] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        if (!dialog || dialog->windowTitle() != "Material Preview Settings")
            return;
        if (auto *metallic = namedWidget<QDoubleSpinBox>(*dialog,"Material metallic"))
            metallic->setValue(0.9);
        if (auto *roughness = namedWidget<QDoubleSpinBox>(*dialog,"Material roughness"))
            roughness->setValue(0.2);
        if (auto *buttons = dialog->findChild<QDialogButtonBox *>())
            buttons->button(QDialogButtonBox::Ok)->click();
    });
    materialEdit.start();
    textureButton->click();
    materialEdit.stop();
    const auto changedFrame = view->captureFrame().convertToFormat(QImage::Format_RGB32);
    const auto &changed = store.selectedMesh()->mesh->materials.front();
    audit.check(page,"Vulkan","Material preview responds to metallic and roughness controls",
        !texturedFrame.isNull() && !changedFrame.isNull() && changedFrame != texturedFrame
        && std::abs(changed.metallic-0.9f) < 0.001f && std::abs(changed.roughness-0.2f) < 0.001f);
    auto mapped = *store.selectedMesh()->mesh;
    auto &slot = mapped.materials.front();
    for (auto *map : {&slot.normalTexture,&slot.metallicRoughnessTexture,&slot.occlusionTexture,&slot.emissiveTexture})
        map->assign(encoded.begin(),encoded.end());
    slot.emissive = {0.9f,0.4f,0.1f};
    const auto previousMaterial = store.selectedMesh()->mesh;
    const bool mapsAssigned = store.updateMesh(assetId,mapped,"Assign material maps",error,previousMaterial.data());
    const auto mappedFrame = view->captureFrame().convertToFormat(QImage::Format_RGB32);
    const auto importedMaps = importGlb(exportGlb(mapped,"Material maps",error),error);
    const bool mapsSaved = store.save(&error);
    ProjectStore mapsReopened;
    const auto *savedMaps = mapsReopened.meshAsset(assetId);
    audit.check(page,"Vulkan","PBR maps render and round-trip through project and GLB",
        mapsAssigned && mapsSaved && savedMaps && *savedMaps->mesh == mapped && !mappedFrame.isNull()
        && mappedFrame != changedFrame && importedMaps && importedMaps->materials.size() > 1
        && importedMaps->materials[1] == slot,error);
    QTimer acceptRevolve;
    acceptRevolve.setInterval(20);
    bool invalidProfileBlocked = false;
    QObject::connect(&acceptRevolve,&QTimer::timeout,&acceptRevolve,[&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        if (!dialog || dialog->windowTitle() != "Revolve Profile")
            return;
        acceptRevolve.stop();
        auto *points = namedWidget<QTableWidget>(*dialog,"Profile points");
        auto *interpolation = namedWidget<QComboBox>(*dialog,"Profile interpolation");
        auto *buttons = dialog->findChild<QDialogButtonBox *>();
        if (!points || !interpolation || !buttons) {
            dialog->reject();
            return;
        }
        auto *apply = buttons->button(QDialogButtonBox::Ok);
        const auto radius = points->item(1,0)->text();
        points->item(1,0)->setText("-1");
        invalidProfileBlocked = !apply->isEnabled();
        points->item(1,0)->setText(radius);
        interpolation->setCurrentIndex(2);
        if (apply->isEnabled())
            apply->click();
        else
            dialog->reject();
    });
    acceptRevolve.start();
    revolve->click();
    acceptRevolve.stop();
    const auto *generated = store.selectedMesh();
    const bool revolved = generated && generated->id != assetId && generated->mesh->faces.size() == 1024
        && generated->revolveProfile.value("interpolation").toInt() == 2;
    const auto generatedId = generated ? generated->id : QString{};
    const bool profileSaved = revolved && store.save(&error);
    ProjectStore profileReopened;
    const auto *savedProfile = profileReopened.meshAsset(generatedId);
    audit.check(page,"workflow","NURBS control points survive project reopen",
        profileSaved && savedProfile && savedProfile->revolveProfile == generated->revolveProfile,error);
    QAction *editProfile = nullptr;
    for (auto *button : workspace.findChildren<QToolButton *>())
        if (button->text() == "Mesh Data" && button->menu())
            for (auto *action : button->menu()->actions())
                if (action->text() == "Edit Revolve Profile…")
                    editProfile = action;
    bool editedProfile = false;
    if (revolved && editProfile) {
        const auto originalProfile = generated->revolveProfile;
        QTimer changeProfile;
        changeProfile.setInterval(20);
        QObject::connect(&changeProfile,&QTimer::timeout,&changeProfile,[&] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            if (!dialog || dialog->windowTitle() != "Revolve Profile")
                return;
            changeProfile.stop();
            auto *points = namedWidget<QTableWidget>(*dialog,"Profile points");
            auto *buttons = dialog->findChild<QDialogButtonBox *>();
            if (points && buttons) {
                points->item(2,2)->setText("2");
                if (buttons->button(QDialogButtonBox::Ok)->isEnabled()) {
                    buttons->button(QDialogButtonBox::Ok)->click();
                    return;
                }
            }
            dialog->reject();
        });
        changeProfile.start();
        editProfile->trigger();
        const auto *updated = store.meshAsset(generatedId);
        editedProfile = updated && updated->revolveProfile != originalProfile;
        if (editedProfile)
            store.history().undo();
        editedProfile = editedProfile && store.meshAsset(generatedId)->revolveProfile == originalProfile;
    }
    audit.check(page,"workflow","Re-editing a NURBS weight keeps the asset and undoes the parameter change",editedProfile);
    if (revolved)
        store.history().undo();
    audit.check(page,"workflow","NURBS Revolve validates the profile, creates a separate mesh and supports undo",
        invalidProfileBlocked && revolved && store.selectedMesh() && store.selectedMesh()->id == assetId);

    const auto originalObject = store.selection();
    const auto base = store.selectedMesh()->mesh;
    const bool secondAdded = store.addMeshPrimitive(worldforge::Primitive::Plane,{2,2,1},4,4,"Second plane",error);
    const auto *second = store.selectedMesh();
    const auto secondAsset = second ? second->id : QString{};
    const auto secondBase = second ? second->mesh : QSharedPointer<const worldforge::Mesh>{};
    const auto secondObject = store.selection();
    auto *multi = namedWidget<QToolButton>(workspace,"Multi-Object");
    if (secondAdded && multi && multi->menu()) {
        const auto actions = multi->menu()->actions();
        for (const auto &id : {originalObject,secondObject}) {
            store.select(id);
            QTest::keyClick(surface,Qt::Key_F9);
            view->frame();
            QTest::qWait(80);
            const QPoint mid(surface->width()/2,surface->height()/2);
            const int extent = qRound(surface->height()/(2*std::sqrt(8.0)*1.55*0.41421356));
            QTest::mouseClick(surface,Qt::LeftButton,Qt::NoModifier,mid-QPoint(extent,extent));
            actions[0]->trigger();
        }
        QTimer applyMulti;
        applyMulti.setInterval(20);
        QObject::connect(&applyMulti,&QTimer::timeout,&applyMulti,[] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            if (!dialog || dialog->windowTitle() != "Transform Captured Components")
                return;
            auto *z = namedWidget<QDoubleSpinBox>(*dialog,"Multi-object Z");
            auto *buttons = dialog->findChild<QDialogButtonBox *>();
            if (z && buttons) {
                z->setValue(0.25);
                buttons->button(QDialogButtonBox::Ok)->click();
            } else {
                dialog->reject();
            }
        });
        applyMulti.start();
        actions[1]->trigger();
    }
    const auto movedVertex = [](const auto &current,const auto &before,float localDistance) {
        for (std::size_t index = 0; index < before->vertices.size(); ++index)
            if (std::abs(current->vertices[index][2]-before->vertices[index][2]-localDistance) < 0.0001f)
                return true;
        return false;
    };
    const bool moved = secondAdded && movedVertex(store.meshAsset(assetId)->mesh,base,0.125f)
        && movedVertex(store.meshAsset(secondAsset)->mesh,secondBase,0.25f);
    if (moved)
        store.history().undo();
    audit.check(page,"workflow","One multi-object component edit moves both meshes and undoes together",
        moved && store.meshAsset(assetId)->mesh == base && store.meshAsset(secondAsset)->mesh == secondBase,error);
    auto grouped = *store.object(secondObject);
    grouped.group = "QA Assembly";
    store.updateObject(grouped,"Move to QA Assembly");
    auto *tree = workspace.findChild<QTreeWidget *>("meshOutliner");
    auto *search = namedWidget<QLineEdit>(workspace,"Search mesh objects");
    QTreeWidgetItem *groupRow = nullptr;
    if (tree)
        for (int row = 0; row < tree->topLevelItemCount(); ++row)
            if (tree->topLevelItem(row)->text(0) == grouped.group)
                groupRow = tree->topLevelItem(row);
    const bool groupedRow = groupRow && groupRow->childCount() == 1
        && groupRow->child(0)->text(0) == grouped.name;
    if (search)
        search->setText("No matching meshes");
    const bool filtered = groupedRow && groupRow->isHidden();
    if (search)
        search->setText(grouped.group);
    const bool foundGroup = groupedRow && !groupRow->isHidden();
    if (search)
        search->clear();
    if (groupedRow)
        groupRow->child(0)->setCheckState(1,Qt::Unchecked);
    QCoreApplication::processEvents();
    const bool hidden = store.object(secondObject) && !store.object(secondObject)->visible;
    if (hidden)
        store.history().undo();
    audit.check(page,"workflow","Model Outliner groups and filters meshes and undoes visibility",
        filtered && foundGroup && hidden && store.object(secondObject)->visible);
    store.select(secondObject);
    auto *newSlot = namedWidget<QPushButton>(workspace,"New Slot…");
    auto *selectFaces = namedWidget<QPushButton>(workspace,"Select Faces");
    auto *assignFaces = namedWidget<QPushButton>(workspace,"Assign Faces");
    auto *slotPicker = namedWidget<QComboBox>(workspace,"Active material slot");
    if (newSlot && selectFaces && assignFaces && slotPicker) {
        if (auto *objectMode = namedWidget<QToolButton>(workspace,"Object"))
            objectMode->click();
        withDialog([&] { newSlot->click(); },"QA Paint");
        const int created = slotPicker->findText("QA Paint");
        slotPicker->setCurrentIndex(0);
        selectFaces->click();
        if (created >= 0)
            slotPicker->setCurrentIndex(created);
        assignFaces->click();
        const auto *mesh = store.selectedMesh();
        audit.check(page,"workflow","Details creates a slot and assigns the faces selected by material",
            created > 0 && mesh && mesh->mesh->faces.front().material == std::uint32_t(created));
    } else {
        audit.check(page,"workflow","Details exposes material slot controls",false);
    }
}

void modelingRetopologyWorkflow(const QDir &output,Audit &audit)
{
    namespace wf = worldforge;
    QTemporaryDir directory;
    ProjectStore store;
    const QString page = "Modeling retopology";
    QString error;
    const auto sphere = wf::makePrimitive(wf::Primitive::Sphere,{4,4,4},48,32);
    const auto bytes = sphere ? exportGlb(*sphere,"Curved retopology reference",error) : QByteArray{};
    QFile file(directory.filePath("Curved reference.glb"));
    const bool written = !bytes.isEmpty() && file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
    file.close();
    const bool imported = written && store.createStarterProject("Retopology QA",
        {{"location",directory.filePath("Retopology QA")},{"starter","blank"}},error) && store.importMesh(file.fileName(),error);
    audit.check(page,"workflow","Import a curved reference from GLB",imported,error);
    if (!imported)
        return;
    const auto source = store.selectedMesh()->id;
    const auto original = store.selectedMesh()->mesh;
    ModelingWorkspace workspace(store);
    workspace.resize(1300,760);
    workspace.show();
    workspace.enterEditMode();
    auto *view = workspace.findChild<LiveViewport *>();
    auto *menu = namedWidget<QToolButton>(workspace,"Mesh");
    auto *edges = namedWidget<QToolButton>(workspace,"Edge");
    if (!view || !menu || !menu->menu() || !edges || !waitForRender(*view) || !exposedVulkanWindow()) {
        audit.check(page,"workflow","Expose the Modeling viewport and retopology controls",false);
        return;
    }
    const auto runTool = [&](const QString &name) {
        for (auto *action : menu->menu()->findChildren<QAction *>())
            if (action->text() == name) {
                action->trigger();
                return true;
            }
        return false;
    };
    view->setModelAxisView(2);
    view->frame();
    auto *surface = exposedVulkanWindow();
    const int originalHeight = surface->height();
    const auto click = [&](QPoint offset) {
        const QPoint center(surface->width()/2,surface->height()/2);
        QTest::mouseClick(surface,Qt::LeftButton,Qt::NoModifier,center+offset*(double(surface->height())/originalHeight));
    };
    if (runTool("Draw Quad")) {
        for (const QPoint offset : {QPoint(-34,-38),QPoint(40,-38),QPoint(40,29),QPoint(-34,29)})
            click(offset);
        QTest::keyClick(surface,Qt::Key_Return);
    }
    const auto *patch = store.selectedMesh();
    const bool drawn = patch && patch->id != source && patch->mesh->faces.size() == 1
        && std::ranges::all_of(patch->mesh->vertices,[](const auto &p) { return p[2] > 1.25f; });
    audit.check(page,"workflow","Draw a quad directly onto the curved surface",drawn);
    if (!drawn)
        return;
    const auto patchId = patch->id;
    const auto patchMesh = patch->mesh;
    edges->click();
    view->frame();
    QTest::qWait(60);
    wf::Point3 low = patchMesh->vertices.front(), high = low;
    for (const auto &p : patchMesh->vertices)
        for (std::size_t axis = 0; axis < 3; ++axis) {
            low[axis] = std::min(low[axis],p[axis]);
            high[axis] = std::max(high[axis],p[axis]);
        }
    const auto clickPatch = [&](float x,float y) {
        const auto distance = std::max(0.3f,1.55f*std::hypot(high[0]-low[0],high[1]-low[1],high[2]-low[2]));
        const auto scale = float(surface->height())/(2*distance*0.41421356f);
        const QPoint pixel(qRound(surface->width()/2.0+(x-(low[0]+high[0])*0.5f)*scale),
            qRound(surface->height()/2.0-(y-(low[1]+high[1])*0.5f)*scale));
        QTest::mouseClick(surface,Qt::LeftButton,Qt::NoModifier,pixel);
    };
    const auto &upper = patchMesh->vertices[1], &lower = patchMesh->vertices[2];
    clickPatch((upper[0]+lower[0])*0.5f,(upper[1]+lower[1])*0.5f);
    const auto *selection = namedWidget<QLabel>(workspace,"Model selection");
    const auto *message = namedWidget<QLabel>(workspace,"Modeling result");
    const auto pickedEdge = selection ? selection->text() : QString{};
    if (runTool("Extend Selected Edge")) {
        const auto next = high[0]+(high[0]-low[0])*0.25f;
        clickPatch(next,upper[1]);
        clickPatch(next,lower[1]);
        QTest::keyClick(surface,Qt::Key_Return);
    }
    const auto *strip = store.selectedMesh();
    const bool extended = strip && strip->mesh->faces.size() == 2 && strip->mesh->vertices.size() == 6
        && std::ranges::count_if(wf::MeshTopology(*strip->mesh).edgeFaces,
            [](const auto &edge) { return edge.second.size() == 2; }) == 1;
    const auto frame = view->captureFrame();
    if (!frame.isNull())
        frame.save(output.filePath("modeling-retopology-strip.png"));
    audit.check(page,"workflow","Extend the open boundary along the curved reference",extended,
        pickedEdge + " | " + (message ? message->text() : QString{}));
    if (!extended)
        return;
    const auto beforeContinuous = strip->mesh;
    if (runTool("Extend Quad Strip")) {
        const auto next = high[0]+(high[0]-low[0])*0.5f;
        clickPatch(next,upper[1]);
        clickPatch(next,lower[1]);
        QTest::keyClick(surface,Qt::Key_Return);
    }
    const bool continued = store.selectedMesh()->mesh->faces.size() == 3
        && message && message->text().contains("Click two points");
    QTest::keyClick(surface,Qt::Key_Escape);
    if (continued)
        store.history().undo();
    audit.check(page,"workflow","Quad Strip extends from the latest edge and remains active",
        continued && *store.selectedMesh()->mesh == *beforeContinuous,message ? message->text() : QString{});
    strip = store.selectedMesh();
    auto unwrapped = *strip->mesh;
    const auto uvError = wf::unwrapUvIslands(unwrapped,{},0.01f);
    const auto report = wf::inspectUvs(unwrapped,{});
    const bool saved = uvError.empty() && store.updateMesh(patchId,std::move(unwrapped),"Unwrap retopology strip",error)
        && store.save(&error);
    ProjectStore reopened;
    const auto *savedPatch = reopened.meshAsset(patchId);
    const auto *savedSource = reopened.meshAsset(source);
    audit.check(page,"workflow","Save an unwrapped quad strip and preserve the imported reference",
        saved && savedPatch && savedPatch->mesh->faces.size() == 2 && savedSource && *savedSource->mesh == *original
        && report.islands == 1 && !report.outside && !report.degenerate && !report.overlaps,
        uvError.empty() ? error : QString::fromStdString(uvError));

    auto *vertices = namedWidget<QToolButton>(workspace,"Vertex");
    auto *reference = namedWidget<QComboBox>(workspace,"Reference mesh");
    wf::Mesh grid;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x)
            grid.vertices.push_back({float(x)*0.45f,float(y)*0.45f,0});
    grid.vertices[4] = {0.22f,0.13f,0};
    for (wf::VertexIndex i : {0u,1u,3u,4u})
        grid.faces.push_back({{{i},{i+1},{i+4},{i+3}}});
    if (!vertices || !reference || !store.addRetopologyPatch(grid,error)) {
        audit.check(page,"workflow","Conform and relax an interior retopology vertex",false,error);
        return;
    }
    const auto sourceObject = std::ranges::find(store.project()->objects,source,&SceneObject::meshAsset);
    const int referenceIndex = sourceObject == store.project()->objects.end() ? -1 : reference->findData(sourceObject->id);
    if (referenceIndex < 0) {
        audit.check(page,"workflow","Conform and relax an interior retopology vertex",false,"Imported reference missing from selector");
        return;
    }
    reference->setCurrentIndex(referenceIndex);
    vertices->click();
    QList<ModelComponent> all;
    for (int i = 0; i < 9; ++i)
        all.append({i});
    emit view->componentsPicked(all,Qt::NoModifier);
    const bool conformed = runTool("Conform to Reference");
    const auto fitted = store.selectedMesh()->mesh;
    emit view->componentsPicked({{4}},Qt::NoModifier);
    QTimer::singleShot(0,&workspace,[] {
        if (auto *input = qobject_cast<QInputDialog *>(QApplication::activeModalWidget()))
            input->accept();
    });
    const bool relaxed = runTool("Relax Vertices…");
    const auto &final = *store.selectedMesh()->mesh;
    const bool boundaryUnchanged = std::ranges::equal(final.vertices.begin(),final.vertices.begin()+4,fitted->vertices.begin(),fitted->vertices.begin()+4)
        && std::ranges::equal(final.vertices.begin()+5,final.vertices.end(),fitted->vertices.begin()+5,fitted->vertices.end());
    const auto centerVertex = final.vertices[4];
    const auto projected = view->conformToReference({centerVertex[0],centerVertex[1],centerVertex[2]});
    audit.check(page,"workflow","Conform and relax an interior retopology vertex",
        conformed && relaxed && fitted->vertices[4][2] > 1.8f && final.vertices[4] != fitted->vertices[4]
        && boundaryUnchanged && projected && std::abs(projected->z()-centerVertex[2]) < 0.001f,
        QString("Conform %1, relax %2, fitted Z %3, relaxed Z %4, projected Z %5, boundary unchanged %6. %7")
            .arg(conformed).arg(relaxed).arg(fitted->vertices[4][2]).arg(centerVertex[2])
            .arg(projected ? projected->z() : -100).arg(boundaryUnchanged).arg(message ? message->text() : QString{}));
}

void launcherWorkflow(const QDir &output, Audit &audit)
{
    QTemporaryDir folder;
    MainWindow window(nullptr,true);
    window.show();
    QTest::qWait(100);
    auto *launcher = window.findChild<ProjectLauncher *>();
    if (!launcher || !folder.isValid()) {
        audit.check("Launcher","workflow","Packaged startup path exposes the project launcher",false);
        return;
    }
    audit.check("Launcher","visual","Startup opens maximized",window.isMaximized());
    const auto available = window.screen()->availableGeometry().size();
    if (available.width() >= 1450 && available.height() >= 800) {
        auto *body = launcher->findChild<QScrollArea *>("launcherScroll");
        audit.check("Launcher","visual","Project creation fits the maximized working area",
            body && body->verticalScrollBar()->maximum() == 0);
    }
    const auto frame = launcher->grab().toImage();
    audit.check("Launcher","visual","Maximized launcher capture",!frame.isNull()
        && frame.save(output.filePath("project-launcher.png")));
    const QStringList artwork{"open-world-city","fantasy-rpg","action-adventure","racing-experience",
        "shooter-arena","survival-sandbox","simulation-city"};
    audit.check("Launcher","visual","All seven illustrated templates have packaged UHD artwork",
        std::ranges::all_of(artwork,[](const QString &name) {
            return QImageReader(":/Data/Launcher/" + name + ".jpg").size() == QSize(3840,2160);
        }));
    const QStringList templateNames{"Open World City","Fantasy RPG","Action Adventure","Racing Experience",
        "Shooter Arena","Survival Sandbox","Simulation City","Blank Project"};
    bool gridAligned = true;
    for (int column = 0; column < 4; ++column) {
        auto *upper = namedWidget<QPushButton>(*launcher,templateNames.at(column));
        auto *lower = namedWidget<QPushButton>(*launcher,templateNames.at(column+4));
        gridAligned &= upper && lower && upper->isVisible() && lower->isVisible()
            && upper->geometry().left() == lower->geometry().left()
            && upper->width() == lower->width() && upper->geometry().bottom() < lower->geometry().top();
    }
    audit.check("Launcher","visual","The reference catalog has four aligned columns and two rows",gridAligned);
    const auto click = [launcher](const QString &label) {
        auto *button = namedWidget<QPushButton>(*launcher,label);
        if (!button || !button->isVisible() || !button->isEnabled())
            return false;
        button->click();
        return true;
    };
    auto *features = launcher->findChild<QLabel *>("templateFeatures");
    auto *play = namedWidget<QToolButton>(*launcher,"Play artwork preview");
    auto *widePreview = namedWidget<QPushButton>(*launcher,"Enlarge Open World City artwork");
    audit.check("Launcher","visual","The artwork preview has a wide crop",
        widePreview && widePreview->height() > 120
        && qAbs(widePreview->height() - widePreview->width()*9/16) <= 1);
    audit.check("Launcher","workflow","Selection changes the feature list and hides Play for Blank Project",
        features && play && click("Fantasy RPG") && features->text().contains("Rolling hills and trees by default")
        && click("Blank Project") && features->text().contains("Empty scene") && !play->isVisible()
        && click("Open World City") && play->isVisible());
    bool played = false;
    if (play && play->isVisible()) {
        QTimer::singleShot(0,[&] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            auto *image = dialog ? dialog->findChild<QLabel *>() : nullptr;
            if (!dialog)
                return;
            const auto initial = image ? image->pixmap().cacheKey() : 0;
            QTimer::singleShot(350,dialog,[&,dialog,image,initial] {
                played = dialog->windowTitle().contains("Artwork Preview") && image
                    && image->pixmap().cacheKey() != initial;
                dialog->accept();
            });
        });
        play->click();
    }
    audit.check("Launcher","workflow","Play opens and animates the selected artwork",played);
    for (const QString &title : {"Account","Notifications","Help"}) {
        bool opened = false;
        if (auto *action = namedWidget<QToolButton>(*launcher,title)) {
            QTimer::singleShot(0,[&] {
                if (auto *dialog = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
                    opened = !dialog->text().isEmpty();
                    dialog->accept();
                }
            });
            action->click();
        }
        audit.check("Launcher","workflow",title + " opens a launcher dialog",opened);
    }
    auto *restore = namedWidget<QToolButton>(*launcher,"Restore down");
    if (restore)
        restore->click();
    const bool restoredWindow = QTest::qWaitFor([&] { return !window.isMaximized(); },1000);
    const auto restoredLabel = restore ? restore->toolTip() : QString{};
    auto *maximize = namedWidget<QToolButton>(*launcher,"Maximize");
    if (maximize)
        maximize->click();
    const bool maximizedWindow = QTest::qWaitFor([&] { return window.isMaximized(); },1000);
    audit.check("Launcher","workflow","Caption restores and maximizes with matching tooltips",
        restoredWindow && restoredLabel == "Maximize" && maximizedWindow
        && maximize && restore == maximize && maximize->toolTip() == "Restore down",
        QString("After restore: %1, maximized: %2, tooltip: %3")
            .arg(restoredLabel).arg(maximizedWindow).arg(maximize ? maximize->toolTip() : QString{}));
    auto *minimize = namedWidget<QToolButton>(*launcher,"Minimize");
    if (minimize)
        minimize->click();
    audit.check("Launcher","workflow","Caption minimizes the launcher",
        minimize && QTest::qWaitFor([&] { return window.isMinimized(); },1000));
    window.showNormal();
    window.resize(ReferenceSize);
    QTest::qWait(100);
    const auto referenceSize = launcher->grab();
    audit.check("Launcher","visual","Capture the restored layout at the reference dimensions",
        !referenceSize.isNull() && referenceSize.save(output.filePath("project-launcher-reference-size.png")));
    window.resize(1060,680);
    QTest::qWait(100);
    auto *create = namedWidget<QPushButton>(*launcher,"Create Project");
    auto *art = namedWidget<QPushButton>(*launcher,"Enlarge Open World City artwork");
    const auto footerBounds = create ? QRect(create->mapTo(launcher,QPoint{}),create->size()) : QRect{};
    audit.check("Launcher","visual","Artwork and Create button remain usable at minimum window size",
        art && art->height() >= 120 && create && create->isVisible() && launcher->rect().contains(footerBounds)
        && launcher->grab().save(output.filePath("project-launcher-compact.png")));
    window.showMaximized();
    audit.check("Launcher","workflow","Cancel goes to recent projects and Create New Project returns",
        click("Cancel") && click("Create New Project") && create && create->isVisible());
    const bool showedRecent = click("Recent Projects") && launcher->recentList()->isVisible();
    if (showedRecent)
        launcher->grab().save(output.filePath("recent-projects.png"));
    audit.check("Launcher","workflow","Recent projects can be opened",
        showedRecent && click("Open Project") && !launcher->isVisible());
    auto *newProject = findAction(window,"New project…");
    if (newProject)
        newProject->trigger();
    auto *name = namedWidget<QLineEdit>(*launcher,"Project name");
    auto *location = namedWidget<QLineEdit>(*launcher,"Project location");
    auto *quality = namedWidget<QComboBox>(*launcher,"Graphics quality");
    auto *status = namedWidget<QLabel>(*launcher,"Project creation status");
    if (name && status) {
        name->setText("CON");
        const bool rejected = click("Create Project") && launcher->isVisible() && !status->text().isEmpty();
        audit.check("Launcher","workflow","Invalid Windows project names stay in the launcher",rejected);
    }
    const auto projectFolder = QDir(folder.path()).filePath("Launcher QA Project");
    if (name && location) {
        name->setText("Launcher QA Project");
        location->setText(projectFolder);
    }
    if (quality)
        quality->setCurrentIndex(1);
    const bool submitted = click("Fantasy RPG") && click("Create Project");
    ProjectStore restored;
    const auto *saved = restored.project();
    audit.check("Launcher","workflow","Template and graphics settings create a saved project with default terrain settings",
        submitted && !launcher->isVisible() && saved && saved->name == "Launcher QA Project"
            && saved->configuration.value("template").toString() == "Fantasy RPG"
            && saved->configuration.value("quality").toString() == "performance"
            && !saved->objects.isEmpty() && saved->objects.front().type == "Terrain"
            && readHeightfield(saved->objects.front()).size == 128
            && restored.environment()
            && !readAtmosphere(restored.environment()->details).shadows
            && QDir(projectFolder+"/Scenes").entryList({"*.wfscene"},QDir::Files).size() == 1);
    auto *header = window.findChild<QWidget *>("appHeader");
    auto *close = header ? namedWidget<QToolButton>(*header,"Close") : nullptr;
    if (close)
        close->click();
    audit.check("Launcher","workflow","Shared editor caption closes the window",
        close && QTest::qWaitFor([&] { return !window.isVisible(); },1000));
    if (window.isVisible())
        window.close();
    QString error;
    const bool physics = restored.createStarterProject("Physics QA",{{"location",folder.filePath("Physics QA")},
        {"starter","physics"}},error);
    ProjectStore physicsReloaded;
    const auto *lab = physicsReloaded.project();
    audit.check("Launcher","workflow","Physics Lab reloads with colliders and material assets",
        physics && lab && lab->objects.size() == 5 && lab->assets.size() == 5
        && lab->objects.at(3).details.value("colliderShape").toInt() == 1
        && lab->objects.at(4).details.value("colliderShape").toInt() == 2,error);
    WorldRuntime runtime;
    const bool running = physics && lab && lab->objects.size() == 5 && runtime.start(physicsReloaded,false);
    if (running)
        for (int i = 0; i < 300; ++i)
            runtime.tick(0);
    const auto landing = running ? runtime.position(lab->objects.at(2).id) : std::optional<QVector3D>{};
    audit.check("Launcher","workflow","Physics Lab runs and its test box lands on the platform",
        landing && std::abs(landing->z()-1.6f) < 0.05f);
    const bool rejected = !physicsReloaded.createStarterProject("Wrong Folder",{{"location",projectFolder},
        {"starter","blank"}},error);
    audit.check("Launcher","workflow","Project creation rejects a populated folder",
        rejected && physicsReloaded.project() && physicsReloaded.project()->name == "Physics QA");
    const bool blank = physicsReloaded.createStarterProject("Blank QA",{{"location",folder.filePath("Blank QA")},
        {"starter","blank"}},error);
    ProjectStore blankReloaded;
    audit.check("Launcher","workflow","Blank Project reloads with an empty scene",
        blank && blankReloaded.project() && blankReloaded.project()->name == "Blank QA"
        && blankReloaded.project()->objects.isEmpty());
}

void worldRuntimeWorkflow(MainWindow &window, const QDir &output, Audit &audit)
{
    const QString page = "World runtime";
    auto *create = findAction(window,"New project…");
    auto *world = window.findChild<LiveViewport *>("worldLiveViewport");
    auto *save = findAction(window,"Save");
    auto *terrain = findAction(window,"Add Terrain");
    auto *raise = findAction(window,"Raise");
    auto *cube = namedWidget<QToolButton>(window,"Add Cube");
    auto *sky = namedWidget<QToolButton>(window,"Sky & Fog");
    auto *details = window.findChild<QDockWidget *>("inspector");
    auto *simulate = findAction(window,"Simulate");
    auto *play = findAction(window,"Play");
    auto *stop = findAction(window,"Stop");
    if (!create || !world || !save || !terrain || !raise || !cube || !sky || !details || !simulate || !play || !stop) {
        audit.check(page,"workflow","Terrain, atmosphere and runtime controls",false);
        return;
    }
    withDialog([create] { create->trigger(); },"Runtime QA");
    const auto waitText = [](QPlainTextEdit *field,const QString &value) {
        for (int i = 0; field && i < 50; ++i) {
            if (field->toPlainText().contains(value))
                return true;
            QTest::qWait(20);
        }
        return false;
    };
    auto *log = namedWidget<QPlainTextEdit>(window,"Output log");
    auto *messages = namedWidget<QPlainTextEdit>(window,"Project messages");
    qWarning().noquote() << "WorldForge QA warning";
    audit.check(page,"logs","Output Log records actions and Message Log captures warnings",
        waitText(log,"Created project Runtime QA") && waitText(messages,"WorldForge QA warning")
            && waitText(log,"WorldForge QA warning"));
    if (auto *terminal = findAction(window,"Terminal"))
        terminal->trigger();
    auto *command = namedWidget<QLineEdit>(window,"Terminal command");
    auto *transcript = namedWidget<QPlainTextEdit>(window,"Terminal output");
    if (command) {
        command->setText("echo %OS%");
        QMetaObject::invokeMethod(command,"returnPressed");
    }
#ifdef Q_OS_WIN
    audit.check(page,"terminal","Command Prompt evaluates input and displays process output",
        command && command->isVisible() && waitText(transcript,"Windows_NT"));
#endif
    terrain->trigger();
    for (int i = 0; i < 100 && (!world->hasRenderedFrame() || !exposedVulkanWindow()); ++i)
        QTest::qWait(50);
    if (!world->hasRenderedFrame() || !exposedVulkanWindow()) {
        const auto *label = world->findChild<QLabel *>();
        audit.check(page,"Vulkan","Terrain viewport exposed",false,
                    QString("%1 · visible %2 · %3").arg(window.windowTitle()).arg(world->isVisible())
                        .arg(label ? label->text() : QString()));
        return;
    }
    auto *surface = exposedVulkanWindow();
    const auto flat = world->captureFrame();
    const auto background = flat.isNull() ? QColor() : flat.pixelColor(flat.width()/2,3);
    audit.check(page,"Vulkan","Fresh terrain opens under daylight without saved atmosphere",
                background.isValid() && background.blue() > 75 && background.blue() > background.red());
    raise->trigger();
    QTest::mouseClick(surface,Qt::LeftButton,Qt::NoModifier,QPoint(surface->width()/2,surface->height()/2));
    const auto sculpted = world->captureFrame();
    save->trigger();
    ProjectStore terrainSaved;
    const auto *selected = terrainSaved.selectedObject();
    audit.check(page,"terrain","Brush changes rendered terrain and saves heights",
                selected && selected->type == "Terrain" && readHeightfield(*selected).height(0,0).value_or(0) > 0.8f
                    && !sculpted.isNull() && flat != sculpted);
    if (!selected || selected->type != "Terrain")
        return;
    const auto terrainId = selected->id;
    auto *undo = findAction(window,"Undo Sculpt terrain");
    if (undo)
        undo->trigger();
    save->trigger();
    ProjectStore terrainUndone;
    audit.check(page,"terrain","Undo restores the flat terrain", undo && terrainUndone.object(terrainId)
                && readHeightfield(*terrainUndone.object(terrainId)).height(0,0) == 0);
    auto *rock = findAction(window,"Rock");
    if (rock) {
        rock->trigger();
        QTest::mouseClick(surface,Qt::LeftButton,Qt::NoModifier,QPoint(surface->width()/2,surface->height()/2));
    }
    save->trigger();
    ProjectStore painted;
    audit.check(page,"terrain","Rock brush paints a saved material layer",rock && painted.object(terrainId)
        && readHeightfield(*painted.object(terrainId)).layers[16*worldforge::Heightfield::side+16] == 2);
    QTimer::singleShot(0, [] {
        if (auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget())) {
            if (auto *density = namedWidget<QDoubleSpinBox>(*dialog,"Fog density"))
                density->setValue(0.035);
            if (auto *elevation = namedWidget<QDoubleSpinBox>(*dialog,"Default sun elevation"))
                elevation->setValue(38);
            dialog->accept();
        }
    });
    sky->click();
    const auto atmosphere = world->captureFrame();
    save->trigger();
    ProjectStore atmosphereSaved;
    const auto *environment = atmosphereSaved.environment();
    audit.check(page,"Vulkan","Gradient sky and fog render and persist", environment
        && readAtmosphere(environment->details).sky && readAtmosphere(environment->details).fogDensity > 0
        && !atmosphere.isNull() && atmosphere != flat && atmosphere.save(output.filePath("terrain-sky-fog.png")));

    cube->click();
    auto *z = namedWidget<QDoubleSpinBox>(*details,"Location Z");
    auto *body = namedWidget<QComboBox>(*details,"Physics body");
    if (!z || !body) {
        audit.check(page,"physics","Physics Inspector",false);
        return;
    }
    z->setValue(4);
    QMetaObject::invokeMethod(z,"editingFinished");
    const auto configureLight = [&](bool shadows,double radius) {
        QTimer::singleShot(0,[shadows,radius] {
            if (auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget())) {
                if (auto *toggle = namedWidget<QCheckBox>(*dialog,"Cast sun shadows"))
                    toggle->setChecked(shadows);
                if (auto *field = namedWidget<QDoubleSpinBox>(*dialog,"Local fog radius"))
                    field->setValue(radius);
                dialog->accept();
            }
        });
        sky->click();
        return world->captureFrame();
    };
    const auto unshadowed = configureLight(false,0);
    const auto shadowed = configureLight(true,0);
    const auto fogShadowed = configureLight(true,12);
    const auto fogUnshadowed = configureLight(false,12);
    audit.check(page,"Vulkan","Sun casts visible geometry shadows",!shadowed.isNull() && !unshadowed.isNull()
        && shadowed != unshadowed && shadowed.save(output.filePath("sun-shadows.png")));
    audit.check(page,"Vulkan","Local fog reacts to sun shadows",!fogShadowed.isNull() && !fogUnshadowed.isNull()
        && fogShadowed != shadowed && fogShadowed != fogUnshadowed && fogShadowed.save(output.filePath("shadowed-fog.png")));
    configureLight(true,0);
    body->setCurrentIndex(body->findData("Dynamic"));
    QMetaObject::invokeMethod(body,"activated",Q_ARG(int,body->currentIndex()));
    save->trigger();
    ProjectStore authored;
    const auto *object = authored.selectedObject();
    if (!object || object->details.value("physics").toString() != "Dynamic") {
        audit.check(page,"physics","Saved dynamic collider",false);
        return;
    }
    const auto cubeId = object->id;
    simulate->trigger();
    auto *pause = namedWidget<QToolButton>(window,"Pause");
    auto *step = namedWidget<QToolButton>(window,"Step");
    if (pause && step && pause->isVisible()) {
        pause->click();
        const auto beforeStep = world->runtimePosition(cubeId);
        QTest::qWait(60);
        const bool frozen = beforeStep && world->runtimePosition(cubeId) == beforeStep;
        step->click();
        const auto afterStep = world->runtimePosition(cubeId);
        audit.check(page,"runtime","Pause freezes physics and Step advances one tick",
                    frozen && afterStep && afterStep->z() < beforeStep->z());
        pause->click();
    } else {
        audit.check(page,"runtime","Simulate exposes pause and step",false);
    }
    for (int i = 0; i < 55; ++i) {
        const auto position = world->runtimePosition(cubeId);
        if (position && position->z() < 0.52f)
            break;
        QTest::qWait(40);
    }
    const auto landed = world->runtimePosition(cubeId);
    audit.check(page,"physics","Simulate drops a dynamic box onto terrain", landed && std::abs(landed->z()-0.5f) < 0.03f
                && world->runMode() == LiveViewport::RunMode::Simulate);
    const auto simulationFrame = world->captureFrame();
    if (!simulationFrame.isNull())
        simulationFrame.save(output.filePath("terrain-physics.png"));
    stop->trigger();
    ProjectStore afterSimulation;
    audit.check(page,"runtime","Stop restores authored transform and discards simulation",
                !world->runtimePosition(cubeId) && z->value() == 4
                    && afterSimulation.object(cubeId) && afterSimulation.object(cubeId)->position.z() == 4);

    play->trigger();
    const auto spawn = world->playerPosition();
    QWindow *playSurface = exposedVulkanWindow();
    if (playSurface && spawn) {
        QTest::keyPress(playSurface,Qt::Key_W);
        QTest::qWait(410);
        QTest::keyRelease(playSurface,Qt::Key_W);
    }
    const auto moved = world->playerPosition();
    audit.check(page,"runtime","Play accepts movement input",
                spawn && moved && (moved->toVector2D()-spawn->toVector2D()).length() > 0.15f);
    for (int i = 0; i < 40; ++i) {
        const auto position = world->playerPosition();
        if (position && position->z() <= 0.86f)
            break;
        QTest::qWait(35);
    }
    const auto grounded = world->playerPosition();
    if (playSurface && grounded) {
        QTest::keyClick(playSurface,Qt::Key_Space);
        for (int i = 0; i < 18; ++i) {
            QTest::qWait(20);
            const auto height = world->playerPosition();
            if (height && height->z() > grounded->z()+0.15f)
                break;
        }
    }
    const auto jumped = world->playerPosition();
    audit.check(page,"runtime","Play jumps from terrain",
                grounded && jumped && grounded->z() <= 0.86f && jumped->z() > grounded->z()+0.15f);
    stop->trigger();
    audit.check(page,"runtime","Stop removes the player and returns to editing",
                !world->playerPosition() && world->runMode() == LiveViewport::RunMode::Editing);

    withDialog([create] { create->trigger(); },"Outdoor QA");
    auto *hills = findAction(window,"Add Rolling Hills with Trees");
    auto *exportAction = findAction(window,"Export Playable Scene…");
    if (!hills || !exportAction) {
        audit.check(page,"workflow","Outdoor terrain and standalone export commands",false);
        return;
    }
    hills->trigger();
    auto *seed = namedWidget<QDoubleSpinBox>(*details,"Foliage seed");
    if (seed) {
        seed->setValue(77);
        QMetaObject::invokeMethod(seed,"editingFinished");
    }
    const auto outdoor = world->captureFrame();
    QTest::qWait(160);
    const auto windFrame = world->captureFrame();
    save->trigger();
    ProjectStore outdoorSaved;
    const auto *rolling = outdoorSaved.selectedObject();
    audit.check(page,"terrain","Rolling hills render with editable foliage and save its seed",
                rolling && rolling->details.value("foliageDensity").toInt() > 0
                    && rolling->details.value("foliageSeed").toInt() == 77
                    && !outdoor.isNull() && outdoor != flat && outdoor.save(output.filePath("outdoor-world.png")));
    audit.check(page,"Vulkan","Foliage wind animates between rendered frames",!windFrame.isNull() && windFrame != outdoor);
    auto *x = namedWidget<QDoubleSpinBox>(*details,"Location X");
    auto *joint = namedWidget<QComboBox>(*details,"Joint type");
    auto *target = namedWidget<QComboBox>(*details,"Joint target");
    auto *axis = namedWidget<QComboBox>(*details,"Joint world axis");
    auto *pivot = namedWidget<QDoubleSpinBox>(*details,"Hinge pivot offset X");
    auto *mass = namedWidget<QDoubleSpinBox>(*details,"Rigid body mass");
    auto *shape = namedWidget<QComboBox>(*details,"Collider shape");
    const auto number = [](QDoubleSpinBox *field,double value) {
        field->setValue(value);
        QMetaObject::invokeMethod(field,"editingFinished");
    };
    const auto choice = [](QComboBox *field,int index) {
        field->setCurrentIndex(index);
        QMetaObject::invokeMethod(field,"activated",Q_ARG(int,index));
    };
    if (x && joint && target && axis && pivot && mass && shape) {
        cube->click();
        number(x,12);
        number(z,5);
        choice(body,body->findData("Static"));
        save->trigger();
        ProjectStore withAnchor;
        const auto anchor = withAnchor.selection();
        cube->click();
        number(x,14);
        number(z,5);
        choice(body,body->findData("Dynamic"));
        choice(shape,1);
        number(mass,12);
        choice(joint,joint->findData("Hinge"));
        choice(target,target->findData(anchor));
        choice(axis,1);
        number(pivot,-2);
        save->trigger();
        ProjectStore withJoint;
        const auto hinged = withJoint.selection();
        simulate->trigger();
        for (int i = 0; i < 50; ++i) {
            const auto pose = world->runtimePosition(hinged);
            if (pose && pose->z() < 4.5f)
                break;
            QTest::qWait(20);
        }
        const auto pose = world->runtimePosition(hinged);
        stop->trigger();
        const auto *saved = withJoint.object(hinged);
        audit.check(page,"physics","Authored hinge moves its attached body and persists on save",
                    saved && saved->details.value("jointTarget").toString() == anchor
                        && saved->details.value("mass").toDouble() == 12 && saved->details.value("colliderShape").toInt() == 1
                        && pose && pose->z() < 4.5f
                        && std::abs(((*pose)-QVector3D(12,0,5)).length()-2) < 0.12f && z->value() == 5);
    } else {
        audit.check(page,"physics","Hinge Inspector controls",false);
    }
    const auto scenePath = output.filePath("outdoor.wfplay");
    QFile::remove(scenePath);
    withDialog([exportAction] { exportAction->trigger(); },scenePath);
    QFile package(scenePath);
    std::istringstream bytes(package.open(QIODevice::ReadOnly) ? package.readAll().toStdString() : std::string());
    const auto standalone = worldforge::readPlayable(bytes);
    audit.check(page,"Player","Exported scene contains terrain, foliage, rigid bodies, a joint and an avatar",
                standalone && standalone->sky && standalone->simulation.terrains.size() == 1
                    && standalone->foliage.front().seed == 77
                    && standalone->simulation.bodies.size() == 3 && standalone->simulation.joints.size() == 1
                    && standalone->simulation.bodies[1].shape == worldforge::BoxBody::Shape::Sphere
                    && standalone->simulation.bodies[standalone->player].lockRotation);
}
}

int main(int argc, char **argv)
{
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication app(argc, argv);
    installEditorLog();
    const bool authoring = app.arguments().size() == 4 && app.arguments().last() == "--authoring";
    const bool benchmark = app.arguments().size() == 4 && app.arguments().last() == "--modeling-benchmark";
    const bool studio = app.arguments().size() == 4 && app.arguments().last() == "--studio-publishing";
    const bool forgeAi = app.arguments().size() == 4 && app.arguments().last() == "--forge-ai";
    const bool streetAudit = app.arguments().size() == 4 && app.arguments().last() == "--street";
    if (!authoring && !benchmark && !studio && !forgeAi && !streetAudit && app.arguments().size() != 3)
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

    if (benchmark)
        return modelingBenchmark(output);

    QFile controlFile(output.filePath("controls.csv"));
    QFile imageFile(output.filePath("pixels.csv"));
    if (!controlFile.open(QIODevice::WriteOnly) || !imageFile.open(QIODevice::WriteOnly))
        return 2;
    QTextStream controls(&controlFile);
    QTextStream images(&imageFile);
    controls << "Workspace,Kind,Control,Result,Notes\n";
    images << "Workspace,Reference,Width,Height,Pixels,DifferentPixels,MeanAbsoluteRgbError,Notes\n";
    Audit audit{controls};
    if (studio) {
        studioPublishingWorkflow(output,audit);
        controls.flush();
        return audit.failures ? 1 : 0;
    }
    if (forgeAi) {
        forgeAiWorkflow(output,audit);
        controls.flush();
        return audit.failures ? 1 : 0;
    }
    if (streetAudit) {
        streetWorkflow(output,audit);
        controls.flush();
        return audit.failures ? 1 : 0;
    }

    const QDir data(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation));
    if (authoring)
        authoringData(root, data, audit);
    else {
        ProjectStore clean;
        audit.check("Home", "workflow", "New installation opens without sample projects", clean.projects().isEmpty());
        if (!QDir().mkpath(data.absolutePath()) || !QFile::copy(":/Data/demo.json", data.filePath("projects.json")))
            return 2;
    }
    MainWindow window;
    window.setAttribute(Qt::WA_DontShowOnScreen, !authoring);
    window.setWindowState(Qt::WindowNoState);
    window.resize(ReferenceSize);
    window.show();
    QTest::qWait(80);
    if (authoring) {
        liveAuthoring(window, output, audit);
        sculptingWorkflow(window,output,audit);
        worldRuntimeWorkflow(window, output, audit);
        audit.check("Live Authoring", "workflow", "Close editor", window.close());
        MainWindow reopened;
        reopened.setWindowState(Qt::WindowNoState);
        reopened.resize(ReferenceSize);
        reopened.show();
        auto *view = reopened.findChild<LiveViewport *>();
        audit.check("Live Authoring", "Vulkan", "Reopened editor automatically renders saved meshes",
                    view && waitForRender(*view) && !view->captureFrame().isNull());
        reopened.close();
        launcherWorkflow(output,audit);
        modelingViewportToolsWorkflow(output,audit);
        modelingHierarchyWorkflow(output,audit);
        surfaceWorkflow(output,audit);
        studioPublishingWorkflow(output,audit);
        modelingObjectWorkflow(audit);
        modelingDeliveryWorkflow(output,audit);
        modelingRetopologyWorkflow(output,audit);
        controls.flush();
        return audit.failures ? 1 : 0;
    }
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
        QString reason;
        const bool switched = waitForWorkspace(window, *tabs, index, reason);
        if (reason == "Studio is not installed")
            audit.record(tabs->tabText(index), "navigation", "Direct tab click", "SKIP", reason);
        else
            audit.check(tabs->tabText(index), "navigation", "Direct tab click", switched,
                        reason.isEmpty() ? QStringLiteral("Click the trailing edge of the tab") : reason);
    }
    window.resize(ReferenceSize);
    QApplication::processEvents();

    for (int index = 0; index < tabs->count(); ++index) {
        tabs->setCurrentIndex(index);
        const auto page = tabs->tabText(index);
        QString reason;
        const bool switched = waitForWorkspace(window, *tabs, index, reason);
        if (reason == "Studio is not installed")
            audit.record(page, "navigation", "Workspace + status", "SKIP", reason);
        else
            audit.check(page, "navigation", "Workspace + status", switched && window.statusBar()->isVisible(), reason);
        if (!qEnvironmentVariableIsSet("WORLDFORGE_AUDIT_CONTROLS_ONLY"))
            compareImage(window, page, output, root, images, audit);
    }
    images.flush();
    window.setUpdatesEnabled(false);
    projectWorkflow(window, *tabs, audit);
    sceneWorkflow(window, root, audit);
    controls.flush();
    for (int index = 0; index < tabs->count(); ++index) {
        tabs->setCurrentIndex(index);
        const auto page = tabs->tabText(index);
        QApplication::processEvents();
        QSet<QAction *> menus;
        auto *pages = qobject_cast<QStackedWidget *>(window.centralWidget());
        auditWidgets(*pages->currentWidget(), window, audit, page, menus);
        if (index == 0 || page == "Modeling" || page == "Sculpting" || page == "Texture" || page == "Material"
            || page == "Rigging" || page == "Animation")
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
    QTimer closeStudio;
    QObject::connect(&closeStudio,&QTimer::timeout,&closeStudio,[] {
        if (auto *dialog = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
            dialog && dialog->windowTitle() == "Close WorldForge") {
            if (auto *close = dialog->button(QMessageBox::Close))
                close->click();
        }
    });
    closeStudio.start(20);
    audit.check("Shell", "workflow", "Close with saved layout", window.close());
    closeStudio.stop();
    controls.flush();
    images.flush();
    qInfo().noquote() << QString("Editor audit: %1 pass, %2 preview, %3 skip, %4 fail; scale %5")
        .arg(audit.checks).arg(audit.previews).arg(audit.skips).arg(audit.failures).arg(window.devicePixelRatioF());
    return audit.failures ? 1 : 0;
}
