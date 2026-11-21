#include "Components/BehaviorCode.h"
#include "State/ProjectStore.h"

#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QLabel>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <iomanip>
#include <locale>
#include <sstream>

bool prepareBehaviorCode(const QString &folder,QString &error)
{
    const QDir dir(folder);
    if (!QDir().mkpath(dir.absolutePath())) {
        error = QObject::tr("Could not create the Programming folder.");
        return false;
    }
    for (const auto *name : {"WorldForgeGameplay.h","WorldForgeBehavior.h","Behavior.cpp","Behavior.vcxproj"}) {
        const auto path = dir.filePath(QLatin1String(name));
        if (QFile::exists(path))
            continue;
        QFile resource(QStringLiteral(":/Resources/BehaviorCode/")+QLatin1String(name));
        QSaveFile destination(path);
        if (!resource.open(QIODevice::ReadOnly) || !destination.open(QIODevice::WriteOnly)) {
            error = QObject::tr("Could not create %1.").arg(path);
            return false;
        }
        const auto bytes = resource.readAll();
        if (destination.write(bytes) != bytes.size() || !destination.commit()) {
            error = QObject::tr("Could not save %1.").arg(path);
            return false;
        }
    }
    return true;
}

QString behaviorVisualStudio()
{
#ifdef Q_OS_WIN
    const auto installer = QDir(qEnvironmentVariable("ProgramFiles(x86)"))
        .filePath(QStringLiteral("Microsoft Visual Studio/Installer/vswhere.exe"));
    if (!QFile::exists(installer))
        return {};
    QProcess lookup;
    lookup.start(installer,{"-latest","-version","[17.0,18.0)","-products","*","-requires",
        "Microsoft.Component.MSBuild","Microsoft.VisualStudio.Component.VC.Tools.x86.x64","-property","installationPath","-utf8"});
    if (lookup.waitForFinished(4000) && lookup.exitCode() == 0)
        return QString::fromUtf8(lookup.readAllStandardOutput()).trimmed();
#endif
    return {};
}

std::optional<ProjectBehavior> readBuiltBehavior(const QByteArray &output,QString &error)
{
    std::istringstream input(output.toStdString());
    input.imbue(std::locale::classic());
    std::string signature,name,mission,condition;
    int version,event,action;
    float amount;
    if (output.size() <= 16384 && input >> signature >> version >> std::quoted(name) >> event >> action
        >> std::quoted(mission) >> std::quoted(condition) >> amount
        && signature == "WORLDFORGE_BEHAVIOR" && (version == 1 || version == 2)
        && event >= 0 && event <= 2 && action >= 0 && action <= version
        && !name.empty() && amount >= 0.1f && amount <= 15 && (event == 1) == !mission.empty()
        && (input >> std::ws).eof()) {
        using Rule = worldforge::BehaviorRule;
        return ProjectBehavior{QString::fromStdString(name),Rule::Event(event),Rule::Action(action),
            QString::fromStdString(mission),QString::fromStdString(condition),amount};
    }
    error = QObject::tr("The program must emit one behavior using worldforge::publish.");
    return {};
}

