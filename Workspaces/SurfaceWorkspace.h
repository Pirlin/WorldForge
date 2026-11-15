#pragma once

#include <QImage>
#include <QSharedPointer>
#include <QWidget>
#include <array>
#include <optional>

namespace worldforge { struct Mesh; }
class ProjectStore;
class LiveViewport;
class TextureUvView;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QToolButton;
class QCheckBox;
class QSplitter;
class QSpinBox;
class QTimer;

class SurfaceWorkspace : public QWidget {
public:
    SurfaceWorkspace(ProjectStore &store,bool textureWorkspace,QWidget *parent = nullptr);
    void resetLayout();

protected:
    void hideEvent(QHideEvent *event) override;

private:
    void refresh();
    void refreshSlot();
    void refreshMaps();
    void changeTexture(bool remove);
    void exportTexture();
    void exportUvLayout();
    void inspectUvLayout();
    void applyMaterial();
    void assignMaterial();
    void linkMaterial();
    bool beginPaint();
    void finishPaint(bool commit);

    ProjectStore &store_;
    LiveViewport *viewport_;
    TextureUvView *uv_;
    QComboBox *object_, *slot_, *uvSet_;
    QLabel *message_, *imageInfo_, *sharedInfo_;
    QLineEdit *name_;
    QDoubleSpinBox *metallic_, *roughness_, *emission_;
    QPushButton *color_, *import_, *remove_, *apply_, *link_, *unlink_, *assign_, *exportUvs_;
    QPushButton *inspectUvs_, *exportMap_;
    QCheckBox *showUvs_;
    QCheckBox *allSlots_;
    QCheckBox *paint_ = nullptr;
    QPushButton *brushColor_ = nullptr;
    QSpinBox *brushSize_ = nullptr, *brushOpacity_ = nullptr;
    QTimer *paintPreviewTimer_ = nullptr;
    QSplitter *split_;
    std::array<QToolButton *,5> channels_{};
    std::array<QImage,5> images_;
    int channel_ = 0;
    QColor tint_;
    QColor paintColor_{"#a56bff"};
    QString assetId_;
    struct PaintTarget {
        QString id;
        QSharedPointer<const worldforge::Mesh> revision;
        int slot;
    };
    std::optional<PaintTarget> paintTarget_;
};
