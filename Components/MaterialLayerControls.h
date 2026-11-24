#pragma once

#include "Engine/Render/ColorPipeline.h"
#include "Engine/Render/MaterialLayers.h"

#include <QColorDialog>
#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QPushButton>
#include <QSignalBlocker>
#include <QStringList>

#include <functional>
#include <utility>

inline QStringList materialMapNames()
{
    return {QObject::tr("Base Color"),QObject::tr("Normal"),QObject::tr("Metallic / Roughness"),
        QObject::tr("Ambient Occlusion"),QObject::tr("Emissive"),QObject::tr("Clear coat weight (R)"),
        QObject::tr("Clear coat roughness (G)"),QObject::tr("Clear coat normal"),
        QObject::tr("Sheen color (RGB)"),QObject::tr("Sheen roughness (A)"),
        QObject::tr("Transmission (R)"),QObject::tr("Anisotropy (RGB)")};
}

class MaterialLayerControls : public QWidget {
public:
    explicit MaterialLayerControls(QWidget *parent) : QWidget(parent)
    {
        auto *form = new QFormLayout(this);
        form->setContentsMargins(0,0,0,0);
        sheenColor_ = new QPushButton(this);
        sheenColor_->setAccessibleName(tr("Sheen color"));
        form->addRow(tr("Sheen color"),sheenColor_);
        sheenRoughness_ = addFactor(*form,tr("Sheen roughness"),0,1,&worldforge::MaterialLayers::sheenRoughness);
        coatNormalScale_ = addFactor(*form,tr("Clear coat normal scale"),-worldforge::MaterialLayers::maxCoatNormalScale,
            worldforge::MaterialLayers::maxCoatNormalScale,&worldforge::MaterialLayers::coatNormalScale);
        transmission_ = addFactor(*form,tr("Transmission"),0,1,&worldforge::MaterialLayers::transmission);
        ior_ = addFactor(*form,tr("Index of refraction"),1,3,&worldforge::MaterialLayers::ior);
        subsurface_ = addFactor(*form,tr("Subsurface weight"),0,1,&worldforge::MaterialLayers::subsurface);
        thickness_ = addFactor(*form,tr("Surface thickness (m)"),0,2,&worldforge::MaterialLayers::thickness);
        distance_ = addFactor(*form,tr("Scattering distance (m)"),0.0001,2,&worldforge::MaterialLayers::scatteringDistance);
        anisotropy_ = addFactor(*form,tr("Anisotropy"),0,1,&worldforge::MaterialLayers::anisotropy);
        rotation_ = addFactor(*form,tr("Anisotropy rotation (°)"),-360,360,&worldforge::MaterialLayers::anisotropyRotation,angleScale);
        wetness_ = addFactor(*form,tr("Wetness"),0,1,&worldforge::MaterialLayers::wetness);
        puddles_ = addFactor(*form,tr("Puddle coverage"),0,1,&worldforge::MaterialLayers::puddles);
        relief_ = addFactor(*form,tr("Parallax height (UV)"),0,worldforge::MaterialLayers::maxHeightScale,
            &worldforge::MaterialLayers::heightScale);
        puddles_->setToolTip(tr("Pools on upward-facing surfaces. The occlusion map's blue channel masks placement when assigned."));
        relief_->setToolTip(tr("Parallax occlusion uses the occlusion map's green channel as a height map. White marks peaks."));
        relief_->setSingleStep(0.005);
        decal_ = new QCheckBox(this);
        decal_->setAccessibleName(tr("Mesh decal"));
        decal_->setToolTip(tr("Place on an overlay mesh. The base-color texture's alpha masks the decal."));
        form->addRow(tr("Mesh decal"),decal_);
        opacity_ = addFactor(*form,tr("Decal opacity"),0,1,&worldforge::MaterialLayers::decalOpacity);
        connect(decal_,&QCheckBox::toggled,this,[this](bool enabled) {
            values_.decal = enabled;
            opacity_->setEnabled(enabled);
            if (changed_)
                changed_();
        });
        transmission_->setToolTip(tr("Light passing straight through a thin pane. Base color tints the transmitted light."));
        connect(sheenColor_,&QPushButton::clicked,this,[this] {
            const auto picked = QColorDialog::getColor(color_,this,tr("Sheen color"));
            if (!picked.isValid())
                return;
            color_ = picked;
            values_.sheenColor = worldforge::color::srgbToLinear({float(color_.redF()),float(color_.greenF()),float(color_.blueF())});
            sheenColor_->setText(color_.name());
            if (changed_)
                changed_();
        });
        setLayers({});
    }

    void setLayers(const worldforge::MaterialLayers &layers)
    {
        values_ = layers;
        const auto channel = [&](int index) { return worldforge::color::linearToSrgb(layers.sheenColor[index]); };
        color_ = QColor::fromRgbF(channel(0),channel(1),channel(2));
        sheenColor_->setText(color_.name());
        showFactor(*sheenRoughness_,layers.sheenRoughness);
        showFactor(*coatNormalScale_,layers.coatNormalScale);
        showFactor(*transmission_,layers.transmission);
        showFactor(*ior_,layers.ior);
        showFactor(*subsurface_,layers.subsurface);
        showFactor(*thickness_,layers.thickness);
        showFactor(*distance_,layers.scatteringDistance);
        showFactor(*anisotropy_,layers.anisotropy);
        showFactor(*rotation_,double(layers.anisotropyRotation)*angleScale);
        showFactor(*wetness_,layers.wetness);
        showFactor(*puddles_,layers.puddles);
        showFactor(*relief_,layers.heightScale);
        const QSignalBlocker decalBlocker(decal_);
        decal_->setChecked(layers.decal);
        showFactor(*opacity_,layers.decalOpacity);
        opacity_->setEnabled(layers.decal);
    }

    worldforge::MaterialLayers layers() const { return values_; }

    void onChanged(std::function<void()> callback) { changed_ = std::move(callback); }

private:
    static void showFactor(QDoubleSpinBox &field,double value)
    {
        const QSignalBlocker blocker(&field);
        field.setValue(value);
    }

    QDoubleSpinBox *addFactor(QFormLayout &form,const QString &name,double minimum,double maximum,
                             float worldforge::MaterialLayers::*member,double displayScale = 1)
    {
        auto *field = new QDoubleSpinBox(this);
        field->setAccessibleName(name);
        field->setRange(minimum,maximum);
        field->setDecimals(4);
        field->setSingleStep(displayScale == 1 ? 0.05 : 5);
        form.addRow(name,field);
        connect(field,&QDoubleSpinBox::valueChanged,this,[this,member,displayScale](double value) {
            values_.*member = float(value/displayScale);
            if (changed_)
                changed_();
        });
        return field;
    }

    QPushButton *sheenColor_;
    QDoubleSpinBox *sheenRoughness_, *transmission_, *ior_, *coatNormalScale_;
    QDoubleSpinBox *subsurface_, *thickness_, *distance_, *anisotropy_, *rotation_;
    QDoubleSpinBox *wetness_, *puddles_, *relief_, *opacity_;
    QCheckBox *decal_;
    static constexpr double angleScale = 360.0/worldforge::MaterialLayers::fullTurn;
    QColor color_;
    worldforge::MaterialLayers values_;
    std::function<void()> changed_;
};
