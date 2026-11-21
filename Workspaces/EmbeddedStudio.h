#pragma once

#include <QProcess>
#include <QTemporaryDir>
#include <QWidget>

class QTimer;

class EmbeddedStudio : public QWidget {
    Q_OBJECT

public:
    explicit EmbeddedStudio(QWidget *parent = nullptr);
    ~EmbeddedStudio() override;
    bool isRunning() const;
    bool showWorkspace(const QString &workspace);
    bool prepareAsset(const QByteArray &glb,const QString &source);
    bool closeForAssetSwitch();
    void clearAsset();
    QString stagedRevision() const;
    // The first installed WorldForgeStudio.exe, or an empty string when none is found.
    static QString installedExecutable();

signals:
    void workspaceReady();
    void sessionClosed(const QString &message);

protected:
    void resizeEvent(QResizeEvent *event) override;

private:
    void start();
    void attach();
    void resizeSurface();
    void returnToEditor(const QString &message = {});
    void destroyStartupWindow();

    QWidget *surface_;
    QByteArray workspaceRequest_;
    quint64 requestId_ = 0;
    QProcess process_;
    QTemporaryDir exchange_;
    QByteArray diagnostic_;
    QString startupError_;
    QTimer *poll_;
    WId studioWindow_ = 0;
    WId startupWindow_ = 0;
    int attempts_ = 0;
    bool assetSession_ = false;
    bool shuttingDown_ = false;
};
