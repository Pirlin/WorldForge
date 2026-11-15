#pragma once

#include "Components/ModelInteraction.h"

#include <QWidget>
#include <QVector3D>
#include <memory>
#include <optional>

class QLabel;
class ProjectStore;
class VulkanSceneWindow;
class QVulkanInstance;
struct SceneObject;

class LiveViewport : public QWidget {
    Q_OBJECT

public:
    enum class RunMode { Editing, Simulate, Play };
    explicit LiveViewport(ProjectStore &store, bool modelOnly, QWidget *parent = nullptr);
    ~LiveViewport() override;
    void frame();
    void previewTerrain();
    void setModelSelection(ModelDomain domain, const std::set<ModelComponent> &selection, bool xray);
    void setModelPreview(QSharedPointer<const worldforge::Mesh> mesh, std::optional<ModelGizmo> gizmo = {});
    void setBaseColorPreview(int slot,QImage image);
    void setObjectPreview(bool enabled,const SceneObject *object = nullptr);
    void setModelOrthographic(bool enabled);
    void setModelShading(ModelShading shading);
    void setModelAxisView(int axis);
    void setModelReference(const QString &id);
    void setModelLodPreview(bool enabled);
    void setModelKnife(bool enabled, std::optional<QVector3D> start = {});
    void setModelRetopology(bool enabled,const QList<QVector3D> &points = {});
    void setSculptMode(bool enabled);
    void setModelSurfaceTool(bool enabled);
    void setModelPairPicking(bool enabled);
    void setModelMarkup(const QList<QVector3D> &measurement,const QList<QVector3D> &stroke);
    std::optional<qsizetype> annotationNear(const QVector3D &position) const;
    std::optional<QVector3D> conformToReference(const QVector3D &position);
    bool hasRenderedFrame() const;
    QImage captureFrame();
    bool start(RunMode mode);
    void stop();
    void pause(bool paused);
    void step();
    RunMode runMode() const;
    void setTerrainBrush(float radius, float strength, bool smooth);
    void setTerrainLayerBrush(float radius,int layer);
    std::optional<QVector3D> runtimePosition(const QString &id) const;
    std::optional<QVector3D> playerPosition() const;
    QString runtimeObjective() const;
    QString rendererName() const { return rendererName_; }
    QString adapterCounterId() const { return adapterCounterId_; }
    quint64 dedicatedMemory() const { return dedicatedMemory_; }
    quint64 sharedMemory() const { return sharedMemory_; }
    void setRenderDevice(const QString &name, const QString &counterId, quint64 dedicatedMemory, quint64 sharedMemory);

signals:
    void componentsPicked(const QList<ModelComponent> &components, Qt::KeyboardModifiers modifiers);
    void modelDragged(const QPointF &pixels, const QVector3D &displacement, std::optional<QVector3D> snapTarget);
    void modelHandlePressed(int axis);
    void modelHandleReleased();
    void modelKeyPressed(int key);
    void modelCutPointPicked(ModelComponent edge, float fraction, int nearbyFace);
    void modelSurfacePicked(const QVector3D &position,const QVector3D &normal);
    void modelSurfaceToolPoint(const QVector3D &position,bool first);
    void modelSurfaceToolReleased();
    void modelPairPoint(ModelComponent point,bool pressed,int nearbyFace);
    void sculptStrokeBegan();
    void sculptSample(const QVector3D &position,const QVector3D &normal,Qt::KeyboardModifiers modifiers);
    void sculptStrokeFinished(bool commit);
    void rendererStatusChanged(const QString &status);
    void frameSubmitted();
    void runtimeChanged();

private:
    QLabel *status_;
    QWidget *container_ = nullptr;
    std::shared_ptr<QVulkanInstance> instance_;
    VulkanSceneWindow *window_ = nullptr;
    QString rendererName_;
    QString adapterCounterId_;
    quint64 dedicatedMemory_ = 0;
    quint64 sharedMemory_ = 0;
};
