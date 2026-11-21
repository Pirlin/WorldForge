#pragma once

#include <QString>

class ProjectStore;
class QWidget;

void editBehaviorAsset(ProjectStore &store,const QString &id,QWidget *parent,bool graph = false);
void editBehaviorComponents(ProjectStore &store,const QString &objectId,QWidget *parent);
QWidget *createBehaviorWorkspace(ProjectStore &store,QWidget *parent);
