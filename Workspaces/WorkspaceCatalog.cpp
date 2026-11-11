#include "Workspaces/WorkspaceCatalog.h"

#include <QPixmapCache>

const QList<Workspace> &workspaces()
{
    static const QList<Workspace> entries{
        {"Home", "home", {}, {}},
        {"World Editor", "world", "418c8208a492dcb2b14aaa90", {352, 199, 967, 454}},
        {"Modeling", "box", "389081f9930a2924e08d6fe0", {310, 278, 1009, 330}},
        {"Sculpting", "sculpt", "9a5c8211a4f785ae1dbbb4d3", {329, 274, 983, 368}},
        {"Texture", "layers", "1b8c8211a2910938bbbedb44", {298, 267, 1024, 323}},
        {"Material", "box", "2aac82099c4b6a6130892725", {324, 206, 537, 386}},
        {"Character", "graph", "3e18820d85b6a524ad596f27", {345, 175, 616, 462}},
        {"Rigging", "graph", "7e9c821183f934c945a7f4da", {355, 162, 635, 435}},
        {"Animation", "run", "4db482119a8a3c703124c125", {325, 161, 785, 297}},
        {"VFX", "vfx", "569c82128e96b4d620ab1fa7", {325, 195, 650, 317}},
        {"Audio", "audio", "694c820ead12cda391661601", {325, 164, 595, 283}},
        {"Cinematic", "film", "0a4481f5b152fe135f0875fb", {341, 155, 730, 261}},
        {"Programming", "code", {}, {}},
        {"Visual Scripting", "graph", "64b4820b8061f40d48c49509", {}},
        {"AI", "gamepad", "cae881f583b6c2525193d90e", {326, 200, 574, 258}},
        {"Physics", "box", {}, {}},
        {"Multiplayer", "flask", "437082119cebcfae73fcd61e", {325, 192, 712, 270}},
        {"UI", "ui", "ffdc81faa622de2e167904bf", {325, 175, 977, 300}},
        {"Testing", "check", "ffdc81faa622de2e167904bf", {296, 158, 1056, 334}},
        {"Profiler", "pulse", {}, {}},
        {"Build", "box", {}, {}},
        {"Asset Hub", "layers", {}, {}},
        {"Team", "team", {}, {}},
        {"Settings", "settings", {}, {}},
    };
    return entries;
}

const Workspace &worldWorkspace()
{
    return workspaces()[1];
}

QPixmap workspaceArt(const Workspace &workspace)
{
    return referenceArt(workspace.reference, workspace.preview);
}

QPixmap referenceArt(const QString &reference, const QRect &area)
{
    if (reference.isEmpty() || !area.isValid())
        return {};
    const auto key = QString("worldforge-art-%1-%2-%3-%4-%5")
        .arg(reference).arg(area.x()).arg(area.y()).arg(area.width()).arg(area.height());
    QPixmap cropped;
    if (QPixmapCache::find(key, &cropped))
        return cropped;
    const QPixmap original(":/Docs/file_00000000" + reference + ".png");
    if (original.isNull() || !original.rect().contains(area))
        return {};
    cropped = original.copy(area);
    if (!cropped.isNull())
        QPixmapCache::insert(key, cropped);
    return cropped;
}
