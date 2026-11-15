#pragma once

#include <QJsonObject>
#include <QObject>
#include <QProcess>
#include <QStringList>
#include <QTimer>

#include <memory>

class QTemporaryDir;

// Runs Workspaces/ForgeJob.py in a background Studio process on a working copy of one asset.
class ForgeAiJob : public QObject {
    Q_OBJECT

public:
    struct Result {
        bool ok = false;
        bool canceled = false;
        QString message;
        QStringList steps;
        QByteArray glb;
        QByteArray source;
    };

    explicit ForgeAiJob(QObject *parent = nullptr);
    ~ForgeAiJob() override;
    // `sourcePath` is the asset's editable .wfasset, or empty to start from `inputGlb`.
    bool start(const QString &studio, const QByteArray &inputGlb, const QString &sourcePath,
               const QJsonObject &job, QString &error, int timeoutMs = 120000);
    void cancel();
    bool isRunning() const;

signals:
    void finished(const ForgeAiJob::Result &result);

private:
    void complete(Result result);
    Result collect(int exitCode, QProcess::ExitStatus status);

    QProcess process_;
    QTimer timeout_;
    std::unique_ptr<QTemporaryDir> exchange_;
    bool canceled_ = false;
    bool timedOut_ = false;
};
