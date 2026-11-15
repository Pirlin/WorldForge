#include "Workspaces/EmbeddedStudio.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSettings>
#include <QSaveFile>
#include <QTimer>
#include <QVBoxLayout>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

EmbeddedStudio::EmbeddedStudio(QWidget *parent)
    : QWidget(parent), surface_(new QWidget(this)), poll_(new QTimer(this))
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0,0,0,0);
    layout->setSpacing(0);
    layout->addWidget(surface_,1);
    surface_->setAttribute(Qt::WA_NativeWindow);
    surface_->setAttribute(Qt::WA_DontCreateNativeAncestors);
    surface_->hide();
    connect(poll_,&QTimer::timeout,this,&EmbeddedStudio::attach);
    connect(&process_,&QProcess::started,this,[this] { poll_->start(100); });
    connect(&process_,&QProcess::finished,this,[this](int exitCode,QProcess::ExitStatus exitStatus) {
        poll_->stop();
        studioWindow_ = 0;
        surface_->hide();
        if (shuttingDown_)
            return;
        if (exitCode == 0 && exitStatus == QProcess::NormalExit)
            returnToEditor();
        else if (!startupError_.isEmpty())
            returnToEditor(startupError_);
        else {
            qWarning().noquote() << "WorldForge Studio exited" << exitCode
                << QString::fromUtf8(diagnostic_ + process_.readAllStandardError()).trimmed().right(320);
            returnToEditor(tr("The authoring workspace closed unexpectedly. Select its tab to reopen it."));
        }
    });
    connect(&process_,&QProcess::errorOccurred,this,[this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            returnToEditor(tr("Could not open the authoring workspace. %1").arg(process_.errorString()));
        }
    });
    connect(&process_,&QProcess::readyReadStandardError,this,[this] {
        diagnostic_ = (diagnostic_ + process_.readAllStandardError()).right(4096);
    });
    process_.setStandardOutputFile(QProcess::nullDevice());
}

EmbeddedStudio::~EmbeddedStudio()
{
    shuttingDown_ = true;
#ifdef Q_OS_WIN
    if (studioWindow_)
        PostMessage(reinterpret_cast<HWND>(studioWindow_),WM_CLOSE,0,0);
#endif
    if (process_.state() != QProcess::NotRunning && !process_.waitForFinished(1000)) {
        process_.kill();
        process_.waitForFinished(2000);
    }
    destroyStartupWindow();
}

void EmbeddedStudio::returnToEditor(const QString &message)
{
    destroyStartupWindow();
    emit sessionClosed(message);
}

void EmbeddedStudio::destroyStartupWindow()
{
#ifdef Q_OS_WIN
    if (startupWindow_) {
        DestroyWindow(reinterpret_cast<HWND>(startupWindow_));
        startupWindow_ = 0;
    }
#endif
}

bool EmbeddedStudio::isRunning() const
{
    return process_.state() != QProcess::NotRunning;
}

bool EmbeddedStudio::prepareAsset(const QByteArray &glb,const QString &source)
{
    if (isRunning() || !exchange_.isValid())
        return false;
    QSaveFile input(exchange_.filePath("input.glb"));
    if (!input.open(QIODevice::WriteOnly) || input.write(glb) != glb.size() || !input.commit())
        return false;
    const auto asset = exchange_.filePath("source.wfasset");
    if (QFile::exists(asset) && !QFile::remove(asset))
        return false;
    if (!source.isEmpty()) {
        QFile inputSource(source);
        QCryptographicHash digest(QCryptographicHash::Sha256);
        if (!inputSource.open(QIODevice::ReadOnly) || inputSource.size() > 128 * 1024 * 1024
            || !digest.addData(&inputSource) || digest.result().toHex() != QFileInfo(source).baseName().toLatin1()
            || !QFile::copy(source,asset))
            return false;
    }
    const auto ready = exchange_.filePath("ready");
    if (QFile::exists(ready) && !QFile::remove(ready))
        return false;
    assetSession_ = true;
    return true;
}

QString EmbeddedStudio::stagedRevision() const
{
    QFile ready(exchange_.filePath("ready"));
    if (!assetSession_ || !ready.open(QIODevice::ReadOnly) || ready.size() > 64)
        return {};
    const auto name = QString::fromLatin1(ready.readAll());
    static const QRegularExpression revisionName("^publish-[a-z0-9_]+$");
    return revisionName.match(name).hasMatch() ? exchange_.filePath(name) : QString{};
}

