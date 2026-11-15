#pragma once

#include "State/ForgeAi.h"
#include "State/PlayableStreet.h"
#include "Workspaces/ForgeAiJob.h"

#include <QDialog>

#include <optional>

class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;

// Plan, preview, apply, and check one Forge AI barrier request.
class ForgeAiPanel final : public QDialog {
    Q_OBJECT

public:
    explicit ForgeAiPanel(ProjectStore &store, QWidget *parent = nullptr);
    const std::optional<forge::Stage> &stage() const { return stage_; }
    const std::optional<street::Plan> &streetPlan() const { return street_; }
    bool isJobRunning() const { return job_.isRunning(); }

signals:
    void applied(const QString &objectId);

private:
    void makePlan();
    void readControls();
    void refreshCommands();
    void runPreview();
    void showPreview(const ForgeAiJob::Result &result);
    void applyStage();
    void checkStage();
    void setReport(const QString &text);
    void updateButtons();

    ProjectStore &store_;
    ForgeAiJob job_;
    forge::Plan plan_;
    std::optional<forge::Stage> stage_;
    // Set when the request edits the street's level, gameplay, and traffic objects instead of the barrier.
    std::optional<street::Plan> street_;
    bool streetReviewed_ = false;
    bool applied_ = false;
    bool updating_ = false;
    QPlainTextEdit *prompt_;
    QComboBox *barrier_;
    QComboBox *entrance_;
    QDoubleSpinBox *bevel_;
    QSpinBox *segments_;
    QSpinBox *stripes_;
    QSpinBox *wear_;
    QSpinBox *fade_;
    QComboBox *color_;
    QLabel *commands_;
    QLabel *thumbnail_;
    QPlainTextEdit *report_;
    QPushButton *planButton_;
    QPushButton *preview_;
    QPushButton *cancel_;
    QPushButton *apply_;
    QPushButton *check_;
};
