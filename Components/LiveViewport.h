#pragma once

#include <QWidget>

class QLabel;
class ProjectStore;
class VulkanSceneWindow;

class LiveViewport : public QWidget {
    Q_OBJECT

public:
    explicit LiveViewport(ProjectStore &store, bool modelOnly, QWidget *parent = nullptr);
    void frame();
    void setFace(int face);
    bool hasRenderedFrame() const;

signals:
    void facePicked(int face);
    void rendererStatusChanged(const QString &status);

private:
    QLabel *status_;
    VulkanSceneWindow *window_ = nullptr;
};
