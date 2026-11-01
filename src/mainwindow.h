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
struct Measurement {
    double target;
    double actual;
    double meanMs;
    int sent;
    int errors;
    bool validation;
};

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow();
private:
    void startExperiment();
    void startValidation();
    void startStage(double rps, bool validation);
    void sendDueRequests();
    void handleReply(QNetworkReply *reply);
    void finishStage();
    void stopExperiment();
    void calculate();
    void forecastResponse(double maxMeasured, double firstExceeded);
    void updateChart();
    void setRunning(bool running);
    void settingsChanged();
    void clearMeasurements();

    QVector<QNetworkAccessManager *> networks;
    Ui::MainWindow *ui;
    QTimer *timer;
    QElapsedTimer clock;
    QSet<QNetworkReply *> active;
    QVector<Measurement> measurements;
    QChart *chart;
    bool running = false;
    bool draining = false;
    bool validating = false;
    int stage = 0;
    int sent = 0;
    int errors = 0;
    int successes = 0;
    int consecutiveErrors = 0;
    double totalMs = 0;
    double target = 0;
    double nextSendMs = 0;
    double intercept = 0;
    double slope = 0;
    double forecast = -1;
    double forecastMs = 0;
    bool thresholdForecast = false;
    bool modelReady = false;
    QString lastError;
};
#endif
