#include "Components/NativeGameplayEditor.h"
#include "Components/BehaviorCode.h"
#include "State/WorldRuntime.h"
#include "State/NativeComponents.h"

#include <QCoreApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QSaveFile>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <sstream>

namespace {
bool writeFile(const QString &path,const QByteArray &bytes,QString &error)
{
    QSaveFile file(path);
    if (file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit())
        return true;
    error = file.errorString();
    return false;
}

QString symbolsPath(const QString &binary)
{
    const QFileInfo file(binary);
    return file.dir().filePath(file.completeBaseName()+".pdb");
}
}

bool prepareNativeGameplay(const QString &directory,QString &error)
{
    const QDir dir(directory);
    if (!QDir().mkpath(dir.absolutePath())) {
        error = QObject::tr("Could not create the native gameplay source directory.");
        return false;
    }
    for (const auto *name : {"NativeGameplay.vcxproj","NativeGameplay.cpp","WorldForgeNative.h","WorldForgeNativeComponents.h"}) {
        const auto path = dir.filePath(QLatin1String(name));
        if (QFile::exists(path))
            continue;
        const auto resourcePath = (std::string_view(name).ends_with(".h")
            ? QStringLiteral(":/Engine/World/") : QStringLiteral(":/Resources/BehaviorCode/"))+QLatin1String(name);
        QFile resource(resourcePath);
        if (!resource.open(QIODevice::ReadOnly) || !writeFile(path,resource.readAll(),error)) {
            if (error.isEmpty())
                error = resource.errorString();
            return false;
        }
    }
    return true;
}

bool activateNativeGameplay(ProjectStore &store,const QString &binary,QString &error)
{
    const auto active = store.nativeGameplayPath();
    QFile file(binary);
    if (active.isEmpty() || !file.open(QIODevice::ReadOnly) || file.size() <= 0 || file.size() > 32*1024*1024) {
        error = QObject::tr("Choose an open project and a built gameplay DLL of up to 32 MiB.");
        return false;
    }
    const auto payload = file.readAll();
    if (payload.size() != file.size()) {
        error = file.errorString();
        return false;
    }
    QTemporaryDir validation;
    if (!validation.isValid() || !writeFile(validation.filePath("Gameplay.dll"),payload,error)) {
        if (error.isEmpty())
            error = QObject::tr("Could not stage the new gameplay module for validation.");
        return false;
    }
    worldforge::NativeGameplay probe;
    std::string detail;
    std::vector<worldforge::NativeInstance> instances;
    if (!collectNativeInstances(store,instances,error))
        return false;
    if (!probe.stage(std::filesystem::path(validation.filePath("Gameplay.dll").toStdWString()),detail,instances)) {
        error = QString::fromStdString(detail);
        return false;
    }
    const auto folder = QFileInfo(active).absoluteDir();
    if (!QDir().mkpath(folder.absolutePath())) {
        error = QObject::tr("Could not create the local gameplay module directory.");
        return false;
    }
    if (!writeFile(active,payload,error))
        return false;
    const auto activeSymbols = symbolsPath(active);
    QFile symbols(symbolsPath(binary));
    QString symbolError;
    if (symbols.open(QIODevice::ReadOnly) && symbols.size() <= 128*1024*1024) {
        if (!writeFile(activeSymbols,symbols.readAll(),symbolError))
            QFile::remove(activeSymbols);
    } else {
        QFile::remove(activeSymbols);
    }
    emit store.nativeGameplayChanged();
    return true;
}

