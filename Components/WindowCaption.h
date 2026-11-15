#pragma once

#include <QWidget>

class QToolButton;

class WindowCaption final : public QWidget {
    Q_OBJECT

public:
    WindowCaption(QWidget &window, int height, QWidget *parent);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void syncMaximize();

    QWidget &window_;
    QToolButton *maximize_;
};
