#include "Workspaces/ForgeAiJob.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTemporaryDir>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

namespace {

bool writeFile(const QString &path, const QByteArray &bytes)
{
    QSaveFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit();
}

QByteArray readFile(const QString &path, qint64 limit)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > limit)
        return {};
    return file.readAll();
}

}

ForgeAiJob::ForgeAiJob(QObject *parent) : QObject(parent)
{
    timeout_.setSingleShot(true);
    connect(&timeout_, &QTimer::timeout, this, [this] {
        timedOut_ = true;
        process_.kill();
    });
    connect(&process_, &QProcess::finished, this, [this](int exitCode, QProcess::ExitStatus status) {
        complete(collect(exitCode, status));
    });
    connect(&process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            complete({false, false, tr("Could not start WorldForge Studio. %1").arg(process_.errorString())});
    });
}

ForgeAiJob::~ForgeAiJob()
{
    disconnect(&process_, nullptr, this, nullptr);
    if (process_.state() != QProcess::NotRunning) {
        process_.kill();
        process_.waitForFinished(3000);
    }
}

bool ForgeAiJob::isRunning() const
{
    return process_.state() != QProcess::NotRunning;
}

bool ForgeAiJob::start(const QString &studio, const QByteArray &inputGlb, const QString &sourcePath,
                       const QJsonObject &job, QString &error, int timeoutMs)
{
    if (isRunning()) {
        error = tr("A Forge AI job is already running.");
        return false;
    }
    if (studio.isEmpty() || !QFile::exists(studio)) {
        error = tr("Forge AI needs WorldForge Studio to run authoring operations. Install Studio or open it once "
                   "from the Modeling tab to locate it.");
        return false;
    }
    exchange_ = std::make_unique<QTemporaryDir>(QDir::temp().filePath("worldforge-forge-XXXXXX"));
    const QDir exchange(exchange_->path());
    bool prepared = exchange_->isValid() && writeFile(exchange.filePath("input.glb"), inputGlb)
        && writeFile(exchange.filePath("forge-job.json"), QJsonDocument(job).toJson(QJsonDocument::Compact));
    if (prepared && !sourcePath.isEmpty())
        prepared = QFile::copy(sourcePath, exchange.filePath("source.wfasset"));
    for (const QString name : {QStringLiteral("StudioPublish.py"), QStringLiteral("ForgeJob.py")}) {
        QFile script(":/Workspaces/" + name);
        prepared = prepared && script.open(QIODevice::ReadOnly) && writeFile(exchange.filePath(name), script.readAll());
    }
    if (!prepared) {
        error = tr("Could not prepare the Studio working copy.");
        exchange_.reset();
        return false;
    }
    canceled_ = timedOut_ = false;
    process_.setWorkingDirectory(QFileInfo(studio).absolutePath());
    process_.setStandardOutputFile(QProcess::nullDevice());
#ifdef Q_OS_WIN
    process_.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *creation) {
        creation->flags |= CREATE_NO_WINDOW;
    });
#endif
    process_.start(studio, {"--background", "--factory-startup", "--disable-autoexec", "--python-exit-code", "1",
        "--python", exchange.filePath("StudioPublish.py"), "--python", exchange.filePath("ForgeJob.py"),
        "--", exchange.path(), "--asset-session"});
    timeout_.start(timeoutMs);
    return true;
}

void ForgeAiJob::cancel()
{
    if (!isRunning())
        return;
    canceled_ = true;
    process_.kill();
}

ForgeAiJob::Result ForgeAiJob::collect(int exitCode, QProcess::ExitStatus status)
{
    Result result;
    if (canceled_) {
        result.canceled = true;
        result.message = tr("Canceled. The project was not changed.");
        return result;
    }
    if (timedOut_) {
        result.message = tr("Studio did not finish within the time limit. The project was not changed.");
        return result;
    }
    if (!exchange_)
        return result;
    const QDir exchange(exchange_->path());
    const auto report = QJsonDocument::fromJson(readFile(exchange.filePath("forge-result.json"), 1024 * 1024)).object();
    for (const auto &step : report.value("steps").toArray())
        result.steps << step.toString();
    if (report.value("status").toString() != "PASS") {
        result.message = report.value("message").toString();
        if (result.message.isEmpty())
            result.message = status == QProcess::CrashExit ? tr("Studio stopped unexpectedly. The project was not changed.")
                : tr("Studio exited with code %1 without a result. The project was not changed.").arg(exitCode);
        return result;
    }
    const auto name = QString::fromLatin1(readFile(exchange.filePath("ready"), 64));
    static const QRegularExpression revisionName("^publish-[a-z0-9_]+$");
    if (!revisionName.match(name).hasMatch()) {
        result.message = tr("Studio did not publish a reviewable revision.");
        return result;
    }
    const QDir revision(exchange.filePath(name));
    result.glb = readFile(revision.filePath("mesh.glb"), 64 * 1024 * 1024);
    result.source = readFile(revision.filePath("source.wfasset"), 128 * 1024 * 1024);
    result.ok = !result.glb.isEmpty() && !result.source.isEmpty();
    if (!result.ok)
        result.message = tr("The published revision is missing its mesh or editable source.");
    return result;
}

void ForgeAiJob::complete(Result result)
{
    timeout_.stop();
    exchange_.reset();
    emit finished(result);
}
