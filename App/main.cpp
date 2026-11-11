#include "App/MainWindow.h"

#include <QApplication>
#include <QFile>
#include <QStyleFactory>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    app.setOrganizationName("WorldForge");
    app.setApplicationName("WorldForge Editor");
    app.setApplicationVersion("0.1.0");
    app.setStyle(QStyleFactory::create("Fusion"));

    QFile theme(":/DesignSystem/WorldForgeTheme.qss");
    if (theme.open(QIODevice::ReadOnly))
        app.setStyleSheet(QString::fromUtf8(theme.readAll()));

    MainWindow window;
    window.show();
    return app.exec();
}
