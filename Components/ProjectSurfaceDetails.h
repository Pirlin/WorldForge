#pragma once

#include <QString>

class ProjectStore;
class QWidget;

namespace projectSurfaces {
QString importTexture(ProjectStore &store,QWidget *parent);
void showTexture(ProjectStore &store,const QString &id,QWidget *parent);
QString editMaterial(ProjectStore &store,const QString &id,const QString &preferredMesh,QWidget *parent);
}
