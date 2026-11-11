#pragma once
#include <QList>
#include <QPixmap>
#include <QRect>
#include <QString>

struct Workspace {
    QString name;
    QString icon;
    QString reference;
    QRect preview;
};

const QList<Workspace> &workspaces();
const Workspace &worldWorkspace();
QPixmap workspaceArt(const Workspace &workspace);
QPixmap referenceArt(const QString &reference, const QRect &area);
