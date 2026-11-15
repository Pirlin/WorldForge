#pragma once

#include <QWidget>
#include <QMatrix4x4>
#include <QMap>
#include <functional>

#include "Components/ModelInteraction.h"
#include "State/ProjectStore.h"

class QLabel;
class QLineEdit;
class QPushButton;
class QListWidget;
class QTreeWidget;
class QTreeWidgetItem;
class QDoubleSpinBox;
class QSpinBox;
class QSplitter;
class QComboBox;
class QToolButton;
class QButtonGroup;
class ProjectStore;
class LiveViewport;
class UvCanvas;

class ModelingWorkspace : public QWidget {
    Q_OBJECT

public:
    explicit ModelingWorkspace(ProjectStore &store, QWidget *parent = nullptr);
    void resetLayout();
    void enterEditMode();
    bool commitPendingTransform();

protected:
    void hideEvent(QHideEvent *event) override;

private:
    enum class Transform { Move, Rotate, Scale };
    enum class SurfaceTool { None, Measure, MeasureAngle, Annotate, EraseAnnotation };
    enum class PairTool { None, TargetWeld, Connect, LoopCut };
    enum class Deform { Shear, ToSphere, AlongNormals };
    enum class SelectionCommand { Connected, Grow, Shrink, Border, Loop, Ring };
    enum class TopologyCommand { WeldDistance, WeldActive, Fill, GridFill, Bridge, Dissolve, LimitedDissolve,
                                 Bevel, Chamfer, SlideEdge, SlideVertex, RotateEdge, Delete };
    enum class UvCommand { Mark, Clear, Sharp, Unwrap, Pack, Density, Stitch, Planar, Box, Cylinder, Inspect };
    struct TransformEdit {
        Transform tool;
        QString asset;
        QSharedPointer<const worldforge::Mesh> base;
        std::set<int> vertices;
        QVector3D pivot;
        QMatrix4x4 localToSpace, spaceToLocal;
        QVector3D dragValues;
        QSharedPointer<const worldforge::Mesh> candidate;
        std::vector<float> falloff;
        std::vector<worldforge::VertexIndex> mirrorPairs;
        std::optional<SceneObject> objectBase, objectCandidate;
    };
    struct KnifeEdit {
        QString asset;
        QSharedPointer<const worldforge::Mesh> base, candidate;
        std::optional<worldforge::MeshCutPoint> start;
        int preferredFace = -1;
        std::vector<worldforge::MeshKnifeSegment> path;
        std::set<ModelComponent> resultFaces;
        std::set<worldforge::MeshEdge> baseSeams, resultSeams;
    };
    struct RetopologyEdit {
        QString asset, source;
        QSharedPointer<const worldforge::Mesh> base, candidate;
        std::optional<worldforge::MeshEdge> edge;
        QList<QVector3D> points;
        QVector3D normal;
        bool continuous = false;
    };

    void rebuildOutliner();
    void filterOutliner();
    void refresh();
    void setDomain(ModelDomain domain);
    void selectComponents(const QList<ModelComponent> &components, Qt::KeyboardModifiers modifiers);
    void selectFace(int face);
    void refreshSelection();
    void expandSelection(SelectionCommand command);
    void convertSelection(ModelDomain destination);
    void beginTransform(Transform tool);
    void deformSelection(Deform tool);
    void refreshGizmo();
    void setParentFromOutliner(const QString &child,const QString &parent);
    void previewTransform();
    void dragTransform(QPointF delta, QVector3D displacement, std::optional<QVector3D> snapTarget);
    bool finishTransform(bool apply);
    void modelKey(int key);
    void edit(const QString &label, const std::function<std::string(worldforge::Mesh &, std::size_t)> &operation);
    void editFaces(const QString &label, const std::function<std::string(worldforge::Mesh &, std::span<const std::size_t>)> &operation);
    void editTopology(TopologyCommand command,const QString &label);
    void startKnife();
    void knifePoint(ModelComponent edge,float fraction,int nearbyFace);
    void finishKnife(bool apply);
    void bisect();
    void spin();
    void revolve(bool edit = false);
    void transformCapturedComponents();
    void setPairTool(PairTool tool);
    void pickPairPoint(ModelComponent component,bool pressed,int nearbyFace);
    void previewLoopCut();
    void applyConnect();
    void setSurfaceTool(SurfaceTool tool);
    void sampleSurface(const QVector3D &point,bool first);
    void finishAnnotation();
    void assignBaseColorTexture();
    void editMaterialSlots(bool create);
    void startRetopology(bool extend,bool continuous = false);
    void retopologyPoint(const QVector3D &position,const QVector3D &normal);
    void finishRetopology(bool apply);
    void conformOrRelax(bool relax);
    void openModifierStack();
    void editMeshLod();
    void remesh();
    void editCollisionProxy();
    void editUvs(UvCommand command);
    void moveUvIsland(worldforge::Point2 delta);
    void transformUvIsland();

