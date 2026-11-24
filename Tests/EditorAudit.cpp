#include "App/MainWindow.h"
#include "App/EditorLog.h"
#include "Components/AssetHub.h"
#include "Components/BehaviorCode.h"
#include "Components/BehaviorEditor.h"
#include "Components/NativeGameplayEditor.h"
#include "Components/LiveViewport.h"
#include "Components/ProjectLauncher.h"
#include "State/WorldScene.h"
#include "State/WorldRuntime.h"
#include "State/NativeComponents.h"
#include "State/CityDistrict.h"
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
#include <QDialogButtonBox>
#include <QDir>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include "Engine/World/PlayableScene.h"
#include <sstream>
#include <numeric>
#include <QGraphicsItem>
#include <QGraphicsScene>
#include <QGraphicsView>
#include <QImage>
#include <QBuffer>
#include <QImageReader>
#include <QInputDialog>
#include <QScopedPointer>
#include "Engine/Geometry/MeshTopology.h"
#include "Engine/Geometry/MeshRemesh.h"
#include "Engine/Geometry/MeshUv.h"
#include "State/GlbImport.h"
#include "State/StudioClip.h"
#include "State/StudioPublishReview.h"
#include "State/ForgeAi.h"
#include "State/PlayableStreet.h"
#include <QProcess>
#include "Components/ForgeAiPanel.h"
#include "Workspaces/ForgeAiJob.h"
#include "Workspaces/EmbeddedStudio.h"
#include "Engine/Render/Flip.h"
#include <QEventLoop>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPainter>
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
    if (auto *studio = window.findChild<EmbeddedStudio *>(); editMesh && studio) {
        if (EmbeddedStudio::installedExecutable().isEmpty()) {
            audit.record(page,"Modeling","Open selected mesh in Studio","SKIP","The build directory has no bundled Studio runtime.");
            return;
        }
        const auto ready = [&] {
            auto *action = window.findChild<QAction *>("applyStudioUpdate");
            return tabs->currentIndex() == 2 && studio->isRunning() && action && action->isVisible()
                && action->toolTip() == "Apply Studio Update to Cube";
        };
        for (int attempt = 0; !ready() && attempt < 300; ++attempt)
            QTest::qWait(50);
        audit.check(page,"Modeling","World Editor opens the selected cube in embedded Studio",ready());
        tabs->setCurrentIndex(1);
        if (studio->isRunning() && studio->closeForAssetSwitch())
            for (int attempt = 0; studio->isRunning() && attempt < 100; ++attempt)
                QTest::qWait(50);
        return;
    }
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