QString EmbeddedStudio::installedExecutable()
{
    const auto root = QCoreApplication::applicationDirPath();
    const auto local = QDir(qEnvironmentVariable("LOCALAPPDATA"));
    const QStringList candidates{
        QDir(root).filePath("Studio/WorldForgeStudio.exe"),
        local.filePath("WorldForge/StudioSource/build-studio-assets-x64/bin/WorldForgeStudio.exe"),
        QSettings().value("studio/executable").toString(),
        QDir(root).filePath("WorldForgeStudio.exe"),
        local.filePath("WorldForge/StudioSource/build-studio-x64/bin/WorldForgeStudio.exe")
    };
    for (const auto &path : candidates)
        if (!path.isEmpty() && QFileInfo(path).isFile())
            return path;
    return {};
}

void EmbeddedStudio::clearAsset()
{
    if (!isRunning())
        assetSession_ = false;
}

bool EmbeddedStudio::showWorkspace(const QString &workspace)
{
    QSaveFile request(exchange_.filePath("workspace"));
    const auto bytes = QByteArray::number(++requestId_) + '\n' + workspace.toUtf8();
    if (!exchange_.isValid() || !request.open(QIODevice::WriteOnly)
        || request.write(bytes) != bytes.size() || !request.commit()) {
        returnToEditor(tr("Could not request the authoring workspace."));
        return false;
    }
    workspaceRequest_ = bytes;
    attempts_ = 0;
    if (!isRunning())
        start();
    else if (!poll_->isActive())
        poll_->start(100);
    return true;
}

void EmbeddedStudio::start()
{
#ifdef Q_OS_WIN
    if (isRunning())
        return;
    for (const QString name : {QStringLiteral("StudioPublish.py"),QStringLiteral("StudioNavigation.py")}) {
        QFile script(":/Workspaces/"+name);
        QSaveFile output(exchange_.filePath(name));
        if (exchange_.isValid() && script.open(QIODevice::ReadOnly) && output.open(QIODevice::WriteOnly)) {
            const auto code = script.readAll();
            if (output.write(code) == code.size() && output.commit())
                continue;
        }
        returnToEditor(tr("Could not prepare the authoring workspace."));
        return;
    }
    QString executable = installedExecutable();
    if (executable.isEmpty()) {
        executable = QFileDialog::getOpenFileName(this,tr("Locate WorldForge Studio"),{},
            tr("WorldForge Studio (WorldForgeStudio.exe)"));
        if (executable.isEmpty()) {
            returnToEditor();
            return;
        }
        QSettings().setValue("studio/executable",executable);
    }
    attempts_ = 0;
    surface_->hide();
    diagnostic_.clear();
    startupError_.clear();
    process_.setWorkingDirectory(QFileInfo(executable).absolutePath());
    QStringList arguments{"--factory-startup","--disable-autoexec","--window-border",
        "--window-geometry","0","0","960","640"};
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert("WORLDFORGE_EMBEDDED_STUDIO","1");
    environment.insert("WORLDFORGE_HOSTED_WINDOW","1");
    // Keep the native viewport away from the active Qt/Vulkan surface until it is ready.
    startupWindow_ = reinterpret_cast<WId>(CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        L"STATIC",nullptr,WS_POPUP,0,0,960,640,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr));
    if (!startupWindow_) {
        returnToEditor(tr("Windows could not prepare the authoring workspace."));
        return;
    }
    environment.insert("WORLDFORGE_HOSTED_PARENT",QString::number(quintptr(startupWindow_)));
    process_.setProcessEnvironment(environment);
    arguments << "--python-exit-code" << "1";
    arguments << "--python" << exchange_.filePath("StudioPublish.py");
    arguments << "--python" << exchange_.filePath("StudioNavigation.py") << "--" << exchange_.path();
    if (assetSession_)
        arguments << "--asset-session";
    else
        arguments << "--authoring-tools-only";
    process_.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *creation) {
        creation->flags |= CREATE_NO_WINDOW;
        creation->startupInfo->dwFlags |= STARTF_USESHOWWINDOW;
        creation->startupInfo->wShowWindow = SW_HIDE;
    });
    process_.start(executable,arguments);
#else
    returnToEditor(tr("The authoring workspace is available on Windows."));
#endif
}