void showBehaviorCode(ProjectStore &store,QWidget *parent)
{
    const auto *project = store.project();
    if (!project)
        return;
    const auto projectId = project->id;
    const auto location = project->configuration.value("location").toString();
    const QDir files(location.isEmpty()
        ? QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).filePath("Programming/"+projectId)
        : QDir(location).filePath("Programming"));
    const auto vs = behaviorVisualStudio();
    const auto projectFile = files.filePath("Behavior.vcxproj");

    QDialog dialog(parent);
    dialog.setObjectName("behaviorCodeProject");
    dialog.setWindowTitle(QObject::tr("C++ Behavior Project"));
    dialog.resize(680,460);
    auto *layout = new QVBoxLayout(&dialog);
    auto *path = new QLabel(QDir::toNativeSeparators(projectFile),&dialog);
    path->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(path);
    auto *open = new QPushButton(QObject::tr("Open in Visual Studio"),&dialog);
    auto *build = new QPushButton(QObject::tr("Build and Preview C++ Behavior"),&dialog);
    build->setObjectName("buildCppBehavior");
    auto *target = new QComboBox(&dialog);
    target->addItem(QObject::tr("Create a behavior asset"),QString{});
    for (auto it = project->behaviors.cbegin(); it != project->behaviors.cend(); ++it)
        target->addItem(QObject::tr("Update %1").arg(it->name),it.key());
    auto *apply = new QPushButton(QObject::tr("Apply Built Behavior"),&dialog);
    apply->setObjectName("applyCppBehavior");
    auto *output = new QPlainTextEdit(&dialog);
    output->setObjectName("behaviorBuildOutput");
    output->setReadOnly(true);
    output->setMaximumBlockCount(2000);
    auto *status = new QLabel(&dialog);
    status->setWordWrap(true);
    for (auto *widget : {static_cast<QWidget *>(open),static_cast<QWidget *>(build),static_cast<QWidget *>(target),
            static_cast<QWidget *>(apply),static_cast<QWidget *>(output),static_cast<QWidget *>(status)})
        layout->addWidget(widget);
    layout->setStretchFactor(output,1);
    const auto msbuild = QDir(vs).filePath("MSBuild/Current/Bin/MSBuild.exe");
    const auto devenv = QDir(vs).filePath("Common7/IDE/devenv.exe");
    build->setEnabled(!vs.isEmpty() && QFile::exists(msbuild));
    open->setEnabled(!vs.isEmpty() && QFile::exists(devenv));
    apply->setEnabled(false);
    if (!build->isEnabled())
        status->setText(QObject::tr("Install Visual Studio 2022 with the Desktop development with C++ workload to build this project."));
    QStringList missions;
    for (const auto &object : project->objects)
        if (object.visible && object.details.value("gameplay").toString() == "missionStart")
            missions << QObject::tr("%1  %2").arg(object.details.value("mission").toString(),object.id);
    if (!missions.isEmpty())
        output->setPlainText(QObject::tr("Mission IDs for the C++ behavior\n")+missions.join('\n'));
    QProcess process;
    process.setWorkingDirectory(files.absolutePath());
    QTimer timeout;
    timeout.setSingleShot(true);
    std::optional<ProjectBehavior> staged;
    std::optional<ProjectBehavior> baseline;
    QByteArray generated;
    bool runningProgram = false;
    const auto drain = [&] {
        const auto standard = process.readAllStandardOutput();
        const auto errors = process.readAllStandardError();
        if (runningProgram) {
            generated += standard.left(std::max(0,16385-int(generated.size())));
            output->appendPlainText(QString::fromLocal8Bit(errors));
        } else {
            output->appendPlainText(QString::fromLocal8Bit(standard+errors));
        }
    };
    QObject::connect(open,&QPushButton::clicked,&dialog,[&] {
        QString error;
        if (!prepareBehaviorCode(files.absolutePath(),error))
            status->setText(error);
        else if (!QProcess::startDetached(devenv,{projectFile},files.absolutePath()))
            status->setText(QObject::tr("Visual Studio could not open the behavior project."));
    });
    QObject::connect(&process,&QProcess::readyReadStandardOutput,&dialog,drain);
    QObject::connect(&process,&QProcess::readyReadStandardError,&dialog,drain);
    QObject::connect(&timeout,&QTimer::timeout,&dialog,[&] { process.kill(); });
    QObject::connect(&process,&QProcess::errorOccurred,&dialog,[&](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart)
            return;
        timeout.stop();
        build->setEnabled(true);
        status->setText(process.errorString());
    });
    QObject::connect(&process,&QProcess::finished,&dialog,[&](int exitCode,QProcess::ExitStatus result) {
        timeout.stop();
        drain();
        if (result != QProcess::NormalExit || exitCode != 0) {
            build->setEnabled(true);
            status->setText(QObject::tr("The build or behavior program failed. Check its output below."));
            return;
        }
        if (!runningProgram) {
            runningProgram = true;
            status->setText(QObject::tr("Running the built behavior program…"));
            process.start(files.filePath("build/Behavior.exe"),QStringList{});
            timeout.start(10000);
            return;
        }
        QString error;
        staged = readBuiltBehavior(generated,error);
        build->setEnabled(true);
        apply->setEnabled(staged.has_value());
        status->setText(staged ? QObject::tr("Built %1. Review the output and choose an asset to apply it to.").arg(staged->name) : error);
        output->appendPlainText(QString::fromUtf8(generated));
    });
    QObject::connect(build,&QPushButton::clicked,&dialog,[&] {
        QString error;
        if (!prepareBehaviorCode(files.absolutePath(),error)) {
            status->setText(error);
            return;
        }
        generated.clear();
        staged.reset();
        runningProgram = false;
        apply->setEnabled(false);
        build->setEnabled(false);
        status->setText(QObject::tr("Building with MSBuild…"));
        process.start(msbuild,{projectFile,"/t:Rebuild","/nologo","/nodeReuse:false","/p:Configuration=Debug","/p:Platform=x64"});
        timeout.start(120000);
    });
    const auto captureTarget = [&] {
        baseline.reset();
        const auto *active = store.project();
        const auto id = target->currentData().toString();
        if (active && active->id == projectId && active->behaviors.contains(id))
            baseline = active->behaviors.value(id);
    };
    QObject::connect(target,&QComboBox::currentIndexChanged,&dialog,captureTarget);
    captureTarget();
    QObject::connect(apply,&QPushButton::clicked,&dialog,[&] {
        const auto *active = store.project();
        const auto id = target->currentData().toString();
        if (!active || active->id != projectId || !staged
            || (!id.isEmpty() && (!baseline || active->behaviors.value(id) != *baseline))) {
            status->setText(QObject::tr("The selected project or behavior changed. Reopen this dialog."));
            return;
        }
        QString error;
        const auto saved = store.saveBehavior(id,*staged,error);
        if (saved.isEmpty()) {
            status->setText(error);
            return;
        }
        if (id.isEmpty()) {
            target->addItem(QObject::tr("Update %1").arg(staged->name),saved);
            target->setCurrentIndex(target->count()-1);
        }
        captureTarget();
        status->setText(QObject::tr("Saved %1. Attach it in Details to run it in Play.").arg(staged->name));
    });
    dialog.exec();
    QObject::disconnect(&process,nullptr,&dialog,nullptr);
    if (process.state() != QProcess::NotRunning) {
        process.kill();
        process.waitForFinished(3000);
    }
}
