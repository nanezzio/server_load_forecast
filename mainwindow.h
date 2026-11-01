#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QElapsedTimer>
#include <QMainWindow>
#include <QSet>
#include <QVector>

class QChart;
class QNetworkAccessManager;
class QNetworkReply;
class QTimer;
namespace Ui { class MainWindow; }

// Результат одной ступени нагрузки.
struct Measurement {
    double target;     // Заданное число запросов в секунду.
    double actual;     // Сколько запросов в секунду отправило приложение.
    double meanMs;     // Среднее время успешного ответа, мс.
    int sent;          // Всего отправлено запросов.
    int errors;        // Из них завершились ошибкой.
    bool validation;   // true — отдельная проверка прогноза.
};

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow();
private:
    // Управление опытом и отправкой запросов.
    void startExperiment();
    void startValidation();
    void startStage(double rps, bool validation);
    void sendDueRequests();
    void handleReply(QNetworkReply *reply);
    void finishStage();
    void stopExperiment();
    // Расчёт модели, запасного прогноза и обновление графика.
    void calculate();
    void forecastResponse(double maxMeasured, double firstExceeded);
    void updateChart();
    void setRunning(bool running);
    void settingsChanged();
    void clearMeasurements();

    QVector<QNetworkAccessManager *> networks; // Менеджеры HTTP-запросов.
    Ui::MainWindow *ui;
    QTimer *timer;
    QElapsedTimer clock;                 // Время с начала текущей ступени.
    QSet<QNetworkReply *> active;        // Запросы, на которые ещё ждём ответ.
    QVector<Measurement> measurements;   // Результаты всех ступеней опыта.
    QChart *chart;
    bool running = false;    // Идёт ли опыт.
    bool draining = false;   // Ждём ли ответы после окончания отправки.
    bool validating = false; // Проверяем ли прогноз на текущей ступени.
    int stage = 0;
    int sent = 0;
    int errors = 0;
    int successes = 0;
    int consecutiveErrors = 0;
    // Данные текущей ступени и коэффициенты модели.
    double totalMs = 0;
    double target = 0;
    double nextSendMs = 0;
    double intercept = 0; // Коэффициент a в формуле 1/T = a + b*x.
    double slope = 0;     // Коэффициент b в той же формуле.
    double forecast = -1; // Нагрузка для прогноза; -1 означает, что его нет.
    double forecastMs = 0; // Прогноз времени ответа, мс.
    bool thresholdForecast = false;
    bool modelReady = false;
    QString lastError;
};
#endif
