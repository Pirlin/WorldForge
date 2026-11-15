#include "App/MainWindow.h"
#include "App/EditorLog.h"

#include <QApplication>
#include <QFile>
#include <QStyleFactory>

#ifdef Q_OS_WIN
extern "C" {
__declspec(dllexport) unsigned long NvOptimusEnablement = 1;
}
#endif

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    app.setOrganizationName("WorldForge");
    app.setApplicationName("WorldForge Editor");
    app.setApplicationVersion("0.1.0");
    app.setStyle(QStyleFactory::create("Fusion"));
    installEditorLog();

    QFile theme(":/DesignSystem/WorldForgeTheme.qss");
    if (theme.open(QIODevice::ReadOnly))
        app.setStyleSheet(QString::fromUtf8(theme.readAll()));

    MainWindow window(nullptr, true);
    window.showMaximized();
    return app.exec();
}
