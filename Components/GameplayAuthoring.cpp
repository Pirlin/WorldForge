#include "Components/GameplayAuthoring.h"
#include "Components/BehaviorEditor.h"
#include "Components/NativeGameplayEditor.h"
#include "Components/LiveViewport.h"
#include "Components/LocalMultiplayer.h"
#include "State/PlayableStreet.h"
#include "State/WorldRuntime.h"

#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSlider>
#include <QTemporaryDir>
#include <QTimer>
#include <QTabWidget>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <sstream>

namespace {
enum class Mode { Programming, Vfx, Cinematic, Ai, Ui, Testing, None };

Mode modeFor(const QString &name)
{
    if (name == "Programming") return Mode::Programming;
    if (name == "VFX") return Mode::Vfx;
    if (name == "Cinematic") return Mode::Cinematic;
    if (name == "AI") return Mode::Ai;
    if (name == "UI") return Mode::Ui;
    if (name == "Testing") return Mode::Testing;
    return Mode::None;
}

QString tr(const char *source) { return QCoreApplication::translate("GameplayAuthoring",source); }

QString targetKey(Mode mode)
{
    if (mode == Mode::Cinematic) return "shotTarget";
    if (mode == Mode::Ai) return "patrolWaypoint";
    return "completionBeacon";
}

QWidget *testingPanel(ProjectStore &store,QWidget *parent)
{
    struct Run {
        QTemporaryDir files;
        QString report;
    };
    auto *panel = new QWidget(parent);
    auto *layout = new QVBoxLayout(panel);
    auto *run = new QPushButton(tr("Run Playable Street Check"),panel);
    run->setObjectName("runStreetCheck");
    auto *results = new QPlainTextEdit(panel);
    results->setObjectName("streetCheckResults");
    results->setReadOnly(true);
    layout->addWidget(run,0,Qt::AlignLeft);
    layout->addWidget(results,1);
    auto test = std::make_shared<Run>();
    auto *process = new QProcess(panel);
    QObject::connect(run,&QPushButton::clicked,panel,[&store,run,results,process,test] {
        const auto local = street::verify(store);
        results->setPlainText(local.summary());
        if (!local.ok())
            return;
        std::ostringstream output;
        QString error;
        if (!test->files.isValid() || !WorldRuntime::exportScene(store,output,&error)) {
            results->appendPlainText(error.isEmpty() ? tr("Could not export the playable test scene.") : error);
            return;
        }
        const auto path = test->files.filePath("street.wfplay");
        test->report = test->files.filePath("street-report.json");
        QFile::remove(test->report);
        QFile file(path);
        const auto bytes = QByteArray::fromStdString(output.str());
        if (!file.open(QIODevice::WriteOnly|QIODevice::Truncate) || file.write(bytes) != bytes.size()) {
            results->appendPlainText(tr("Could not write the playable test scene."));
            return;
        }
        file.close();
        run->setEnabled(false);
        results->appendPlainText(tr("Running the packaged Player…"));
        process->start(QDir(QCoreApplication::applicationDirPath()).filePath("WorldForgePlayer.exe"),
            {"--verify-street",path,test->report});
    });
    QObject::connect(process,&QProcess::errorOccurred,panel,[run,results,process](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart)
            return;
        run->setEnabled(true);
        results->appendPlainText(tr("Could not start the packaged Player. %1").arg(process->errorString()));
    });
    QObject::connect(process,&QProcess::finished,panel,[run,results,test](int code,QProcess::ExitStatus status) {
        run->setEnabled(true);
        results->appendPlainText(status == QProcess::NormalExit && code == 0 ? tr("Packaged Player passed.")
            : tr("Packaged Player failed with exit code %1.").arg(code));
        QFile report(test->report);
        if (report.open(QIODevice::ReadOnly))
            results->appendPlainText(QString::fromUtf8(report.readAll()));
    });
    return panel;
}

