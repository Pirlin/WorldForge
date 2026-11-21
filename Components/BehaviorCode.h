#pragma once

#include <QString>
#include <QByteArray>
#include <optional>

class ProjectStore;
class QWidget;
struct ProjectBehavior;

bool prepareBehaviorCode(const QString &folder,QString &error);
QString behaviorVisualStudio();
std::optional<ProjectBehavior> readBuiltBehavior(const QByteArray &output,QString &error);
void showBehaviorCode(ProjectStore &store,QWidget *parent);