bool exportNativeGame(const ProjectStore &store,const QString &directory,QString &error)
{
    const auto active = store.nativeGameplayPath();
    const QFileInfo destination(directory);
    const auto player = QDir(QCoreApplication::applicationDirPath()).filePath("WorldForgePlayer.exe");
    if (!QFile::exists(active) || !QFile::exists(player) || destination.exists() || !destination.dir().exists()) {
        error = QObject::tr("Activate a C++ module, use a build containing the Player, and choose a new game folder.");
        return false;
    }
    std::ostringstream data;
    if (!WorldRuntime::exportScene(store,data,&error,true)) {
        if (error.isEmpty())
            error = QObject::tr("The scene could not be exported.");
        return false;
    }
    QTemporaryDir staging(destination.dir().filePath(".worldforge-game-XXXXXX"));
    const auto scene = std::move(data).str();
    if (!staging.isValid()
        || !writeFile(staging.filePath("Game.wfplay"),QByteArray(scene.data(),qsizetype(scene.size())),error)
        || !QFile::copy(player,staging.filePath("WorldForgePlayer.exe"))
        || !QFile::copy(active,staging.filePath("Gameplay.dll"))) {
        if (error.isEmpty())
            error = QObject::tr("Could not write the game package.");
        return false;
    }
    const auto symbols = symbolsPath(active);
    if (QFile::exists(symbols) && !QFile::copy(symbols,staging.filePath("Gameplay.pdb"))) {
        error = QObject::tr("Could not copy the gameplay debug symbols.");
        return false;
    }
    const auto redistributable = QDir(QCoreApplication::applicationDirPath()).filePath("vc_redist.x64.exe");
    if (QFile::exists(redistributable) && !QFile::copy(redistributable,staging.filePath("vc_redist.x64.exe"))) {
        error = QObject::tr("Could not copy the Visual C++ Redistributable.");
        return false;
    }
    const QDir licenses(QDir(QCoreApplication::applicationDirPath()).filePath("../licenses"));
    for (const auto &notice : licenses.entryInfoList({"*-LICENSE.txt"},QDir::Files)) {
        if (!QDir(staging.path()).mkpath("licenses")
            || !QFile::copy(notice.absoluteFilePath(),staging.filePath("licenses/"+notice.fileName()))) {
            error = QObject::tr("Could not include the Player's third-party license notices.");
            return false;
        }
    }
    if (!destination.dir().rename(QFileInfo(staging.path()).fileName(),destination.fileName())) {
        error = QObject::tr("Could not move the completed game into the selected folder.");
        return false;
    }
    staging.setAutoRemove(false);
    return true;
}