class AuthoringPanel : public QWidget {
public:
    AuthoringPanel(Mode mode,ProjectStore &store,QWidget *parent) : QWidget(parent), mode_(mode), store_(store)
    {
        auto *layout = new QVBoxLayout(this);
        auto *form = new QFormLayout;
        layout->addLayout(form);
        source_ = new QComboBox(this);
        source_->setObjectName("gameplaySource");
        form->addRow(mode == Mode::Ai ? tr("Patrol start") : tr("Mission"),source_);
        if (mode != Mode::Ui) {
            target_ = new QComboBox(this);
            target_->setObjectName("gameplayTarget");
            form->addRow(mode == Mode::Ai ? tr("Waypoint") : mode == Mode::Cinematic ? tr("Look at") : tr("Beacon location"),target_);
        }
        if (mode == Mode::Programming)
            form->addRow(tr("Rule"),new QLabel(tr("On mission completed → Activate beacon"),this));
        if (mode == Mode::Vfx || mode == Mode::Cinematic || mode == Mode::Ai) {
            seconds_ = spin(this,mode == Mode::Ai ? 0 : 0.1,15);
            form->addRow(mode == Mode::Ai ? tr("Wait at waypoint (s)") : tr("Duration (s)"),seconds_);
        }
        if (mode == Mode::Vfx) {
            colorButton_ = new QPushButton(this);
            form->addRow(tr("Beacon color"),colorButton_);
            connect(colorButton_,&QPushButton::clicked,this,[this] {
                const auto picked = QColorDialog::getColor(color_,this,tr("Beacon color"));
                if (picked.isValid()) {
                    color_ = picked;
                    refreshColor();
                }
            });
        }
        if (mode == Mode::Ai) {
            speed_ = spin(this,0.1,10);
            form->addRow(tr("Walking speed (m/s)"),speed_);
        }
        if (mode == Mode::Ui) {
            hud_ = new QCheckBox(tr("Show live mission objective"),this);
            message_ = new QLineEdit(this);
            message_->setObjectName("missionCompletionText");
            message_->setMaxLength(120);
            message_->setPlaceholderText(tr("Use the default completion message"));
            form->addRow(hud_);
            form->addRow(tr("Completion message"),message_);
        }
        if (mode == Mode::Cinematic) {
            form->addRow(tr("First key offset (X/Y/Z)"),coordinates(first_));
            form->addRow(tr("Last key offset (X/Y/Z)"),coordinates(last_));
        }
        auto *actions = new QHBoxLayout;
        auto *save = new QPushButton(tr("Apply to Project"),this);
        save->setObjectName("gameplayApply");
        actions->addWidget(save);
        if (mode == Mode::Cinematic || mode == Mode::Vfx || mode == Mode::Ai) {
            auto *preview = new QPushButton(tr("Preview"),this);
            preview->setObjectName("gameplayPreview");
            actions->addWidget(preview);
            connect(preview,&QPushButton::clicked,this,[this] { showPreview(); });
        }
        actions->addStretch();
        layout->addLayout(actions);
        status_ = new QLabel(this);
        status_->setObjectName("gameplayStatus");
        status_->setTextFormat(Qt::PlainText);
        status_->setWordWrap(true);
        layout->addWidget(status_);
        if (mode == Mode::Cinematic || mode == Mode::Vfx || mode == Mode::Ai) {
            preview_ = new LiveViewport(store_,mode != Mode::Cinematic,this);
            layout->addWidget(preview_,1);
            preview_->hide();
        }
        if (mode == Mode::Cinematic) {
            scrub_ = new QSlider(Qt::Horizontal,this);
            scrub_->setRange(0,100);
            scrub_->setAccessibleName(tr("Scrub camera shot"));
            layout->addWidget(scrub_);
            scrub_->hide();
            connect(scrub_,&QSlider::valueChanged,this,[this] { showPreview(); });
        }
        layout->addStretch();
        connect(save,&QPushButton::clicked,this,[this] { apply(); });
        connect(source_,&QComboBox::currentIndexChanged,this,[this] { load(); });
        connect(&store_,&ProjectStore::sceneChanged,this,[this] { reload(); });
        reload();
    }

private:
    static QDoubleSpinBox *spin(QWidget *parent,double minimum,double maximum)
    {
        auto *control = new QDoubleSpinBox(parent);
        control->setRange(minimum,maximum);
        control->setDecimals(2);
        return control;
    }

