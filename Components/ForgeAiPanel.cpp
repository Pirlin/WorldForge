#include "Components/ForgeAiPanel.h"

#include "State/GlbImport.h"
#include "State/StudioPublishReview.h"
#include "Workspaces/EmbeddedStudio.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

namespace {

const QList<std::pair<QString, QString>> &stripeColors()
{
    static const QList<std::pair<QString, QString>> colors{
        {QObject::tr("Yellow"), "#f2b705"}, {QObject::tr("Orange"), "#f26b0f"}, {QObject::tr("Red"), "#c8261d"},
        {QObject::tr("White"), "#ece9e1"}, {QObject::tr("Blue"), "#1f5fbf"}, {QObject::tr("Green"), "#2f8f3a"},
        {QObject::tr("Black"), "#1b1b1d"}};
    return colors;
}

}

ForgeAiPanel::ForgeAiPanel(ProjectStore &store, QWidget *parent)
    : QDialog(parent), store_(store)
{
    setObjectName("forgeAiPanel");
    setWindowTitle(tr("Forge AI"));
    setModal(false);
    resize(560, 720);
    auto *layout = new QVBoxLayout(this);
    auto *intro = new QLabel(tr("Describe a barrier change. Forge AI plans it, runs it in Studio on a working copy, "
                                "and shows the result before anything in your level changes."));
    intro->setWordWrap(true);
    layout->addWidget(intro);

    prompt_ = new QPlainTextEdit;
    prompt_->setObjectName("forgePrompt");
    prompt_->setPlaceholderText(tr("Make a worn barrier with faded yellow stripes and place it at the north entrance"));
    prompt_->setFixedHeight(64);
    layout->addWidget(prompt_);
    planButton_ = new QPushButton(tr("Plan"));
    planButton_->setObjectName("forgePlanButton");
    planButton_->setToolTip(tr("Read the request and choose the barrier, entrance, and settings"));
    auto *planRow = new QHBoxLayout;
    planRow->addStretch();
    planRow->addWidget(planButton_);
    layout->addLayout(planRow);

    auto *form = new QFormLayout;
    barrier_ = new QComboBox;
    barrier_->setObjectName("forgeBarrier");
    entrance_ = new QComboBox;
    entrance_->setObjectName("forgeEntrance");
    bevel_ = new QDoubleSpinBox;
    bevel_->setObjectName("forgeBevel");
    bevel_->setRange(0, 0.25);
    bevel_->setSingleStep(0.01);
    bevel_->setDecimals(3);
    bevel_->setSuffix(tr(" m"));
    bevel_->setToolTip(tr("Width of the rounded sharp edges. Use 0 to keep them sharp."));
    segments_ = new QSpinBox;
    segments_->setRange(1, 6);
    stripes_ = new QSpinBox;
    stripes_->setObjectName("forgeStripes");
    stripes_->setRange(1, 12);
    wear_ = new QSpinBox;
    wear_->setObjectName("forgeWear");
    wear_->setRange(0, 100);
    wear_->setSuffix(tr("%"));
    fade_ = new QSpinBox;
    fade_->setRange(0, 100);
    fade_->setSuffix(tr("%"));
    color_ = new QComboBox;
    for (const auto &[name, hex] : stripeColors())
        color_->addItem(name, hex);
    form->addRow(tr("Barrier"), barrier_);
    form->addRow(tr("Entrance"), entrance_);
    form->addRow(tr("Bevel width"), bevel_);
    form->addRow(tr("Bevel segments"), segments_);
    form->addRow(tr("Stripes"), stripes_);
    form->addRow(tr("Stripe color"), color_);
    form->addRow(tr("Wear"), wear_);
    form->addRow(tr("Fade"), fade_);
    layout->addLayout(form);

    commands_ = new QLabel;
    commands_->setObjectName("forgeCommands");
    commands_->setWordWrap(true);
    commands_->setTextFormat(Qt::PlainText);
    commands_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(commands_);

    auto *actions = new QHBoxLayout;
    preview_ = new QPushButton(tr("Generate Preview"));
    preview_->setObjectName("forgePreview");
    preview_->setToolTip(tr("Run the plan in Studio on a working copy. Your level does not change."));
    cancel_ = new QPushButton(tr("Cancel Job"));
    cancel_->setObjectName("forgeCancel");
    apply_ = new QPushButton(tr("Apply as One Change"));
    apply_->setObjectName("forgeApply");
    apply_->setToolTip(tr("Publish the barrier and add its placement in one undoable edit"));
    check_ = new QPushButton(tr("Check in Play"));
    check_->setObjectName("forgeCheck");
    check_->setToolTip(tr("Confirm placement, appearance, and collision in a Play simulation"));
    for (auto *button : {preview_, cancel_, apply_, check_})
        actions->addWidget(button);
    layout->addLayout(actions);

    auto *result = new QHBoxLayout;
    thumbnail_ = new QLabel;
    thumbnail_->setObjectName("forgeThumbnail");
    thumbnail_->setFixedSize(128, 128);
    thumbnail_->setAlignment(Qt::AlignCenter);
    report_ = new QPlainTextEdit;
    report_->setObjectName("forgeReport");
    report_->setReadOnly(true);
    result->addWidget(thumbnail_, 0, Qt::AlignTop);
    result->addWidget(report_, 1);
    layout->addLayout(result, 1);
    auto *close = new QDialogButtonBox(QDialogButtonBox::Close);
    layout->addWidget(close);

    connect(close, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(planButton_, &QPushButton::clicked, this, &ForgeAiPanel::makePlan);
    connect(preview_, &QPushButton::clicked, this, &ForgeAiPanel::runPreview);
    connect(cancel_, &QPushButton::clicked, &job_, &ForgeAiJob::cancel);
    connect(apply_, &QPushButton::clicked, this, &ForgeAiPanel::applyStage);
    connect(check_, &QPushButton::clicked, this, &ForgeAiPanel::checkStage);
    connect(&job_, &ForgeAiJob::finished, this, &ForgeAiPanel::showPreview);
    for (auto *box : {barrier_, entrance_, color_})
        connect(box, &QComboBox::currentIndexChanged, this, &ForgeAiPanel::readControls);
    for (auto *box : {segments_, stripes_, wear_, fade_})
        connect(box, &QSpinBox::valueChanged, this, &ForgeAiPanel::readControls);
    connect(bevel_, &QDoubleSpinBox::valueChanged, this, &ForgeAiPanel::readControls);
    connect(&store_, &ProjectStore::sceneChanged, this, &ForgeAiPanel::updateButtons);
    setReport(tr("Write a request and select Plan."));
    updateButtons();
}

void ForgeAiPanel::makePlan()
{
    if (job_.isRunning())
        return;
    const auto *project = store_.project();
    if (!project) {
        setReport(tr("Open a project first."));
        return;
    }
    auto text = prompt_->toPlainText();
    if (text.trimmed().isEmpty())
        text = prompt_->placeholderText();
    stage_.reset();
    street_.reset();
    streetReviewed_ = false;
    applied_ = false;
    thumbnail_->clear();
    if (auto streetPlan = street::plan(store_, text); streetPlan.error.isEmpty()
        || streetPlan.moveEntrance || streetPlan.rerouteTraffic) {
        plan_ = {};
        street_ = std::move(streetPlan);
        setReport(street_->error.isEmpty()
            ? tr("This request edits the level, mission, and traffic together. Select Generate Preview to review "
                 "every change.") : street_->error);
        refreshCommands();
        return;
    }
    plan_ = forge::plan(*project, text);
    updating_ = true;
    barrier_->clear();
    entrance_->clear();
    for (const auto &candidate : plan_.barriers)
        barrier_->addItem(candidate.name, candidate.id);
    for (const auto &candidate : plan_.entrances)
        entrance_->addItem(candidate.name, candidate.id);
    for (auto *box : {barrier_, entrance_})
        box->insertItem(0, tr("Choose…"), QString());
    barrier_->setCurrentIndex(std::max(0, barrier_->findData(plan_.barrierId)));
    entrance_->setCurrentIndex(std::max(0, entrance_->findData(plan_.entranceId)));
    const auto &style = plan_.style;
    bevel_->setValue(style.bevelWidth);
    segments_->setValue(style.bevelSegments);
    stripes_->setValue(std::clamp(style.stripes, 1, 12));
    wear_->setValue(qRound(style.wear * 100));
    fade_->setValue(qRound(style.fade * 100));
    color_->setCurrentIndex(std::max(0, color_->findData(style.color.name())));
    updating_ = false;
    if (!plan_.error.isEmpty())
        setReport(plan_.error);
    else if (plan_.needsChoice())
        setReport(tr("More than one %1 matches the request. Choose one above, or name it in the request.")
            .arg(plan_.barrierId.isEmpty() ? tr("barrier") : tr("entrance")));
    else
        setReport(tr("Review the plan, adjust the settings if needed, then select Generate Preview."));
    refreshCommands();
}

void ForgeAiPanel::readControls()
{
    if (updating_)
        return;
    if (!plan_.barriers.isEmpty())
        plan_.barrierId = barrier_->currentData().toString();
    if (!plan_.entrances.isEmpty())
        plan_.entranceId = entrance_->currentData().toString();
    auto &style = plan_.style;
    style.bevelWidth = float(bevel_->value());
    style.bevelSegments = segments_->value();
    style.stripes = stripes_->value();
    style.wear = float(wear_->value()) / 100;
    style.fade = float(fade_->value()) / 100;
    style.color = QColor(color_->currentData().toString());
    if (stage_ && !applied_) {
        stage_.reset();
        setReport(tr("Settings changed. Generate the preview again."));
    }
    refreshCommands();
}

void ForgeAiPanel::refreshCommands()
{
    QString error;
    const auto *project = store_.project();
    const auto list = street_ ? street_->commands
        : project ? forge::commands(*project, plan_, error) : QList<forge::Command>{};
    if (street_)
        error = street_->error;
    QStringList lines;
    for (qsizetype i = 0; i < list.size(); ++i)
        lines << (list[i].target.isEmpty() ? tr("%1. %2").arg(i + 1).arg(list[i].describe())
            : tr("%1. %2  [target %3, revision %4]").arg(i + 1).arg(list[i].describe(),
                  list[i].target.left(8), list[i].expectedRevision));
    commands_->setText(lines.isEmpty() ? error : lines.join('\n'));
    for (QWidget *control : std::initializer_list<QWidget *>{barrier_, entrance_, bevel_, segments_, stripes_,
                                                            wear_, fade_, color_})
        control->setVisible(!street_);
    updateButtons();
}

void ForgeAiPanel::runPreview()
{
    if (street_) {
        // Re-plan against the current level so the review shows fresh revisions.
        street_ = street::plan(store_, street_->prompt);
        streetReviewed_ = street_->error.isEmpty() && !street_->objects.isEmpty();
        QStringList lines{street_->error.isEmpty() ? tr("Forge AI will make these changes as one undoable edit:")
                                                   : street_->error};
        for (const auto &change : street_->changes)
            lines << tr("• %1").arg(change);
        if (streetReviewed_)
            lines << tr("Apply the change, edit the request and plan again, or close to cancel.");
        else if (street_->error.isEmpty())
            lines << tr("Nothing needs to change.");
        setReport(lines.join('\n'));
        refreshCommands();
        return;
    }
    const auto *project = store_.project();
    QString error;
    const auto list = project ? forge::commands(*project, plan_, error) : QList<forge::Command>{};
    const auto *asset = store_.meshAsset(plan_.barrierId);
    if (list.isEmpty() || !asset) {
        setReport(error.isEmpty() ? tr("Plan the request first.") : error);
        return;
    }
    const auto glb = exportGlb(*asset->mesh, asset->name, error, true);
    const auto source = store_.studioSourcePath(*asset);
    if (glb.isEmpty() || !job_.start(EmbeddedStudio::installedExecutable(), glb,
                                     QFile::exists(source) ? source : QString(), forge::jobDescription(plan_), error)) {
        setReport(error);
        return;
    }
    stage_.reset();
    applied_ = false;
    thumbnail_->clear();
    setReport(tr("Running the plan in Studio on a working copy of %1…").arg(asset->name));
    updateButtons();
}

void ForgeAiPanel::showPreview(const ForgeAiJob::Result &result)
{
    QStringList lines;
    if (!result.steps.isEmpty())
        lines << tr("Studio steps:") << result.steps;
    if (!result.ok) {
        lines.prepend(result.message);
        setReport(lines.join('\n'));
        updateButtons();
        return;
    }
    QString error;
    const auto *project = store_.project();
    const auto list = project ? forge::commands(*project, plan_, error) : QList<forge::Command>{};
    auto staged = project ? forge::stage(*project, plan_, list, result.glb, result.source, result.steps, error)
                          : std::nullopt;
    if (!staged) {
        lines.prepend(error);
        setReport(lines.join('\n'));
        updateButtons();
        return;
    }
    stage_ = std::move(staged);
    const auto review = reviewStudioPublish(*project, store_.meshAsset(stage_->assetId), {}, stage_->mesh);
    const auto &p = stage_->placement.position;
    lines.prepend(tr("New placement: %1 at (%2, %3, %4), rotated %5°.").arg(stage_->placement.name)
        .arg(p.x(), 0, 'f', 2).arg(p.y(), 0, 'f', 2).arg(p.z(), 0, 'f', 2)
        .arg(stage_->placement.rotation.z(), 0, 'f', 0));
    lines.prepend(review.blocker.isEmpty() ? review.summary() : review.blocker);
    if (!review.placements.isEmpty())
        lines << tr("Existing placements that also change:") << review.placements;
    lines << tr("Apply the change, adjust the settings and preview again, or close to cancel. After applying, "
                "Edit Selected Mesh in Studio opens the result for manual edits.");
    if (!review.blocker.isEmpty())
        stage_.reset();
    for (const auto &slot : stage_ ? stage_->mesh.materials : std::vector<worldforge::MeshMaterial>{})
        if (!slot.baseColorTexture.empty()) {
            const auto image = QImage::fromData(slot.baseColorTexture.data(), int(slot.baseColorTexture.size()));
            thumbnail_->setPixmap(QPixmap::fromImage(image.scaled(thumbnail_->size(), Qt::KeepAspectRatio,
                                                                  Qt::SmoothTransformation)));
            break;
        }
    setReport(lines.join('\n'));
    updateButtons();
}

void ForgeAiPanel::applyStage()
{
    if (street_ && streetReviewed_ && !applied_) {
        QString error;
        if (!street::apply(store_, *street_, error)) {
            streetReviewed_ = false;
            setReport(error);
            updateButtons();
            return;
        }
        applied_ = true;
        setReport(tr("Applied as one change. Undo reverts the entrance, mission marker, and traffic together.\n"
                     "Select Check in Play to drive the street and complete the mission."));
        updateButtons();
        return;
    }
    if (!stage_ || applied_)
        return;
    QString error;
    if (!forge::apply(store_, *stage_, error)) {
        stage_.reset();
        setReport(error);
        updateButtons();
        return;
    }
    applied_ = true;
    emit applied(stage_->placement.id);
    setReport(tr("Applied as one change. Undo reverts both the barrier and its placement.\n"
                 "Select Check in Play to confirm placement, appearance, and collision."));
    updateButtons();
}

void ForgeAiPanel::checkStage()
{
    if (street_ && applied_) {
        setReport(tr("Running the street in Play…"));
        const auto report = street::verify(store_);
        setReport((report.ok() ? tr("Play check passed.") : tr("Play check found problems.")) + '\n' + report.summary());
        return;
    }
    if (!stage_ || !applied_)
        return;
    const auto report = forge::checkInPlay(store_, *stage_);
    QStringList lines{report.ok() ? tr("Play check passed.") : tr("Play check found problems.")};
    for (const auto &line : report.passed)
        lines << tr("✓ %1").arg(line);
    for (const auto &line : report.failed)
        lines << tr("✗ %1").arg(line);
    setReport(lines.join('\n'));
}

void ForgeAiPanel::setReport(const QString &text)
{
    report_->setPlainText(text);
}

void ForgeAiPanel::updateButtons()
{
    const bool running = job_.isRunning();
    QString error;
    const auto *project = store_.project();
    const bool ready = project && (street_ ? street_->error.isEmpty() : !forge::commands(*project, plan_, error).isEmpty());
    planButton_->setEnabled(!running);
    preview_->setEnabled(!running && ready && !applied_);
    cancel_->setEnabled(running);
    apply_->setEnabled(!running && !applied_ && (street_ ? streetReviewed_ : stage_.has_value()));
    const bool placed = applied_ && (street_ || (stage_ && store_.object(stage_->placement.id)));
    check_->setEnabled(!running && placed);
    for (QWidget *control : std::initializer_list<QWidget *>{prompt_, barrier_, entrance_, bevel_, segments_,
                                                            stripes_, wear_, fade_, color_})
        control->setEnabled(!running);
}
