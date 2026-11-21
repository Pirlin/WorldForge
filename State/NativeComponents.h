#pragma once

#include "Engine/World/NativeGameplay.h"
#include <QJsonObject>

class ProjectStore;

bool validNativeComponentData(const QJsonValue &value);
worldforge::NativePropertyValue readNativeProperty(const QJsonValue &value);
QJsonObject writeNativeProperty(const worldforge::NativePropertyValue &value);
bool collectNativeInstances(const ProjectStore &store,std::vector<worldforge::NativeInstance> &instances,QString &error);