    QWidget *coordinates(std::array<QDoubleSpinBox *,3> &inputs)
    {
        auto *row = new QWidget(this);
        auto *layout = new QHBoxLayout(row);
        layout->setContentsMargins(0,0,0,0);
        for (auto &input : inputs) {
            input = spin(row,-100,100);
            layout->addWidget(input);
        }
        return row;
    }

    static QJsonArray values(const std::array<QDoubleSpinBox *,3> &inputs)
    {
        return {inputs[0]->value(),inputs[1]->value(),inputs[2]->value()};
    }

    void reload()
    {
        const QSignalBlocker sourceBlock(source_);
        const auto before = source_->currentData();
        source_->clear();
        if (const auto *project = store_.project())
            for (const auto &object : project->objects) {
                if (!object.visible)
                    continue;
                const auto role = object.details.value("gameplay").toString();
                if (mode_ == Mode::Ai ? object.type == "Empty" && role.isEmpty() : role == "missionStart")
                    source_->addItem(mode_ == Mode::Ai ? object.name : object.details.value("mission").toString(),object.id);
            }
        if (const int index = source_->findData(before); index >= 0)
            source_->setCurrentIndex(index);
        load();
    }

    void load()
    {
        const auto *source = store_.object(source_->currentData().toString());
        const auto settings = source ? source->details : QJsonObject{};
        if (target_) {
            target_->clear();
            target_->addItem(tr("Disabled"),QString{});
            if (const auto *project = store_.project())
                for (const auto &item : project->objects)
                    if (item.visible && item.type == "Empty" && (!source || item.id != source->id))
                        target_->addItem(item.name,item.id);
            const auto stored = settings.value(targetKey(mode_)).toString();
            const int index = target_->findData(stored);
            if (!stored.isEmpty() && index < 0)
                target_->addItem(tr("Missing target"),stored);
            target_->setCurrentIndex(index >= 0 ? index : target_->count()-1);
        }
        if (seconds_)
            seconds_->setValue(settings.value(mode_ == Mode::Ai ? "patrolWait" : mode_ == Mode::Cinematic
                ? "shotSeconds" : "beaconSeconds").toDouble(mode_ == Mode::Ai ? 1 : 2));
        if (speed_)
            speed_->setValue(settings.value("patrolSpeed").toDouble(2));
        if (colorButton_) {
            const auto color = settings.value("beaconColor").toArray();
            color_ = color.size() == 3 ? QColor::fromRgbF(color.at(0).toDouble(),color.at(1).toDouble(),color.at(2).toDouble())
                : QColor("#ffa626");
            refreshColor();
        }
        if (hud_) {
            hud_->setChecked(settings.value("showMissionHud").toBool(true));
            message_->setText(settings.value("completionText").toString());
        }
        if (mode_ == Mode::Cinematic) {
            const auto first = settings.value("shotFirst").toArray();
            const auto last = settings.value("shotLast").toArray();
            constexpr double defaults[2][3]{{3,-4,1.2},{-2,-3,1.1}};
            for (int axis = 0; axis < 3; ++axis) {
                first_[axis]->setValue(first.size() == 3 ? first.at(axis).toDouble() : defaults[0][axis]);
                last_[axis]->setValue(last.size() == 3 ? last.at(axis).toDouble() : defaults[1][axis]);
            }
        }
        if (preview_)
            preview_->hide();
        if (animation_)
            animation_->stop();
        if (scrub_)
            scrub_->hide();
        status_->setText(source ? QString{} : mode_ == Mode::Ai ? tr("Add two Empty markers to author a patrol.")
            : tr("This workspace needs a mission. Open a Playable Street project to get started."));
    }

    void refreshColor() { colorButton_->setText(color_.name()); }

