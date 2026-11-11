#pragma once

#include <QJsonArray>
#include <QString>

class QWidget;

QWidget *createStudioView(const QJsonArray &view, QWidget *parent, const QString &reference = {});
QString studioViewName(const QJsonArray &view);
