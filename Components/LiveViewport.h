#pragma once

#include <QWidget>
#include <memory>

class QLabel;
class ProjectStore;
class VulkanSceneWindow;
class QVulkanInstance;

class LiveViewport : public QWidget {
    Q_OBJECT

public:
    explicit LiveViewport(ProjectStore &store, bool modelOnly, QWidget *parent = nullptr);
    ~LiveViewport() override;
    void frame();
    void setFace(int face);
    bool hasRenderedFrame() const;

signals:
    void facePicked(int face);
    void rendererStatusChanged(const QString &status);

private:
    QLabel *status_;
    QWidget *container_ = nullptr;
    std::shared_ptr<QVulkanInstance> instance_;
    VulkanSceneWindow *window_ = nullptr;
};