    void apply()
    {
        const auto *original = store_.object(source_->currentData().toString());
        if (!original) {
            status_->setText(tr("Select a valid source first."));
            return;
        }
        auto source = *original;
        if (target_) {
            const auto id = target_->currentData().toString();
            const auto *target = store_.object(id);
            if (!id.isEmpty() && (!target || !target->visible ||
                (mode_ == Mode::Ai && store_.worldMatrix(id).map(QVector3D{}) == store_.worldMatrix(source.id).map(QVector3D{})))) {
                status_->setText(tr("Choose a valid destination marker at a different position."));
                return;
            }
            source.details.insert(targetKey(mode_),id);
        }
        if (colorButton_)
            source.details.insert("beaconColor",QJsonArray{color_.redF(),color_.greenF(),color_.blueF()});
        if (seconds_)
            source.details.insert(mode_ == Mode::Ai ? "patrolWait" : mode_ == Mode::Cinematic
                ? "shotSeconds" : "beaconSeconds",seconds_->value());
        if (speed_)
            source.details.insert("patrolSpeed",speed_->value());
        if (mode_ == Mode::Cinematic) {
            const auto first = values(first_), last = values(last_);
            const auto zMin = std::min(first.at(2).toDouble(),last.at(2).toDouble());
            if (zMin < 0.1) {
                status_->setText(tr("Keep both camera keys at least 0.1 m above the look-at point."));
                return;
            }
            source.details.insert("shotFirst",first);
            source.details.insert("shotLast",last);
        }
        if (hud_) {
            source.details.insert("showMissionHud",hud_->isChecked());
            source.details.insert("completionText",message_->text().trimmed());
        }
        QString error;
        status_->setText(store_.updateObjects({source},tr("Edit Gameplay Settings"),error)
            ? tr("Applied to project. Save the project to keep the edit.") : error);
    }

    void showPreview()
    {
        const auto id = target_->currentData().toString();
        if (!store_.object(id)) {
            status_->setText(tr("Choose a scene marker to preview."));
            return;
        }
        if (mode_ == Mode::Vfx) {
            startBeaconPreview();
        } else if (mode_ == Mode::Ai) {
            startPatrolPreview(id);
        } else {
            const auto focus = store_.worldMatrix(id).map(QVector3D{})+QVector3D(0,0,1.2f);
            const float progress = scrub_->value()/100.0f;
            QVector3D eye = focus;
            for (int axis = 0; axis < 3; ++axis)
                eye[axis] += float(std::lerp(first_[axis]->value(),last_[axis]->value(),progress));
            if ((eye-focus).lengthSquared() < 0.0001f)
                eye.setZ(eye.z()+0.1f);
            preview_->setCameraPreview(eye,focus);
            scrub_->show();
        }
        preview_->show();
    }

    QTimer *animationTimer()
    {
        if (!animation_) {
            animation_ = new QTimer(this);
            animation_->setInterval(33);
        }
        animation_->stop();
        animation_->disconnect(this);
        return animation_;
    }

    void startBeaconPreview()
    {
        worldforge::MissionBeacon sample;
        sample.remaining = sample.seconds = float(seconds_->value());
        sample.color = {color_.redF(),color_.greenF(),color_.blueF()};
        preview_->setAssetPreview(QSharedPointer<worldforge::Mesh>::create(worldforge::makeBeaconMesh(sample)));
        auto *timer = animationTimer();
        connect(timer,&QTimer::timeout,this,[this,timer,sample]() mutable {
            sample.remaining -= 0.033f;
            if (sample.remaining <= 0 || !isVisible()) {
                timer->stop();
                return;
            }
            preview_->setAssetPreview(QSharedPointer<worldforge::Mesh>::create(worldforge::makeBeaconMesh(sample)),false);
        });
        timer->start();
    }

