#include "DesignSystem/Icons.h"

#include <QHash>
#include <QPainter>
#include <QPixmap>
#include <QSvgRenderer>

QIcon editorIcon(const QString &name, const QColor &color)
{
    static const QHash<QString, QByteArray> paths{
        {"forge", "M2 4h5l3 10 4-9h3l3 10 3-11h3l-5 16h-3l-3-9-4 9H8L2 4Z"},
        {"home", "M3 11l9-8 9 8M5 10v10h5v-6h4v6h5V10"},
        {"folder", "M3 7h7l2 3h9v9H3zM3 7V5h7l2 2h7v3"},
        {"folderFill", "M2 4h8l3 3h9v13H2z"},
        {"world", "M2 20L11 4l9 16zM8 10l3 4 3-4m3 3 2-4 4 11h-3"},
        {"box", "M12 3l9 5v9l-9 5-9-5V8zm0 9 9-4m-9 4L3 8m9 4v10"},
        {"layers", "M12 3l10 5-10 5L2 8zm-9 9 9 5 9-5M3 16l9 5 9-5"},
        {"graph", "M12 7v5m-6 5v-5h12v5M12 3a2 2 0 1 1 0 4 2 2 0 0 1 0-4zM6 17a2 2 0 1 1 0 4 2 2 0 0 1 0-4zm12 0a2 2 0 1 1 0 4 2 2 0 0 1 0-4z"},
        {"person", "M12 3a3 3 0 1 1 0 6 3 3 0 0 1 0-6zm-7 18v-2a6 6 0 0 1 6-6h2a6 6 0 0 1 6 6v2"},
        {"team", "M9 3a3 3 0 1 0 0 6 3 3 0 0 0 0-6zm7 1a3 3 0 0 1 0 6M3 20v-3a5 5 0 0 1 5-5h2a5 5 0 0 1 5 5v3m3-7a4 4 0 0 1 3 4v3"},
        {"sculpt", "m6 13 7-7 5 5-7 7-7 2zm6-6 3 3m0-6 2-2a2 2 0 0 1 3 3l-2 2"},
        {"rig", "M12 2a2 2 0 1 1 0 4 2 2 0 0 1 0-4zm0 4v6m-7-3h14m-7 3-5 8m5-8 5 8M5 7v4m14-4v4"},
        {"run", "M15 2a2 2 0 1 1 0 4 2 2 0 0 1 0-4zm-5 9 3-4 4 5h5m-12-1 5 4v6m-5-10-3 6H2m8-6H6l-2 3"},
        {"vfx", "M12 2v5m0 10v5M2 12h5m10 0h5M5 5l4 4m6 6 4 4m0-14-4 4m-6 6-4 4"},
        {"gamepad", "M7 8h10c2 0 3 1 4 4l1 4c1 3-2 5-4 3l-3-3H9l-3 3c-2 2-5 0-4-3l1-4c1-3 2-4 4-4zm0 2v5m-2.5-2.5h5m6.5 0h1m2-2h1"},
        {"flask", "M9 3h6m-5 0v7l-6 9a1 1 0 0 0 1 2h14a1 1 0 0 0 1-2l-6-9V3m-7 13h10"},
        {"ui", "M3 4h18v16H3zm0 5h18M6 7h1m2 0h1"},
        {"code", "M8 6l-6 6 6 6m8-12 6 6-6 6m-2-16-4 20"},
        {"spark", "M12 2l2.5 7.5L22 12l-7.5 2.5L12 22l-2.5-7.5L2 12l7.5-2.5z"},
        {"audio", "M3 10v4m4-7v10m5-14v18m5-14v10m4-7v4"},
        {"film", "M3 9h18v12H3zm0 0 16-6M8 7l-2-3m8 1-2-3M3 13h18"},
        {"video", "M3 6h12v12H3zm12 4 6-4v12l-6-4"},
        {"play", "M8 4l13 8-13 8z"},
        {"playFill", "M4 2l19 10L4 22z"},
        {"cloud", "M7 18H6a4 4 0 0 1-1-8 7 7 0 0 1 13-1 4 4 0 1 1 1 9h-2M9 18l3 3 5-6"},
        {"branch", "M6 7v11m12-9a6 6 0 0 1-6 6H6M6 3a2 2 0 1 1 0 4 2 2 0 1 1 0-4zm0 15a2 2 0 1 1 0 4 2 2 0 1 1 0-4zm12-13a2 2 0 1 1 0 4 2 2 0 1 1 0-4z"},
        {"bell", "M4 17h16l-2-4V9a6 6 0 0 0-12 0v4zm6 3a2 2 0 0 0 4 0"},
        {"help", "M12 2a10 10 0 1 0 0 20 10 10 0 0 0 0-20zM9 9a3 3 0 1 1 4 2.8c-1 .4-1 1.2-1 2.2m0 3h.01"},
        {"checkCircle", "M12 2a10 10 0 1 0 0 20 10 10 0 0 0 0-20zm-5 10 3 3 6-6"},
        {"memory", "M3 7h18v12H3zm3 3h3v5H6zm5 0h3v5h-3zm5 0h3v5h-3zM6 19v2m4-2v2m4-2v2m4-2v2"},
        {"hardware", "M2 7h20v11H2zm4 5a2 2 0 1 0 4 0 2 2 0 1 0-4 0zm8 0a2 2 0 1 0 4 0 2 2 0 1 0-4 0zM6 5v2m6-2v2m6-2v2"},
        {"pin", "m15 3 6 6-4 1-3 6-2-2-7 7 5-9-2-2 6-3z"},
        {"lock", "M5 11h14v10H5zm3 0V7a4 4 0 1 1 8 0v4m-4 4v2"},
        {"sort", "M4 5h16m-14 5h12m-10 5h8m-6 5h4"},
        {"list", "M7 5h14M7 12h14M7 19h14M3 5h1m-1 7h1m-1 7h1"},
        {"import", "M12 2v13m-5-5 5 5 5-5M4 15v6h16v-6"},
        {"copy", "M8 8h12v13H8zm-4 8V3h12"},
        {"link", "M10 8l3-3a4 4 0 0 1 6 6l-3 3m-2 2-3 3a4 4 0 0 1-6-6l3-3m1 5 6-6"},
        {"globe", "M12 2a10 10 0 1 0 0 20 10 10 0 0 0 0-20zm0 0a5 10 0 1 0 0 20 5 10 0 0 0 0-20zM2 12h20"},
        {"lit", "M12 2a10 10 0 1 0 0 20 10 10 0 0 0 0-20zm0 0v20"},
        {"snap", "M4 4h16v16H4zm0 5h16M4 15h16M9 4v16m6-16v16"},
        {"magnet", "M4 4v9a8 8 0 0 0 16 0V4h-5v9a3 3 0 0 1-6 0V4zM4 8h5m6 0h5"},
        {"expand", "M3 9V3h6m6 0h6v6m0 6v6h-6m-6 0H3v-6"},
        {"stop", "M5 5h14v14H5z"},
        {"hammer", "M4 19l8-8m-2-5 3-3 8 8-3 3zM2 19l3 3 8-8"},
        {"check", "M4 12l5 5L20 6"},
        {"pulse", "M2 12h5l3-8 4 16 3-8h5"},
        {"screen", "M3 4h18v13H3zm5 17h8m-4-4v4"},
        {"settings", "M10 3h4l1 3 3 1 3 1v4l-3 1-1 3-1 3h-4l-1-3-3-1-3-1v-4l3-1 1-3zm2 6a3 3 0 1 1 0 6 3 3 0 0 1 0-6z"},
        {"plus", "M5 12h14m-7-7v14"},
        {"search", "M10 4a6 6 0 1 1 0 12 6 6 0 0 1 0-12zm4 10 6 6"},
        {"save", "M4 3h13l4 4v14H3V3zm3 0v6h9V3M7 21v-8h10v8"},
        {"undo", "M8 5l-5 5 5 5M3 10h12a5 5 0 0 1 5 5v3"},
        {"redo", "M16 5l5 5-5 5m5-5H9a5 5 0 0 0-5 5v3"},
        {"mouse", "M5 3l14 10-7 1-3 6z"},
        {"move", "M12 2v20M2 12h20m-13-7 3-3 3 3m-6 14 3 3 3-3M5 9l-3 3 3 3m14-6 3 3-3 3"},
        {"rotate", "M20 7v5h-5m5 0a8 8 0 1 0-2 6"},
        {"spin", "M12 2v20m-2-19 2-2 2 2M7 9C0 11 3 17 12 17c10 0 13-6 5-8m0-3v3h3"},
        {"measure", "m3 15 12-12 6 6L9 21zm4-4 2 2m1-5 2 2m1-5 2 2"},
        {"annotate", "m4 16-1 5 5-1L21 7l-4-4zm11-11 4 4M12 21h9"},
        {"scale", "M5 13v6h6m8-8V5h-6m5 1L6 18"},
        {"extrude", "M4 9l8-4 8 4-8 4zm0 0v7l8 4 8-4V9m-8 4v7m0-15V2m-3 3 3-3 3 3"},
        {"inset", "M3 3h18v18H3zm5 5h8v8H8zM3 3l5 5m8 8 5 5M3 21l5-5m8-8 5-5"},
        {"bevel", "M7 3h10l4 4v10l-4 4H7l-4-4V7zm0 0v14l-4 4m4-4h14"},
        {"loopcut", "M3 5h18v14H3zm6 0v14m6-14v14m-12-7h18"},
        {"knife", "m3 20 8-8m-2-2 9-7 3 3-7 9zm-6 7 3 3"},
        {"split", "M4 4h16v16H4zm14-2L6 22"},
        {"bridge", "M3 7h5v10H3zm13 0h5v10h-5zM8 9h8m-8 6h8"},
        {"dots", "M4 12h1m6 0h1m6 0h1"},
        {"eye", "M2 12s4-6 10-6 10 6 10 6-4 6-10 6-10-6-10-6zm10-3a3 3 0 1 1 0 6 3 3 0 0 1 0-6z"},
        {"log", "M5 3h14v18H5zm3 5h8m-8 4h8m-8 4h6"},
        {"asset", "M3 3h7v7H3zm11 0h7v7h-7zM3 14h7v7H3zm11 0h7v7h-7z"},
        {"chevron", "M6 9l6 6 6-6"},
        {"close", "M6 6l12 12M18 6 6 18"},
        {"minimize", "M4 12h16"},
        {"maximize", "M4 4h16v16H4z"},
        {"restore", "M4 8h12v12H4zM8 8V4h12v12h-4"},
        {"float", "M4 9h11v11H4zm5 0V4h11v11h-5"},
        {"trash", "M4 6h16m-14 0 1 15h10l1-15M9 6V3h6v3m-5 4v7m4-7v7"},
    };
    const auto path = paths.value(name, paths.value("box"));
    const bool filled = name == "playFill" || name == "forge" || name == "folderFill";
    const auto ink = color.name().toLatin1();
    const auto svg = QByteArray("<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24'>"
        "<path stroke-width='1.65' stroke-linecap='round' stroke-linejoin='round' fill='")
        + (filled ? ink : QByteArray("none")) + "' stroke='" + (filled ? QByteArray("none") : ink)
        + "' d='" + path + "'/></svg>";
    QSvgRenderer renderer(svg);
    QIcon icon;
    for (const int size : {16, 20, 24, 32, 48, 64}) {
        QPixmap pixmap(size, size);
        pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap);
        renderer.render(&painter);
        painter.end();
        icon.addPixmap(pixmap);
    }
    return icon;
}