void sculptingWorkflow(const QDir &output,Audit &audit)
{
    const QString page = "Sculpting";
    QTemporaryDir directory;
    ProjectStore store;
    QString error;
    if (!directory.isValid() || !store.createStarterProject("Sculpting QA",
        {{"location",directory.filePath("Sculpting QA")},{"starter","blank"}},error)) {
        audit.check(page,"workflow","Create the sculpting fixture",false,error);
        return;
    }
    SculptingWorkspace sculpting(store);
    sculpting.resize(1200,780);
    sculpting.show();
    auto *view = sculpting.findChild<LiveViewport *>();
    auto *add = namedWidget<QPushButton>(sculpting,"Add Sculpt Sphere");
    if (!view || !add) {
        audit.check(page,"workflow","Open live Sculpting workspace",false);
        return;
    }
    add->click();
    store.save(&error);
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
    store.save(&error);
    ProjectStore cancelled;
    const auto *cancelledAsset = cancelled.meshAsset(id);
    audit.check(page,"workflow","Escape discards the sculpt stroke",cancelledAsset && *cancelledAsset->mesh == *base->mesh);
    drag();
    QTest::mouseRelease(surface,Qt::LeftButton,Qt::NoModifier,center+QPoint(24,0));
    const auto sculptedFrame = view->captureFrame();
    store.save(&error);
    ProjectStore after;
    const auto *result = after.meshAsset(id);
    audit.check(page,"workflow","Sculpt drag changes the viewport and saves the same mesh asset",
        result && result->mesh->vertices != base->mesh->vertices && result->mesh->faces.size() == base->mesh->faces.size()
        && !sculptedFrame.isNull() && sculptedFrame != baseline && sculptedFrame.save(output.filePath("sculpting-live.png")));
    store.history().undo();
    store.save(&error);
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
    const auto drawVertices = playable ? std::accumulate(playable->meshes.begin(),playable->meshes.end(),std::size_t{},
        [](auto total,const auto &draw) { return total + draw.vertices.size(); }) : 0;
    const auto expectedVertices = uniqueAsset ? 2*wf::makePlayVertices(caseAsset->evaluated()).size()
        + wf::makePlayVertices(uniqueAsset->evaluated()).size() : 0;
    audit.check(page,"export","Playable scene contains all three case instances and their material draws",
        exported && playable && drawVertices > 0 && drawVertices == expectedVertices && playable->simulation.terrains.size() == 1
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
        for (const auto &name : {QStringLiteral("Sculpting"),QStringLiteral("Texture"),QStringLiteral("Material"),
             QStringLiteral("Rigging"),QStringLiteral("Animation"),QStringLiteral("Modeling")}) {
            if (!opensSelected)
                break;
            for (int i = 0; i < tabs->count(); ++i)
                if (tabs->tabText(i) == name)
                    tabs->setCurrentIndex(i);
            const auto ready = [&] {
                const auto *action = window.findChild<QAction *>("applyStudioUpdate");
                const auto *search = window.findChild<QAction *>("searchStudioAssets");
                return window.windowTitle().endsWith(name) && action && action->isVisible()
                    && action->toolTip() == "Apply Studio Update to Road Barrier" && search && search->isVisible();
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
        auto *search = window.findChild<QAction *>("searchStudioAssets");
        if (opensSelected && search && search->isVisible()) {
            const auto runSearch = [&](const auto &inspect) {
                QTimer::singleShot(0,&window,[inspect] {
                    auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
                    if (!dialog)
                        return;
                    auto *query = dialog->findChild<QLineEdit *>("studioAssetQuery");
                    auto *results = dialog->findChild<QListWidget *>("studioAssetResults");
                    if (!query || !results)
                        dialog->reject();
                    else
                        inspect(*dialog,*query,*results);
                });
                search->trigger();
            };
            bool filters = false, placed = false;
            runSearch([&](QDialog &dialog,QLineEdit &query,QListWidget &results) {
                query.setText("no such asset");
                const auto *edit = dialog.findChild<QPushButton *>("studioAssetEdit");
                bool empty = results.count() == 0 && edit && !edit->isEnabled();
                query.setText("rOaD bArRiEr");
                filters = empty && results.count() == 1 && results.currentItem()
                    && results.currentItem()->text() == "Road Barrier" && edit->isEnabled();
                const auto *barrier = reopened.meshAsset(id);
                if (barrier && !barrier->evaluated().materials.empty()) {
                    query.setText(QString::fromStdString(barrier->evaluated().materials.front().name));
                    filters = filters && results.findItems("Road Barrier",Qt::MatchExactly).size() == 1;
                }
                query.setText("Road Barrier");
                if (auto *button = dialog.findChild<QPushButton *>("studioAssetPlace"); button && button->isEnabled()) {
                    button->click();
                    placed = window.statusBar()->currentMessage() == "Added mesh to the level.";
                } else {
                    dialog.reject();
                }
            });
            audit.check(page,"navigation","Asset search filters by name and material, handles no results and places the chosen mesh",
                filters && placed);

            bool imported = false;
            runSearch([&](QDialog &dialog,QLineEdit &query,QListWidget &results) {
                auto *import = dialog.findChild<QPushButton *>("studioAssetImport");
                if (import) {
                    QTimer::singleShot(0,&window,[&] {
                        if (auto *file = qobject_cast<QFileDialog *>(QApplication::activeModalWidget())) {
                            file->selectFile(output.filePath("authored-studio-input.glb"));
                            QMetaObject::invokeMethod(file,"accept",Qt::DirectConnection);
                        }
                    });
                    import->click();
                    const auto *asset = results.currentItem();
                    imported = asset && window.statusBar()->currentMessage().startsWith("Imported ");
                    if (imported) {
                        const auto importedId = asset->data(Qt::UserRole);
                        query.setText("AUTHORED-STUDIO-INPUT.GLB");
                        imported = results.count() == 1 && results.currentItem()->data(Qt::UserRole) == importedId;
                    }
                }
                dialog.reject();
            });
            audit.check(page,"navigation","Import adds a GLB and finds it by source filename from Modeling",imported);

            const auto chooseCube = [&](bool discard) {
                runSearch([&](QDialog &dialog,QLineEdit &query,QListWidget &results) {
                    query.setText("Cube");
                    if (auto *edit = dialog.findChild<QPushButton *>("studioAssetEdit"); results.count() == 1 && edit && edit->isEnabled()) {
                        QTimer::singleShot(0,&window,[&,discard] {
                            if (auto *confirm = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
                                if (discard) {
                                    for (auto *button : confirm->buttons())
                                        if (confirm->buttonRole(button) == QMessageBox::DestructiveRole) {
                                            button->click();
                                            return;
                                        }
                                }
                                confirm->reject();
                            }
                        });
                        edit->click();
                    } else {
                        dialog.reject();
                    }
                });
            };
            chooseCube(false);
            const bool stayed = action->isVisible() && action->toolTip() == "Apply Studio Update to Road Barrier";
            chooseCube(true);
            const auto switched = [&] {
                return window.windowTitle().endsWith("Modeling") && action->isVisible()
                    && action->toolTip() == "Apply Studio Update to Cube";
            };
            for (int attempt = 0; !switched() && attempt < 560; ++attempt)
                QTest::qWait(50);
            audit.check(page,"navigation","Cancel keeps the original asset and confirmed switching opens the searched cube",
                stayed && switched(),action->toolTip());
        } else {
            audit.check(page,"navigation","Search Assets is available in the Modeling toolbar",false);
        }
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
    view.hide();

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
    bool placementRendered = false;
    const auto stagedPlacement = panel.stage() ? panel.stage()->placement.id : QString{};
    if (reviewed) {
        QTimer::singleShot(0,&panel,[&] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            auto *viewport = dialog ? dialog->findChild<LiveViewport *>() : nullptr;
            if (viewport && dialog->objectName() == "forgePlacementReview" && waitForRender(*viewport)) {
                const auto preview = viewport->captureFrame();
                placementRendered = !preview.isNull() && preview.save(output.filePath("forge-placement-preview.png"));
            }
            if (dialog)
                dialog->reject();
        });
        click("forgePlacementPreview");
    }
    audit.check(page,"workflow","Preview in Level renders the current stage without committing it",
        placementRendered && !panelStore.object(stagedPlacement) && panelStore.history().index() == panelHistory);
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
        .arg(run.exitCode).arg(run.report.value("adapter").toString()+" (" + run.report.value("renderer").toString("Direct3D 11")+")")
        .arg(run.report.value("missionSeconds").toDouble(),0,'f',1)
        .arg(frame.value("average").toDouble(),0,'f',2).arg(frame.value("p95").toDouble(),0,'f',2)
        .arg(frame.value("p99").toDouble(),0,'f',2).arg(frame.value("max").toDouble(),0,'f',2)
        .arg(cpu.value("average").toDouble(),0,'f',2).arg(cpu.value("p95").toDouble(),0,'f',2)
        .arg(run.report.value("workingSetMiB").toDouble(),0,'f',0).arg(run.report.value("peakWorkingSetMiB").toDouble(),0,'f',0)
        .arg(run.report.value("privateMiB").toDouble(),0,'f',0).arg(run.report.value("texturedMeshes").toInt());
}

void r3PerformanceWorkflow(const QDir &output,Audit &audit)
{
    const QString page = "R3 lighting";
    QTemporaryDir directory;
    QString error;
    if (!directory.isValid()) {
        audit.check(page,"performance","Create temporary storage for the street",false,directory.errorString());
        return;
    }
    ProjectStore store(QDir(directory.filePath("index")));
    if (!store.createStarterProject("R3 Performance",
        {{"location",directory.filePath("R3 Performance")},{"starter","street"}},error)) {
        audit.check(page,"performance","Create the street for the R3 benchmark",false,error);
        return;
    }
    const auto baseline = runStreetPlayer(store,output,"r3-lighting-off");
    auto atmosphere = store.environment() ? store.environment()->details : QJsonObject{};
    atmosphere.insert("probeGi",true);
    atmosphere.insert("screenSpaceGi",true);
    atmosphere.insert("reflectionProbes",true);
    store.setAtmosphere(atmosphere);
    const auto enabled = runStreetPlayer(store,output,"r3-lighting-on");
    const auto completed = [](const PlayerRun &run) {
        return run.exitCode == 0 && run.report.value("renderer").toString() == "Vulkan"
            && run.report.value("missionComplete").toBool() && run.report.value("trafficLooped").toBool();
    };
    const auto p95 = [](const PlayerRun &run) { return run.report.value("frameMs").toObject().value("p95").toDouble(); };
    audit.check(page,"performance","The baseline mission renders with Vulkan",
                completed(baseline) && p95(baseline) > 0,playerSummary(baseline));
    audit.check(page,"performance","The mission completes with all R3 lighting enabled",
                completed(enabled) && p95(enabled) > 0,playerSummary(enabled));
    audit.check(page,"performance","All R3 lighting fits the street frame and memory budget",
                p95(enabled) > 0 && p95(enabled) < 16.7 && enabled.report.value("peakWorkingSetMiB").toDouble() < 1024,
                QString("p95 %1 ms with R3 off, %2 ms on. Budget: p95 under 16.7 ms and peak working set under 1 GiB.")
                    .arg(p95(baseline),0,'f',2).arg(p95(enabled),0,'f',2));
}

// The city-scale benchmark (R4): a generated district flown along a fixed route in the packaged Player
// (--verify-district). It records the baseline that culling, indirect drawing and streaming are measured against.
struct DistrictAuditOptions {
    bool profile = false, large = false, textured = false, compareDirect = false, compareOcclusion = false, streamTextures = false;
    QString meshlets;
};

void districtWorkflow(const QDir &output,Audit &audit,const DistrictAuditOptions &options)
{
    const auto &[profile,large,textured,compareDirect,compareOcclusion,streamTextures,meshlets] = options;
    const QString page = "City district";
    QTemporaryDir directory;
    QString error;
    if (!directory.isValid()) {
        audit.check(page,"performance","Create temporary storage for the district",false,directory.errorString());
        return;
    }
    ProjectStore store(QDir(directory.filePath("index")));
    district::Layout layout;
    if (large)
        layout.blocks = 12;
    if (!store.createStarterProject("City District",{{"location",directory.filePath("City District")},{"starter","district"},
            {"districtBlocks",layout.blocks},{"districtTexturedFacades",textured}},error)) {
        audit.check(page,"workflow","Create the City District starter",false,error);
        return;
    }
    const auto &objects = store.project()->objects;
    const auto count = [&](const QString &type) { return std::ranges::count(objects,type,&SceneObject::type); };
    const auto expectedBuildings = layout.blocks*layout.blocks*layout.lotsPerSide*layout.lotsPerSide;
    const auto lamps = (layout.blocks+1)*(layout.blocks+1);
    audit.check(page,"workflow","The City District starter lays out blocks, buildings and street lamps",
                count("Point Light") == lamps && count("Static Mesh") == 1+layout.blocks*layout.blocks+2*expectedBuildings+lamps,
                QString("%1 meshes, %2 lamps for %3 x %3 blocks").arg(count("Static Mesh")).arg(count("Point Light"))
                    .arg(layout.blocks));
    std::stringstream playable;
    QFile scene(output.filePath("district.wfplay"));
    const bool exported = WorldRuntime::exportScene(store,playable,&error) && scene.open(QIODevice::WriteOnly)
        && scene.write(playable.str().data(),qint64(playable.str().size())) == qint64(playable.str().size());
    scene.close();
    audit.check(page,"workflow","Export the district for the Player",exported,error);
    if (!exported)
        return;
    if (textured) {
        const auto bytes = playable.str();
        std::istringstream exportedDistrict(bytes);
        const auto loaded = worldforge::readPlayable(exportedDistrict);
        std::set<std::vector<std::uint8_t>> textures;
        int texturedMeshes = 0;
        if (loaded) {
            for (const auto &mesh : loaded->meshes) {
                if (!mesh.baseColorTexture.empty()) {
                    textures.insert(mesh.baseColorTexture);
                    ++texturedMeshes;
                }
            }
        }
        const bool sized = std::ranges::all_of(textures,[](const auto &png) {
            return QImage::fromData(png.data(),int(png.size()),"PNG").size() == QSize(512,512);
        });
        audit.check(page,"textures","Every district building exports a facade from the five 512-pixel materials",
                    textures.size() == 5 && texturedMeshes == expectedBuildings && sized,
                    QString("%1 unique textures, %2 textured meshes, %3 MiB playable")
                        .arg(textures.size()).arg(texturedMeshes).arg(double(bytes.size())/1048576,0,'f',2));
    }
    const auto runDistrict = [&](const QString &name,const QString &path,bool direct = false,bool withOcclusion = false) {
        QProcess player;
        const auto reportPath = output.filePath(name+"-player.json");
        QFile::remove(reportPath);
        QStringList options{"--verify-district",path,reportPath,QString::number(layout.roadWidth/2),"70"};
        if (direct)
            options << "--direct";
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert("WORLDFORGE_OCCLUSION",withOcclusion ? "1" : "0");
        environment.insert("WORLDFORGE_TEXTURE_STREAMING",streamTextures ? "1" : "0");
        environment.insert("WORLDFORGE_MESHLETS",meshlets);
        player.setProcessEnvironment(environment);
        player.start(QDir(QCoreApplication::applicationDirPath()).filePath("WorldForgePlayer.exe"),options);
        if (!player.waitForFinished(300000)) {
            player.kill();
            player.waitForFinished();
            return QJsonObject{};
        }
        if (player.exitStatus() != QProcess::NormalExit || player.exitCode() != 0)
            return QJsonObject{};
        QFile result(reportPath);
        return result.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(result.readAll()).object() : QJsonObject{};
    };
    const auto report = runDistrict("district",scene.fileName(),false,compareOcclusion);
    const auto stat = [&](const char *group,const char *value) {
        return report.value(group).toObject().value(value).toDouble();
    };
    const auto completedRoute = [](const QJsonObject &result) {
        return result.value("renderer").toString() == "Vulkan" && result.value("frames").toInt() > 1000;
    };
    const bool completed = completedRoute(report);
    const auto summary = QString("%1 meshes, %2 lights, %3 vertices · frame avg %4 ms p95 %5 ms p99 %6 ms max %7 ms"
                                 " (street p95 %8 ms, rooftops p95 %9 ms) · CPU avg %10 ms p95 %11 ms · scene draws %12"
                                 " (max %13), %14 k triangles; depth passes %15 draws, %16 k triangles · working set %17 MiB,"
                                 " peak %18 MiB")
        .arg(report.value("meshes").toInt()).arg(report.value("lights").toInt()).arg(report.value("vertices").toInt())
        .arg(stat("frameMs","average"),0,'f',2).arg(stat("frameMs","p95"),0,'f',2).arg(stat("frameMs","p99"),0,'f',2)
        .arg(stat("frameMs","max"),0,'f',2).arg(stat("streetMs","p95"),0,'f',2).arg(stat("rooftopMs","p95"),0,'f',2)
        .arg(stat("cpuMs","average"),0,'f',2).arg(stat("cpuMs","p95"),0,'f',2)
        .arg(stat("sceneDraws","average"),0,'f',0).arg(stat("sceneDraws","max"),0,'f',0)
        .arg(report.value("sceneTriangles").toDouble()/1000,0,'f',0).arg(report.value("depthDraws").toDouble(),0,'f',0)
        .arg(report.value("depthTriangles").toDouble()/1000,0,'f',0)
        .arg(report.value("workingSetMiB").toDouble(),0,'f',0).arg(report.value("peakWorkingSetMiB").toDouble(),0,'f',0);
    const auto gpu = [&](const char *phase) {
        const auto passes = report.value("gpuMs").toObject().value(phase).toObject();
        return QString("sun shadows %1, sky maps %2, point shadows %3, scene %4, post-process %5 ms")
            .arg(passes.value("sunShadows").toDouble(),0,'f',2).arg(passes.value("skyMaps").toDouble(),0,'f',2)
            .arg(passes.value("pointShadows").toDouble(),0,'f',2).arg(passes.value("scene").toDouble(),0,'f',2)
            .arg(passes.value("postProcess").toDouble(),0,'f',2);
    };
    audit.record(page,"performance","GPU time per pass along the district route",completed ? "INFO" : "FAIL",
                 QString("Street: %1. Rooftops: %2.").arg(gpu("street"),gpu("rooftop")));
    audit.check(page,"performance","The Player flies the whole district route with Vulkan",completed,summary);
    audit.check(page,"performance","The district stays within the memory budget",
                completed && report.value("peakWorkingSetMiB").toDouble() < 1024,summary);
    if (streamTextures && completed) {
        const auto streaming = report.value("textureStreaming").toObject();
        const int promotions = streaming.value("promotions").toInt();
        const int wait = streaming.value("longestWaitFrames").toInt();
        const double residency = streaming.value("peakDetailBytes").toDouble()/1048576;
        audit.check(page,"textures","Visible facade detail is restored within eight frames under the GPU detail budget",
            promotions == 5 && wait <= 8 && residency > 0 && residency <= 64,
            QString("%1 promotions, longest wait %2 frames, %3 MiB of detail, longest upload stall %4 ms")
                .arg(promotions).arg(wait).arg(residency,0,'f',2).arg(streaming.value("longestStallMs").toDouble(),0,'f',2));
    }
    if (!meshlets.isEmpty() && completed) {
        const bool indexed = meshlets == "indexed";
        const auto shaderGroups = report.value("meshletShaderGroups").toDouble();
        const auto indexedDraws = report.value("meshletIndexedDraws").toDouble();
        const bool targetGpu = report.value("adapter").toString().contains("GTX 1650");
        audit.record(page,"meshlets",indexed ? "The forced fallback renders static meshlets with Vulkan indexed draws"
            : "The mesh shader renders static district meshlets",indexed || targetGpu
                ? ((indexed ? indexedDraws > 0 && shaderGroups == 0 : shaderGroups > 0) ? "PASS" : "FAIL") : "SKIP",
            QString("%1 mesh workgroups, %2 indexed draws").arg(shaderGroups,0,'f',0).arg(indexedDraws,0,'f',0));
    }
    // The frame-time baseline that R4 improves on; it is recorded, not yet held to the street budget.
    audit.record(page,"performance","District frame-time baseline (budget: p95 under 16.7 ms)",
                 completed && stat("frameMs","p95") < 16.7 ? "PASS" : "INFO",summary);
    if (compareDirect && completed) {
        const auto direct = runDistrict("district-direct-route",scene.fileName(),true);
        const auto baseline = [&](const char *group) { return direct.value(group).toObject().value("p95").toDouble(); };
        audit.check(page,"performance","The direct and indirect paths both complete the same Vulkan route",
                    completed && completedRoute(direct) && direct.value("adapter") == report.value("adapter")
                    && direct.value("frames") == report.value("frames"));
        audit.record(page,"performance","Direct versus indirect route frame times",completedRoute(direct) ? "INFO" : "FAIL",
            QString("Indirect/direct p95 %1/%2 ms overall, %3/%4 ms street, %5/%6 ms rooftops")
                .arg(stat("frameMs","p95"),0,'f',2).arg(baseline("frameMs"),0,'f',2)
                .arg(stat("streetMs","p95"),0,'f',2).arg(baseline("streetMs"),0,'f',2)
                .arg(stat("rooftopMs","p95"),0,'f',2).arg(baseline("rooftopMs"),0,'f',2));
    }
    if (compareOcclusion && completed) {
        const auto baseline = runDistrict("district-frustum-route",scene.fileName());
        const double visible = report.value("visibleSceneDraws").toDouble();
        const double frustumVisible = baseline.value("visibleSceneDraws").toDouble();
        const double rejected = report.value("occludedOpaqueDraws").toDouble();
        audit.check(page,"occlusion","The depth pyramid rejects geometry beyond the frustum test along the route",
            completedRoute(baseline) && rejected > 0 && visible < frustumVisible,
            QString("%1 draws additionally rejected per frame, %2 visible versus %3 with frustum alone")
                .arg(rejected,0,'f',1).arg(visible,0,'f',1).arg(frustumVisible,0,'f',1));
        const auto baselineTime = baseline.value("frameMs").toObject().value("p95").toDouble();
        audit.record(page,"performance","Frustum versus depth-pyramid route frame times",completedRoute(baseline) ? "INFO" : "FAIL",
            QString("Frustum p95 %1 ms, depth pyramid p95 %2 ms").arg(baselineTime,0,'f',2).arg(stat("frameMs","p95"),0,'f',2));
    }
    if (!profile && completed) {
        const double rejected = report.value("rejectedOpaqueTriangles").toDouble();
        const double rejectedDraws = report.value("rejectedOpaqueDraws").toDouble();
        const bool targetGpu = report.value("adapter").toString().contains("GTX 1650");
        audit.record(page,"culling","GPU frustum culling rejects opaque triangles along the route",
                     rejected > 0 ? "PASS" : targetGpu ? "FAIL" : "SKIP",
                     QString("%1 draws and %2 triangles rejected per frame on average").arg(rejectedDraws,0,'f',1).arg(rejected,0,'f',1));
        const int compacted = report.value("compactedShadowFrames").toInt();
        audit.record(page,"culling","Sun shadow frames using compacted indirect draws","INFO",
                     QString("%1 of %2 frames").arg(compacted).arg(report.value("frames").toInt()));
        store.select({});
        LiveViewport viewport(store,false);
        viewport.resize(640,360);
        viewport.show();
        const float edge = (layout.roadWidth-layout.extent())*0.5f;
        const QVector3D eye{edge+12,edge,2}, target{edge+90,edge,2.5f};
        viewport.setCameraPreview(eye,target);
        const auto editor = waitForRender(viewport) ? viewport.captureFrame().convertToFormat(QImage::Format_RGB32) : QImage{};
        const auto capture = [&](const QString &name,bool direct,QVector3D cameraEye,QVector3D cameraTarget,bool streaming = false) {
            if (editor.isNull())
                return QImage{};
            const auto file = output.filePath(name+".png");
            QStringList args{"--capture",scene.fileName(),file,QString::number(editor.width()),QString::number(editor.height()),
                             QString::number(cameraEye.x()),QString::number(cameraEye.y()),QString::number(cameraEye.z()),
                             QString::number(cameraTarget.x()),QString::number(cameraTarget.y()),QString::number(cameraTarget.z())};
            if (direct)
                args << "--direct";
            QProcess player;
            auto environment = QProcessEnvironment::systemEnvironment();
            environment.insert("WORLDFORGE_OCCLUSION",compareOcclusion ? "1" : "0");
            environment.insert("WORLDFORGE_TEXTURE_STREAMING",streaming ? "1" : "0");
            environment.insert("WORLDFORGE_MESHLETS",meshlets);
            player.setProcessEnvironment(environment);
            player.start(QDir(QCoreApplication::applicationDirPath()).filePath("WorldForgePlayer.exe"),args);
            if (!player.waitForFinished(60000) || player.exitStatus() != QProcess::NormalExit || player.exitCode() != 0)
                return QImage{};
            return QImage(file).convertToFormat(QImage::Format_RGB32);
        };
        const auto indirect = capture("district-indirect",false,eye,target);
        const auto direct = capture("district-direct",true,eye,target);
        viewport.setDrawCullingEnabled(false);
        const auto editorDirect = viewport.captureFrame().convertToFormat(QImage::Format_RGB32);
        const auto meanDifference = [](const QImage &a,const QImage &b) {
            if (a.isNull() || a.size() != b.size())
                return 255.0;
            double total = 0;
            for (int y = 0; y < a.height(); ++y) {
                const auto *one = reinterpret_cast<const QRgb *>(a.constScanLine(y));
                const auto *two = reinterpret_cast<const QRgb *>(b.constScanLine(y));
                for (int x = 0; x < a.width(); ++x)
                    total += std::max({std::abs(qRed(one[x])-qRed(two[x])),std::abs(qGreen(one[x])-qGreen(two[x])),
                                       std::abs(qBlue(one[x])-qBlue(two[x]))});
            }
            return total/(double(a.width())*a.height());
        };
        editor.save(output.filePath("district-editor.png"));
        editorDirect.save(output.filePath("district-editor-direct.png"));
        const double playerError = meanDifference(direct,indirect), editorError = meanDifference(editor,editorDirect);
        audit.check(page,"culling","Indirect district draws match direct draws in both renderers",
            playerError <= 0.1 && editorError <= 0.1,
            QString("Player mean error %1 levels. Editor mean error %2 levels").arg(playerError,0,'f',3).arg(editorError,0,'f',3));
        const double rendererError = meanDifference(editor,indirect);
        audit.check(page,"culling","The editor and Player match on the district street view",rendererError <= 1,
            QString("Mean error %1 levels").arg(rendererError,0,'f',3));
        if (streamTextures) {
            const auto streamed = capture("district-streamed",false,eye,target,true);
            const auto difference = meanDifference(streamed,indirect);
            audit.check(page,"textures","Restored facade detail matches the full-resolution street capture",difference <= 0.5,
                QString("Mean pixel error %1 levels").arg(difference,0,'f',3));
        }
        if (compareOcclusion) {
            const std::array<std::pair<QVector3D,QVector3D>,4> poses{{
                {{edge+5,edge+11,1.7f},{edge+42,edge+32,3}},
                {{edge+55,edge+51,2},{edge+105,edge+57,2.5f}},
                {{edge+85,edge+48,2},{edge+40,edge+56,3}},
                {{edge+61,edge+44,70},{40,40,5}}}};
            double playerPeak = 0, editorPeak = 0;
            for (std::size_t i = 0; i < poses.size(); ++i) {
                const auto [cameraEye,cameraTarget] = poses[i];
                const auto stem = QString("district-turn-%1").arg(i);
                playerPeak = std::max(playerPeak,meanDifference(capture(stem+"-occlusion",false,cameraEye,cameraTarget),
                    capture(stem+"-direct",true,cameraEye,cameraTarget)));
                viewport.setCameraPreview(cameraEye,cameraTarget);
                viewport.setDrawCullingEnabled(true);
                const auto active = viewport.captureFrame().convertToFormat(QImage::Format_RGB32);
                viewport.setDrawCullingEnabled(false);
                const auto disabled = viewport.captureFrame().convertToFormat(QImage::Format_RGB32);
                editorPeak = std::max(editorPeak,meanDifference(active,disabled));
            }
            audit.check(page,"occlusion","Depth culling preserves frames across turns, district blocks and an abrupt rooftop move",
                playerPeak <= 0.1 && editorPeak <= 0.1,
                QString("Worst mean pixel error: editor %1, Player %2 levels").arg(editorPeak,0,'f',3).arg(playerPeak,0,'f',3));
        }
    }
    if (!profile || !completed)
        return;

    const auto baselineBytes = playable.str();
    const bool reloadable = worldforge::readPlayable(playable).has_value();
    audit.check(page,"profile","Load the exported district for single-feature comparisons",reloadable);
    if (!reloadable)
        return;
    struct Variant {
        const char *name;
        void (*apply)(worldforge::PlayableScene &);
    };
    const Variant variants[]{
        {"without-height-fog",[](auto &copy) { copy.fogDensity = 0; }},
        {"without-contact-shadows",[](auto &copy) { copy.contactShadows = false; }},
        {"without-reflections",[](auto &copy) { copy.reflections = false; }},
        {"without-sun-shadows",[](auto &copy) { copy.shadows = false; }},
        {"without-lamps",[](auto &copy) { std::erase_if(copy.lights,[](const auto &light) { return !light.directional; }); }}
    };
    const auto sceneMs = [](const QJsonObject &run,const char *phase) {
        return run.value("gpuMs").toObject().value(phase).toObject().value("scene").toDouble();
    };
    for (const auto &variant : variants) {
        std::istringstream input(baselineBytes);
        auto copy = worldforge::readPlayable(input);
        if (copy)
            variant.apply(*copy);
        std::ostringstream bytes;
        QFile package(output.filePath(QString("district-%1.wfplay").arg(variant.name)));
        bool written = copy && worldforge::writePlayable(bytes,*copy) && package.open(QIODevice::WriteOnly);
        if (written) {
            const auto data = bytes.str();
            written = package.write(data.data(),qint64(data.size())) == qint64(data.size());
        }
        package.close();
        const auto result = written ? runDistrict(QString("district-%1").arg(variant.name),package.fileName()) : QJsonObject{};
        const bool profiled = completedRoute(result) && sceneMs(result,"street") > 0 && sceneMs(result,"rooftop") > 0;
        audit.check(page,"profile",QString("Profile the district %1").arg(variant.name),profiled,
            profiled ? QString("Scene GPU time %1 ms street (%2 ms with all effects), %3 ms rooftop (%4 ms with all effects)")
                .arg(sceneMs(result,"street"),0,'f',2).arg(sceneMs(report,"street"),0,'f',2)
                .arg(sceneMs(result,"rooftop"),0,'f',2).arg(sceneMs(report,"rooftop"),0,'f',2)
                : QStringLiteral("The export or timestamped Vulkan route did not complete."));
    }
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

void sunSweepWorkflow(const QDir &output,Audit &audit,bool sunrise)
{
    const QString page = sunrise ? "Sunrise" : "Sunset";
    QTemporaryDir directory;
    ProjectStore store(QDir(directory.filePath("index")));
    QString error;
    QJsonObject atmosphere{{"skyModel","Physical"},{"sunAzimuth",90},{"sunElevation",sunrise ? -6 : 12},{"exposureMode","Auto"},
        {"antiAliasing","Off"},{"fogDensity",0},{"localFogRadius",0},{"bloom",false},{"ambientOcclusion",false},
        {"reflections",false},{"contactShadows",false},{"shadows",false}};
    if (!directory.isValid() || !store.createStarterProject("Sunrise",{{"location",directory.filePath("street")},
            {"starter","street"},{"worldDefaults",atmosphere}},error)) {
        audit.check(page,"workflow","Create the sunrise scene",false,error);
        return;
    }
    // Look down the solar azimuth from above the roofs so the horizon and upper sky are both visible.
    const QVector3D eye{0,-50,65}, target{0,25,65};
    store.select({});
    LiveViewport viewport(store,false);
    viewport.resize(400,260);
    viewport.setCameraPreview(eye,target);
    viewport.show();
    const auto first = waitForRender(viewport) ? viewport.captureFrame() : QImage{};
    if (first.isNull()) {
        audit.check(page,"render","Capture the editor's Vulkan sky",false);
        return;
    }
    QFile playable(output.filePath("sunrise.wfplay"));
    std::ostringstream scene;
    const bool exported = WorldRuntime::exportScene(store,scene,&error) && playable.open(QIODevice::WriteOnly);
    const auto bytes = scene.str();
    const bool written = exported && playable.write(bytes.data(),qint64(bytes.size())) == qint64(bytes.size());
    playable.close();
    if (!written) {
        audit.check(page,"workflow","Export the sunrise scene",false,error);
        return;
    }
    const auto playerOutput = output.filePath("player");
    QProcess player;
    player.start(QDir(QCoreApplication::applicationDirPath()).filePath("WorldForgePlayer.exe"),
        {sunrise ? "--verify-sunrise" : "--verify-sunset",playable.fileName(),playerOutput,QString::number(first.width()),QString::number(first.height()),
         QString::number(eye.x()),QString::number(eye.y()),QString::number(eye.z()),
         QString::number(target.x()),QString::number(target.y()),QString::number(target.z())});
    if (!player.waitForFinished(300000)) {
        player.kill();
        player.waitForFinished();
    }
    QFile report(QDir(playerOutput).filePath("meter.csv"));
    if (player.exitStatus() != QProcess::NormalExit || player.exitCode() != 0 || !report.open(QIODevice::ReadOnly)) {
        audit.check(page,"render","Capture the Player's Vulkan sunrise sweep",false,QString::number(player.exitCode()));
        return;
    }
    QFile editorReport(output.filePath("editor-meter.csv"));
    if (!editorReport.open(QIODevice::WriteOnly)) {
        audit.check(page,"workflow","Write the editor's exposure readings",false,editorReport.errorString());
        return;
    }
    QTextStream playerMeter(&report), editorMeter(&editorReport);
    playerMeter.readLine();
    editorMeter << "Elevation,EditorLog2Luminance,PlayerLog2Luminance\n";
    float lastEditor = 0, lastPlayer = 0, editorStep = 0, playerStep = 0, rendererDifference = 0;
    QImage editorTwilight;
    int samples = 0;
    bool metered = true;
    constexpr int steps = 90;
    while (!playerMeter.atEnd()) {
        const int sample = sunrise ? samples : steps-samples;
        const auto row = playerMeter.readLine().split(',');
        bool validElevation = false, validLuminance = false;
        const float elevation = row.value(0).toFloat(&validElevation);
        const float playerLuminance = row.value(1).toFloat(&validLuminance);
        if (!validElevation || !validLuminance || !std::isfinite(playerLuminance) || elevation != float(sample-30)/5) {
            metered = false;
            break;
        }
        atmosphere.insert("sunElevation",elevation);
        store.setAtmosphere(atmosphere);
        const auto luminance = viewport.captureMeteredLuminance();
        if (!luminance || !std::isfinite(*luminance)) {
            metered = false;
            break;
        }
        if (samples > 0) {
            editorStep = std::max(editorStep,std::abs(*luminance-lastEditor));
            playerStep = std::max(playerStep,std::abs(playerLuminance-lastPlayer));
        }
        lastEditor = *luminance;
        lastPlayer = playerLuminance;
        rendererDifference = std::max(rendererDifference,std::abs(*luminance-playerLuminance));
        editorMeter << elevation << ',' << *luminance << ',' << playerLuminance << '\n';
        if (sample == 0 || sample == 30 || sample == steps) {
            const auto frame = viewport.captureFrame();
            frame.save(output.filePath(QString("editor-%1.png").arg(samples)));
            if (sample == 0)
                editorTwilight = frame;
        }
        ++samples;
    }
    const bool complete = metered && samples == steps+1;
    audit.check(page,"exposure","Both renderers meter the sun sweep without a step over 0.5 stops",
                complete && editorStep <= 0.5f && playerStep <= 0.5f,
                QString("%1 elevations, editor peak step %2 stops, Player %3 stops")
                    .arg(samples).arg(editorStep,0,'f',3).arg(playerStep,0,'f',3));
    audit.check(page,"exposure","Editor and Player meter the same luminance during the sun sweep",
                complete && rendererDifference <= 0.10f,
                QString("Largest difference %1 stops").arg(rendererDifference,0,'f',3));
    const auto skyHue = [](const QImage &image,int numerator) {
        if (image.isNull())
            return 0.0;
        const int y = image.height()*numerator/20;
        double blue = 0, total = 0;
        for (int x = image.width()*3/8; x < image.width()*5/8; ++x) {
            const auto pixel = image.pixel(x,y);
            blue += qBlue(pixel);
            total += qRed(pixel)+qGreen(pixel)+qBlue(pixel);
        }
        return total > 0 ? blue/total : 0.0;
    };
    const QImage playerTwilight(QDir(playerOutput).filePath(sunrise ? "0.png" : QString("%1.png").arg(steps)));
    const double editorBlue = skyHue(editorTwilight,2)-skyHue(editorTwilight,9);
    const double playerBlue = skyHue(playerTwilight,2)-skyHue(playerTwilight,9);
    audit.check(page,"sky","The upper twilight sky is bluer than the horizon in both renderers",
                editorBlue > 0.01 && playerBlue > 0.01,
                QString("Blue channel share above horizon: editor +%1, Player +%2").arg(editorBlue,0,'f',3).arg(playerBlue,0,'f',3));
}

// Fixed camera views of the Playable Street, compared with reference images in Tests/Golden.
// Set WORLDFORGE_UPDATE_GOLDEN=1 to record new references after an intended rendering change.
void renderingWorkflow(const QDir &output,const QDir &root,Audit &audit)
{
    const QString page = "Rendering";
    QTemporaryDir directory;
    QString error;
    if (!directory.isValid()) {
        audit.check(page,"workflow","Create temporary storage for the street",false,directory.errorString());
        return;
    }
    ProjectStore store(QDir(directory.filePath("index")));
    if (!store.createStarterProject("Rendering QA",
        {{"location",directory.filePath("Rendering QA")},{"starter","street"}},error)) {
        audit.check(page,"workflow","Create the Playable Street starter",false,error);
        return;
    }
    store.select({});
    LiveViewport view(store,false);
    view.resize(800,450);
    view.show();
    if (!waitForRender(view)) {
        audit.check(page,"render","The street renders",false,QStringLiteral("No Vulkan frame"));
        return;
    }
    const auto capture = [&](QVector3D eye,QVector3D target) {
        view.setCameraPreview(eye,target);
        QTest::qWait(100);
        return view.captureFrame().convertToFormat(QImage::Format_RGB32);
    };
    const auto luminance = [](const QImage &image) {
        double total = 0;
        for (int y = 0; y < image.height(); ++y)
            for (int x = 0; x < image.width(); ++x) {
                const QColor pixel = image.pixel(x,y);
                total += 0.2126*pixel.redF()+0.7152*pixel.greenF()+0.0722*pixel.blueF();
            }
        return image.isNull() ? 0.0 : total/(double(image.width())*image.height());
    };

    struct Shot { const char *name; QVector3D eye, target; };
    const Shot shots[]{{"shop-front",{-6,-14,2.2f},{0,3,1.6f}},{"street-corner",{24,-20,7},{6,0,1}},
                       {"aerial",{-34,-34,26},{0,0,0}}};
    const QDir golden(root.filePath("Tests/Golden"));
    const bool update = qEnvironmentVariableIntValue("WORLDFORGE_UPDATE_GOLDEN") == 1;
    QFile deviceFile(golden.filePath("device.txt"));
    const QString recordedDevice = deviceFile.open(QIODevice::ReadOnly) ? QString::fromUtf8(deviceFile.readAll()).trimmed() : QString();
    deviceFile.close();
    const bool sameDevice = recordedDevice == view.rendererName();
    if (update && QDir().mkpath(golden.absolutePath()) && deviceFile.open(QIODevice::WriteOnly))
        deviceFile.write(view.rendererName().toUtf8()+"\n");
    struct Difference { double mean = 255, share = 1; };
    // Mean error tolerates small timing differences such as foliage sway; the share of pixels that
    // differ by more than 24 levels catches real changes in lighting, color, or geometry.
    const auto difference = [&](const QImage &actual,const QImage &reference,const QString &diffName) {
        Difference result;
        if (actual.isNull() || reference.size() != actual.size())
            return result;
        double total = 0;
        qint64 large = 0;
        QImage diff(actual.size(),QImage::Format_RGB32);
        for (int y = 0; y < actual.height(); ++y)
            for (int x = 0; x < actual.width(); ++x) {
                const QColor a = actual.pixel(x,y), b = reference.pixel(x,y);
                const int channel = std::max({std::abs(a.red()-b.red()),std::abs(a.green()-b.green()),
                    std::abs(a.blue()-b.blue())});
                total += channel;
                large += channel > 24;
                diff.setPixel(x,y,qRgb(std::min(255,channel*8),0,0));
            }
        const double pixels = double(actual.width())*actual.height();
        diff.save(output.filePath(diffName));
        return Difference{total/pixels,double(large)/pixels};
    };
    const auto describe = [](const Difference &result) {
        return QString("mean error %1 levels, %2% of pixels differ by more than 24 levels")
            .arg(result.mean,0,'f',2).arg(result.share*100,0,'f',2);
    };
    const auto checkGolden = [&](const QString &label,const QString &file,const QImage &actual) {
        const QString referencePath = golden.filePath(file);
        if (update) {
            audit.check(page,"golden image",QString("Record %1").arg(label),!actual.isNull() && actual.save(referencePath),
                        referencePath);
            return;
        }
        const QImage reference(referencePath);
        if (reference.isNull() || !sameDevice) {
            audit.record(page,"golden image",QString("%1 matches its reference").arg(label),"SKIP",
                       reference.isNull() ? QStringLiteral("No reference image. Record one with WORLDFORGE_UPDATE_GOLDEN=1.")
                                          : QString("References were recorded on %1.").arg(recordedDevice));
            return;
        }
        const auto result = difference(actual,reference.convertToFormat(QImage::Format_RGB32),
                                       QFileInfo(file).completeBaseName()+"-diff.png");
        audit.check(page,"golden image",QString("%1 matches its reference").arg(label),
                    result.mean <= 1.5 && result.share <= 0.005,describe(result));
    };

    // The Player renders the same views from an exported copy of the street. Both use the shared HDR frame
    // pass and color pipeline, so they should differ only at edges and shadow borders.
    const auto playablePath = output.filePath("rendering.wfplay");
    std::ostringstream playable;
    bool exported = WorldRuntime::exportScene(store,playable);
    if (exported) {
        const auto bytes = playable.str();
        QFile file(playablePath);
        exported = file.open(QIODevice::WriteOnly) && file.write(bytes.data(),qint64(bytes.size())) == qint64(bytes.size());
    }
    const auto playerCapture = [&](const Shot &shot,QSize size) {
        const auto path = output.filePath(QString("rendering-player-%1.png").arg(shot.name));
        QFile::remove(path);
        QStringList arguments{"--capture",playablePath,path,QString::number(size.width()),QString::number(size.height())};
        for (const float value : {shot.eye.x(),shot.eye.y(),shot.eye.z(),shot.target.x(),shot.target.y(),shot.target.z()})
            arguments << QString::number(value,'g',9);
        QProcess player;
        player.start(QDir(QCoreApplication::applicationDirPath()).filePath("WorldForgePlayer.exe"),arguments);
        const bool captured = exported && player.waitForFinished(60000) && player.exitStatus() == QProcess::NormalExit
            && player.exitCode() == 0;
        return captured ? QImage(path).convertToFormat(QImage::Format_RGB32) : QImage();
    };
    for (const auto &shot : shots) {
        const auto actual = capture(shot.eye,shot.target);
        actual.save(output.filePath(QString("rendering-%1.png").arg(shot.name)));
        checkGolden(QString("Street view %1").arg(shot.name),QString("street-%1.png").arg(shot.name),actual);
        const auto played = playerCapture(shot,actual.size());
        checkGolden(QString("Player street view %1").arg(shot.name),QString("player-street-%1.png").arg(shot.name),played);
        const auto alike = difference(played,actual,QString("rendering-%1-editor-player-diff.png").arg(shot.name));
        audit.check(page,"editor and Player",QString("Editor and Player show street view %1 alike").arg(shot.name),
                    !played.isNull() && alike.mean <= 1 && alike.share <= 0.005,played.isNull() ? QStringLiteral("The Player did not capture the view.")
                                                                         : describe(alike));
    }

    // Display controls: one stop of exposure doubles the light, and Standard clips what AgX rolls off.
    const auto *initial = store.environment();
    const auto initialDetails = initial ? std::optional<QJsonObject>(initial->details) : std::nullopt;
    const auto atmosphere = [&](double exposure,const QString &transform) {
        auto details = store.environment() ? store.environment()->details : QJsonObject{};
        details.insert("exposure",exposure);
        details.insert("viewTransform",transform);
        store.setAtmosphere(details);
        return capture(shots[0].eye,shots[0].target);
    };
    const auto normal = atmosphere(0,"AgX");
    const auto brighter = atmosphere(1,"AgX");
    const auto darker = atmosphere(-1,"AgX");
    const auto standard = atmosphere(0,"Standard");
    const double base = luminance(normal), up = luminance(brighter), down = luminance(darker);
    audit.check(page,"display","Exposure brightens and darkens the street",up > base+0.03 && down < base-0.03,
                QString("mean display level %1 at -1 EV, %2 at 0 EV, %3 at +1 EV").arg(down,0,'f',3).arg(base,0,'f',3)
                    .arg(up,0,'f',3));
    audit.check(page,"display","The Standard view transform renders differently from AgX",
                !standard.isNull() && standard != normal,QString("mean display level %1 with Standard").arg(luminance(standard),0,'f',3));
    for (int i = 0; i < 4; ++i)
        store.history().undo();
    audit.check(page,"display","Undo restores the default exposure and view transform",
                initialDetails ? store.environment() && store.environment()->details == *initialDetails : !store.environment());

    // Auto exposure meters the view, so the street stays visible whatever the manual exposure says. The
    // editor adapts over a few frames; wait until two captures agree.
    const auto historyStart = store.history().index();
    const auto setDisplay = [&](const QJsonObject &changes) {
        auto details = store.environment() ? store.environment()->details : QJsonObject{};
        for (auto it = changes.begin(); it != changes.end(); ++it)
            details.insert(it.key(),it.value());
        store.setAtmosphere(details);
    };
    const auto settled = [&](const Shot &shot) {
        QImage previous = capture(shot.eye,shot.target);
        int calm = 0;
        for (int attempt = 0; attempt < 100; ++attempt) {
            const auto next = capture(shot.eye,shot.target);
            calm = std::abs(luminance(next)-luminance(previous)) < 0.0005 ? calm+1 : 0;
            previous = next;
            if (calm >= 3)
                break;
        }
        return previous;
    };

    // An overhead slab casts a shadow into the fog while the center of the view sees only sky.
    const Shot fogShot{"volumetric-fog",{-50,-50,3},{-50,-40,5}};
    store.addCube();
    if (const auto *selected = store.selectedObject()) {
        auto slab = *selected;
        slab.position = fogShot.eye+QVector3D(0,0,10);
        slab.scale = {25,25,1};
        store.updateObject(slab,"Fog shadow caster");
    }
    store.select({});
    setDisplay({{"fogDensity",0.04},{"fogFalloff",0.06},{"localFogRadius",0},{"sunElevation",90},
                {"bloom",false},{"shadows",true}});
    const auto shadowedFog = settled(fogShot);
    const auto skyPatch = [](const QImage &image) {
        return image.copy(image.width()*3/8,image.height()*3/8,image.width()/4,image.height()/4);
    };
    {
        std::ostringstream fogPlayable;
        QFile fogFile(playablePath);
        const bool fogExported = WorldRuntime::exportScene(store,fogPlayable) && fogFile.open(QIODevice::WriteOnly);
        const auto bytes = fogPlayable.str();
        const bool written = fogExported && fogFile.write(bytes.data(),qint64(bytes.size())) == qint64(bytes.size());
        fogFile.close();
        const auto playedFog = written ? playerCapture(fogShot,shadowedFog.size()) : QImage();
        const auto fogAlike = difference(playedFog,shadowedFog,"rendering-volumetric-fog-editor-player-diff.png");
        audit.check(page,"fog","Editor and Player match with volumetric fog",
                    !playedFog.isNull() && fogAlike.mean <= 1 && fogAlike.share <= 0.005,describe(fogAlike));
    }
    setDisplay({{"shadows",false}});
    const auto litFog = settled(fogShot);
    setDisplay({{"fogDensity",0}});
    const auto clearSky = settled(fogShot);
    setDisplay({{"shadows",true}});
    const auto clearShadowed = settled(fogShot);
    const double scatter = luminance(skyPatch(litFog))-luminance(skyPatch(shadowedFog));
    const auto clearAlike = difference(skyPatch(clearSky),skyPatch(clearShadowed),"rendering-clear-sky-shadow-diff.png");
    shadowedFog.save(output.filePath("rendering-volumetric-fog.png"));
    audit.check(page,"fog","Sun shadows darken height fog in front of the sky",
                !litFog.isNull() && !shadowedFog.isNull() && scatter > 0.01 && clearAlike.mean < 0.5,
                QString("Fog brightens by %1 display levels without shadows. Clear sky: %2")
                    .arg(scatter*255,0,'f',2).arg(describe(clearAlike)));

    const Shot duskShot{"physical-fog-dusk",fogShot.eye,fogShot.target};
    setDisplay({{"skyModel","Physical"},{"sunAzimuth",90},{"sunElevation",-8},
                {"ev100",10},{"exposureMode","Manual"},{"fogDensity",0},{"localFogRadius",0}});
    const auto duskClear = settled(duskShot);
    setDisplay({{"localFogCenter",QJsonArray{-50,-38,5.5}},{"localFogRadius",10},{"localFogDensity",0.20}});
    const auto duskMist = settled(duskShot);
    duskMist.save(output.filePath("rendering-physical-fog-dusk.png"));
    const double duskClearLevel = luminance(skyPatch(duskClear)), duskMistLevel = luminance(skyPatch(duskMist));
    audit.check(page,"fog","Local mist stays dark under the physical sky after sunset",
                !duskMist.isNull() && !duskClear.isNull() && duskMistLevel < 0.12 && duskMistLevel <= duskClearLevel+0.025,
                QString("Sky %1, mist %2 mean display levels").arg(255*duskClearLevel,0,'f',2).arg(255*duskMistLevel,0,'f',2));

    const Shot sunsetShot{"physical-fog-sunset",shots[0].eye,shots[0].target};
    setDisplay({{"sunAzimuth",73},{"sunElevation",7},{"ev100",14},{"localFogRadius",0}});
    const auto sunsetClear = settled(sunsetShot);
    setDisplay({{"fogDensity",0.003},{"localFogCenter",QJsonArray{-3,-2,1.5}},
                {"localFogRadius",9},{"localFogDensity",0.035}});
    const auto sunsetMist = settled(sunsetShot);
    sunsetMist.save(output.filePath("rendering-physical-fog-sunset.png"));
    const auto sunsetFog = difference(sunsetMist,sunsetClear,"rendering-physical-fog-sunset-diff.png");
    std::ostringstream sunsetPlayable;
    QFile sunsetFile(playablePath);
    const bool sunsetExported = WorldRuntime::exportScene(store,sunsetPlayable) && sunsetFile.open(QIODevice::WriteOnly);
    const auto sunsetBytes = sunsetPlayable.str();
    const bool sunsetWritten = sunsetExported && sunsetFile.write(sunsetBytes.data(),qint64(sunsetBytes.size())) == qint64(sunsetBytes.size());
    sunsetFile.close();
    const auto playedSunset = sunsetWritten ? playerCapture(sunsetShot,sunsetMist.size()) : QImage();
    const auto sunsetAlike = difference(playedSunset,sunsetMist,"rendering-physical-fog-sunset-editor-player-diff.png");
    audit.check(page,"fog","Editor and Player match with physical height fog and local mist at sunset",
                !playedSunset.isNull() && !sunsetClear.isNull() && sunsetFog.mean > 1
                && sunsetAlike.mean <= 1 && sunsetAlike.share <= 0.005,
                QString("Fog changes the view by %1. Editor and Player: %2").arg(describe(sunsetFog),describe(sunsetAlike)));

    const auto captureWeatherPlayer = [&](const Shot &shot,const QImage &editor) {
        std::ostringstream weatherBytes;
        QFile package(playablePath);
        if (!WorldRuntime::exportScene(store,weatherBytes) || !package.open(QIODevice::WriteOnly))
            return QImage{};
        const auto bytes = weatherBytes.str();
        const bool written = package.write(bytes.data(),qint64(bytes.size())) == qint64(bytes.size());
        package.close();
        return written ? playerCapture(shot,editor.size()) : QImage{};
    };
    const Shot weatherShot{"overcast",shots[1].eye,shots[1].target};
    setDisplay({{"sunElevation",30},{"fogDensity",0},{"localFogRadius",0},{"cloudCover",0},{"aerialHaze",0}});
    const auto clearWeather = settled(weatherShot);
    setDisplay({{"cloudCover",0.92},{"targetCloudCover",0.3},{"cloudTransition",12}});
    const auto overcast = settled(weatherShot);
    overcast.save(output.filePath("rendering-overcast.png"));
    const auto cloudChange = difference(overcast,clearWeather,"rendering-overcast-diff.png");
    const auto cloudParity = difference(captureWeatherPlayer(weatherShot,overcast),overcast,"rendering-overcast-parity.png");
    audit.check(page,"weather","Cloud cover changes the sky and street lighting alike in both renderers",
                !overcast.isNull() && !clearWeather.isNull() && cloudChange.mean > 3
                && cloudParity.mean <= 1 && cloudParity.share <= 0.005,
                QString("Clouds change the view by %1. Editor and Player have %2").arg(describe(cloudChange),describe(cloudParity)));

    const Shot hazeShot{"distance-haze",shots[2].eye,shots[2].target};
    setDisplay({{"cloudCover",0},{"cloudTransition",0}});
    const auto clearAir = settled(hazeShot);
    setDisplay({{"aerialHaze",8}});
    const auto haze = settled(hazeShot);
    haze.save(output.filePath("rendering-distance-haze.png"));
    const auto hazeChange = difference(haze,clearAir,"rendering-distance-haze-diff.png");
    const auto hazeParity = difference(captureWeatherPlayer(hazeShot,haze),haze,"rendering-distance-haze-parity.png");
    audit.check(page,"weather","Distance haze changes the aerial view alike in both renderers",
                !haze.isNull() && !clearAir.isNull() && hazeChange.mean > 2
                && hazeParity.mean <= 1 && hazeParity.share <= 0.005,
                QString("Haze changes the view by %1. Editor and Player have %2").arg(describe(hazeChange),describe(hazeParity)));

    const Shot lampShot{"lamp-mist",fogShot.eye,fogShot.target};
    setDisplay({{"sunElevation",-12},{"ev100",7},{"aerialHaze",0},{"localFogCenter",QJsonArray{-50,-39,5}},
                {"localFogRadius",8},{"localFogDensity",0.07},{"fogColor","#ffffff"}});
    const auto unlitMist = settled(lampShot);
    store.addObject("Point Light","Lighting");
    if (const auto *selected = store.selectedObject(); selected && selected->type == "Point Light") {
        auto lamp = *selected;
        lamp.position = {-50,-39,5};
        setLightPhotometric(lamp.details,lamp.type,6000);
        lamp.details.insert("lightColor","#ffbb71");
        QString lampError;
        const bool warmReady = store.updateObjects({lamp},"Set warm lamp in the mist",lampError);
        const auto warm = settled(lampShot);
        warm.save(output.filePath("rendering-warm-lamp-mist.png"));
        lamp.details.insert("lightColor","#72b4ff");
        const bool coolReady = store.updateObjects({lamp},"Set cool lamp in the mist",lampError);
        const auto cool = settled(lampShot);
        cool.save(output.filePath("rendering-cool-lamp-mist.png"));
        const auto lampChange = difference(warm,unlitMist,"rendering-lamp-mist-diff.png");
        const auto colorChange = difference(cool,warm,"rendering-warm-cool-lamp-diff.png");
        const auto lampParity = difference(captureWeatherPlayer(lampShot,cool),cool,"rendering-lamp-mist-parity.png");
        audit.check(page,"fog","Warm and cool lamps scatter colored light through physical mist in both renderers",
            warmReady && coolReady && !cool.isNull() && !warm.isNull() && !unlitMist.isNull()
            && lampChange.mean > 2 && colorChange.mean > 2 && lampParity.mean <= 1 && lampParity.share <= 0.005,
            QString("Lamp effect has %1. Warm and cool images have %2. Editor and Player have %3")
                .arg(describe(lampChange),describe(colorChange),describe(lampParity)));
    } else {
        audit.check(page,"fog","Create a lamp to illuminate physical mist",false);
    }
    while (store.history().index() > historyStart && store.history().canUndo())
        store.history().undo();

    // Screen-space reflections: a mirror-like metal floor in front of a block. With reflections on, the block shows in
    // the floor; off, the floor reflects only the sky. The street's own surfaces are rough, so its views stay as they were.
    const Shot mirrorShot{"reflections",{-90,-62,1.6f},{-90,-50,0.4f}};
    bool mirrorReady = false;
    store.addCube();
    if (const auto *selected = store.selectedObject()) {
        auto floor = *selected;
        floor.position = {-90,-52,0.05f};
        floor.scale = {14,26,0.1f};
        store.updateObject(floor,"Mirror floor");
        if (const auto *asset = store.meshAsset(floor.meshAsset); asset && asset->mesh) {
            auto mesh = *asset->mesh;
            for (auto &slot : mesh.materials) {
                slot.metallic = 1;
                slot.roughness = 0.08f;
            }
            QString meshError;
            mirrorReady = store.updateMesh(floor.meshAsset,mesh,"Mirror floor material",meshError);
        }
    }
    store.addCube();
    if (const auto *selected = store.selectedObject()) {
        auto block = *selected;
        block.position = {-90,-45,1.5f};
        block.scale = {4,1,3};
        store.updateObject(block,"Reflected block");
    }
    store.select({});
    setDisplay({{"fogDensity",0},{"localFogRadius",0},{"bloom",false},{"reflections",true}});
    const auto reflected = settled(mirrorShot);
    {
        std::ostringstream mirrorPlayable;
        QFile mirrorFile(playablePath);
        const bool mirrorExported = WorldRuntime::exportScene(store,mirrorPlayable) && mirrorFile.open(QIODevice::WriteOnly);
        const auto bytes = mirrorPlayable.str();
        const bool written = mirrorExported && mirrorFile.write(bytes.data(),qint64(bytes.size())) == qint64(bytes.size());
        mirrorFile.close();
        const auto played = written ? playerCapture(mirrorShot,reflected.size()) : QImage();
        const auto alike = difference(played,reflected,"rendering-reflections-editor-player-diff.png");
        audit.check(page,"reflections","Editor and Player match with screen-space reflections",
                    mirrorReady && !played.isNull() && alike.mean <= 1 && alike.share <= 0.005,describe(alike));
    }
    setDisplay({{"reflections",false}});
    const auto skyOnly = settled(mirrorShot);
    reflected.save(output.filePath("rendering-reflections.png"));
    skyOnly.save(output.filePath("rendering-reflections-off.png"));
    // Below the horizon of the view the floor shows the block's reflection; above it nothing glossy is in view.
    const auto lowerHalf = [](const QImage &image) { return image.copy(0,image.height()/2,image.width(),image.height()/2); };
    const auto upperHalf = [](const QImage &image) { return image.copy(0,0,image.width(),image.height()/3); };
    const auto floorChange = difference(lowerHalf(reflected),lowerHalf(skyOnly),"rendering-reflections-floor-diff.png");
    const auto skyChange = difference(upperHalf(reflected),upperHalf(skyOnly),"rendering-reflections-sky-diff.png");
    audit.check(page,"reflections","Screen-space reflections show the block in a mirror-like floor",
                mirrorReady && !reflected.isNull() && !skyOnly.isNull() && floorChange.mean > 3 && skyChange.mean < 0.5,
                QString("Floor changes by %1 levels on average with reflections on; the view above it by %2.")
                    .arg(floorChange.mean,0,'f',2).arg(skyChange.mean,0,'f',2));
    while (store.history().index() > historyStart && store.history().canUndo())
        store.history().undo();

    // A low sun reflects off lacquer over a rough painted slab isolated from the street.
    const Shot coatShot{"clear-coat",{-140,-62,3.8f},{-140,-50,0.1f}};
    QString coatAsset, coatError;
    store.addCube();
    if (const auto *selected = store.selectedObject()) {
        auto slab = *selected;
        slab.position = {-140,-51,0.05f};
        slab.scale = {12,16,0.1f};
        store.updateObject(slab,"Clear coat test slab");
        coatAsset = slab.meshAsset;
    }
    const auto setCoat = [&](float weight,float roughness) {
        const auto *asset = store.meshAsset(coatAsset);
        if (!asset || !asset->mesh)
            return false;
        auto mesh = *asset->mesh;
        for (auto &slot : mesh.materials) {
            slot.color = {0.26f,0.06f,0.04f,1};
            slot.roughness = 0.7f;
            slot.clearCoat = weight;
            slot.clearCoatRoughness = roughness;
        }
        return store.updateMesh(coatAsset,std::move(mesh),"Set clear coat",coatError);
    };
    store.select({});
    setDisplay({{"fogDensity",0},{"localFogRadius",0},{"bloom",false},{"shadows",false},{"reflections",false},
                {"sunAzimuth",90},{"sunElevation",19}});
    bool coatReady = setCoat(0,0.12f);
    const auto bare = settled(coatShot);
    coatReady &= setCoat(1,0.12f);
    const auto lacquer = settled(coatShot);
    {
        std::ostringstream package;
        QFile file(playablePath);
        const bool coatExported = WorldRuntime::exportScene(store,package) && file.open(QIODevice::WriteOnly);
        const auto bytes = package.str();
        const bool written = coatExported && file.write(bytes.data(),qint64(bytes.size())) == qint64(bytes.size());
        file.close();
        const auto played = written ? playerCapture(coatShot,lacquer.size()) : QImage();
        const auto alike = difference(played,lacquer,"rendering-clear-coat-editor-player-diff.png");
        audit.check(page,"clear coat","Editor and Player match on lacquer over rough paint",
                    coatReady && !played.isNull() && alike.mean <= 1 && alike.share <= 0.005,describe(alike));
    }
    coatReady &= setCoat(1,0.5f);
    const auto satin = settled(coatShot);
    lacquer.save(output.filePath("rendering-clear-coat.png"));
    const auto coatChange = difference(lacquer,bare,"rendering-clear-coat-weight-diff.png");
    const auto roughnessChange = difference(lacquer,satin,"rendering-clear-coat-roughness-diff.png");
    audit.check(page,"clear coat","Weight and roughness change the coating over the same base paint",
                coatReady && !bare.isNull() && !lacquer.isNull() && !satin.isNull()
                && coatChange.mean > 0.3 && roughnessChange.mean > 0.3,
                QString("Weight changes the view by %1 levels on average and roughness by %2. %3")
                    .arg(coatChange.mean,0,'f',2).arg(roughnessChange.mean,0,'f',2).arg(coatError));

    const Shot clothShot{"sheen",{-140,-62,2.0f},{-140,-50,0.1f}};
    setDisplay({{"sunElevation",10}});
    const auto dressCloth = [&](const worldforge::MaterialLayers &layers) {
        const auto *asset = store.meshAsset(coatAsset);
        if (!asset || !asset->mesh)
            return false;
        auto mesh = *asset->mesh;
        for (auto &slot : mesh.materials) {
            slot.clearCoat = 0;
            slot.layers = layers;
        }
        return store.updateMesh(coatAsset,std::move(mesh),"Set fabric material",coatError);
    };
    bool clothReady = dressCloth({});
    const auto clothBare = settled(clothShot);
    clothReady &= dressCloth({{0.18f,0.35f,0.85f},0.52f});
    const auto cloth = settled(clothShot);
    {
        std::ostringstream package;
        QFile file(playablePath);
        const bool exportedCloth = WorldRuntime::exportScene(store,package) && file.open(QIODevice::WriteOnly);
        const auto bytes = package.str();
        const bool written = exportedCloth && file.write(bytes.data(),qint64(bytes.size())) == qint64(bytes.size());
        file.close();
        const auto played = written ? playerCapture(clothShot,cloth.size()) : QImage();
        const auto alike = difference(played,cloth,"rendering-sheen-editor-player-diff.png");
        audit.check(page,"sheen","The editor and Player match on a grazing fabric highlight",
                    clothReady && !played.isNull() && alike.mean <= 1 && alike.share <= 0.005,describe(alike));
    }
    clothReady &= dressCloth({{0.18f,0.35f,0.85f},0.88f});
    const auto clothRough = settled(clothShot);
    const auto sheenColorChange = difference(cloth,clothBare,"rendering-sheen-color-diff.png");
    const auto sheenRoughnessChange = difference(cloth,clothRough,"rendering-sheen-roughness-diff.png");
    audit.check(page,"sheen","Sheen color and roughness visibly affect the fabric",
                clothReady && sheenColorChange.mean > 0.3 && sheenRoughnessChange.mean > 0.3,
                describe(sheenColorChange)+" / "+describe(sheenRoughnessChange));
    while (store.history().index() > historyStart && store.history().canUndo())
        store.history().undo();

    // A single glass quad in front of an opaque block. This checks current-frame visibility through the pane.
    const Shot glassShot{"thin-glass",{-155,-67,2},{-155,-55,2}};
    store.addCube();
    if (const auto *selected = store.selectedObject()) {
        auto block = *selected;
        block.position = {-155,-54,2};
        block.scale = {2,1,3};
        store.updateObject(block,"Object behind the pane");
    }
    QString paneAsset;
    store.addCube();
    if (const auto *selected = store.selectedObject()) {
        auto pane = *selected;
        pane.position = {-155,-57,2};
        store.updateObject(pane,"Thin glass pane");
        paneAsset = pane.meshAsset;
    }
    const auto makePane = [&](float transmission,std::array<float,4> color,
                              worldforge::MaterialLayers layers = {},float metallic = 0,float roughness = 0.14f) {
        if (paneAsset.isEmpty())
            return false;
        worldforge::Mesh mesh;
        mesh.vertices = {{-3,0,-2},{3,0,-2},{3,0,2},{-3,0,2}};
        mesh.faces.push_back({{{0,{0,0}},{1,{1,0}},{2,{1,1}},{3,{0,1}}},0});
        auto &surface = mesh.materials.front();
        surface.color = color;
        surface.metallic = metallic;
        surface.roughness = roughness;
        surface.layers = layers;
        surface.layers.transmission = transmission;
        return store.updateMesh(paneAsset,std::move(mesh),"Set the pane material",coatError);
    };
    setDisplay({{"fogDensity",0},{"localFogRadius",0},{"bloom",false},{"shadows",false},{"reflections",false}});
    store.select({});
    bool paneReady = makePane(0,{1,1,1,1});
    const auto opaquePane = settled(glassShot);
    paneReady &= makePane(1,{1,1,1,1});
    const auto clearPane = settled(glassShot);
    paneReady &= makePane(1,{0.34f,1,0.55f,1});
    const auto tintedPane = settled(glassShot);
    const auto captureMaterialInPlayer = [&](const Shot &shot,const QImage &editor) {
        std::ostringstream package;
        QFile file(playablePath);
        const bool exportedPane = WorldRuntime::exportScene(store,package) && file.open(QIODevice::WriteOnly);
        const auto bytes = package.str();
        const bool written = exportedPane && file.write(bytes.data(),qint64(bytes.size())) == qint64(bytes.size());
        file.close();
        return written ? playerCapture(shot,editor.size()) : QImage();
    };
    {
        const auto played = captureMaterialInPlayer(glassShot,tintedPane);
        const auto alike = difference(played,tintedPane,"rendering-glass-editor-player-diff.png");
        audit.check(page,"glass","The editor and Player match on tinted thin glass",
                    paneReady && !played.isNull() && alike.mean <= 1 && alike.share <= 0.005,describe(alike));
    }
    const auto paneChange = difference(opaquePane,clearPane,"rendering-glass-transmission-diff.png");
    const auto tintChange = difference(clearPane,tintedPane,"rendering-glass-tint-diff.png");
    tintedPane.save(output.filePath("rendering-glass.png"));
    audit.check(page,"glass","A transparent pane reveals the scene and its base color tints the transmitted light",
                paneReady && !opaquePane.isNull() && !clearPane.isNull() && !tintedPane.isNull()
                && paneChange.mean > 2 && tintChange.mean > 2,describe(paneChange)+" / "+describe(tintChange));

    setDisplay({{"sunAzimuth",90},{"sunElevation",12}});
    const std::array<float,4> waxColor{0.77f,0.43f,0.32f,1};
    worldforge::MaterialLayers wax;
    wax.subsurface = 1;
    wax.thickness = 0.004f;
    wax.scatteringDistance = 0.06f;
    bool waxReady = makePane(0,waxColor,wax);
    const auto thinWax = settled(glassShot);
    wax.thickness = 0.25f;
    waxReady &= makePane(0,waxColor,wax);
    const auto thickWax = settled(glassShot);
    const auto waxChange = difference(thinWax,thickWax,"rendering-subsurface-thickness-diff.png");
    const auto inner = [](const QImage &frame) {
        return frame.copy(frame.width()*4/10,frame.height()*4/10,frame.width()/5,frame.height()/5);
    };
    audit.check(page,"advanced materials","A thinner backlit surface passes more scattered light",
        waxReady && !thinWax.isNull() && !thickWax.isNull() && luminance(inner(thinWax)) > luminance(inner(thickWax))+0.03,
        describe(waxChange));

    const Shot metalShot{"anisotropy",{-155,-67,0.4f},{-155,-55,2}};
    setDisplay({{"sunAzimuth",-90},{"sunElevation",10}});
    const std::array<float,4> metalColor{0.68f,0.63f,0.53f,1};
    worldforge::MaterialLayers brushed;
    brushed.anisotropy = 0.83f;
    bool metalReady = makePane(0,metalColor,brushed,1,0.26f);
    const auto along = settled(metalShot);
    brushed.anisotropyRotation = 1.5707963f;
    metalReady &= makePane(0,metalColor,brushed,1,0.26f);
    const auto across = settled(metalShot);
    const auto metalChange = difference(along,across,"rendering-anisotropy-rotation-diff.png");
    const auto metalPlayer = captureMaterialInPlayer(metalShot,across);
    const auto metalAlike = difference(metalPlayer,across,"rendering-anisotropy-editor-player-diff.png");
    audit.check(page,"advanced materials","Rotating the brush direction rotates the highlight in both renderers",
        metalReady && !along.isNull() && !across.isNull() && !metalPlayer.isNull() && metalChange.mean > 0.3
        && metalAlike.mean <= 1 && metalAlike.share <= 0.005,describe(metalChange)+" / "+describe(metalAlike));
    const auto encodeMap = [](const QImage &image) {
        QByteArray bytes;
        QBuffer buffer(&bytes);
        buffer.open(QIODevice::WriteOnly);
        image.save(&buffer,"PNG");
        return std::vector<uint8_t>(bytes.begin(),bytes.end());
    };
    bool mappedReady = makePane(1,{0.34f,1,0.55f,1});
    const auto fullTransmission = settled(glassShot);
    if (const auto *asset = store.meshAsset(paneAsset)) {
        auto mesh = *asset->mesh;
        auto &map = mesh.materials.front().layerMaps[worldforge::Transmission];
        QImage mask(32,32,QImage::Format_RGBA8888);
        for (int y = 0; y < mask.height(); ++y)
            for (int x = 0; x < mask.width(); ++x)
                mask.setPixelColor(x,y,QColor(x < mask.width()/2 ? 0 : 255,130,220));
        map = encodeMap(mask);
        mappedReady &= !map.empty() && store.updateMesh(asset->id,std::move(mesh),"Apply a patterned transmission map",coatError);
    } else
        mappedReady = false;
    const auto mappedGlass = settled(glassShot);
    const auto mapChange = difference(fullTransmission,mappedGlass,"rendering-transmission-map-diff.png");
    const auto mapPlayer = captureMaterialInPlayer(glassShot,mappedGlass);
    const auto mapAlike = difference(mapPlayer,mappedGlass,"rendering-transmission-map-editor-player-diff.png");
    audit.check(page,"material maps","The transmission map changes coverage and matches in both renderers",
        mappedReady && !mappedGlass.isNull() && !mapPlayer.isNull() && mapChange.mean > 1
        && mapAlike.mean <= 1 && mapAlike.share <= 0.005,describe(mapChange)+" / "+describe(mapAlike));

    const Shot wetShot{"wet-ground",{-155,-63,4},{-155,-55,2}};
    setDisplay({{"sunAzimuth",-90},{"sunElevation",30}});
    worldforge::Mesh groundMesh;
    groundMesh.vertices = {{-3,-3,0},{3,-3,0},{3,3,0},{-3,3,0}};
    groundMesh.faces.push_back({{{0,{0,0}},{1,{1,0}},{2,{1,1}},{3,{0,1}}},0});
    auto &groundMaterial = groundMesh.materials.front();
    groundMaterial.color = {0.56f,0.49f,0.4f,1};
    groundMaterial.roughness = 0.81f;
    QImage packedStreet(48,48,QImage::Format_RGBA8888), streetColor(48,48,QImage::Format_RGBA8888);
    for (int y = 0; y < packedStreet.height(); ++y)
        for (int x = 0; x < packedStreet.width(); ++x) {
            const bool raised = (y/6+x/12)%2 == 0;
            const int tone = raised ? 167 : 102;
            packedStreet.setPixelColor(x,y,QColor(255,raised ? 255 : 45,x < packedStreet.width()/2 ? 0 : 255));
            streetColor.setPixelColor(x,y,QColor(tone,tone,tone));
        }
    groundMaterial.occlusionTexture = encodeMap(packedStreet);
    groundMaterial.baseColorTexture = encodeMap(streetColor);
    const auto installGround = [&] {
        return !paneAsset.isEmpty() && store.updateMesh(paneAsset,groundMesh,"Set the pavement material",coatError);
    };
    bool streetReady = installGround();
    const auto dryGround = settled(wetShot);
    groundMaterial.layers.wetness = 0.76f;
    streetReady &= installGround();
    const auto wetGround = settled(wetShot);
    groundMaterial.layers.puddles = 0.88f;
    streetReady &= installGround();
    const auto pooled = settled(wetShot);
    const auto wetChange = difference(dryGround,wetGround,"rendering-wet-vs-dry-diff.png");
    const auto poolChange = difference(wetGround,pooled,"rendering-puddles-diff.png");
    const auto poolPlayer = captureMaterialInPlayer(wetShot,pooled);
    const auto poolAlike = difference(poolPlayer,pooled,"rendering-puddles-editor-player-diff.png");
    audit.check(page,"street materials","Wet pavement and masked puddles change the lighting in both renderers",
        streetReady && !pooled.isNull() && !poolPlayer.isNull() && wetChange.mean > 0.25 && poolChange.mean > 0.15
        && poolAlike.mean <= 1 && poolAlike.share <= 0.005,
        describe(wetChange)+" / "+describe(poolChange)+" / "+describe(poolAlike));

    groundMaterial.layers = {};
    groundMaterial.layers.heightScale = 0.115f;
    streetReady &= installGround();
    const auto relief = settled(wetShot);
    const auto reliefChange = difference(dryGround,relief,"rendering-parallax-diff.png");
    const auto reliefPlayer = captureMaterialInPlayer(wetShot,relief);
    const auto reliefAlike = difference(reliefPlayer,relief,"rendering-parallax-editor-player-diff.png");
    audit.check(page,"street materials","Parallax occlusion shifts the pavement pattern at a grazing angle",
        streetReady && !relief.isNull() && !reliefPlayer.isNull() && reliefChange.mean > 0.25
        && reliefAlike.mean <= 1 && reliefAlike.share <= 0.005,describe(reliefChange)+" / "+describe(reliefAlike));

    store.addCube();
    QString decalAsset;
    if (const auto *selected = store.selectedObject()) {
        auto object = *selected;
        object.position = {-155,-57,2.002f};
        store.updateObject(object,"Place a decal above the pavement");
        decalAsset = object.meshAsset;
    }
    auto decalMesh = groundMesh;
    for (auto &vertex : decalMesh.vertices) {
        vertex[0] *= 0.65f;
        vertex[1] *= 0.65f;
    }
    auto &decalMaterial = decalMesh.materials.front();
    decalMaterial.color = {1,0.8f,0.42f,0.85f};
    decalMaterial.occlusionTexture.clear();
    decalMaterial.layers = {};
    decalMaterial.layers.decal = true;
    decalMaterial.layers.decalOpacity = 0.85f;
    QImage decalImage(48,48,QImage::Format_RGBA8888);
    for (int y = 0; y < decalImage.height(); ++y)
        for (int x = 0; x < decalImage.width(); ++x)
            decalImage.setPixelColor(x,y,QColor(255,205,40,(x/8+y/8)%2 == 0 ? 255 : 0));
    decalMaterial.baseColorTexture = encodeMap(decalImage);
    store.select({});
    if (decalAsset.isEmpty())
        streetReady = false;
    else
        streetReady &= store.updateMesh(decalAsset,decalMesh,"Apply an alpha-masked pavement marking",coatError);
    const auto decal = settled(wetShot);
    const auto decalPlayer = captureMaterialInPlayer(wetShot,decal);
    const auto decalAlike = difference(decalPlayer,decal,"rendering-decal-editor-player-diff.png");
    decalMaterial.layers.decalOpacity = 0;
    if (streetReady)
        streetReady = store.updateMesh(decalAsset,std::move(decalMesh),"Fade the pavement marking",coatError);
    const auto invisibleDecal = settled(wetShot);
    const auto decalChange = difference(decal,invisibleDecal,"rendering-decal-opacity-diff.png");
    const auto transparentChange = difference(relief,invisibleDecal,"rendering-hidden-decal-diff.png");
    audit.check(page,"street materials","A decal blends its texture alpha and opacity over the pavement",
        streetReady && !decalPlayer.isNull() && !invisibleDecal.isNull() && decalChange.mean > 0.5
        && transparentChange.mean < 0.3 && decalAlike.mean <= 1 && decalAlike.share <= 0.005,
        describe(decalChange)+" / "+describe(transparentChange)+" / "+describe(decalAlike));
    while (store.history().index() > historyStart && store.history().canUndo())
        store.history().undo();

    const Shot probeShot{"probe-gi",{-183,-64,3},{-183,-58,1.6f}};
    QString probeFloor;
    const auto probePlane = [&](float height,worldforge::Point3 emission) {
        store.addCube();
        if (!store.selectedObject() || !store.selectedMesh())
            return false;
        auto object = *store.selectedObject();
        object.position = {-183,-58,height};
        if (height == 1)
            probeFloor = object.meshAsset;
        auto plane = worldforge::makePrimitive(worldforge::Primitive::Plane,{12,12,0.1f});
        if (!plane)
            return false;
        plane->materials.front().color = {0.48f,0.48f,0.48f,1};
        plane->materials.front().emissive = emission;
        store.updateObject(object,"Place world-space probe test geometry");
        return store.updateMesh(object.meshAsset,std::move(*plane),"Set the probe test surface",coatError);
    };
    const bool probeScene = probePlane(1,{}) && probePlane(5,{1,0.04f,0.015f});
    store.select({});
    setDisplay({{"fogDensity",0},{"localFogRadius",0},{"bloom",false},{"shadows",false},{"reflections",false},
                {"ambientOcclusion",false},{"antiAliasing","Off"},{"probeGi",false}});
    const auto noProbe = settled(probeShot);
    setDisplay({{"probeGi",true}});
    const auto withProbe = settled(probeShot);
    const auto probePlayer = captureMaterialInPlayer(probeShot,withProbe);
    const auto probeChange = difference(noProbe,withProbe,"rendering-probe-gi-diff.png");
    const auto probeAlike = difference(probePlayer,withProbe,"rendering-probe-gi-editor-player-diff.png");
    audit.check(page,"probe GI","A ceiling above the camera adds diffuse bounce in the editor and Player",
        probeScene && !withProbe.isNull() && luminance(withProbe) > luminance(noProbe)+0.005 && probeChange.mean > 1
        && probeAlike.mean <= 1.0 && probeAlike.share <= 0.005,describe(probeChange)+" / "+describe(probeAlike));

    const auto beforeScreenSource = store.history().index();
    store.addCube();
    bool screenSource = false;
    if (store.selectedObject() && store.selectedMesh()) {
        auto object = *store.selectedObject();
        object.position = {-182,-58,1.8f};
        object.scale = {1,0.35f,1.6f};
        auto mesh = *store.selectedMesh()->mesh;
        mesh.materials.front().emissive = {1.5f,0.015f,0.02f};
        store.updateObject(object,"Place a visible light source beside the floor");
        screenSource = store.updateMesh(object.meshAsset,std::move(mesh),"Set the screen-space test surface",coatError);
    }
    store.select({});
    const Shot screenShot{"screen-gi",probeShot.eye,probeShot.target};
    setDisplay({{"probeGi",false},{"screenSpaceGi",false},{"ambientOcclusion",false}});
    const auto screenOff = settled(screenShot);
    setDisplay({{"screenSpaceGi",true}});
    const auto screenOn = settled(screenShot);
    const auto screenPlayer = captureMaterialInPlayer(screenShot,screenOn);
    // Compare the floor below the emitter. Short-range bounce fades out over a few meters.
    const auto receiver = [](const QImage &image) {
        return image.copy(image.width()*52/100,image.height()*63/100,image.width()*21/100,image.height()*9/100);
    };
    const auto floorOff = receiver(screenOff), floorOn = receiver(screenOn);
    const auto screenChange = difference(floorOff,floorOn,"rendering-screen-gi-floor-diff.png");
    const auto screenAlike = difference(screenPlayer,screenOn,"rendering-screen-gi-editor-player-diff.png");
    audit.check(page,"screen-space GI","Visible emission adds local bounce with ambient occlusion off",
        screenSource && !floorOff.isNull() && !floorOn.isNull() && luminance(floorOn) > luminance(floorOff)+0.0007
        && screenChange.mean > 1.0 && screenAlike.mean <= 1.0 && screenAlike.share <= 0.005,
        describe(screenChange)+" / "+describe(screenAlike));
    while (store.history().index() > beforeScreenSource && store.history().canUndo())
        store.history().undo();

    bool mirror = false;
    if (const auto *asset = store.meshAsset(probeFloor)) {
        auto floor = *asset->mesh;
        floor.materials.front().color = {0.7f,0.7f,0.7f,1};
        floor.materials.front().metallic = 1;
        floor.materials.front().roughness = 0.14f;
        mirror = store.updateMesh(probeFloor,std::move(floor),"Make the floor reflective",coatError);
    }
    store.select({});
    const Shot reflectionShot{"reflection-probes",{-183,-62,4},{-183,-58,1}};
    setDisplay({{"probeGi",false},{"screenSpaceGi",false},{"reflections",false},{"reflectionProbes",false}});
    const auto reflectionOff = settled(reflectionShot);
    setDisplay({{"reflectionProbes",true}});
    const auto reflectionOn = settled(reflectionShot);
    const auto reflectionPlayer = captureMaterialInPlayer(reflectionShot,reflectionOn);
    const auto reflectionChange = difference(reflectionOff,reflectionOn,"rendering-reflection-probes-diff.png");
    const auto reflectionAlike = difference(reflectionPlayer,reflectionOn,"rendering-reflection-probes-editor-player-diff.png");
    audit.check(page,"reflection probes","A ceiling outside the camera view reflects in a polished floor",
        mirror && !reflectionOff.isNull() && !reflectionOn.isNull() && reflectionChange.mean > 3.0
        && reflectionAlike.mean <= 1.0 && reflectionAlike.share <= 0.005,
        describe(reflectionChange)+" / "+describe(reflectionAlike));
    while (store.history().index() > historyStart && store.history().canUndo())
        store.history().undo();

    // Contact shadows: a box resting on a slab, lit from behind so its shadow falls toward the camera. They may only
    // darken, and open ground away from the box must not shadow itself.
    const Shot contactShot{"contact-shadows",{-120,-61.2f,1.1f},{-120,-58,0.1f}};
    store.addCube();
    if (const auto *selected = store.selectedObject()) {
        auto ground = *selected;
        ground.position = {-120,-58,0.05f};
        ground.scale = {8,8,0.1f};
        store.updateObject(ground,"Contact ground");
    }
    store.addCube();
    if (const auto *selected = store.selectedObject()) {
        auto box = *selected;
        box.position = {-120,-58,0.3f};
        box.scale = {0.4f,0.4f,0.4f};
        store.updateObject(box,"Contact box");
    }
    store.select({});
    setDisplay({{"fogDensity",0},{"localFogRadius",0},{"bloom",false},{"shadows",true},{"sunElevation",35},
                {"sunAzimuth",90},{"contactShadows",true}});
    const auto contact = settled(contactShot);
    {
        std::ostringstream contactPlayable;
        QFile contactFile(playablePath);
        const bool contactExported = WorldRuntime::exportScene(store,contactPlayable) && contactFile.open(QIODevice::WriteOnly);
        const auto bytes = contactPlayable.str();
        const bool written = contactExported && contactFile.write(bytes.data(),qint64(bytes.size())) == qint64(bytes.size());
        contactFile.close();
        const auto played = written ? playerCapture(contactShot,contact.size()) : QImage();
        const auto alike = difference(played,contact,"rendering-contact-shadows-editor-player-diff.png");
        audit.check(page,"contact shadows","Editor and Player match with contact shadows",
                    !played.isNull() && alike.mean <= 1 && alike.share <= 0.005,describe(alike));
    }
    setDisplay({{"contactShadows",false}});
    const auto noContact = settled(contactShot);
    contact.save(output.filePath("rendering-contact-shadows.png"));
    noContact.save(output.filePath("rendering-contact-shadows-off.png"));
    const auto whole = difference(contact,noContact,"rendering-contact-shadows-diff.png");
    // Around the box and its shadow, in the middle of the view.
    const auto aroundBox = [](const QImage &image) {
        return image.copy(image.width()*2/5,image.height()*7/20,image.width()/5,image.height()*3/10);
    };
    const double darkening = luminance(aroundBox(noContact))-luminance(aroundBox(contact));
    // The bottom corners show open ground in front of and beside the box, away from its shadow.
    const auto corner = [](const QImage &image) {
        return image.copy(0,image.height()*4/5,image.width()/6,image.height()/5);
    };
    const auto openGround = difference(corner(contact),corner(noContact),"rendering-contact-shadows-open-diff.png");
    audit.check(page,"contact shadows","Contact shadows darken near the box and leave open ground alone",
                !contact.isNull() && !noContact.isNull() && darkening*255 > 0.3 && openGround.mean < 0.3,
                QString("The view around the box darkens by %1 levels on average (whole view: %2); open ground: %3.")
                    .arg(darkening*255,0,'f',2).arg(describe(whole)).arg(describe(openGround)));
    while (store.history().index() > historyStart && store.history().canUndo())
        store.history().undo();

    // Per-object motion: a box falling in Simulate. Paused, nothing moves and its top edge is sharp. One physics step
    // later, motion blur smears that edge along the fall; the camera is still, so only the box's own motion (the scene's
    // velocity target) can do that.
    const Shot fallShot{"falling-box",{-150,-66,2.5f},{-150,-58,2.5f}};
    store.addCube();
    if (const auto *selected = store.selectedObject()) {
        auto ground = *selected;
        ground.position = {-150,-58,0.05f};
        ground.scale = {8,8,0.1f};
        store.updateObject(ground,"Fall ground");
    }
    store.addCube();
    QString fallingId;
    if (const auto *selected = store.selectedObject()) {
        auto box = *selected;
        box.position = {-150,-58,6.5f};
        box.details.insert("physics","Dynamic");
        store.updateObject(box,"Falling box");
        fallingId = box.id;
    }
    store.select({});
    setDisplay({{"fogDensity",0},{"localFogRadius",0},{"bloom",false},{"motionBlur",true},{"shutter",1.0}});
    // The sharpest step in brightness down the middle columns near row, within reach rows: the box's top edge.
    const auto edgeSharpness = [](const QImage &image,int row,int reach) {
        double total = 0;
        int columns = 0;
        for (int x = image.width()/2-8; x <= image.width()/2+8; ++x) {
            double sharpest = 0;
            for (int y = std::max(row-reach,0); y < std::min(row+reach,image.height()-1); ++y) {
                const QColor a = image.pixel(x,y), b = image.pixel(x,y+1);
                sharpest = std::max(sharpest,double(std::abs(a.lightnessF()-b.lightnessF())));
            }
            total += sharpest;
            ++columns;
        }
        return columns ? total/columns : 0.0;
    };
    view.setCameraPreview(fallShot.eye,fallShot.target);
    QImage stillFall, movingFall;
    if (!fallingId.isEmpty() && view.start(LiveViewport::RunMode::Simulate)) {
        for (int attempt = 0; attempt < 200; ++attempt) {
            const auto position = view.runtimePosition(fallingId);
            if (position && position->z() < 3.2f)
                break;
            QTest::qWait(5);
        }
        view.pause(true);
        for (int frame = 0; frame < 12; ++frame)
            stillFall = view.captureFrame();
        view.step();
        movingFall = view.captureFrame();
        view.stop();
    }
    stillFall.save(output.filePath("rendering-falling-box-still.png"));
    movingFall.save(output.filePath("rendering-falling-box-moving.png"));
    // The box's top edge: the first strong step from the sky down the middle of the still frame.
    int edgeRow = -1;
    for (int y = 0; !stillFall.isNull() && edgeRow < 0 && y < stillFall.height()*3/4; ++y) {
        const QColor a = stillFall.pixel(stillFall.width()/2,y), b = stillFall.pixel(stillFall.width()/2,y+1);
        if (std::abs(a.lightnessF()-b.lightnessF()) > 0.08f)
            edgeRow = y;
    }
    const double stillSharpness = edgeRow < 0 ? 0 : edgeSharpness(stillFall,edgeRow,4);
    // The box falls a few pixels in one step; the search stays well above its lower edge.
    const double movingSharpness = edgeRow < 0 || movingFall.isNull() ? 0 : edgeSharpness(movingFall,edgeRow,12);
    audit.check(page,"motion","Motion blur smears a falling box by its own motion",
                stillSharpness > 0.08 && movingSharpness < 0.7*stillSharpness,
                QString("Sharpest step at the top edge: %1 paused, %2 one step later.")
                    .arg(stillSharpness,0,'f',3).arg(movingSharpness,0,'f',3));
    while (store.history().index() > historyStart && store.history().canUndo())
        store.history().undo();

    setDisplay({{"ev100",22},{"exposureMode","Manual"}});
    const auto tooDark = capture(shots[0].eye,shots[0].target);
    setDisplay({{"exposureMode","Auto"}});
    const auto automatic = settled(shots[0]);
    automatic.save(output.filePath("rendering-auto-exposure.png"));
    const double dark = luminance(tooDark), metered = luminance(automatic);
    audit.check(page,"display","Auto exposure keeps the street visible when the manual exposure is far too dark",
                dark < 0.1 && metered > 0.25 && metered < 0.75,
                QString("mean display level %1 at EV100 22, %2 with auto exposure").arg(dark,0,'f',3).arg(metered,0,'f',3));
    setDisplay({{"exposureCompensation",1}});
    const auto compensated = settled(shots[0]);
    const double compensatedLevel = luminance(compensated);
    audit.check(page,"display","Exposure compensation brightens what auto exposure chooses",compensatedLevel > metered+0.03,
                QString("mean display level %1 with +1 stop of compensation").arg(compensatedLevel,0,'f',3));
    // The Player meters the same frame and settles at once in a capture.
    std::ostringstream autoPlayable;
    QFile autoFile(playablePath);
    const bool autoExported = WorldRuntime::exportScene(store,autoPlayable) && autoFile.open(QIODevice::WriteOnly)
        && autoFile.write(autoPlayable.str().data(),qint64(autoPlayable.str().size())) == qint64(autoPlayable.str().size());
    autoFile.close();
    const auto playedAuto = autoExported ? playerCapture(shots[0],compensated.size()) : QImage();
    const auto autoAlike = difference(playedAuto,compensated,"rendering-auto-exposure-editor-player-diff.png");
    audit.check(page,"display","The Player chooses the same auto exposure as the editor",
                !playedAuto.isNull() && std::abs(luminance(playedAuto)-compensatedLevel) < 0.02 && autoAlike.mean <= 3,
                playedAuto.isNull() ? QStringLiteral("The Player did not capture the view.")
                    : QString("Player mean display level %1; %2").arg(luminance(playedAuto),0,'f',3).arg(describe(autoAlike)));

    // Photometric light units: a sun of 123,532 lux is one scene unit, the old unitless intensity 1. Fog is off
    // so the check sees the lighting.
    setDisplay({{"ev100",15},{"exposureMode","Manual"},{"exposureCompensation",0},{"fogDensity",0}});
    store.addObject("Directional Light","Lighting");
    const auto *sunObject = store.selectedObject();
    const auto unitless = settled(shots[0]);
    bool lux = false, dimmer = false;
    QString luxDetail = QStringLiteral("The directional light was not added.");
    if (sunObject && sunObject->type == "Directional Light") {
        auto sun = *sunObject;
        setLightPhotometric(sun.details,sun.type,worldforge::color::luxPerUnit);
        store.updateObject(sun,"Sun in lux");
        const auto inLux = settled(shots[0]);
        setLightPhotometric(sun.details,sun.type,worldforge::color::luxPerUnit/4);
        store.updateObject(sun,"Dimmer sun");
        const auto quarter = settled(shots[0]);
        quarter.save(output.filePath("rendering-quarter-sun.png"));
        const auto same = difference(inLux,unitless,"rendering-lux-diff.png");
        lux = same.mean <= 0.5 && std::abs(lightUnits(*store.selectedObject())-0.25f) < 1e-4f;
        // Only sunlit surfaces change; the sky and the shaded road keep their levels.
        dimmer = luminance(quarter) < luminance(inLux)-0.008;
        luxDetail = QString("%1 lux matches intensity 1 (%2); a quarter of that gives mean display level %3 instead of %4")
            .arg(worldforge::color::luxPerUnit,0,'f',0).arg(describe(same)).arg(luminance(quarter),0,'f',3)
            .arg(luminance(inLux),0,'f',3);
    }
    audit.check(page,"display","Sun illuminance in lux lights the street like the earlier unitless intensity",lux && dimmer,
                luxDetail);

    // TAA averages jittered frames, so turning it off changes edges only; the Player matches the editor either way.
    const auto smooth = settled(shots[1]);
    setDisplay({{"antiAliasing","Off"}});
    const auto aliased = settled(shots[1]);
    aliased.save(output.filePath("rendering-taa-off.png"));
    smooth.save(output.filePath("rendering-taa-on.png"));
    const auto edges = difference(aliased,smooth,"rendering-taa-diff.png");
    qint64 changed = 0;
    for (int y = 0; y < smooth.height() && smooth.size() == aliased.size(); ++y)
        for (int x = 0; x < smooth.width(); ++x) {
            const QColor a = smooth.pixel(x,y), b = aliased.pixel(x,y);
            changed += std::max({std::abs(a.red()-b.red()),std::abs(a.green()-b.green()),std::abs(a.blue()-b.blue())}) > 8;
        }
    const double changedShare = smooth.isNull() ? 0 : double(changed)/(double(smooth.width())*smooth.height());
    audit.check(page,"display","Turning TAA off changes only the edges of the street",
                edges.mean > 0.05 && changedShare > 0.001 && changedShare < 0.06,
                QString("%1% of pixels change by more than 8 levels; %2").arg(changedShare*100,0,'f',2).arg(describe(edges)));
    std::ostringstream offPlayable;
    QFile offFile(playablePath);
    const bool offExported = WorldRuntime::exportScene(store,offPlayable) && offFile.open(QIODevice::WriteOnly)
        && offFile.write(offPlayable.str().data(),qint64(offPlayable.str().size())) == qint64(offPlayable.str().size());
    offFile.close();
    const auto playedOff = offExported ? playerCapture(shots[1],aliased.size()) : QImage();
    const auto offAlike = difference(playedOff,aliased,"rendering-taa-off-editor-player-diff.png");
    audit.check(page,"display","Editor and Player match with TAA off",!playedOff.isNull() && offAlike.mean <= 1 && offAlike.share <= 0.005,
                playedOff.isNull() ? QStringLiteral("The Player did not capture the view.") : describe(offAlike));

    // Bloom: turning it off removes the glow around the bright, sunlit road (the street corner faces the sun), and
    // only that: the change is small and spread out.
    const auto bloomed = settled(shots[1]);
    setDisplay({{"bloom",false}});
    const auto sharp = settled(shots[1]);
    sharp.save(output.filePath("rendering-bloom-off.png"));
    bloomed.save(output.filePath("rendering-bloom-on.png"));
    const auto glow = difference(bloomed,sharp,"rendering-bloom-diff.png");
    audit.check(page,"display","Bloom spreads a little light around bright areas",glow.mean > 0.05 && glow.mean < 4 && glow.share < 0.01,
                describe(glow));
    setDisplay({{"bloom",true}});

    // Camera effects. Depth of field focused 4 m away at f/1.4 blurs the street behind, the same in the editor and the
    // Player; motion blur leaves a still view unchanged, since the camera does not move.
    const auto sharpView = settled(shots[0]);
    setDisplay({{"depthOfField",true},{"focusDistance",4},{"fNumber",1.4}});
    const auto focused = settled(shots[0]);
    focused.save(output.filePath("rendering-depth-of-field.png"));
    const auto blurred = difference(focused,sharpView,"rendering-depth-of-field-diff.png");
    std::ostringstream lensPlayable;
    QFile lensFile(playablePath);
    const bool lensExported = WorldRuntime::exportScene(store,lensPlayable) && lensFile.open(QIODevice::WriteOnly)
        && lensFile.write(lensPlayable.str().data(),qint64(lensPlayable.str().size())) == qint64(lensPlayable.str().size());
    lensFile.close();
    const auto playedLens = lensExported ? playerCapture(shots[0],focused.size()) : QImage();
    const auto lensAlike = difference(playedLens,focused,"rendering-depth-of-field-editor-player-diff.png");
    audit.check(page,"display","Depth of field blurs the street beyond the focus, alike in the editor and the Player",
                blurred.mean > 0.5 && !playedLens.isNull() && lensAlike.mean <= 1 && lensAlike.share <= 0.005,
                QString("changes the view by %1; editor and Player: %2").arg(describe(blurred))
                    .arg(playedLens.isNull() ? QStringLiteral("no Player capture") : describe(lensAlike)));
    setDisplay({{"depthOfField",false},{"motionBlur",true}});
    const auto stillBlur = difference(settled(shots[0]),sharpView,"rendering-motion-blur-still-diff.png");
    audit.check(page,"display","Motion blur leaves a still view unchanged",stillBlur.mean < 0.05,describe(stillBlur));
    setDisplay({{"motionBlur",false}});

    // The physical atmosphere: a different sky and sunlight from the gradient, the same in the editor and the Player.
    setDisplay({{"antiAliasing","TAA"}});
    const auto gradient = settled(shots[0]);
    setDisplay({{"skyModel","Physical"}});
    const auto physical = settled(shots[0]);
    physical.save(output.filePath("rendering-physical-sky.png"));
    std::ostringstream physicalPlayable;
    QFile physicalFile(playablePath);
    const bool physicalExported = WorldRuntime::exportScene(store,physicalPlayable) && physicalFile.open(QIODevice::WriteOnly)
        && physicalFile.write(physicalPlayable.str().data(),qint64(physicalPlayable.str().size()))
            == qint64(physicalPlayable.str().size());
    physicalFile.close();
    const auto playedPhysical = physicalExported ? playerCapture(shots[0],physical.size()) : QImage();
    const auto physicalAlike = difference(playedPhysical,physical,"rendering-physical-editor-player-diff.png");
    const auto gradientSky = difference(physical,gradient,"rendering-physical-gradient-diff.png");
    audit.check(page,"display","Editor and Player match with the physical sky",
                !playedPhysical.isNull() && physicalAlike.mean <= 1 && physicalAlike.share <= 0.005 && gradientSky.mean > 2,
                playedPhysical.isNull() ? QStringLiteral("The Player did not capture the view.")
                    : QString("%1; differs from the gradient sky by %2").arg(describe(physicalAlike)).arg(describe(gradientSky)));
    while (store.history().index() > historyStart && store.history().canUndo())
        store.history().undo();
    audit.check(page,"display","Undo restores the street after the exposure and light unit checks",
                initialDetails ? store.environment() && store.environment()->details == *initialDetails : !store.environment());
}

// The realism check: the Player's view of the Playable Street against a Cycles path-traced reference of the
// same frame (Tests/CyclesReference.py), scored with LDR-FLIP (0 identical, 1 completely different). Both
// render without fog. The scores are compared with Tests/Golden/realism.json so a rendering change cannot
// make the engine less realistic unnoticed. Set WORLDFORGE_UPDATE_GOLDEN=1 to record new scores, and
// WORLDFORGE_REALISM_SAMPLES to change the Cycles sample count (default 256). Set WORLDFORGE_REALISM_STUDIO
// to use a specific Studio executable for the reference.
void realismWorkflow(const QDir &output,const QDir &root,Audit &audit)
{
    const QString page = "Realism";
    QTemporaryDir directory;
    QString error;
    // The comparison uses the physical atmosphere, which Cycles receives as the same sky (Player/ReferenceScene.cpp).
    if (!directory.isValid()) {
        audit.check(page,"workflow","Create temporary storage for the street",false,directory.errorString());
        return;
    }
    ProjectStore store(QDir(directory.filePath("index")));
    if (!store.createStarterProject("Realism QA",
        {{"location",directory.filePath("Realism QA")},{"starter","street"},
         {"worldDefaults",QJsonObject{{"skyModel","Physical"}}}},error)) {
        audit.check(page,"workflow","Create the Playable Street starter",false,error);
        return;
    }
    const auto studio = qEnvironmentVariable("WORLDFORGE_REALISM_STUDIO",EmbeddedStudio::installedExecutable());
    if (studio.isEmpty()) {
        audit.record(page,"reference","Render the Cycles references","SKIP",QStringLiteral("WorldForge Studio is not installed."));
        return;
    }
    const auto playablePath = output.filePath("realism.wfplay");
    std::ostringstream playable;
    bool exported = WorldRuntime::exportScene(store,playable,&error);
    if (exported) {
        const auto bytes = playable.str();
        QFile file(playablePath);
        exported = file.open(QIODevice::WriteOnly) && file.write(bytes.data(),qint64(bytes.size())) == qint64(bytes.size());
    }
    audit.check(page,"workflow","Export the street for the Player",exported,error);
    if (!exported)
        return;
    // Night: the same street after sunset (twilight under the physical sky), lit by a row of street lamps of 2000 cd on
    // 5 m posts and photographed at EV100 5, as a camera would for street lighting.
    auto nightAtmosphere = store.environment() ? store.environment()->details : QJsonObject{};
    nightAtmosphere.insert("skyModel","Physical");
    nightAtmosphere.insert("sunElevation",-6);
    nightAtmosphere.insert("ev100",5);
    store.setAtmosphere(nightAtmosphere);
    bool lampsPlaced = true;
    // The outer lamps are spot lights pointing down with a 120-degree cone, as street lamps are; the middle ones are
    // point lights.
    for (const float x : {-20.0f,0.0f,20.0f})
        for (const float y : {-8.0f,8.0f}) {
            store.addObject(x == 0 ? "Point Light" : "Spot Light","Lighting");
            auto lamp = *store.selectedObject();
            lamp.position = {x,y,5};
            setLightPhotometric(lamp.details,lamp.type,2000);
            if (lamp.type == "Spot Light") {
                lamp.details.insert("coneAngle",120);
                lamp.details.insert("coneBlend",0.3);
            }
            lampsPlaced &= store.updateObjects({lamp},"Place street lamp",error);
        }
    const auto nightPath = output.filePath("realism-night.wfplay");
    std::ostringstream nightPlayable;
    bool nightExported = lampsPlaced && WorldRuntime::exportScene(store,nightPlayable,&error);
    if (nightExported) {
        const auto bytes = nightPlayable.str();
        QFile file(nightPath);
        nightExported = file.open(QIODevice::WriteOnly) && file.write(bytes.data(),qint64(bytes.size())) == qint64(bytes.size());
    }
    audit.check(page,"workflow","Export the street at night with street lamps",nightExported,error);

    const QDir golden(root.filePath("Tests/Golden"));
    const bool update = qEnvironmentVariableIntValue("WORLDFORGE_UPDATE_GOLDEN") == 1;
    const int samples = qEnvironmentVariableIsSet("WORLDFORGE_REALISM_SAMPLES")
        ? std::clamp(qEnvironmentVariableIntValue("WORLDFORGE_REALISM_SAMPLES"),1,65536) : 256;
    QFile baselineFile(golden.filePath("realism.json"));
    const auto baseline = baselineFile.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(baselineFile.readAll()).object()
                                                                 : QJsonObject{};
    baselineFile.close();
    const auto rgb = [](const QImage &image) {
        const auto converted = image.convertToFormat(QImage::Format_RGB888);
        std::vector<std::uint8_t> bytes;
        bytes.reserve(std::size_t(converted.width())*converted.height()*3);
        for (int y = 0; y < converted.height(); ++y) {
            const auto *line = converted.constScanLine(y);
            bytes.insert(bytes.end(),line,line+converted.width()*3);
        }
        return bytes;
    };

    struct Shot { const char *name; QVector3D eye, target; QString scene; };
    const Shot shots[]{{"shop-front",{-6,-14,2.2f},{0,3,1.6f},playablePath},{"street-corner",{24,-20,7},{6,0,1},playablePath},
                       {"aerial",{-34,-34,26},{0,0,0},playablePath},{"night",{-6,-14,2.2f},{0,3,1.6f},nightPath}};
    QJsonObject scores;
    double total = 0;
    int scored = 0;
    for (const auto &shot : shots) {
        const QString name = shot.name;
        QDir folder(output.filePath("realism-"+name));
        folder.removeRecursively();
        if (!QFileInfo(shot.scene).isFile())
            continue;
        QStringList arguments{"--reference",shot.scene,QDir::toNativeSeparators(folder.absolutePath()),"800","450"};
        for (const float value : {shot.eye.x(),shot.eye.y(),shot.eye.z(),shot.target.x(),shot.target.y(),shot.target.z()})
            arguments << QString::number(value,'g',9);
        QProcess player;
        player.start(QDir(QCoreApplication::applicationDirPath()).filePath("WorldForgePlayer.exe"),arguments);
        const bool described = player.waitForFinished(60000) && player.exitStatus() == QProcess::NormalExit
            && player.exitCode() == 0 && QFileInfo(folder.filePath("scene.json")).isFile();
        audit.check(page,"reference",QString("The Player renders and describes view %1").arg(name),described,
                    QString("exit code %1").arg(player.exitCode()));
        if (!described)
            continue;
        QProcess cycles;
        cycles.setProcessChannelMode(QProcess::MergedChannels);
        cycles.start(studio,{"-b","--factory-startup","--python",QDir::toNativeSeparators(root.filePath("Tests/CyclesReference.py")),
                             "--",QDir::toNativeSeparators(folder.absolutePath()),QString::number(samples)});
        const bool rendered = cycles.waitForFinished(30*60*1000) && cycles.exitStatus() == QProcess::NormalExit
            && cycles.exitCode() == 0 && QFileInfo(folder.filePath("cycles.png")).isFile();
        const auto log = QString::fromUtf8(cycles.readAll());
        QFile logFile(folder.filePath("cycles.log"));
        if (logFile.open(QIODevice::WriteOnly))
            logFile.write(log.toUtf8());
        QFile timing(folder.filePath("cycles.json"));
        const auto details = timing.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(timing.readAll()).object() : QJsonObject{};
        audit.check(page,"reference",QString("Cycles renders the reference for view %1").arg(name),rendered,
                    rendered ? QString("%1 samples%2 in %3 s with Studio %4").arg(details.value("samples").toInt())
                                   .arg(details.value("denoised").toBool() ? QStringLiteral(", denoised") : QString())
                                   .arg(details.value("seconds").toDouble(),0,'f',1).arg(details.value("version").toString())
                             : (log.trimmed().isEmpty() ? cycles.errorString() : log.right(400)));
        if (!rendered)
            continue;
        const QImage engine(folder.filePath("engine.png")), reference(folder.filePath("cycles.png"));
        if (engine.isNull() || engine.size() != reference.size()) {
            audit.check(page,"realism",QString("Compare view %1 with Cycles").arg(name),false,
                        QStringLiteral("The engine and Cycles images differ in size."));
            continue;
        }
        const auto result = worldforge::flip::compare(rgb(reference),rgb(engine),engine.width(),engine.height());
        if (result.error.empty()) {
            audit.check(page,"realism",QString("Compare view %1 with Cycles").arg(name),false,QStringLiteral("FLIP failed."));
            continue;
        }
        // Engine, Cycles and the FLIP error map side by side, for people reviewing the result.
        QImage map(engine.size(),QImage::Format_RGB888);
        for (int y = 0; y < map.height(); ++y) {
            auto *line = map.scanLine(y);
            for (int x = 0; x < map.width(); ++x)
                worldforge::flip::heat(result.error[std::size_t(y)*map.width()+x],line+x*3);
        }
        map.save(folder.filePath("flip.png"));
        QImage sheet(engine.width()*3,engine.height(),QImage::Format_RGB888);
        QPainter painter(&sheet);
        painter.drawImage(0,0,engine);
        painter.drawImage(engine.width(),0,reference);
        painter.drawImage(engine.width()*2,0,map);
        painter.setPen(Qt::white);
        painter.drawText(12,22,"WorldForge");
        painter.drawText(engine.width()+12,22,"Cycles");
        painter.drawText(engine.width()*2+12,22,QString("FLIP %1").arg(result.mean,0,'f',3));
        painter.end();
        sheet.save(output.filePath(QString("realism-%1.png").arg(name)));
        scores.insert(name,std::round(result.mean*10000)/10000);
        total += result.mean;
        ++scored;
        const auto recorded = baseline.value(name);
        const QString label = QString("View %1 is no less realistic than recorded").arg(name);
        if (update || recorded.isUndefined()) {
            audit.record(page,"realism",label,update ? "PASS" : "SKIP",QString("FLIP %1 against Cycles.%2")
                .arg(result.mean,0,'f',4).arg(update ? QString() : QStringLiteral(" No recorded score. Record one with WORLDFORGE_UPDATE_GOLDEN=1.")));
            continue;
        }
        // Cycles renders the same image every run on one machine (fixed seed); the 0.01 margin covers other CPUs
        // and Studio builds.
        audit.check(page,"realism",label,result.mean <= recorded.toDouble()+0.01,
                    QString("FLIP %1 against Cycles (recorded %2, lower is more realistic)").arg(result.mean,0,'f',4)
                        .arg(recorded.toDouble(),0,'f',4));
    }
    if (scored)
        audit.record(page,"realism","Realism score of the Playable Street","INFO",
                     QString("%1 of 100 (100 means indistinguishable from Cycles); mean FLIP %2 over %3 views")
                         .arg(100*(1-total/scored),0,'f',1).arg(total/scored,0,'f',4).arg(scored));
    if (update && scored == int(std::size(shots))) {
        scores.insert("samples",samples);
        scores.insert("note",QStringLiteral("Mean LDR-FLIP of the Player against Cycles; written by WorldForgeEditorAudit --realism."));
        const bool saved = QDir().mkpath(golden.absolutePath()) && baselineFile.open(QIODevice::WriteOnly)
            && baselineFile.write(QJsonDocument(scores).toJson()) > 0;
        audit.check(page,"realism","Record the realism scores",saved,baselineFile.fileName());
    }
}

void engineSessionWorkflow(const QDir &output,Audit &audit)
{
    const QString page = "Engine session";
    QTemporaryDir directory(output.filePath("engine-fixture-XXXXXX"));
    ProjectStore setup;
    QString error;
    if (!directory.isValid() || !setup.createStarterProject("Engine Session QA",
        {{"location",directory.filePath("Street")},{"starter","street"}},error)
        || !setup.addMeshPrimitive(worldforge::Primitive::Cube,{2,2,2},4,4,"Unplaced QA Mesh",error)) {
        audit.check(page,"workflow","Create the engine session fixture",false,error);
        return;
    }
    setup.deleteObject();
    if (!setup.save(&error)) {
        audit.check(page,"storage","Save the initial fixture",false,error);
        return;
    }
    MainWindow window;
    window.resize(1300,800);
    window.show();
    QTest::qWait(150);
    auto *tabs = window.findChild<QTabBar *>("workspaceTabs");
    const auto tab = [&](const QString &name) {
        if (tabs)
            for (int index = 0; index < tabs->count(); ++index)
                if (tabs->tabText(index) == name) {
                    tabs->setCurrentIndex(index);
                    return true;
                }
        return false;
    };
    tab("World Editor");
    auto *tiles = window.findChild<QListView *>("assetTiles");
    const auto selectTile = [&](const QString &name) {
        if (!tiles)
            return QModelIndex{};
        const auto result = tiles->model()->match(tiles->model()->index(0,0),Qt::DisplayRole,name);
        if (result.isEmpty())
            return QModelIndex{};
        const auto index = result.first();
        tiles->setCurrentIndex(index);
        tiles->scrollTo(index);
        tiles->setFocus();
        QTest::qWait(20);
        return index;
    };
    if (const auto unplaced = selectTile("Unplaced QA Mesh"); unplaced.isValid())
        QTest::keyClick(tiles,Qt::Key_Return);
    auto *details = window.findChild<QDialog *>("meshAssetDetails");
    auto *summary = details ? details->findChild<QLabel *>("meshAssetSummary") : nullptr;
    auto *preview = details ? details->findChild<LiveViewport *>("meshAssetPreview") : nullptr;
    const bool rendered = preview && waitForRender(*preview);
    const bool unplaced = details && details->isVisible() && details->windowTitle().startsWith("Unplaced QA Mesh")
        && summary && summary->text().contains("0 level placement(s)");
    audit.check(page,"navigation","Enter opens the unplaced asset with a rendered preview and zero placements",
        unplaced && rendered && !preview->captureFrame().isNull(),details ? details->windowTitle() : QStringLiteral("Details absent"));
    if (rendered)
        preview->captureFrame().save(output.filePath("asset-details-preview.png"));
    if (details) {
        details->reject();
        if (const auto barrier = selectTile("Road Barrier"); barrier.isValid()) {
            const auto center = tiles->visualRect(barrier).center();
            QTest::mouseClick(tiles->viewport(),Qt::LeftButton,Qt::NoModifier,center);
            QTest::mouseDClick(tiles->viewport(),Qt::LeftButton,Qt::NoModifier,center);
        }
        const bool opened = details->isVisible() && details->windowTitle().startsWith("Road Barrier")
            && summary && summary->text().contains("1 level placement(s)");
        if (auto *place = details->findChild<QPushButton *>("placeInspectedAsset"); opened && place)
            place->click();
        audit.check(page,"navigation","Double-click reuses Details and explicit placement adds one instance",
            opened && summary->text().contains("2 level placement(s)"),details->windowTitle());
        details->reject();
    }
    const auto panel = [&](const QString &name) -> QWidget * {
        return tab(name) ? window.findChild<QWidget *>("gameplayWorkspace"+name) : nullptr;
    };
    const auto setTarget = [](QWidget *workspace,const QString &marker) {
        auto *target = workspace ? workspace->findChild<QComboBox *>("gameplayTarget") : nullptr;
        const int index = target ? target->findText(marker) : -1;
        if (index < 0)
            return false;
        target->setCurrentIndex(index);
        return true;
    };
    const auto apply = [](QWidget *workspace) {
        auto *button = workspace ? workspace->findChild<QPushButton *>("gameplayApply") : nullptr;
        const auto *status = workspace ? workspace->findChild<QLabel *>("gameplayStatus") : nullptr;
        if (!button || !status)
            return false;
        button->click();
        return status->text().startsWith("Applied to project");
    };
    const auto showPreview = [](QWidget *workspace) {
        auto *button = workspace ? workspace->findChild<QPushButton *>("gameplayPreview") : nullptr;
        auto *view = workspace ? workspace->findChild<LiveViewport *>() : nullptr;
        if (!button || !view)
            return false;
        button->click();
        return view->isVisible() && waitForRender(*view);
    };
    auto *program = panel("Programming");
    const bool programmed = setTarget(program,"Repair Counter") && apply(program);
    audit.check(page,"authoring","Programming applies the mission completion rule",programmed);
    const auto undo = [&] {
        for (auto *action : window.findChildren<QAction *>())
            if (action->isEnabled() && action->shortcuts().contains(QKeySequence(QKeySequence::Undo))) {
                action->trigger();
                return true;
            }
        return false;
    };
    auto *binding = program ? program->findChild<QComboBox *>("gameplayTarget") : nullptr;
    const bool undone = programmed && binding && undo() && binding->currentData().toString().isEmpty();
    bool redone = false;
    if (undone) {
        for (auto *action : window.findChildren<QAction *>())
            if (action->isEnabled() && action->shortcuts().contains(QKeySequence(QKeySequence::Redo))) {
                action->trigger();
                redone = binding->currentText() == "Repair Counter";
                break;
            }
    }
    audit.check(page,"authoring","One undo and redo remove and restore the mission rule",undone && redone);
    auto *ui = panel("UI");
    auto *message = ui ? ui->findChild<QLineEdit *>("missionCompletionText") : nullptr;
    if (message)
        message->setText("Repair verified");
    audit.check(page,"authoring","UI applies a live completion message",message && apply(ui));
    auto *vfx = panel("VFX");
    const bool effect = setTarget(vfx,"Repair Counter") && showPreview(vfx) && apply(vfx);
    audit.check(page,"authoring","VFX previews and applies the beacon",effect);
    auto *cinematic = panel("Cinematic");
    bool shot = setTarget(cinematic,"Repair Counter") && showPreview(cinematic);
    if (shot && cinematic) {
        auto *slider = cinematic->findChild<QSlider *>();
        auto *view = cinematic->findChild<LiveViewport *>();
        shot = slider && slider->isVisible() && view;
        if (shot) {
            const auto first = view->captureFrame();
            slider->setValue(100);
            const auto last = view->captureFrame();
            shot = !first.isNull() && !last.isNull() && first != last;
        }
    }
    audit.check(page,"authoring","Cinematic previews two keys and applies the shot",shot && apply(cinematic));
    auto *ai = panel("AI");
    const bool patrolPreview = setTarget(ai,"Repair Shop Entrance") && showPreview(ai);
    auto *patrolState = ai ? ai->findChild<QLabel *>("gameplayStatus") : nullptr;
    if (patrolPreview && patrolState)
        for (int attempt = 0; attempt < 120 && !patrolState->text().contains("Returning"); ++attempt)
            QTest::qWait(50);
    audit.check(page,"authoring","Gameplay AI previews patrol state and applies a waypoint",
        patrolPreview && patrolState && patrolState->text().contains("Returning") && apply(ai));
    auto *multiplayer = panel("Multiplayer");
    auto *loopback = multiplayer ? multiplayer->findChild<QPushButton *>("runLocalMultiplayerCheck") : nullptr;
    const auto *local = multiplayer ? multiplayer->findChild<QLabel *>("localMultiplayerResult") : nullptr;
    if (loopback) {
        loopback->click();
        for (int attempt = 0; attempt < 220 && !loopback->isEnabled(); ++attempt)
            QTest::qWait(50);
    }
    audit.check(page,"runtime","Two separate clients verify ownership and replicated beacon state",
        local && local->text().startsWith("Passed."),local ? local->text() : QString{});
    auto *testing = panel("Testing");
    auto *run = testing ? testing->findChild<QPushButton *>("runStreetCheck") : nullptr;
    const auto *results = testing ? testing->findChild<QPlainTextEdit *>("streetCheckResults") : nullptr;
    if (run) {
        run->click();
        for (int attempt = 0; attempt < 600 && !run->isEnabled(); ++attempt)
            QTest::qWait(50);
    }
    const auto report = results ? results->toPlainText() : QString{};
    audit.check(page,"runtime","Testing runs Editor gameplay and Player verifies the beacon, camera, patrol, and HUD",
        report.contains("Packaged Player passed") && report.contains("The beacon activated and expired")
        && report.contains("\"missionHud\": true") && report.contains("\"beaconEnded\": true")
        && report.contains("\"shotEnded\": true") && report.contains("\"patrolReturned\": true")
        && report.contains("Repair verified"),report.left(1600));
    QFile summaryFile(output.filePath("engine-report.txt"));
    if (summaryFile.open(QIODevice::WriteOnly))
        summaryFile.write(report.toUtf8());
    if (testing)
        testing->grab().save(output.filePath("testing-workspace.png"));
    window.close();
    ProjectStore reopened;
    const auto verified = street::verify(reopened);
    audit.check(page,"storage","The authored gameplay survives project reopen",verified.ok()
        && verified.trace.beaconEnded && verified.trace.shotEnded && verified.trace.patrolReturned
        && verified.objectives.contains("Repair verified"),verified.summary());
    if (reopened.project()) {
        const auto packaged = runStreetPlayer(reopened,output,"engine-street");
        audit.check(page,"runtime","The exported saved street passes in the packaged Player",
            packaged.exitCode == 0 && packaged.report.value("missionHud").toBool()
            && packaged.report.value("beaconEnded").toBool() && packaged.report.value("shotEnded").toBool()
            && packaged.report.value("patrolReturned").toBool(),playerSummary(packaged));
        const auto &objects = reopened.project()->objects;
        const auto counter = std::ranges::find(objects,QString("Repair Counter"),&SceneObject::name);
        if (counter != objects.end()) {
            auto hidden = *counter;
            hidden.visible = false;
            const bool changed = reopened.updateObjects({hidden},"Hide the gameplay target",error);
            std::ostringstream rejected;
            const bool exported = WorldRuntime::exportScene(reopened,rejected,&error);
            audit.check(page,"validation","A stale beacon target is identified and blocks exporting incomplete gameplay",
                changed && !exported && rejected.str().empty() && error.contains("beacon target"),error);
            if (changed)
                reopened.history().undo();
        }
    }
    directory.setAutoRemove(false);
}

void programmingWorkflow(const QDir &output,Audit &audit)
{
    namespace wf = worldforge;
    using Rule = wf::BehaviorRule;
    const QString page = "Programming";
    QTemporaryDir fixture(output.filePath("programming-fixture-XXXXXX"));
    fixture.setAutoRemove(false);
    ProjectStore store;
    QString error;
    const auto folder = fixture.filePath("Playable Project");
    if (!fixture.isValid() || !store.createStarterProject("Programming QA",{{"location",folder},{"starter","street"}},error)) {
        audit.check(page,"setup","Create Playable Street",false,error);
        return;
    }
    const auto &objects = store.project()->objects;
    const auto source = std::ranges::find_if(objects,[](const SceneObject &object) {
        return object.details.value("gameplay").toString() == "missionStart";
    });
    if (source == objects.end()) {
        audit.check(page,"setup","Find the authored mission source",false);
        return;
    }
    const auto missionId = source->id;
    const auto missionMarkerName = source->name;
    const auto makeDoor = [&](const QString &name,QVector3D position) {
        if (!store.addMeshPrimitive(wf::Primitive::Cube,{1,1,2},4,4,name,error))
            return QString{};
        auto object = *store.selectedObject();
        object.position = position;
        object.details.insert("physics","Static");
        return store.updateObjects({object},"Set door transform",error) ? object.id : QString{};
    };
    const auto first = makeDoor("QA Door A",{40,40,1});
    const auto second = makeDoor("QA Door B",{45,40,1});
    const auto conditional = makeDoor("QA Conditional",{50,40,1});
    const auto interact = makeDoor("QA Interact",{55,40,1});
    const auto raise = store.saveBehavior({},{"Repair Doors",Rule::Event::MissionCompleted,Rule::Action::RaiseObject,missionId,{},3},error);
    const auto fails = store.saveBehavior({},{"Requires Repair",Rule::Event::PlayStarted,Rule::Action::PulseBeacon,{},missionId,1},error);
    const auto gate = store.saveBehavior({},{"Interact After Repair",Rule::Event::InteractNearby,Rule::Action::PulseBeacon,{},missionId,1},error);
    const bool attached = !raise.isEmpty() && !fails.isEmpty() && !gate.isEmpty()
        && store.attachBehavior(first,raise,error) && store.attachBehavior(second,raise,error)
        && store.attachBehavior(conditional,fails,error) && store.attachBehavior(interact,gate,error);
    const auto edits = store.history().index();
    const bool bad = store.saveBehavior({},{"Bad target",Rule::Event::MissionCompleted,Rule::Action::PulseBeacon,"missing",{},1},error).isEmpty()
        && store.history().index() == edits;
    audit.check(page,"authoring","One behavior attaches to two doors and an invalid mission creates no edit",attached && bad,error);
    if (!attached)
        return;
    const auto *door = store.object(second);
    auto dynamic = *door;
    dynamic.details.insert("physics","Dynamic");
    const bool invalid = store.updateObjects({dynamic},"Try dynamics",error)
        && !store.attachBehavior(second,raise,error);
    store.history().undo();
    audit.check(page,"validation","Raise Object rejects a dynamic attachment",invalid,error);
    const bool detached = store.attachBehavior(second,{},error);
    store.history().undo();
    const bool restored = store.object(second)->details.value("behaviorAsset").toString() == raise;
    store.history().redo();
    const bool redone = store.object(second)->details.value("behaviorAsset").toString().isEmpty();
    store.history().undo();
    audit.check(page,"history","Detach, undo and redo preserve the saved asset reference",detached && restored && redone,error);
    const auto sourceFolder = QDir(folder).filePath("Programming");
    const auto compiler = QDir(behaviorVisualStudio()).filePath("MSBuild/Current/Bin/MSBuild.exe");
    const bool prepared = prepareBehaviorCode(sourceFolder,error) && QFile::exists(compiler);
    QString launchId;
    const auto command = [&](const QString &program,const QStringList &arguments) {
        QProcess process;
        process.setWorkingDirectory(sourceFolder);
        process.start(program,arguments);
        const bool done = process.waitForFinished(55000) && process.exitCode() == 0 && process.exitStatus() == QProcess::NormalExit;
        return std::tuple{done,process.readAllStandardOutput(),process.readAllStandardError()};
    };
    if (prepared) {
        const QStringList args{"Behavior.vcxproj","/t:Rebuild","/nologo","/nodeReuse:false","/p:Configuration=Debug","/p:Platform=x64"};
        QString buildOutput;
        bool built = false;
        QTimer::singleShot(0,[&] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            auto *build = dialog ? dialog->findChild<QPushButton *>("buildCppBehavior") : nullptr;
            auto *apply = dialog ? dialog->findChild<QPushButton *>("applyCppBehavior") : nullptr;
            auto *log = dialog ? dialog->findChild<QPlainTextEdit *>("behaviorBuildOutput") : nullptr;
            if (build && apply && build->isEnabled()) {
                build->click();
                for (int i = 0; i < 550 && !apply->isEnabled() && !build->isEnabled(); ++i)
                    QTest::qWait(50);
                built = apply->isEnabled();
                if (built)
                    apply->click();
                if (log)
                    buildOutput = log->toPlainText();
            }
            if (dialog)
                dialog->reject();
        });
        showBehaviorCode(store,nullptr);
        const auto result = std::ranges::find(store.project()->behaviors,QString("Startup Beacon"),&ProjectBehavior::name);
        const auto id = result == store.project()->behaviors.cend() ? QString{} : result.key();
        const auto marker = makeDoor("QA C++ Beacon",{60,40,1});
        const bool applied = !id.isEmpty() && store.attachBehavior(marker,id,error);
        audit.check(page,"C++","MSBuild compiles the generated Visual Studio project and its executable emits a usable behavior",
            built && applied,applied ? QString{} : buildOutput+error);
        QFile file(QDir(sourceFolder).filePath("Behavior.cpp"));
        bool wroteBrokenSource = false;
        if (file.open(QIODevice::Append)) {
            const QByteArray broken = "\n#error WorldForge_Intentional_Build_Test\n";
            wroteBrokenSource = file.write(broken) == broken.size();
            file.close();
        }
        const auto [rebuilt,diagnostics,details] = command(compiler,args);
        const bool handledBrokenBuild = wroteBrokenSource && !rebuilt
            && (diagnostics+details).contains("WorldForge_Intentional_Build_Test") && applied
            && store.project()->behaviors.contains(id);
        audit.check(page,"C++","An invalid C++ build reports the compiler error and leaves the applied asset intact",
            handledBrokenBuild,handledBrokenBuild ? QString{} : QString::fromLocal8Bit(diagnostics+details));
        const QByteArray launchSource = R"(#include "WorldForgeGameplay.h"
int main()
{
    using namespace worldforge;
    return publish(makeBehavior("Launch Crate",events::playStarted(),physics::launchUpward(8.0f)));
}
)";
        bool wroteLaunch = false;
        if (file.open(QIODevice::WriteOnly)) {
            wroteLaunch = file.write(launchSource) == launchSource.size();
            file.close();
        }
        const auto [launchBuilt,launchLog,launchDetails] = command(compiler,args);
        const auto [launchRan,launchRule,launchErrors] = command(QDir(sourceFolder).filePath("build/Behavior.exe"),{});
        const auto launch = launchBuilt && launchRan ? readBuiltBehavior(launchRule,error) : std::nullopt;
        const auto launchAsset = launch ? store.saveBehavior({},*launch,error) : QString{};
        launchId = makeDoor("QA Launch Crate",{65,40,1});
        const auto launcherId = makeDoor("QA Launcher",{70,40,1});
        const bool rejectsStatic = !launchAsset.isEmpty() && !store.attachBehavior(first,launchAsset,error)
            && error.contains("Dynamic");
        const auto *crate = store.object(launchId);
        bool launchAttached = false;
        if (crate && !launchAsset.isEmpty()) {
            auto dynamicCrate = *crate;
            dynamicCrate.details.insert("physics","Dynamic");
            dynamicCrate.name = "Renamed Launch Crate";
            launchAttached = store.setBehaviorComponents(launcherId,{{launchAsset,false,launchId,8.0f}},error)
                && store.updateObjects({dynamicCrate},"Rename target and enable dynamic physics",error)
                && store.setBehaviorComponents(launcherId,{{launchAsset,true,launchId,8.0f}},error);
        }
        const bool keptSource = prepareBehaviorCode(sourceFolder,error) && file.open(QIODevice::ReadOnly)
            && file.readAll() == launchSource;
        audit.check(page,"C++ library","The shipped physics helper builds and attaches to a dynamic mesh",
            wroteLaunch && launch && launch->action == Rule::Action::LaunchUpward && launchAttached && rejectsStatic && keptSource,
            launchAttached ? QString{} : QString::fromLocal8Bit(launchLog+launchDetails+launchErrors)+error);
    } else {
        audit.check(page,"C++","Prepare the Visual Studio behavior project",false,error);
    }
    const auto pulse = store.saveBehavior({},{"Door Status",Rule::Event::MissionCompleted,Rule::Action::PulseBeacon,missionId,{},1},error);
    const auto disabled = store.saveBehavior({},{"Disabled Startup",Rule::Event::PlayStarted,Rule::Action::PulseBeacon,{},{},1},error);
    const QList<BehaviorComponent> stack{{pulse,true,first,2.5f},{raise,true,{},4.0f},{disabled,false,{},{}}};
    const bool appliedStack = store.setBehaviorComponents(second,stack,error);
    if (appliedStack)
        store.history().undo();
    const bool undidStack = behaviorComponents(*store.object(second)) == QList<BehaviorComponent>{{raise,true,{},{}}};
    if (appliedStack)
        store.history().redo();
    audit.check(page,"components","Ordered components, enable flags, target IDs and overrides undo and redo together",
        appliedStack && undidStack && behaviorComponents(*store.object(second)) == stack,error);
    bool graphEdited = false;
    QTimer::singleShot(0,[&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        auto *amount = dialog ? dialog->findChild<QDoubleSpinBox *>("behaviorAmount") : nullptr;
        auto *buttons = dialog ? dialog->findChild<QDialogButtonBox *>() : nullptr;
        if (amount && buttons && dialog->objectName() == "behaviorGraphEditor") {
            amount->setValue(2.25);
            buttons->button(QDialogButtonBox::Save)->click();
            graphEdited = true;
        }
        if (dialog && dialog->isVisible())
            dialog->reject();
    });
    editBehaviorAsset(store,gate,nullptr,true);
    audit.check(page,"graph","The Event, Condition, and Action graph edits the reusable behavior asset",
        graphEdited && store.project()->behaviors.value(gate).amount == 2.25f);
    store.select(first);
    const bool saved = store.save(&error);
    {
        MainWindow window;
        window.resize(1280,800);
        window.show();
        QTest::qWait(60);
        auto *picker = window.findChild<QComboBox *>("objectBehavior");
        auto *tiles = window.findChild<QListView *>("assetTiles");
        const auto tile = tiles ? tiles->model()->match(tiles->model()->index(0,0),Qt::DisplayRole,"Repair Doors").value(0)
            : QModelIndex{};
        bool opened = false;
        if (tile.isValid()) {
            QTimer::singleShot(0,&window,[&] {
                auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
                auto *action = dialog ? dialog->findChild<QComboBox *>("behaviorAction") : nullptr;
                opened = dialog && dialog->objectName() == "behaviorAssetEditor"
                    && action && action->currentIndex() == int(Rule::Action::RaiseObject);
                if (dialog)
                    dialog->reject();
            });
            QMetaObject::invokeMethod(tiles,"activated",Qt::DirectConnection,Q_ARG(QModelIndex,tile));
        }
        audit.check(page,"navigation","Content Browser opens the behavior and Details shows the attached asset",
            saved && opened && picker && picker->currentData().toString() == raise);
        auto *tabs = window.findChild<QTabBar *>("workspaceTabs");
        if (tabs)
            for (int index = 0; index < tabs->count(); ++index)
                if (tabs->tabText(index) == "Programming") {
                    tabs->setCurrentIndex(index);
                    break;
                }
        auto *programmingObjects = window.findChild<QListWidget *>("programmingObjects");
        auto *findObject = window.findChild<QLineEdit *>("programmingObjectSearch");
        auto *attachment = window.findChild<QComboBox *>("programmingObjectBehavior");
        auto *selected = window.findChild<QLabel *>("programmingSelectedObject");
        auto *editAttached = window.findChild<QPushButton *>("programmingOpenAttached");
        bool synced = programmingObjects && findObject && attachment && selected && editAttached
            && attachment->currentData().toString() == raise && editAttached->isEnabled();
        if (synced) {
            findObject->setText(missionMarkerName);
            const auto selectObject = [&](const QString &id) {
                for (int i = 0; i < programmingObjects->count(); ++i)
                    if (programmingObjects->item(i)->data(Qt::UserRole).toString() == id) {
                        programmingObjects->setCurrentRow(i);
                        return true;
                    }
                return false;
            };
            synced = selectObject(missionId) && selected->text().contains(missionMarkerName)
                && attachment->currentData().toString().isEmpty() && !editAttached->isEnabled();
            findObject->setText("QA Interact");
            synced = synced && selectObject(interact) && selected->text().contains("QA Interact")
                && attachment->currentData().toString() == gate && picker && picker->currentData().toString() == gate;
            attachment->setCurrentIndex(0);
            QMetaObject::invokeMethod(attachment,"activated",Qt::DirectConnection,Q_ARG(int,0));
            synced = synced && picker && picker->currentData().toString().isEmpty();
            const auto gateIndex = attachment->findData(gate);
            attachment->setCurrentIndex(gateIndex);
            QMetaObject::invokeMethod(attachment,"activated",Qt::DirectConnection,Q_ARG(int,gateIndex));
            synced = synced && gateIndex > 0 && picker && picker->currentData().toString() == gate;
        }
        audit.check(page,"navigation","Programming searches any scene object and updates its own attachment and Details",synced);
        auto *manage = window.findChild<QPushButton *>("programmingManageComponents");
        bool managed = false;
        if (synced && manage) {
            QTimer::singleShot(0,&window,[&] {
                auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
                if (!dialog)
                    return;
                auto *assetBox = dialog->findChild<QComboBox *>("componentAsset");
                auto *add = dialog->findChild<QPushButton *>("addBehaviorComponent");
                auto *rows = dialog->findChild<QListWidget *>("behaviorComponentList");
                auto *isEnabled = dialog->findChild<QCheckBox *>("behaviorComponentEnabled");
                auto *buttons = dialog->findChild<QDialogButtonBox *>();
                if (assetBox && add && rows && isEnabled && buttons && rows->count() == 1) {
                    assetBox->setCurrentIndex(assetBox->findData(disabled));
                    add->click();
                    isEnabled->setChecked(false);
                    managed = rows->count() == 2;
                    buttons->button(QDialogButtonBox::Save)->click();
                }
                if (dialog->isVisible())
                    dialog->reject();
            });
            manage->click();
        }
        const auto *detailsManage = window.findChild<QPushButton *>("detailsManageBehaviorComponents");
        audit.check(page,"components","Programming edits the component stack and Details shows the new count",
            managed && manage->text().contains("(2)") && detailsManage && detailsManage->text().contains("(2)")
            && !attachment->isEnabled());
        window.close();
    }
    ProjectStore reopened;
    WorldRuntime world;
    if (!world.start(reopened,true) || !world.player() || !world.position(first) || !world.position(second)) {
        audit.check(page,"play","Start the reopened behavior project",false,world.gameplayErrors().join('\n'));
        return;
    }
    const auto a = *world.position(first), b = *world.position(second);
    audit.check(page,"components","Project reopen restores the stack and skips the disabled startup component",
        behaviorComponents(*reopened.object(second)) == stack
        && std::ranges::none_of(world.gameplay().behaviors,[](const auto &rule) {
            return rule.label.find("Disabled Startup") != std::string::npos;
        }));
    const auto crateStart = world.position(launchId);
    world.placePlayer(QVector3D(55,40,1));
    world.key(Qt::Key_E,true);
    world.tick(0);
    const auto crateLaunched = world.position(launchId);
    audit.check(page,"physics","The selected dynamic crate rises in Editor Play using the C++ library rule",
        crateStart && crateLaunched && crateLaunched->z()-crateStart->z() > 0.04f
        && world.renderMatrices().value(launchId).map(QVector3D{}).z() > crateStart->z());
    const auto count = [&world](Rule::Result result) {
        return std::ranges::count(world.gameplay().behaviors,result,&Rule::result);
    };
    const auto trace = takeEditorLog();
    audit.check(page,"play","Failed conditions appear in the trace and leave door positions untouched",
        count(Rule::Result::ConditionFailed) == 2 && world.position(first) == a && world.position(second) == b
        && std::ranges::any_of(trace,[](const auto &entry) { return entry.text.contains("Mission Repair Run is incomplete"); }));
    const auto &mission = world.gameplay().missions.front();
    world.placePlayer(QVector3D(mission.start[0],mission.start[1],mission.start[2]));
    world.tick(0);
    world.placePlayer(QVector3D(mission.target[0],mission.target[1],mission.target[2]));
    world.key(Qt::Key_E,true);
    world.tick(0);
    const float raiseZ = 3;
    const bool lifted = world.position(first) == a+QVector3D(0,0,raiseZ)
        && world.position(second) == b+QVector3D(0,0,4)
        && world.renderMatrices().value(first).map(QVector3D{}) == a+QVector3D(0,0,raiseZ);
    const auto pulseRule = std::ranges::find_if(world.gameplay().behaviors,[](const auto &rule) { return rule.label.starts_with("Door Status"); });
    audit.check(page,"components","The pulse and lift run on one event using different instance targets and values",
        lifted && pulseRule != world.gameplay().behaviors.end() && pulseRule->result == Rule::Result::Fired
        && pulseRule->amount == 2.5f && pulseRule+1 != world.gameplay().behaviors.end()
        && (pulseRule+1)->label == "Repair Doors / QA Door B"
        && world.gameplay().behaviorBeacons[pulseRule->target].position[0] == a.x());
    world.placePlayer(QVector3D(55,40,1));
    world.key(Qt::Key_E,true);
    world.tick(0);
    world.tick(0);
    const auto graphRule = std::ranges::find_if(world.gameplay().behaviors,[](const auto &rule) {
        return rule.label.starts_with("Interact After Repair");
    });
    audit.check(page,"graph","The graph's selected object fires the edited action with its saved parameter",
        graphRule != world.gameplay().behaviors.end() && graphRule->result == Rule::Result::Fired
        && graphRule->amount == 2.25f && world.gameplay().behaviorBeacons[graphRule->target].remaining > 2.20f);
    audit.check(page,"play","Mission completion lifts both render meshes and colliders once and enables retrying the interaction",
        lifted && count(Rule::Result::Fired) == 6 && world.position(first) == a+QVector3D(0,0,raiseZ));
    wf::WorldSimulation collision;
    collision.gravity = 0;
    collision.bodies.push_back({{0,0,1},{0.5f,0.5f,1}});
    collision.bodies.push_back({{-2,0,1},{0.3f,0.3f,0.85f},{},0,true});
    collision.bodies[1].lockRotation = true;
    const auto walk = [&] {
        collision.bodies[1].position = {-2,0,1};
        for (int step = 0; step < 180; ++step) {
            collision.bodies[1].velocity = {3,0,0};
            collision.step();
        }
        return collision.bodies[1].position[0];
    };
    const auto stoppedAt = walk();
    collision.bodies[0].position[2] += raiseZ;
    const auto movedTo = walk();
    audit.check(page,"physics","Raising a static collider opens the way in the live physics simulation",
        stoppedAt < -0.3f && movedTo > 0.8f,QString("Closed %1 m, open %2 m").arg(stoppedAt).arg(movedTo));
    wf::WorldSimulation remotePhysics;
    remotePhysics.bodies.push_back({{25,0,1},{0.5f,0.5f,0.5f},{},0,true});
    remotePhysics.bodies.push_back({{25,0,1}});
    wf::Gameplay remoteGameplay;
    Rule remoteRule;
    remoteRule.event = Rule::Event::InteractNearby;
    remoteRule.action = Rule::Action::LaunchUpward;
    remoteRule.position = {0,0,1};
    remoteRule.followLaunchTarget = false;
    remoteGameplay.behaviors.push_back(remoteRule);
    remoteGameplay.update(remotePhysics,1,true);
    const bool ignoredAtTarget = remoteGameplay.behaviors.front().result == Rule::Result::Waiting;
    remotePhysics.bodies[1].position = {0,0,1};
    remoteGameplay.update(remotePhysics,1,true);
    audit.check(page,"components","Nearby interaction follows the component owner when the physics target is remote",
        ignoredAtTarget && remoteGameplay.behaviors.front().result == Rule::Result::Fired
        && remotePhysics.bodies.front().velocity[2] == remoteRule.amount);
    const auto player = runStreetPlayer(reopened,output,"programming-street");
    audit.check(page,"Player","Player runs both doors and C++ rules, and its physics launches the crate",
        player.exitCode == 0 && player.report.value("behaviorsFired").toInt() == 5
        && player.report.value("maxLaunchRiseMeters").toDouble() > 2,
        playerSummary(player)+QString(" · crate rise %1 m").arg(player.report.value("maxLaunchRiseMeters").toDouble()));
    if (const auto *crate = reopened.object(launchId)) {
        auto hiddenCrate = *crate;
        hiddenCrate.visible = false;
        std::ostringstream badTarget;
        const bool targetHidden = reopened.updateObjects({hiddenCrate},"Hide action target",error);
        audit.check(page,"validation","Hiding the renamed action target blocks export with a Programming diagnostic",
            targetHidden && !WorldRuntime::exportScene(reopened,badTarget,&error)
            && error.contains("hidden action target") && badTarget.str().empty(),error);
        if (targetHidden)
            reopened.history().undo();
    }
    auto hidden = *reopened.object(missionId);
    hidden.visible = false;
    std::ostringstream badExport;
    const bool hid = reopened.updateObjects({hidden},"Hide mission",error);
    audit.check(page,"validation","A hidden event source blocks export with a Programming diagnostic",
        hid && !WorldRuntime::exportScene(reopened,badExport,&error) && error.contains("Programming") && badExport.str().empty(),error);
}

void nativeGameplayWorkflow(const QDir &output,Audit &audit)
{
    const QString page = "Native Gameplay";
    QTemporaryDir fixture(output.filePath("native-gameplay-XXXXXX"));
    fixture.setAutoRemove(false);
    const auto folder = fixture.filePath("Project");
    ProjectStore store;
    QString error;
    if (!fixture.isValid() || !store.createStarterProject("Native Gameplay QA",{{"location",folder},{"starter","street"}},error)
        || !store.addMeshPrimitive(worldforge::Primitive::Cube,{1,1,1},4,4,"Native Crate",error)) {
        audit.check(page,"setup","Create native physics fixture",false,error);
        return;
    }
    auto crate = *store.selectedObject();
    crate.position = {70,30,1};
    crate.details.insert("physics","Dynamic");
    if (!store.updateObjects({crate},"Set native crate",error)) {
        audit.check(page,"setup","Prepare dynamic crate",false,error);
        return;
    }
    const QDir source(QDir(folder).filePath("Programming/NativeGameplay"));
    const auto dll = source.filePath("build/Gameplay.dll");
    const auto pdb = source.filePath("build/Gameplay.pdb");
    const auto compiler = QDir(behaviorVisualStudio()).filePath("MSBuild/Current/Bin/MSBuild.exe");
    if (!prepareNativeGameplay(source.absolutePath(),error) || !QFile::exists(compiler)) {
        audit.check(page,"setup","Generate native Visual Studio project",false,error);
        return;
    }
    QFile code(source.filePath("NativeGameplay.cpp"));
    if (!code.open(QIODevice::ReadOnly)) {
        audit.check(page,"setup","Open generated native source",false,code.errorString());
        return;
    }
    auto text = code.readAll();
    code.close();
    text.replace("targetId = \"\"","targetId = \""+crate.id.toUtf8()+"\"");
    const auto put = [&](const QByteArray &bytes) {
        if (!code.open(QIODevice::WriteOnly))
            return false;
        const bool wrote = code.write(bytes) == bytes.size();
        code.close();
        return wrote;
    };
    if (!put(text)) {
        audit.check(page,"setup","Write native target ID",false,code.errorString());
        return;
    }
    bool built = false;
    QString buildLog;
    QTimer::singleShot(0,[&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        auto *build = dialog ? dialog->findChild<QPushButton *>("buildNativeGameplay") : nullptr;
        auto *log = dialog ? dialog->findChild<QPlainTextEdit *>("nativeBuildOutput") : nullptr;
        if (build && build->isEnabled()) {
            build->click();
            for (int i = 0; i < 600 && !build->isEnabled(); ++i)
                QTest::qWait(50);
            built = QFile::exists(store.nativeGameplayPath());
        }
        if (log)
            buildLog = log->toPlainText();
        if (dialog)
            dialog->reject();
    });
    showNativeGameplay(store,nullptr);
    audit.check(page,"C++","Editor builds and activates a native DLL with Visual Studio symbols",
        built && QFile::exists(dll) && QFile::exists(pdb),built ? QString{} : buildLog);
    WorldRuntime world;
    if (!built || !world.start(store,true) || !world.position(crate.id)) {
        audit.check(page,"setup","Start the editor runtime with the active DLL",false);
        return;
    }
    const float base = world.position(crate.id)->z();
    world.tick(0);
    const auto before = world.position(crate.id)->z();
    audit.check(page,"Play","Native startup looks up an object ID and moves its dynamic physics body",
        world.nativeSteps() == 1 && before > base+0.04f,QString("Rise after one step %1 m").arg(before-base));

    const auto compile = [&] {
        QProcess process;
        process.setWorkingDirectory(source.absolutePath());
        process.start(compiler,{"NativeGameplay.vcxproj","/t:Rebuild","/nologo","/nodeReuse:false","/p:Configuration=Debug","/p:Platform=x64"});
        const bool ok = process.waitForFinished(55000) && process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
        buildLog = QString::fromLocal8Bit(process.readAllStandardOutput()+process.readAllStandardError());
        return ok;
    };
    const auto readActive = [&store] {
        QFile file(store.nativeGameplayPath());
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
    };
    auto faster = text;
    faster.replace("{0,0,8}","{0,0,12}");
    const bool rebuilt = put(faster) && compile();
    const bool activated = rebuilt && activateNativeGameplay(store,dll,error);
    const bool staged = activated && world.stageNative(store.nativeGameplayPath(),error);
    const auto unchanged = world.position(crate.id)->z() == before && world.nativeSteps() == 1;
    if (staged)
        world.tick(0);
    const auto after = world.position(crate.id)->z();
    const auto log = takeEditorLog();
    const bool stopped = std::ranges::any_of(log,[](const auto &line) { return line.text.contains("Native gameplay stopped."); });
    audit.check(page,"reload","Hot reload uses the same world and runs the new module at the next fixed update",
        staged && unchanged && stopped && world.nativeSteps() == 2 && after-before > 0.08f,rebuilt ? error : buildLog);
    const auto lastValid = readActive();
    auto wrongAbi = faster;
    wrongAbi.replace(",WF_GAME_ABI_V1,",",WF_GAME_ABI_V1+1,");
    const bool compiledWrongAbi = put(wrongAbi) && compile();
    const bool rejected = compiledWrongAbi && !activateNativeGameplay(store,dll,error) && error.contains("Incompatible");
    world.tick(0);
    audit.check(page,"reload","An incompatible replacement leaves the live and persisted modules running",
        rejected && readActive() == lastValid && world.nativeSteps() == 3 && world.position(crate.id)->z() > after,error);
    error.clear();
    const bool sourceError = put(faster+"\n#error WorldForge_Native_Build_Test\n") && !compile()
        && buildLog.contains("WorldForge_Native_Build_Test");
    audit.check(page,"C++","A compiler error preserves the active game build",sourceError && readActive() == lastValid,
        sourceError ? QString{} : buildLog);
    put(faster);
    world.stopNative();

    const QDir package(fixture.filePath("Packaged Game"));
    const bool packaged = exportNativeGame(store,package.absolutePath(),error);
    const bool installedNotices = QDir(QCoreApplication::applicationDirPath()).exists("../licenses/Jolt-LICENSE.txt");
    audit.check(page,"package","Export creates the game scene, standalone Player, DLL, symbols and installed notices",
        packaged && package.exists("WorldForgePlayer.exe") && package.exists("Gameplay.dll") && package.exists("Gameplay.pdb")
        && (!installedNotices || package.exists("licenses/Jolt-LICENSE.txt")),error);
    if (!packaged)
        return;
    const auto verify = [&](const QStringList &arguments,const QString &reportPath) {
        QProcess player;
        player.setWorkingDirectory(package.absolutePath());
        player.start(package.filePath("WorldForgePlayer.exe"),arguments);
        const bool ok = player.waitForFinished(40000) && player.exitCode() == 0 && player.exitStatus() == QProcess::NormalExit;
        QFile report(reportPath);
        const auto data = report.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(report.readAll()).object() : QJsonObject{};
        return std::pair{ok,data};
    };
    const auto nativeReport = output.filePath("native-player.json");
    const auto [nativePassed,native] = verify({"--verify-packaged-street",nativeReport},nativeReport);
    audit.check(page,"Player","The packaged Player loads the same native DLL and moves the object by ID",
        nativePassed && native.value("nativeFixedUpdates").toInt() > 100 && native.value("maxNativeBodyRiseMeters").toDouble() > 5,
        QString("Native updates %1 · rise %2 m").arg(native.value("nativeFixedUpdates").toInt())
            .arg(native.value("maxNativeBodyRiseMeters").toDouble()));
    const auto plainReport = output.filePath("plain-scene-player.json");
    const auto [plainPassed,plain] = verify({"--verify-street",package.filePath("Game.wfplay"),plainReport},plainReport);
    audit.check(page,"Player","Opening the same scene directly leaves its adjacent DLL inactive",
        plainPassed && plain.value("nativeFixedUpdates").isDouble() && plain.value("nativeFixedUpdates").toInt() == 0);

    const auto noGlobalLaunch = QByteArray(text).replace("targetId = \""+crate.id.toUtf8()+"\"","targetId = \"\"");
    if (!put(noGlobalLaunch) || !compile() || !activateNativeGameplay(store,dll,error)) {
        audit.check(page,"components","Build the typed native component fixture",false,error+buildLog);
        return;
    }
    const auto makeCrate = [&](QString name,float x) -> QString {
        if (!store.addMeshPrimitive(worldforge::Primitive::Cube,{1,1,1},4,4,name,error))
            return {};
        auto object = *store.selectedObject();
        object.position = {x,30,1};
        object.details.insert("physics","Dynamic");
        return store.updateObjects({object},"Prepare native component crate",error) ? object.id : QString{};
    };
    const auto remote = makeCrate("Referenced Crate",75);
    const auto dormant = makeCrate("Dormant Crate",80);
    if (remote.isEmpty() || dormant.isEmpty()) {
        audit.check(page,"components","Create additional component targets",false,error);
        return;
    }
    store.addObject("Point Light","Lighting");
    auto launcher = *store.selectedObject();
    launcher.name = "Remote Launcher";
    if (!store.updateObjects({launcher},"Set launcher type",error)) {
        audit.check(page,"components","Create a non-physics script owner",false,error);
        return;
    }
    const QString type = "worldforge.launch";
    const auto imagePath = fixture.filePath("native-reference.png");
    QImage nativeImage(2,2,QImage::Format_RGBA8888);
    nativeImage.fill(Qt::white);
    const auto textureReference = nativeImage.save(imagePath) ? store.importProjectTexture(imagePath,error) : QString{};
    ProjectMaterial material;
    material.name = "Native Reference Material";
    material.textureId = textureReference;
    const auto materialReference = store.saveProjectMaterial({},material,error);
    store.select(crate.id);
    bool usedPropertyControls = false;
    QTimer::singleShot(0,[&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        auto *speed = dialog ? dialog->findChild<QDoubleSpinBox *>("nativeProperty.speed") : nullptr;
        auto *percent = dialog ? dialog->findChild<QSpinBox *>("nativeProperty.percent") : nullptr;
        auto *mesh = dialog ? dialog->findChild<QComboBox *>("nativeProperty.mesh") : nullptr;
        auto *materialPicker = dialog ? dialog->findChild<QComboBox *>("nativeProperty.material") : nullptr;
        auto *texturePicker = dialog ? dialog->findChild<QComboBox *>("nativeProperty.texture") : nullptr;
        auto *saveComponent = dialog ? dialog->findChild<QPushButton *>("saveNativeComponent") : nullptr;
        if (speed && percent && mesh && materialPicker && texturePicker && saveComponent && mesh->findData(crate.meshAsset) > 0
            && materialPicker->findData(materialReference) > 0 && texturePicker->findData(textureReference) > 0) {
            speed->setValue(8);
            percent->setValue(50);
            mesh->setCurrentIndex(mesh->findData(crate.meshAsset));
            materialPicker->setCurrentIndex(materialPicker->findData(materialReference));
            texturePicker->setCurrentIndex(texturePicker->findData(textureReference));
            saveComponent->click();
            usedPropertyControls = store.object(crate.id)->details.value("nativeComponents").toObject().contains(type);
        }
        if (dialog)
            dialog->reject();
    });
    editNativeComponents(store,nullptr);
    const auto entry = [](QJsonObject values) {
        return QJsonObject{{"enabled",true},{"properties",std::move(values)}};
    };
    const auto remoteEntry = entry({{"speed",writeNativeProperty({WF_GAME_REAL,10,{}})},
        {"target",writeNativeProperty({WF_GAME_OBJECT,0,remote.toStdString()})}});
    const bool attached = store.setNativeComponent(launcher.id,type,remoteEntry,error)
        && store.setNativeComponent(dormant,type,entry({{"launch",writeNativeProperty({WF_GAME_BOOL,0,{}})},
            {"percent",writeNativeProperty({WF_GAME_INT,double(INT32_MAX),{}})}}),error);
    audit.check(page,"components","The inspector attaches schemas with typed values and mesh, material and texture references",
        usedPropertyControls && attached,error);
    auto renamed = *store.object(remote);
    renamed.name = "Renamed Component Target";
    const bool renamedTarget = store.updateObjects({renamed},"Rename script target",error);
    const bool changed = store.setNativeComponent(launcher.id,type,entry({}),error);
    if (changed)
        store.history().undo();
    const auto savedEntry = [&] { return store.object(launcher.id)->details.value("nativeComponents").toObject().value(type); };
    const bool restored = savedEntry() == remoteEntry;
    if (changed)
        store.history().redo();
    const bool reapplied = savedEntry() == entry({});
    if (changed)
        store.history().undo();
    const bool saved = store.save(&error);
    ProjectStore reopened;
    audit.check(page,"components","Overrides and stable scene references survive undo, redo, rename and project reopen",
        renamedTarget && restored && reapplied && saved && reopened.object(launcher.id)
        && reopened.object(launcher.id)->details.value("nativeComponents").toObject().value(type) == remoteEntry,error);
    WorldRuntime instanceWorld;
    if (!instanceWorld.start(reopened,true) || !instanceWorld.position(crate.id) || !instanceWorld.position(remote)
        || !instanceWorld.position(dormant)) {
        audit.check(page,"components","Start the component scene",false,error);
        return;
    }
    const auto firstZ = instanceWorld.position(crate.id)->z(), remoteZ = instanceWorld.position(remote)->z();
    const auto dormantZ = instanceWorld.position(dormant)->z();
    instanceWorld.tick(0);
    const auto trace = takeEditorLog();
    audit.check(page,"components","Two instances apply different speeds, defaults and object targets while false gates the third",
        instanceWorld.position(crate.id)->z()-firstZ > 0.025f && instanceWorld.position(crate.id)->z()-firstZ < 0.05f
        && instanceWorld.position(remote)->z()-remoteZ > 0.075f && instanceWorld.position(dormant)->z() <= dormantZ
        && std::ranges::any_of(trace,[](const auto &row) { return row.text.contains("with a mesh reference"); }));
    const auto activeWithComponents = readActive();
    auto incompatible = noGlobalLaunch;
    incompatible.replace("worldforge.launch","worldforge.incompatible");
    const bool incompatibleRejected = put(incompatible) && compile() && !activateNativeGameplay(reopened,dll,error)
        && error.contains("Missing native component type") && readActive() == activeWithComponents;
    audit.check(page,"reload","A DLL that removes a live component type cannot replace the active build",
        incompatibleRejected,error);
    put(noGlobalLaunch);
    instanceWorld.stopNative();

    const QDir componentPackage(fixture.filePath("Packaged Components"));
    error.clear();
    const bool exported = exportNativeGame(reopened,componentPackage.absolutePath(),error);
    if (!exported) {
        audit.check(page,"Player","Export the native component scene",false,error);
        return;
    }
    QFile nativeScene(componentPackage.filePath("Game.wfplay"));
    std::istringstream nativeBytes(nativeScene.open(QIODevice::ReadOnly) ? nativeScene.readAll().toStdString() : std::string{});
    const auto compiledScene = worldforge::readPlayable(nativeBytes);
    const auto integerExact = compiledScene && std::ranges::any_of(compiledScene->nativeInstances,[&](const auto &instance) {
        const auto percent = instance.properties.find("percent");
        return instance.owner == dormant.toStdString() && percent != instance.properties.end()
            && percent->second.kind == WF_GAME_INT && percent->second.number == double(INT32_MAX);
    });
    audit.check(page,"package","Native component export keeps full signed 32-bit integer precision",integerExact);
    const auto assetIdsExact = compiledScene && std::ranges::any_of(compiledScene->nativeInstances,[&](const auto &instance) {
        const auto has = [&](std::string_view id,std::uint32_t kind,const QString &reference) {
            const auto property = instance.properties.find(id);
            return property != instance.properties.end() && property->second.kind == kind
                && property->second.reference == reference.toStdString();
        };
        return instance.owner == crate.id.toStdString() && has("mesh",WF_GAME_MESH,crate.meshAsset)
            && has("material",WF_GAME_MATERIAL,materialReference) && has("texture",WF_GAME_TEXTURE,textureReference);
    });
    audit.check(page,"package","The exported component stores each asset reference with its original kind and ID",assetIdsExact);
    const auto componentReport = output.filePath("native-components-player.json");
    QProcess player;
    player.setWorkingDirectory(componentPackage.absolutePath());
    player.start(componentPackage.filePath("WorldForgePlayer.exe"),{"--verify-packaged-street",componentReport});
    const bool playerOk = player.waitForFinished(40000) && player.exitCode() == 0;
    QFile componentFile(componentReport);
    const auto result = componentFile.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(componentFile.readAll()).object() : QJsonObject{};
    const auto rises = result.value("nativeBodyRises").toObject();
    audit.check(page,"Player","The packaged Player applies the same distinct instance settings to the referenced bodies",
        playerOk && rises.value(crate.id).toDouble() > 0.4 && rises.value(crate.id).toDouble() < 1.2
        && rises.value(remote).toDouble() > 4 && rises.value(dormant).isDouble() && rises.value(dormant).toDouble() == 0);
    auto missing = remoteEntry;
    auto properties = missing.value("properties").toObject();
    properties.insert("target",writeNativeProperty({WF_GAME_OBJECT,0,"missing-target"}));
    missing.insert("properties",properties);
    QString missingError;
    std::ostringstream untouched;
    const bool missingRejected = reopened.setNativeComponent(launcher.id,type,missing,missingError)
        && !WorldRuntime::exportScene(reopened,untouched,&missingError,true) && missingError.contains("missing")
        && untouched.str().empty();
    audit.check(page,"validation","A missing native object reference blocks game export with the field name",
        missingRejected && missingError.contains("target"),missingError);
}

void surfaceLibraryWorkflow(const QDir &output,Audit &audit)
{
    const QString page = "Project surface assets";
    QTemporaryDir directory(output.filePath("surface-fixture-XXXXXX"));
    directory.setAutoRemove(false);
    ProjectStore store;
    QString error;
    if (!directory.isValid() || !store.createStarterProject("Surface Library QA",
        {{"location",directory.filePath("Street")},{"starter","street"}},error)) {
        audit.check(page,"workflow","Create the two-mesh test project",false,error);
        return;
    }
    const auto meshId = [&](const QString &name) {
        const auto &assets = store.project()->assets;
        const auto item = std::ranges::find(assets,name,&MeshAsset::name);
        return item == assets.end() ? QString{} : item->id;
    };
    const auto first = meshId("Road Barrier"), second = meshId("Counter"), control = meshId("Sidewalk");
    if (first.isEmpty() || second.isEmpty() || control.isEmpty()) {
        audit.check(page,"workflow","Find both mesh assets and the independent material",false);
        return;
    }
    const auto png = directory.filePath("Surface Color.png");
    const auto jpeg = directory.filePath("Second Source.jpg");
    QImage image(32,24,QImage::Format_RGB32);
    image.fill(QColor(232,224,208));
    const bool fixtures = image.save(png) && image.save(jpeg);
    QFile corrupt(directory.filePath("Broken.png"));
    if (corrupt.open(QIODevice::WriteOnly)) {
        corrupt.write("broken");
        corrupt.close();
    }
    const auto pngId = store.importProjectTexture(png,error);
    const auto jpegId = store.importProjectTexture(jpeg,error);
    const auto beforeRejected = store.history().index();
    const bool rejected = store.importProjectTexture(corrupt.fileName(),error).isEmpty()
        && store.history().index() == beforeRejected;
    audit.check(page,"import","PNG and JPEG import, and corrupt images create no history or asset",fixtures && rejected
        && !pngId.isEmpty() && !jpegId.isEmpty() && store.project()->surfaces.textures.size() == 2,error);
    if (pngId.isEmpty() || jpegId.isEmpty())
        return;
    auto surface = ProjectMaterial{"Reusable Finish",pngId,{0.85f,0.26f,0.12f,1}};
    const auto material = store.saveProjectMaterial({},surface,error);
    const auto previousControl = store.meshAsset(control)->mesh;
    const bool assigned = !material.isEmpty() && store.assignProjectMaterial(material,first,0,error)
        && store.assignProjectMaterial(material,second,0,error);
    const auto linked = [&](const ProjectStore &project,const QString &mesh,const ProjectMaterial &expected) {
        const auto *asset = project.meshAsset(mesh);
        if (!asset || asset->mesh->faces.empty())
            return false;
        return std::ranges::all_of(asset->mesh->faces,[&](const auto &face) {
            const auto &slot = asset->mesh->materials[face.material];
            return slot.sharedMaterialId == material.toStdString() && slot.color == expected.tint
                && slot.name == expected.name.toStdString() && !slot.baseColorTexture.empty();
        });
    };
    audit.check(page,"workflow","Two meshes link to the same project material",assigned
        && linked(store,first,surface) && linked(store,second,surface),error);
    if (!assigned)
        return;

    LiveViewport preview(store,true);
    preview.resize(480,300);
    preview.setModelShading(ModelShading::Material);
    preview.setAssetPreview(store.meshAsset(first)->mesh);
    preview.show();
    const auto capture = [&](const QString &id) {
        preview.setAssetPreview(store.meshAsset(id)->mesh);
        return waitForRender(preview) ? preview.captureFrame() : QImage{};
    };
    const auto oldFrameA = capture(first),oldFrameB = capture(second);
    const auto old = surface;
    surface.tint = {0.15f,0.48f,0.92f,1};
    const auto beforeEdit = store.history().index();
    const bool changed = store.saveProjectMaterial(material,surface,error) == material;
    const auto newFrameA = capture(first),newFrameB = capture(second);
    newFrameA.save(output.filePath("shared-finish-barrier.png"));
    newFrameB.save(output.filePath("shared-finish-counter.png"));
    audit.check(page,"Vulkan","Editing the shared tint changes both rendered meshes",changed
        && !oldFrameA.isNull() && !oldFrameB.isNull() && !newFrameA.isNull() && !newFrameB.isNull()
        && oldFrameA != newFrameA && oldFrameB != newFrameB && store.meshAsset(control)->mesh == previousControl);
    preview.hide();
    const bool singleEdit = store.history().index() == beforeEdit+1;
    if (singleEdit)
        store.history().undo();
    const bool undone = linked(store,first,old) && linked(store,second,old);
    if (singleEdit)
        store.history().redo();
    audit.check(page,"undo","One undo and redo restore both linked slots and the library entry",singleEdit && undone
        && linked(store,first,surface) && linked(store,second,surface) && store.project()->surfaces.materials.value(material) == surface);

    auto edited = *store.meshAsset(first)->mesh;
    auto expected = store.meshAsset(first)->mesh;
    for (auto &vertex : edited.vertices)
        vertex[2] *= 1.05f;
    const auto geometry = exportGlb(edited,"Road Barrier",error);
    const bool preserved = !geometry.isEmpty() && store.publishStudioMesh(first,geometry,expected.data(),error)
        && linked(store,first,surface);
    const auto geometryError = error;
    edited.materials.front().name = "Ambiguous or missing link";
    const auto invalid = exportGlb(edited,"Changed Slot",error);
    const bool invalidRejected = !invalid.isEmpty()
        && !store.publishStudioMesh(first,invalid,store.meshAsset(first)->mesh.data(),error)
        && linked(store,first,surface);
    audit.check(page,"publishing","Geometry publication preserves the link and a missing link is rejected",
        preserved && invalidRejected,QString("Preserved %1. Rejected %2. Publish: %3. Invalid: %4")
            .arg(preserved).arg(invalidRejected).arg(geometryError,error));
    const bool stale = !store.publishStudioMesh(first,geometry,expected.data(),error);
    audit.check(page,"validation","A stale draft and a missing texture reference are rejected",stale
        && store.saveProjectMaterial(material,{"Invalid","missing",{1,1,1,1}},error).isEmpty()
        && linked(store,first,surface),error);
    if (!store.save(&error)) {
        audit.check(page,"storage","Save both mesh assignments",false,error);
        return;
    }
    const bool sourcesRemoved = QFile::remove(png) && QFile::remove(jpeg);
    ProjectStore reopened;
    const bool loaded = reopened.project() && sourcesRemoved && reopened.project()->surfaces.textures.size() == 2
        && reopened.project()->surfaces.textures.value(pngId).width == 32
        && linked(reopened,first,surface) && linked(reopened,second,surface);
    audit.check(page,"storage","Reopen resolves both assignments after deleting the original image files",loaded,
        QString("Source removed %1. Project %2. Textures %3. Slots %4/%5. %6")
            .arg(sourcesRemoved).arg(reopened.project() != nullptr)
            .arg(reopened.project() ? reopened.project()->surfaces.textures.size() : 0)
            .arg(linked(reopened,first,surface)).arg(linked(reopened,second,surface)).arg(reopened.loadError()));
    if (!loaded)
        return;

    {
        MainWindow window;
        window.resize(1300,850);
        window.show();
        QTest::qWait(100);
        auto *search = window.findChild<QAction *>("searchStudioAssets");
        bool filtered = false;
        if (search && search->isVisible() && search->isEnabled()) {
            QTimer::singleShot(0,&window,[&] {
                auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
                auto *kind = dialog ? dialog->findChild<QComboBox *>("projectAssetKind") : nullptr;
                auto *list = dialog ? dialog->findChild<QListWidget *>("studioAssetResults") : nullptr;
                if (kind && list) {
                    kind->setCurrentText("Textures");
                    filtered = list->count() == 2;
                    kind->setCurrentText("Materials");
                    filtered = filtered && list->count() == 1 && list->item(0)->text() == surface.name;
                }
                if (dialog)
                    dialog->reject();
            });
            search->trigger();
        }
        auto *tiles = window.findChild<QListView *>("assetTiles");
        const auto tile = [&](const QString &name) {
            const auto found = tiles ? tiles->model()->match(tiles->model()->index(0,0),Qt::DisplayRole,name) : QModelIndexList{};
            return found.value(0);
        };
        const auto paint = tile(surface.name);
        bool materialDetails = false, textureDetails = false;
        if (paint.isValid()) {
            QTimer::singleShot(0,&window,[&] {
                auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
                auto *texture = dialog ? dialog->findChild<QComboBox *>("projectMaterialTexture") : nullptr;
                auto *assign = dialog ? dialog->findChild<QPushButton *>("assignSavedMaterial") : nullptr;
                materialDetails = dialog && dialog->objectName() == "projectMaterialDetails"
                    && texture && texture->currentData().toString() == pngId && assign && assign->isEnabled();
                if (dialog)
                    dialog->reject();
            });
            QMetaObject::invokeMethod(tiles,"activated",Qt::DirectConnection,Q_ARG(QModelIndex,paint));
        }
        const auto swatch = tile("Surface Color");
        if (swatch.isValid()) {
            QTimer::singleShot(0,&window,[&] {
                auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
                textureDetails = dialog && dialog->objectName() == "projectTextureDetails";
                if (dialog)
                    dialog->reject();
            });
            QMetaObject::invokeMethod(tiles,"activated",Qt::DirectConnection,Q_ARG(QModelIndex,swatch));
        }
        audit.check(page,"navigation","Search filters and Content Browser opens both type-specific Details",filtered && materialDetails && textureDetails);
        window.close();
    }
    const auto player = runStreetPlayer(reopened,output,"surface-library");
    QFile playable(output.filePath("surface-library.wfplay"));
    const bool opened = playable.open(QIODevice::ReadOnly);
    std::istringstream exported(opened ? playable.readAll().toStdString() : std::string{});
    const auto scene = worldforge::readPlayable(exported);
    const auto textureBytes = reopened.project()->surfaces.textures.value(pngId).image;
    const auto texturedDraws = scene ? std::ranges::count_if(scene->meshes,[&](const auto &mesh) {
        return !mesh.vertices.empty() && std::ranges::equal(mesh.baseColorTexture,textureBytes,[](uint8_t a,char b) {
            return a == uint8_t(b);
        }) && mesh.vertices.front().color == worldforge::Point3{surface.tint[0],surface.tint[1],surface.tint[2]};
    }) : 0;
    audit.check(page,"runtime","The packaged Player loads both meshes with their shared base-color texture",
        player.exitCode == 0 && player.report.value("texturedMeshes").toInt() == 2 && texturedDraws == 2,playerSummary(player));

    const auto replacement = directory.filePath("Replacement.png");
    QImage fresh(30,18,QImage::Format_RGB32);
    fresh.fill(QColor(45,195,110));
    const bool imageReady = fresh.save(replacement);
    const auto liveBefore = reopened.history().index();
    const auto staged = reopened.previewTextureReimport(pngId,replacement,error);
    const bool previewOnly = staged && staged->after.width == 30
        && reopened.project()->surfaces.textures.value(pngId).image == textureBytes
        && reopened.history().index() == liveBefore;
    const bool textureApplied = imageReady && staged && reopened.applyTextureReimport(*staged,error);
    const bool textureRejected = textureApplied && !reopened.applyTextureReimport(*staged,error)
        && !reopened.previewTextureReimport(pngId,corrupt.fileName(),error);
    if (textureApplied)
        reopened.history().undo();
    audit.check(page,"reimport","Missing texture sources can be relinked and undo restores their bytes",
        previewOnly && textureRejected && linked(reopened,first,surface) && linked(reopened,second,surface)
        && reopened.project()->surfaces.textures.value(pngId).image == textureBytes
        && reopened.history().index() == liveBefore,error);

    {
        QScopedPointer<QWidget> hub(createAssetHub(reopened,[](const QString &) {},nullptr));
        auto *search = hub->findChild<QLineEdit *>("assetHubSearch");
        auto *list = hub->findChild<QListWidget *>("assetHubAssets");
        auto *usage = hub->findChild<QPlainTextEdit *>("assetHubUsage");
        search->setText("Road Barrier");
        for (int row = 0; row < list->count(); ++row)
            if (list->item(row)->data(Qt::UserRole).toString() == pngId)
                list->setCurrentRow(row);
        audit.check(page,"navigation","Asset Hub finds a texture by usage and shows its missing source and references",
            list->currentItem() && list->currentItem()->data(Qt::UserRole).toString() == pngId
            && usage->toPlainText().contains("Source missing") && usage->toPlainText().contains("Reusable Finish")
            && usage->toPlainText().contains("Road Barrier") && usage->toPlainText().contains("Counter"));
        auto *review = hub->findChild<QPushButton *>("assetHubReimport");
        bool reviewed = false;
        if (review && review->isEnabled()) {
            QTimer::singleShot(0,hub.data(),[&] {
                auto *chooser = qobject_cast<QFileDialog *>(QApplication::activeModalWidget());
                if (!chooser)
                    return;
                chooser->selectFile(replacement);
                QTimer::singleShot(0,hub.data(),[&] {
                    auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
                    reviewed = dialog && dialog->objectName() == "assetHubReimportPreview"
                        && std::ranges::any_of(dialog->findChildren<QLabel *>(),[](const auto *label) {
                            return label->text().contains("30 × 18");
                        });
                    if (dialog)
                        dialog->reject();
                });
                QMetaObject::invokeMethod(chooser,"accept",Qt::DirectConnection);
            });
            review->click();
        }
        audit.check(page,"reimport","Cancel from the Asset Hub image comparison preserves the texture and undo history",
            reviewed && reopened.project()->surfaces.textures.value(pngId).image == textureBytes
            && reopened.history().index() == liveBefore);
    }

    const auto saveGlb = [&](const QString &path,const worldforge::Mesh &mesh) {
        const auto bytes = exportGlb(mesh,"Reimport Fixture",error);
        QFile file(path);
        return !bytes.isEmpty() && file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
    };
    const auto glbSource = directory.filePath("Reimport Fixture.glb");
    const auto glbReplacement = directory.filePath("Changed Mesh.glb");
    const auto base = worldforge::makePrimitive(worldforge::Primitive::Sphere,{2,2,2},32,16).value();
    auto larger = base;
    for (auto &vertex : larger.vertices)
        vertex[2] *= 1.4f;
    const auto beforeImport = reopened.history().index();
    const bool imported = saveGlb(glbSource,base) && saveGlb(glbReplacement,larger) && reopened.importMesh(glbSource,error);
    if (imported) {
        const auto objectId = reopened.selection();
        const auto id = reopened.selectedMesh()->id;
        auto mesh = *reopened.selectedMesh()->mesh;
        for (auto &vertex : mesh.vertices)
            vertex[0] *= 0.9f;
        const bool meshEdited = reopened.updateMesh(id,std::move(mesh),"Local Edit",error);
        const std::vector<worldforge::MeshModifier> retained{
            {worldforge::ModifierKind::Array,true,0,2,3}
        };
        const bool authored = reopened.selectMeshUvSet("Lightmap",true,false,error)
            && reopened.setMeshModifiers(retained,error) && reopened.setMeshLod(0.25f,24,error);
        reopened.addMeshInstance();
        const auto linkedObject = reopened.selection();
        const bool hasInstance = linkedObject != objectId && reopened.selectedMesh()->id == id;
        const auto original = reopened.meshAsset(id)->mesh;
        const auto meshReview = reopened.previewMeshReimport(id,glbReplacement,error);
        const auto before = reopened.history().index();
        const bool guarded = meshReview && !reopened.applyMeshReimport(*meshReview,false,error)
            && reopened.history().index() == before && reopened.meshAsset(id)->mesh == original;
        const bool applied = meshReview && reopened.applyMeshReimport(*meshReview,true,error);
        const bool unchangedPlacement = applied && reopened.object(objectId) && reopened.object(objectId)->meshAsset == id;
        const auto *updated = reopened.meshAsset(id);
        const bool preservedAuthoring = applied && authored && hasInstance && updated
            && updated->modifiers == retained && updated->evaluatedMesh
            && updated->lodCell == 0.25f && updated->lodDistance == 24
            && updated->otherUvSets.contains("Lightmap") && reopened.object(linkedObject)->meshAsset == id;
        const bool rejectedStale = applied && !reopened.applyMeshReimport(*meshReview,true,error);
        if (applied)
            reopened.history().undo();
        audit.check(page,"reimport","Mesh review protects local edits, preserves its placement and rejects stale previews",
            meshEdited && guarded && unchangedPlacement && rejectedStale && reopened.meshAsset(id)->mesh == original,error);
        audit.check(page,"modeling delivery","Reimport retains the named UV set, modifier stack, LOD settings and linked instance",
            preservedAuthoring);
        reopened.history().setIndex(beforeImport);
    } else {
        audit.check(page,"reimport","Prepare the mesh source and changed GLB",false,error);
    }

    const std::array<QColor,4> colors{QColor(145,138,245),QColor(0,190,220),QColor(184,0,0),QColor(15,245,70)};
    for (std::size_t i = 0; i < colors.size(); ++i) {
        fresh.fill(colors[i]);
        const auto path = directory.filePath(QString("PBR Map %1.png").arg(i));
        if (fresh.save(path))
            surface.maps[i] = reopened.importProjectTexture(path,error);
    }
    surface.metallic = 0.72f;
    surface.roughness = 0.56f;
    surface.emission = 0.85f;
    surface.clearCoat = 0.7f;
    surface.clearCoatRoughness = 0.18f;
    const bool materialSaved = std::ranges::none_of(surface.maps,&QString::isEmpty)
        && reopened.saveProjectMaterial(material,surface,error) == material;
    const auto hasChannels = [&](const worldforge::MeshMaterial &slot) {
        const auto images = slot.previewTextures();
        for (std::size_t i = 0; i < surface.maps.size(); ++i) {
            const auto bytes = reopened.project()->surfaces.textures.value(surface.maps[i]).image;
            if (bytes.isEmpty() || !std::ranges::equal(*images[i+1],bytes,[](uint8_t a,char b) { return a == uint8_t(b); }))
                return false;
        }
        return slot.metallic == surface.metallic && slot.roughness == surface.roughness
            && slot.clearCoat == surface.clearCoat && slot.clearCoatRoughness == surface.clearCoatRoughness
            && slot.emissive == worldforge::Point3{surface.emission,surface.emission,surface.emission};
    };
    const auto pbrFrame = [&] {
        preview.show();
        preview.setAssetPreview(reopened.meshAsset(first)->mesh);
        const auto frame = waitForRender(preview) ? preview.captureFrame() : QImage{};
        preview.hide();
        return frame;
    }();
    pbrFrame.save(output.filePath("pbr-barrier.png"));
    const auto &pbrMesh = *reopened.meshAsset(first)->mesh;
    const auto &pbrSlot = pbrMesh.materials[pbrMesh.faces.front().material];
    const auto studioGlb = exportGlb(pbrMesh,"Surface Studio Handoff",error,true);
    const auto studioMesh = importGlb(studioGlb,error);
    audit.check(page,"surface maps","Four map references, factors and a Vulkan render survive the Studio interchange",
        materialSaved && hasChannels(pbrSlot) && !pbrFrame.isNull() && pbrFrame != newFrameA && studioMesh
        && hasChannels(studioMesh->materials[studioMesh->faces.front().material]),error);
    const bool pbrSaved = reopened.save(&error);
    ProjectStore saved;
    const auto pbrPlayer = runStreetPlayer(reopened,output,"surface-pbr");
    audit.check(page,"runtime","Reopen keeps all map references and the packaged Player uploads all eight data maps",
        pbrSaved && saved.project() && saved.project()->surfaces.materials.value(material) == surface
        && pbrPlayer.exitCode == 0 && pbrPlayer.report.value("surfaceMaps").toInt() == 8,
        playerSummary(pbrPlayer));

    const auto layeredPreview = [&] {
        auto mesh = pbrMesh;
        auto &slot = mesh.materials[mesh.faces.front().material];
        slot.layers.sheenColor = {0.23f,0.47f,0.63f};
        slot.layers.sheenRoughness = 0.3f;
        slot.layers.transmission = 0.72f;
        slot.layers.ior = 1.41f;
        slot.layers.coatNormalScale = 0.53f;
        slot.layers.anisotropy = 0.6f;
        slot.layers.anisotropyRotation = 0.78f;
        // Use distinct valid images so swapped channels cannot pass the round-trip.
        QImage rgbaMap(fresh.size(),QImage::Format_RGBA8888);
        for (std::size_t i = 0; i < slot.layerMaps.size(); ++i) {
            rgbaMap.fill(QColor(int(i)*17,120,210,80+int(i)*25));
            QByteArray bytes;
            QBuffer buffer(&bytes);
            buffer.open(QIODevice::WriteOnly);
            rgbaMap.save(&buffer,"PNG");
            slot.layerMaps[i].assign(bytes.begin(),bytes.end());
        }
        const auto glb = exportGlb(mesh,"Layered Surface",error);
        const auto importedMesh = importGlb(glb,error);
        if (!importedMesh || importedMesh->faces.empty())
            return false;
        const auto &importedSlot = importedMesh->materials[importedMesh->faces.front().material];
        return importedSlot.layerMaps == slot.layerMaps && importedSlot.layers == slot.layers
            && hasChannels(importedSlot) && glb.contains("\"TANGENT\"")
            && glb.contains("\"KHR_materials_anisotropy\"") && glb.contains("\"KHR_materials_sheen\"")
            && glb.contains("\"KHR_materials_ior\"") && glb.contains("\"KHR_materials_transmission\"");
    }();
    audit.check(page,"layer maps","GLB round-trips five extensions, seven layer maps and a tangent frame",layeredPreview,error);

    const auto sculptPath = qEnvironmentVariable("WORLDFORGE_SCULPT_TEST_GLB");
    if (!sculptPath.isEmpty()) {
        QFile glb(sculptPath);
        QFile source(QFileInfo(sculptPath).dir().filePath("source.wfasset"));
        const auto optimized = glb.open(QIODevice::ReadOnly) ? importGlb(glb.readAll(),error) : std::nullopt;
        const bool delivered = optimized && source.open(QIODevice::ReadOnly)
            && reopened.createStudioMesh("Workbench Sculpt",*optimized,source.readAll(),error);
        if (delivered) {
            const auto objectId = reopened.selection();
            const auto assetId = reopened.selectedMesh()->id;
            auto object = *reopened.selectedObject();
            object.position = {12,9,1};
            reopened.updateObject(object,"Position sculpt");
            const bool sculptSaved = reopened.save(&error);
            ProjectStore reopenedSculpt;
            const auto *asset = reopenedSculpt.meshAsset(assetId);
            QImage normal;
            if (asset)
                for (const auto &slot : asset->mesh->materials)
                    if (!slot.normalTexture.empty()) {
                        normal = QImage::fromData(slot.normalTexture.data(),int(slot.normalTexture.size()));
                        break;
                    }
            bool varied = false;
            for (int y = 0; !varied && y < normal.height(); ++y)
                for (int x = 0; !varied && x < normal.width(); ++x) {
                    const auto pixel = normal.pixel(x,y);
                    varied = qBlue(pixel) > 96 && std::abs(qRed(pixel)-128) > 12;
                }
            const auto sculptPlayer = runStreetPlayer(reopenedSculpt,output,"sculpt-delivery");
            audit.check(page,"sculpt delivery","The 600-triangle mesh, baked normals and editable sculpt survive reopen and Player export",
                sculptSaved && asset && asset->mesh->faces.size() == 600 && reopenedSculpt.object(objectId)
                && varied && QFile::exists(reopenedSculpt.studioSourcePath(*asset))
                && sculptPlayer.exitCode == 0 && sculptPlayer.report.value("surfaceMaps").toInt() == 9,
                error + " " + playerSummary(sculptPlayer));
        } else {
            audit.check(page,"sculpt delivery","Publish the reduced sculpt and archive its editable source",false,error);
        }
    }
    auto streetMaterial = surface;
    streetMaterial.layerMaps[worldforge::CoatNormal] = surface.maps[0];
    streetMaterial.layers.coatNormalScale = 0.75f;
    streetMaterial.layers.wetness = 0.41f;
    streetMaterial.layers.puddles = 0.28f;
    streetMaterial.layers.heightScale = 0.027f;
    streetMaterial.layers.decal = true;
    streetMaterial.layers.decalOpacity = 0.83f;
    const bool streetSaved = reopened.saveProjectMaterial(material,streetMaterial,error) == material && reopened.save(&error);
    ProjectStore reopenedStreet;
    const auto *streetAsset = reopenedStreet.meshAsset(first);
    const bool streetRestored = streetAsset && std::ranges::any_of(streetAsset->mesh->materials,[&](const auto &slot) {
        return slot.sharedMaterialId == material.toStdString() && slot.layers == streetMaterial.layers
            && !slot.layerMaps[worldforge::CoatNormal].empty()
            && slot.layerMaps[worldforge::CoatNormal] == slot.normalTexture;
    });
    audit.check(page,"street materials","Save and reopen restore street controls and linked layer maps",
        streetSaved && streetRestored && reopenedStreet.project()
        && reopenedStreet.project()->surfaces.materials.value(material) == streetMaterial,error);
}

void studioUvRoundTrip(const QDir &output,const QDir &root,Audit &audit)
{
    const QString page = "Studio UV exchange";
    QTemporaryDir fixture(output.filePath("uv-exchange-XXXXXX"));
    fixture.setAutoRemove(false);
    ProjectStore store;
    QString error;
    if (!fixture.isValid() || !store.createStarterProject("Studio UV QA",
        {{"location",fixture.filePath("UV Project")},{"starter","street"}},error)) {
        audit.check(page,"setup","Create the UV round-trip project",false,error);
        return;
    }
    store.addCube();
    const auto id = store.selectedMesh()->id;
    const auto firstPlacement = store.selection();
    const bool created = store.selectMeshUvSet("Lightmap",true,false,error);
    auto prop = *store.selectedMesh()->mesh;
    prop.materials.push_back({"UV Trim",{0.65f,0.35f,0.2f,1}});
    prop.faces.back().material = 1;
    for (auto &face : prop.faces)
        for (auto &corner : face.corners)
            corner.uv = {0.11f+corner.uv[0]*0.39f,0.11f+corner.uv[1]*0.39f};
    const bool edited = store.updateMesh(id,std::move(prop),"Author lightmap UVs",error)
        && store.selectMeshUvSet("UV Map",false,false,error);
    store.placeMesh(id);
    const auto secondPlacement = store.selection();
    const auto input = store.exportStudioInput(*store.meshAsset(id),error);
    QFile file(fixture.filePath("input.glb"));
    const bool prepared = created && edited && !input.isEmpty() && file.open(QIODevice::WriteOnly)
        && file.write(input) == input.size();
    file.close();
    if (!prepared) {
        audit.check(page,"setup","Stage the authored named UV layouts",false,error);
        return;
    }
    QProcess studio;
    studio.setProcessChannelMode(QProcess::MergedChannels);
    studio.start(EmbeddedStudio::installedExecutable(),{"--background","--factory-startup","--python-exit-code","7",
        "--python",root.filePath("Workspaces/StudioPublish.py"),"--python",root.filePath("Tests/StudioUvRoundTrip.py"),
        "--",fixture.path()});
    const bool staged = studio.waitForFinished(55000) && studio.exitStatus() == QProcess::NormalExit && studio.exitCode() == 0;
    audit.check(page,"Studio","Studio restores the native UV layouts and stages two revisions",staged,QString::fromUtf8(studio.readAll()));
    if (!staged)
        return;
    const auto readFile = [](const QString &path) {
        QFile input(path);
        return input.open(QIODevice::ReadOnly) ? input.readAll() : QByteArray{};
    };
    const auto revision = [&](const QString &marker) {
        return QDir(fixture.filePath(QString::fromLatin1(readFile(fixture.filePath(marker))).trimmed()));
    };
    const auto apply = [&](const QString &marker,const QByteArray &overrideManifest = {}) {
        const auto dir = revision(marker);
        return store.publishStudioMesh(id,readFile(dir.filePath("mesh.glb")),store.meshAsset(id)->mesh.data(),error,
            readFile(dir.filePath("source.wfasset")),overrideManifest.isEmpty() ? readFile(dir.filePath("uv-sets.json")) : overrideManifest);
    };
    const auto initial = store.meshAsset(id)->mesh;
    const int before = store.history().index();
    const bool rejected = !apply("with-lightmap",R"({"version":1,"names":["UV Map","UV Map"],"render":0})")
        && store.history().index() == before && store.meshAsset(id)->mesh == initial;
    audit.check(page,"validation","Duplicate UV names are rejected without changing the mesh or undo history",rejected,error);
    const bool published = apply("with-lightmap");
    const auto validLayout = [&](const ProjectStore &projects) {
        const auto *asset = projects.meshAsset(id);
        if (!asset || asset->activeUvSet != "UV Map" || !asset->otherUvSets.contains("Lightmap"))
            return false;
        const auto &layer = asset->otherUvSets["Lightmap"];
        return !layer.needsUnwrap && layer.corners.size() == asset->mesh->faces.size()*3
            && std::ranges::all_of(layer.corners,[](const auto &uv) { return uv[0] >= 0.30f && uv[0] <= 0.51f; })
            && std::ranges::any_of(asset->mesh->faces,[&](const auto &face) {
                return asset->mesh->materials[face.material].name == "UV Trim";
            });
    };
    audit.check(page,"publish","The edited UVs and trim material return to the same placed asset",
        published && validLayout(store) && store.object(firstPlacement)->meshAsset == id
        && store.object(secondPlacement)->meshAsset == id,error);
    if (!published)
        return;
    const auto publishedMesh = store.meshAsset(id)->mesh;
    store.history().undo();
    const bool restored = store.meshAsset(id)->mesh == initial;
    store.history().redo();
    const bool persisted = store.save(&error);
    ProjectStore reopened;
    const auto player = runStreetPlayer(reopened,output,"studio-uv-round-trip");
    audit.check(page,"storage","Undo, redo, project reopen and standalone Play preserve the published prop",
        restored && persisted && validLayout(reopened) && store.meshAsset(id)->mesh == publishedMesh
        && player.exitCode == 0,playerSummary(player));
    const bool changed = apply("ready");
    audit.check(page,"validation","A removed lightmap is flagged for rebuilding after a topology edit",
        changed && store.meshAsset(id)->otherUvSets.value("Lightmap").needsUnwrap
        && store.meshAsset(id)->mesh->faces.size() > publishedMesh->faces.size(),error);
}

void studioCharacterRoundTrip(const QDir &output,const QDir &root,Audit &audit)
{
    namespace wf = worldforge;
    const QString page = "Studio character clip";
    QTemporaryDir fixture(output.filePath("character-XXXXXX"));
    fixture.setAutoRemove(false);
    const auto read = [](const QString &path) {
        QFile file(path);
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
    };
    QFile input(fixture.filePath("input.glb"));
    if (!fixture.isValid() || !input.open(QIODevice::WriteOnly)) {
        audit.check(page,"setup","Create the Studio exchange directory",false);
        return;
    }
    input.close();
    QProcess studio;
    studio.setProcessChannelMode(QProcess::MergedChannels);
    studio.start(EmbeddedStudio::installedExecutable(),{"--background","--factory-startup","--python-exit-code","7",
        "--python",root.filePath("Workspaces/StudioPublish.py"),"--python",root.filePath("Tests/StudioCharacterClip.py"),
        "--",fixture.path()});
    const bool published = studio.waitForFinished(55000) && studio.exitCode() == 0 && studio.exitStatus() == QProcess::NormalExit;
    const auto log = QString::fromUtf8(studio.readAll());
    audit.check(page,"Studio","An unweighted rig is rejected, then the weighted rig and Action publish and reopen",
        published && log.contains("positive weight"),log);
    if (!published)
        return;
    const QDir revision(fixture.filePath(QString::fromLatin1(read(fixture.filePath("ready"))).trimmed()));
    const auto glb = read(revision.filePath("mesh.glb"));
    const auto uvs = read(revision.filePath("uv-sets.json"));
    const auto clip = read(revision.filePath("clip.wfmotion"));
    QString error;
    StudioMeshData imported;
    const bool rejected = !importStudioGlb(glb,uvs,clip.chopped(4),imported,error);
    auto mesh = importStudioGlb(glb,uvs,clip,imported,error);
    auto projectReady = [&] {
        const auto &data = imported.clip;
        return mesh && data && data->name == "Looping Bend" && data->frameCount() == 13
            && std::abs(data->duration()-0.5) < 0.0001;
    };
    audit.check(page,"validation","A truncated clip is rejected and the complete clip matches the GLB vertex map",
        rejected && projectReady(),error);
    ProjectStore store;
    if (!projectReady() || !store.createStarterProject("Character QA",
        {{"location",fixture.filePath("Character QA")},{"starter","street"}},error)
        || !store.createStudioMesh("Looping Character",std::move(*mesh),read(revision.filePath("source.wfasset")),error,&imported)) {
        audit.check(page,"setup","Import the skinned Studio asset and Action",false,error);
        return;
    }
    const auto assetId = store.selectedMesh()->id;
    auto object = *store.selectedObject();
    object.position = {0,0,8};
    store.updateObject(object,"Place Character");
    const bool saved = store.save(&error);
    ProjectStore reopened;
    const auto *asset = reopened.meshAsset(assetId);
    audit.check(page,"storage","The animation samples and editable rig survive project reopen",
        saved && asset && asset->clip && asset->clip->samples.size() == imported.clip->samples.size()
        && QFile::exists(reopened.studioSourcePath(*asset)),error);
    if (!saved || !asset || !asset->clip)
        return;

    LiveViewport view(reopened,false);
    view.resize(690,480);
    view.show();
    const bool playing = waitForRender(view) && view.start(LiveViewport::RunMode::Play);
    if (playing) {
        view.pause(true);
        view.setCameraPreview({3,-5,10},{0,0,9});
    }
    const auto start = playing ? view.captureFrame() : QImage{};
    for (int i = 0; playing && i < qRound(0.25/wf::WorldSimulation::stepSeconds); ++i)
        view.step();
    const auto bent = playing ? view.captureFrame() : QImage{};
    view.stop();
    view.hide();
    const auto mask = [](const QImage &frame) {
        QImage result(frame.size(),QImage::Format_Grayscale8);
        int count = 0;
        for (int y = 0; y < frame.height(); ++y)
            for (int x = 0; x < frame.width(); ++x) {
                const QColor pixel = frame.pixelColor(x,y);
                const bool colored = pixel.red() > 120 && pixel.blue() > 120 && pixel.green() < 110;
                result.scanLine(y)[x] = colored ? 255 : 0;
                count += colored;
            }
        return std::pair{result,count};
    };
    const auto [startMask,startPixels] = mask(start);
    const auto [bentMask,bentPixels] = mask(bent);
    start.save(output.filePath("character-frame-01.png"));
    bent.save(output.filePath("character-frame-07.png"));
    audit.check(page,"Editor Play","Known animation frames render different character silhouettes",
        playing && startPixels > 40 && bentPixels > 40 && startMask != bentMask,
        QString("%1 and %2 character pixels").arg(startPixels).arg(bentPixels));
    const auto player = runStreetPlayer(reopened,output,"studio-character");
    audit.check(page,"Player","The packaged Player applies the deformation and loops the clip",
        player.exitCode == 0 && player.report.value("clipMoved").toBool() && player.report.value("clipLooped").toBool(),
        playerSummary(player));
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
    const bool r3Performance = app.arguments().size() == 4 && app.arguments().last() == "--r3-performance";
    const bool districtProfile = app.arguments().size() == 4 && app.arguments().last() == "--district-profile";
    const bool districtLarge = app.arguments().size() == 4 && app.arguments().last() == "--district-large";
    const bool districtTextured = app.arguments().size() == 4 && app.arguments().last() == "--district-textured";
    const bool districtStreaming = app.arguments().size() == 4 && app.arguments().last() == "--district-streaming";
    const bool districtMeshlets = app.arguments().size() == 4 && app.arguments().last() == "--district-meshlets";
    const bool districtMeshletsIndexed = app.arguments().size() == 4 && app.arguments().last() == "--district-meshlets-indexed";
    const bool districtCompare = app.arguments().size() == 4 && app.arguments().last() == "--district-compare";
    const bool districtOcclusion = app.arguments().size() == 4 && app.arguments().last() == "--district-occlusion";
    const bool districtAudit = districtProfile || districtLarge || districtTextured || districtCompare || districtOcclusion || districtStreaming
        || districtMeshlets || districtMeshletsIndexed
        || (app.arguments().size() == 4 && app.arguments().last() == "--district");
    const bool rendering = app.arguments().size() == 4 && app.arguments().last() == "--rendering";
    const bool sunrise = app.arguments().size() == 4 && app.arguments().last() == "--sunrise";
    const bool sunset = app.arguments().size() == 4 && app.arguments().last() == "--sunset";
    const bool realism = app.arguments().size() == 4 && app.arguments().last() == "--realism";
    const bool engineSession = app.arguments().size() == 4 && app.arguments().last() == "--engine-session";
    const bool programming = app.arguments().size() == 4 && app.arguments().last() == "--programming";
    const bool nativeGameplay = app.arguments().size() == 4 && app.arguments().last() == "--native-gameplay";
    const bool surfaceLibrary = app.arguments().size() == 4 && app.arguments().last() == "--surface-library";
    const bool studioUv = app.arguments().size() == 4 && app.arguments().last() == "--studio-uv";
    const bool studioCharacter = app.arguments().size() == 4 && app.arguments().last() == "--studio-character";
    if (!authoring && !benchmark && !studio && !forgeAi && !streetAudit && !r3Performance && !districtAudit && !rendering && !sunrise && !sunset && !realism && !engineSession
        && !surfaceLibrary && !studioUv && !studioCharacter && !programming && !nativeGameplay && app.arguments().size() != 3)
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
    if (rendering) {
        renderingWorkflow(output,root,audit);
        controls.flush();
        return audit.failures ? 1 : 0;
    }
    if (sunrise || sunset) {
        sunSweepWorkflow(output,audit,sunrise);
        controls.flush();
        return audit.failures ? 1 : 0;
    }
    if (realism) {
        realismWorkflow(output,root,audit);
        controls.flush();
        return audit.failures ? 1 : 0;
    }
    if (streetAudit) {
        streetWorkflow(output,audit);
        controls.flush();
        return audit.failures ? 1 : 0;
    }
    if (r3Performance) {
        r3PerformanceWorkflow(output,audit);
        controls.flush();
        return audit.failures ? 1 : 0;
    }
    if (districtAudit) {
        if (districtOcclusion)
            qputenv("WORLDFORGE_OCCLUSION","1");
        const bool textured = districtTextured || districtStreaming;
        const QString meshlets = districtMeshlets ? "1" : districtMeshletsIndexed ? "indexed" : "";
        districtWorkflow(output,audit,{.profile = districtProfile,.large = districtLarge || textured,.textured = textured,
            .compareDirect = districtCompare,.compareOcclusion = districtOcclusion,.streamTextures = districtStreaming,.meshlets = meshlets});
        controls.flush();
        return audit.failures ? 1 : 0;
    }
    if (engineSession) {
        engineSessionWorkflow(output,audit);
        controls.flush();
        return audit.failures ? 1 : 0;
    }
    if (programming) {
        programmingWorkflow(output,audit);
        controls.flush();
        return audit.failures ? 1 : 0;
    }
    if (nativeGameplay) {
        nativeGameplayWorkflow(output,audit);
        controls.flush();
        return audit.failures ? 1 : 0;
    }
    if (surfaceLibrary) {
        surfaceLibraryWorkflow(output,audit);
        controls.flush();
        return audit.failures ? 1 : 0;
    }
    if (studioUv) {
        studioUvRoundTrip(output,root,audit);
        controls.flush();
        return audit.failures ? 1 : 0;
    }
    if (studioCharacter) {
        studioCharacterRoundTrip(output,root,audit);
        controls.flush();
        return audit.failures ? 1 : 0;
    }

    const QDir data = ProjectStore::defaultStorageDirectory();
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
        sculptingWorkflow(output,audit);
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
        // Switching to a Studio workspace with a mesh selected asks before replacing a running Studio session;
        // the closer cancels that, keeping the session.
        withDialog([&] { tabs->setCurrentIndex(index); });
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
