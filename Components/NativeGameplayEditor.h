#pragma once

#include <QString>

class ProjectStore;
class QWidget;

bool prepareNativeGameplay(const QString &directory,QString &error);
bool activateNativeGameplay(ProjectStore &store,const QString &binary,QString &error);
bool exportNativeGame(const ProjectStore &store,const QString &directory,QString &error);
void showNativeGameplay(ProjectStore &store,QWidget *parent);
void editNativeComponents(ProjectStore &store,QWidget *parent);
