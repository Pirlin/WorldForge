#pragma once

#include <QString>
#include <functional>

class ProjectStore;
class QWidget;

QWidget *createAssetHub(ProjectStore &store,std::function<void(const QString &)> open,QWidget *parent);
