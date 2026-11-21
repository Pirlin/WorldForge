#pragma once

class ProjectStore;
class QString;
class QWidget;

bool hasGameplayAuthoring(const QString &workspace);
QWidget *createGameplayAuthoring(const QString &workspace,ProjectStore &store,QWidget *parent);
