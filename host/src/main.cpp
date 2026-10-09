#include <QApplication>
#include <QMessageBox>

#include "ui/mainwindow.h"
int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("仓储环境安全巡检机器人 · 上位机"));

    MainWindow w;
    w.show();
    return app.exec();
}