void editNativeComponents(ProjectStore &store,QWidget *parent)
{
    const auto *owner = store.selectedObject();
    if (!owner)
        return;
    const auto ownerId = owner->id;
    const auto projectId = store.project()->id;
    auto authored = owner->details.value("nativeComponents").toObject();
    worldforge::NativeGameplay module;
    std::string diagnostic;
    module.stage(std::filesystem::path(store.nativeGameplayPath().toStdWString()),diagnostic);
    const auto &types = module.componentTypes();

    QDialog dialog(parent);
    dialog.setObjectName("nativeComponentsEditor");
    dialog.setWindowTitle(QObject::tr("Native Components · %1").arg(owner->name));
    dialog.resize(535,360);
    auto *layout = new QVBoxLayout(&dialog);
    auto *chooser = new QComboBox(&dialog);
    chooser->setObjectName("nativeComponentType");
    layout->addWidget(chooser);
    auto *enabled = new QCheckBox(QObject::tr("Enabled"),&dialog);
    enabled->setChecked(true);
    layout->addWidget(enabled);
    auto *scroll = new QScrollArea(&dialog);
    scroll->setWidgetResizable(true);
    auto *fieldPanel = new QWidget(scroll);
    auto *form = new QFormLayout(fieldPanel);
    scroll->setWidget(fieldPanel);
    layout->addWidget(scroll,1);
    auto *status = new QLabel(&dialog);
    status->setWordWrap(true);
    status->setTextFormat(Qt::PlainText);
    layout->addWidget(status);
    auto *commands = new QHBoxLayout;
    auto *apply = new QPushButton(QObject::tr("Save Component"),&dialog);
    apply->setObjectName("saveNativeComponent");
    auto *remove = new QPushButton(QObject::tr("Remove"),&dialog);
    auto *close = new QPushButton(QObject::tr("Close"),&dialog);
    commands->addWidget(apply);
    commands->addWidget(remove);
    commands->addStretch();
    commands->addWidget(close);
    layout->addLayout(commands);
    std::vector<std::pair<const worldforge::NativeProperty *,QWidget *>> fields;
    const auto currentType = [&]() -> const worldforge::NativeComponentType * {
        const auto id = chooser->currentData().toString().toStdString();
        const auto item = std::ranges::find(types,id,&worldforge::NativeComponentType::id);
        return item == types.end() ? nullptr : &*item;
    };
    const auto refresh = [&] {
        fields.clear();
        while (form->rowCount())
            form->removeRow(0);
        const auto id = chooser->currentData().toString();
        const auto *type = currentType();
        const auto stored = authored.value(id).toObject();
        const auto overrides = stored.value("properties").toObject();
        apply->setText(stored.isEmpty() ? QObject::tr("Attach Component") : QObject::tr("Save Component"));
        apply->setEnabled(type);
        remove->setEnabled(!stored.isEmpty());
        enabled->setEnabled(type);
        enabled->setChecked(stored.value("enabled").toBool(true));
        if (!type)
            return;
        const auto *project = store.project();
        for (const auto &field : type->properties) {
            const auto key = QString::fromStdString(field.id);
            const auto value = overrides.contains(key) ? readNativeProperty(overrides.value(key)) : field.initial;
            QWidget *input = nullptr;
            switch (field.initial.kind) {
            case WF_GAME_BOOL: {
                auto *check = new QCheckBox(&dialog);
                check->setChecked(value.number != 0);
                input = check;
                break;
            }
            case WF_GAME_INT: {
                auto *spin = new QSpinBox(&dialog);
                spin->setRange(INT32_MIN,INT32_MAX);
                spin->setValue(int(value.number));
                input = spin;
                break;
            }
            case WF_GAME_REAL: {
                auto *spin = new QDoubleSpinBox(&dialog);
                spin->setDecimals(6);
                spin->setRange(-1e9,1e9);
                spin->setValue(value.number);
                input = spin;
                break;
            }
            default: {
                auto *picker = new QComboBox(&dialog);
                picker->addItem(QObject::tr("None"),QString{});
                const auto addAssets = [picker](const auto &assets) {
                    for (auto it = assets.begin(); it != assets.end(); ++it)
                        picker->addItem(it->name,it.key());
                };
                switch (field.initial.kind) {
                case WF_GAME_OBJECT:
                    for (const auto &object : project->objects)
                        if (!store.isLegacyPlaceholder(object))
                            picker->addItem(object.name,object.id);
                    break;
                case WF_GAME_MESH: addAssets(project->assets); break;
                case WF_GAME_MATERIAL: addAssets(project->surfaces.materials); break;
                case WF_GAME_TEXTURE: addAssets(project->surfaces.textures); break;
                }
                const auto selected = QString::fromStdString(value.reference);
                if (picker->findData(selected) < 0)
                    picker->addItem(QObject::tr("Missing reference"),selected);
                picker->setCurrentIndex(picker->findData(selected));
                input = picker;
                break;
            }
            }
            input->setObjectName("nativeProperty."+key);
            form->addRow(QString::fromStdString(field.label),input);
            fields.emplace_back(&field,input);
        }
    };
    const auto populate = [&](const QString &selected) {
        const QSignalBlocker guard(chooser);
        chooser->clear();
        for (const auto &type : types) {
            const auto id = QString::fromStdString(type.id);
            auto label = QString::fromStdString(type.label);
            if (authored.contains(id))
                label += QObject::tr(" (attached)");
            chooser->addItem(label,id);
        }
        for (auto it = authored.begin(); it != authored.end(); ++it)
            if (chooser->findData(it.key()) < 0)
                chooser->addItem(QObject::tr("Missing type · %1").arg(it.key()),it.key());
        const auto index = chooser->findData(selected);
        chooser->setCurrentIndex(index < 0 ? 0 : index);
        refresh();
    };
    const auto save = [&](std::optional<QJsonObject> entry) {
        const auto *current = store.object(ownerId);
        if (!current || store.project()->id != projectId || current->details.value("nativeComponents").toObject() != authored) {
            status->setText(QObject::tr("The object's components changed. Reopen this panel."));
            return;
        }
        const auto id = chooser->currentData().toString();
        QString error;
        if (!store.setNativeComponent(ownerId,id,std::move(entry),error)) {
            status->setText(error);
            return;
        }
        authored = store.object(ownerId)->details.value("nativeComponents").toObject();
        populate(id);
        status->setText(QObject::tr("Saved. Start Play to use the new component settings."));
    };
    QObject::connect(chooser,&QComboBox::currentIndexChanged,&dialog,refresh);
    QObject::connect(apply,&QPushButton::clicked,&dialog,[&] {
        QJsonObject overrides;
        for (const auto &[property,input] : fields) {
            worldforge::NativePropertyValue value{property->initial.kind,0,{}};
            if (const auto *check = qobject_cast<QCheckBox *>(input))
                value.number = check->isChecked() ? 1 : 0;
            else if (const auto *integer = qobject_cast<QSpinBox *>(input))
                value.number = integer->value();
            else if (const auto *real = qobject_cast<QDoubleSpinBox *>(input))
                value.number = real->value();
            else
                value.reference = qobject_cast<QComboBox *>(input)->currentData().toString().toStdString();
            if (value != property->initial)
                overrides.insert(QString::fromStdString(property->id),writeNativeProperty(value));
        }
        save(QJsonObject{{"enabled",enabled->isChecked()},{"properties",overrides}});
    });
    QObject::connect(remove,&QPushButton::clicked,&dialog,[&] { save(std::nullopt); });
    QObject::connect(close,&QPushButton::clicked,&dialog,&QDialog::accept);
    populate({});
    if (types.empty())
        status->setText(diagnostic.empty() ? QObject::tr("The active module exposes no native component types.")
            : QString::fromStdString(diagnostic));
    dialog.exec();
}