    ProjectStore &store_;
    LiveViewport *viewport_;
    UvCanvas *uv_;
    QTreeWidget *outliner_;
    QLineEdit *outlinerFilter_ = nullptr;
    QMap<QString,QTreeWidgetItem *> outlinerItems_;
    QLabel *summary_;
    QLabel *message_;
    QDoubleSpinBox *amount_;
    QSpinBox *edge_;
    QSpinBox *cuts_ = nullptr;
    QDoubleSpinBox *slide_ = nullptr;
    QSplitter *columns_;
    std::array<QToolButton *, 4> domains_{};
    std::array<QToolButton *, 3> tools_{};
    QToolButton *editMode_ = nullptr;
    QToolButton *xray_ = nullptr;
    QToolButton *ortho_ = nullptr;
    QToolButton *symmetry_ = nullptr;
    QToolButton *meshCommands_ = nullptr;
    QToolButton *knifeTool_ = nullptr;
    QToolButton *soft_ = nullptr;
    QWidget *transformBar_ = nullptr;
    QWidget *facePanel_ = nullptr;
    QLabel *transformLabel_ = nullptr;
    QComboBox *axis_ = nullptr;
    QComboBox *orientation_ = nullptr;
    QComboBox *pivotMode_ = nullptr;
    QComboBox *snap_ = nullptr;
    QComboBox *reference_ = nullptr;
    QButtonGroup *shading_ = nullptr;
    QToolButton *measureTool_ = nullptr;
    QToolButton *annotateTool_ = nullptr;
    QToolButton *eraseAnnotation_ = nullptr;
    QToolButton *weldTool_ = nullptr;
    QToolButton *connectTool_ = nullptr;
    QToolButton *loopCutTool_ = nullptr;
    QLabel *measurement_ = nullptr;
    QLabel *objectParent_ = nullptr;
    QLabel *objectWorld_ = nullptr;
    QComboBox *measurementMode_ = nullptr;
    QComboBox *materialSlot_ = nullptr;
    QPushButton *assignMaterial_ = nullptr;
    QPushButton *selectMaterialFaces_ = nullptr;
    SurfaceTool surfaceTool_ = SurfaceTool::None;
    PairTool pairTool_ = PairTool::None;
    std::optional<ModelComponent> pairStart_;
    int loopFace_ = -1;
    QSharedPointer<const worldforge::Mesh> connectPreview_;
    std::set<worldforge::MeshEdge> connectSeams_;
    QList<QVector3D> measurementPoints_, annotationPoints_;
    bool drawingAnnotation_ = false;
    std::array<QDoubleSpinBox *,9> objectTransform_{};
    QDoubleSpinBox *snapStep_ = nullptr;
    QDoubleSpinBox *softRadius_ = nullptr;
    QToolButton *apply_ = nullptr;
    std::array<QDoubleSpinBox *, 3> values_{};
    QList<QWidget *> faceControls_;
    QList<QWidget *> regionControls_;
    QString object_;
    QSharedPointer<const worldforge::Mesh> observed_;
    ModelDomain domain_ = ModelDomain::Object;
    std::set<ModelComponent> selected_;
    std::optional<ModelComponent> active_;
    std::optional<TransformEdit> transform_;
    std::optional<Transform> activeTool_ = Transform::Move;
    QMap<QString,std::set<int>> capturedComponents_;
    std::optional<KnifeEdit> knife_;
    std::optional<RetopologyEdit> retopology_;
    bool uvIslands_ = false;
};