void EmbeddedStudio::attach()
{
#ifdef Q_OS_WIN
    struct WindowSearch { DWORD pid; HWND window; } search{DWORD(process_.processId()),nullptr};
    const auto findWindow = [](HWND window,LPARAM parameter) -> BOOL {
        auto &result = *reinterpret_cast<WindowSearch *>(parameter);
        DWORD pid = 0;
        GetWindowThreadProcessId(window,&pid);
        if (pid == result.pid && !GetWindow(window,GW_OWNER)) {
            const bool ready = GetPropW(window,L"WorldForgeHostedWindow") != nullptr;
            wchar_t title[256]{};
            GetWindowTextW(window,title,256);
            if (ready || QString::fromWCharArray(title).contains(QStringLiteral("WorldForge Studio"))) {
                result.window = window;
                return FALSE;
            }
        }
        return TRUE;
    };
    if (++attempts_ >= 300) {
        startupError_ = tr("The authoring workspace did not finish opening.");
        poll_->stop();
        process_.kill();
        return;
    }
    QFile ready(exchange_.filePath("workspace-ready"));
    if (!ready.open(QIODevice::ReadOnly) || ready.read(256) != workspaceRequest_)
        return;
    ready.close();
    if (!studioWindow_) {
        if (startupWindow_)
            EnumChildWindows(reinterpret_cast<HWND>(startupWindow_),findWindow,reinterpret_cast<LPARAM>(&search));
        if (!search.window)
            EnumWindows(findWindow,reinterpret_cast<LPARAM>(&search));
        if (!search.window)
            return;
        const auto window = search.window;
        const auto host = reinterpret_cast<HWND>(surface_->winId());
        if (GetParent(window) != host) {
            const auto style = GetWindowLongPtr(window,GWL_STYLE);
            SetWindowLongPtr(window,GWL_STYLE,(style & ~LONG_PTR(WS_POPUP | WS_OVERLAPPEDWINDOW)) | WS_CHILD);
            SetLastError(ERROR_SUCCESS);
            if (!SetParent(window,host) && GetLastError() != ERROR_SUCCESS) {
                startupError_ = tr("Windows could not attach the authoring workspace.");
                poll_->stop();
                process_.kill();
                return;
            }
            SetWindowLongPtr(window,GWL_EXSTYLE,GetWindowLongPtr(window,GWL_EXSTYLE) & ~LONG_PTR(WS_EX_APPWINDOW));
        }
        studioWindow_ = reinterpret_cast<WId>(window);
        destroyStartupWindow();
    }
    poll_->stop();
    const auto window = reinterpret_cast<HWND>(studioWindow_);
    const auto style = GetWindowLongPtr(window,GWL_STYLE);
    const auto flatStyle = (style & ~LONG_PTR(WS_POPUP | WS_OVERLAPPEDWINDOW)) | WS_CHILD;
    if (style != flatStyle)
        SetWindowLongPtr(window,GWL_STYLE,flatStyle);
    const auto exStyle = GetWindowLongPtr(window,GWL_EXSTYLE);
    const auto flatExStyle = exStyle & ~LONG_PTR(WS_EX_APPWINDOW | WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE);
    if (exStyle != flatExStyle)
        SetWindowLongPtr(window,GWL_EXSTYLE,flatExStyle);
    emit workspaceReady();
    surface_->show();
    resizeSurface();
    ShowWindowAsync(window,SW_SHOWNOACTIVATE);
    static const UINT focusMessage = RegisterWindowMessageW(L"WorldForge.Studio.Focus");
    if (focusMessage && GetForegroundWindow() == reinterpret_cast<HWND>(this->window()->winId()))
        PostMessageW(window,focusMessage,0,0);
#endif
}

void EmbeddedStudio::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    QTimer::singleShot(0,this,&EmbeddedStudio::resizeSurface);
}

void EmbeddedStudio::resizeSurface()
{
#ifdef Q_OS_WIN
    if (!studioWindow_)
        return;
    RECT bounds{};
    GetClientRect(reinterpret_cast<HWND>(surface_->winId()),&bounds);
    SetWindowPos(reinterpret_cast<HWND>(studioWindow_),nullptr,0,0,bounds.right,bounds.bottom,
        SWP_NOACTIVATE | SWP_NOZORDER | SWP_ASYNCWINDOWPOS | SWP_FRAMECHANGED);
#endif
}