void showNativeGameplay(ProjectStore &store,QWidget *parent)
{
    const auto *project = store.project();
    if (!project)
        return;
    const auto projectId = project->id;
    const auto location = project->configuration.value("location").toString();
    const QDir folder(location.isEmpty() ? QFileInfo(store.nativeGameplayPath()).dir().filePath("Source")
        : QDir(location).filePath("Programming/NativeGameplay"));
    const auto projectFile = folder.filePath("NativeGameplay.vcxproj");
    const auto vs = behaviorVisualStudio();
    const auto devenv = QDir(vs).filePath("Common7/IDE/devenv.exe");
    const auto msbuild = QDir(vs).filePath("MSBuild/Current/Bin/MSBuild.exe");
    QDialog dialog(parent);
    dialog.setObjectName("nativeGameplayEditor");
    dialog.setWindowTitle(QObject::tr("C++ Gameplay Module"));
    dialog.resize(650,460);
    auto *layout = new QVBoxLayout(&dialog);
    auto *path = new QLabel(QDir::toNativeSeparators(projectFile),&dialog);
    path->setTextFormat(Qt::PlainText);
    path->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(path);
    if (const auto *object = store.selectedObject()) {
        auto *id = new QLineEdit(object->id,&dialog);
        id->setReadOnly(true);
        id->setAccessibleName(QObject::tr("Selected object ID"));
        layout->addWidget(new QLabel(QObject::tr("Selected object ID for find_body"),&dialog));
        layout->addWidget(id);
    }
    auto *open = new QPushButton(QObject::tr("Open in Visual Studio"),&dialog);
    auto *build = new QPushButton(QObject::tr("Build and Activate"),&dialog);
    build->setObjectName("buildNativeGameplay");
    auto *disable = new QPushButton(QObject::tr("Disable for this Project"),&dialog);
    auto *package = new QPushButton(QObject::tr("Export C++ Game…"),&dialog);
    auto *output = new QPlainTextEdit(&dialog);
    output->setObjectName("nativeBuildOutput");
    output->setReadOnly(true);
    output->setMaximumBlockCount(2000);
    auto *status = new QLabel(&dialog);
    status->setObjectName("nativeBuildStatus");
    status->setTextFormat(Qt::PlainText);
    status->setWordWrap(true);
    for (auto *button : {open,build,disable,package})
        layout->addWidget(button);
    layout->addWidget(output,1);
    layout->addWidget(status);
    const bool hasCompiler = !vs.isEmpty() && QFile::exists(msbuild);
    open->setEnabled(!vs.isEmpty() && QFile::exists(devenv));
    build->setEnabled(hasCompiler);
    const auto refresh = [&] {
        const bool active = QFile::exists(store.nativeGameplayPath());
        disable->setEnabled(active);
        package->setEnabled(active);
    };
    refresh();
    status->setText(hasCompiler
        ? QObject::tr("Build activates trusted project code for Play on this PC. Use Tools to rebuild during Play.")
        : QObject::tr("Install Visual Studio 2022 with Desktop development with C++."));
    QProcess process;
    process.setWorkingDirectory(folder.absolutePath());
    process.setProcessChannelMode(QProcess::MergedChannels);
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout,&QTimer::timeout,&dialog,[&] {
        process.kill();
    });
    QObject::connect(&process,&QProcess::readyReadStandardOutput,&dialog,[&] {
        output->appendPlainText(QString::fromLocal8Bit(process.readAllStandardOutput()));
    });
    QObject::connect(open,&QPushButton::clicked,&dialog,[&] {
        QString error;
        if (!prepareNativeGameplay(folder.absolutePath(),error))
            status->setText(error);
        else if (!QProcess::startDetached(devenv,{projectFile},folder.absolutePath()))
            status->setText(QObject::tr("Could not open the native project in Visual Studio."));
    });
    QObject::connect(build,&QPushButton::clicked,&dialog,[&] {
        QString error;
        if (!prepareNativeGameplay(folder.absolutePath(),error)) {
            status->setText(error);
            return;
        }
        output->clear();
        build->setEnabled(false);
        disable->setEnabled(false);
        package->setEnabled(false);
        status->setText(QObject::tr("Building the C++ gameplay DLL…"));
        process.start(msbuild,{projectFile,"/t:Rebuild","/nologo","/nodeReuse:false","/p:Configuration=Debug","/p:Platform=x64"});
        timeout.start(120000);
    });
    QObject::connect(&process,&QProcess::errorOccurred,&dialog,[&](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            timeout.stop();
            status->setText(process.errorString());
            build->setEnabled(hasCompiler);
            refresh();
        }
    });
    QObject::connect(&process,&QProcess::finished,&dialog,[&](int code,QProcess::ExitStatus result) {
        timeout.stop();
        output->appendPlainText(QString::fromLocal8Bit(process.readAllStandardOutput()));
        build->setEnabled(hasCompiler);
        QString error;
        if (result != QProcess::NormalExit || code != 0)
            error = QObject::tr("Build failed. The last active module is still selected.");
        else if (!store.project() || store.project()->id != projectId)
            error = QObject::tr("The active project changed. Reopen this dialog.");
        else if (activateNativeGameplay(store,folder.filePath("build/Gameplay.dll"),error))
            status->setText(QObject::tr("Activated. Start Play, or resume Play to apply the module at the next fixed update."));
        if (!error.isEmpty())
            status->setText(error);
        refresh();
    });
    QObject::connect(disable,&QPushButton::clicked,&dialog,[&] {
        if (QFile::remove(store.nativeGameplayPath())) {
            emit store.nativeGameplayChanged();
            status->setText(QObject::tr("C++ gameplay is disabled for this project."));
        } else {
            status->setText(QObject::tr("Could not disable the active module."));
        }
        refresh();
    });
    QObject::connect(package,&QPushButton::clicked,&dialog,[&] {
        const auto base = QFileDialog::getExistingDirectory(&dialog,QObject::tr("Choose a parent folder for the game"));
        if (base.isEmpty())
            return;
        QDir parentDir(base);
        QString name = "WorldForge Game";
        for (int i = 2; parentDir.exists(name); ++i)
            name = QStringLiteral("WorldForge Game %1").arg(i);
        const auto destination = parentDir.filePath(name);
        QString error;
        status->setText(exportNativeGame(store,destination,error)
            ? QObject::tr("Exported %1. Open WorldForgePlayer.exe there.").arg(QDir::toNativeSeparators(destination))
            : error);
    });
    dialog.exec();
    QObject::disconnect(&process,nullptr,&dialog,nullptr);
    if (process.state() != QProcess::NotRunning) {
        process.kill();
        process.waitForFinished(3000);
    }
}
