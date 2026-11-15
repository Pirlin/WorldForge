#pragma once

#include <QList>
#include <QString>
#include <QtLogging>

struct EditorLogEntry {
    QtMsgType level;
    QString text;
};

void installEditorLog();
QList<EditorLogEntry> takeEditorLog();
