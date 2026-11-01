#include "mainwindow.h"

#include <QApplication>

int main(int argc, char *argv[])
{
    // Создаём приложение Qt и показываем главное окно.
    QApplication a(argc, argv);
    MainWindow w;
    w.show();
    // Запускаем обработку событий: нажатий, таймера и ответов сервера.
    return QApplication::exec();
}