    void startPatrolPreview(const QString &target)
    {
        const auto start = store_.worldMatrix(source_->currentData().toString()).map(QVector3D{});
        const auto delta = store_.worldMatrix(target).map(QVector3D{})-start;
        if (delta.isNull()) {
            status_->setText(tr("Choose different start and destination positions."));
            return;
        }
        worldforge::Gameplay gameplay;
        const float shortening = std::min(1.0f,3.0f/delta.length());
        gameplay.patrols.push_back({{}, {delta.x()*shortening,delta.y()*shortening,delta.z()*shortening},
            {},float(speed_->value()),float(seconds_->value())});
        worldforge::WorldSimulation world;
        gameplay.start(world);
        preview_->setAssetPreview(QSharedPointer<worldforge::Mesh>::create(worldforge::makePatrolMesh(gameplay.patrols.front())));
        const auto targetPosition = delta*shortening/2+QVector3D(0,0,0.7f);
        preview_->setCameraPreview(targetPosition+QVector3D(4,-6,3),targetPosition);
        auto *timer = animationTimer();
        connect(timer,&QTimer::timeout,this,[this,timer,gameplay = std::move(gameplay),world = std::move(world)]() mutable {
            if (!isVisible()) {
                timer->stop();
                return;
            }
            for (int step = 0; step < 4; ++step)
                gameplay.update(world,std::nullopt,false);
            const auto &patrol = gameplay.patrols.front();
            using State = worldforge::NpcPatrol::State;
            status_->setText(patrol.state == State::Walking ? tr("Walking to waypoint")
                : patrol.state == State::Waiting ? tr("Waiting at waypoint") : tr("Returning to start"));
            preview_->setAssetPreview(QSharedPointer<worldforge::Mesh>::create(worldforge::makePatrolMesh(patrol)),false);
        });
        timer->start();
    }

    const Mode mode_;
    ProjectStore &store_;
    QComboBox *source_ = nullptr, *target_ = nullptr;
    QDoubleSpinBox *seconds_ = nullptr, *speed_ = nullptr;
    QCheckBox *hud_ = nullptr;
    QLineEdit *message_ = nullptr;
    QPushButton *colorButton_ = nullptr;
    QLabel *status_ = nullptr;
    QColor color_;
    LiveViewport *preview_ = nullptr;
    QSlider *scrub_ = nullptr;
    QTimer *animation_ = nullptr;
    std::array<QDoubleSpinBox *,3> first_{}, last_{};
};
}

bool hasGameplayAuthoring(const QString &workspace) { return workspace == "Multiplayer" || modeFor(workspace) != Mode::None; }

QWidget *createGameplayAuthoring(const QString &workspace,ProjectStore &store,QWidget *parent)
{
    if (workspace == "Multiplayer")
        return createLocalMultiplayerPanel(parent);
    const auto mode = modeFor(workspace);
    if (mode == Mode::Programming) {
        auto *tabs = new QTabWidget(parent);
        tabs->addTab(createBehaviorWorkspace(store,tabs),QObject::tr("Behaviors"));
        tabs->addTab(new AuthoringPanel(mode,store,tabs),QObject::tr("Mission Beacon"));
        auto *native = new QWidget(tabs);
        auto *layout = new QVBoxLayout(native);
        auto *open = new QPushButton(QObject::tr("C++ Gameplay Module…"),native);
        layout->addWidget(open);
        auto *components = new QPushButton(QObject::tr("Edit Selected Object's Native Components…"),native);
        components->setObjectName("editNativeComponents");
        layout->addWidget(components);
        components->setEnabled(store.selectedObject());
        QObject::connect(&store,&ProjectStore::selectionChanged,native,[&store,components] {
            components->setEnabled(store.selectedObject());
        });
        QObject::connect(components,&QPushButton::clicked,native,[&store,native] { editNativeComponents(store,native); });
        layout->addStretch();
        QObject::connect(open,&QPushButton::clicked,native,[&store,native] { showNativeGameplay(store,native); });
        tabs->addTab(native,QObject::tr("Native C++"));
        return tabs;
    }
    return mode == Mode::Testing ? testingPanel(store,parent) : new AuthoringPanel(mode,store,parent);
}
