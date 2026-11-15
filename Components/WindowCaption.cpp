#include "Components/WindowCaption.h"
#include "DesignSystem/Icons.h"

#include <QEvent>
#include <QHBoxLayout>
#include <QToolButton>

WindowCaption::WindowCaption(QWidget &window, int height, QWidget *parent)
    : QWidget(parent), window_(window)
{
    auto *row = new QHBoxLayout(this);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(0);
    const auto addButton = [this, row, height](const QString &icon, const QString &name) {
        auto *button = new QToolButton(this);
        button->setObjectName(icon == "close" ? "captionClose" : "captionButton");
        button->setIcon(editorIcon(icon));
        button->setIconSize({14, 14});
        button->setFixedSize(44, height);
        button->setAccessibleName(name);
        button->setToolTip(name);
        row->addWidget(button);
        return button;
    };
    connect(addButton("minimize", tr("Minimize")), &QToolButton::clicked, &window_, &QWidget::showMinimized);
    maximize_ = addButton("maximize", tr("Maximize"));
    connect(maximize_, &QToolButton::clicked, this, [this] {
        if (window_.isMaximized())
            window_.showNormal();
        else
            window_.showMaximized();
    });
    connect(addButton("close", tr("Close")), &QToolButton::clicked, &window_, &QWidget::close);
    window_.installEventFilter(this);
    syncMaximize();
}

bool WindowCaption::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == &window_ && event->type() == QEvent::WindowStateChange)
        syncMaximize();
    return QWidget::eventFilter(watched, event);
}

void WindowCaption::syncMaximize()
{
    const bool maximized = window_.isMaximized();
    const auto label = maximized ? tr("Restore down") : tr("Maximize");
    maximize_->setIcon(editorIcon(maximized ? "restore" : "maximize"));
    maximize_->setAccessibleName(label);
    maximize_->setToolTip(label);
}
