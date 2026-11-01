#include "mainwindow.h"
#include "ui_mainwindow.h"

#include <QChartView>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineSeries>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QScatterSeries>
#include <QTimer>
#include <QValueAxis>
#include <cmath>

// Адрес тестового сервера; не показывается в интерфейсе.
static const QString serverAddress = qEnvironmentVariable("SERVER_BENCHMARK_URL", "http://alko.space:8888");
static constexpr int stages = 5;        // Число ступеней за одно нажатие.
static constexpr int maxConcurrent = 40; // Максимум одновременно ожидаемых ответов.

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent),
    ui(new Ui::MainWindow), timer(new QTimer(this))
{
    // Создаём элементы окна из файла mainwindow.ui.
    ui->setupUi(this);
    ui->stopButton->setEnabled(false); ui->validateButton->setEnabled(false);
    ui->measurementsTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    // Создаём график и помещаем его в область, подготовленную в Designer.
    chart = new QChart;
    auto *view = new QChartView(chart); view->setRenderHint(QPainter::Antialiasing);
    ui->chartLayout->addWidget(view);
    // Каждые 10 мс проверяем, пора ли отправлять очередной запрос.
    timer->setInterval(10);
    connect(timer, &QTimer::timeout, this, &MainWindow::sendDueRequests);
    // У одного менеджера Qt ограничивает число соединений с одним сервером.
    for (int i = 0; i < 8; ++i) {
        auto *manager = new QNetworkAccessManager(this);
        connect(manager, &QNetworkAccessManager::finished, this, &MainWindow::handleReply);
        networks.append(manager);
    }
    // Связываем нажатия кнопок с функциями окна.
    connect(ui->startButton, &QPushButton::clicked, this, &MainWindow::startExperiment);
    connect(ui->stopButton, &QPushButton::clicked, this, &MainWindow::stopExperiment);
    connect(ui->validateButton, &QPushButton::clicked, this, &MainWindow::startValidation);
    connect(ui->clearButton, &QPushButton::clicked, this, &MainWindow::clearMeasurements);
    // При изменении настроек обновляем график или пересчитываем прогноз.
    connect(ui->iterationsSpinBox, &QSpinBox::valueChanged, this, &MainWindow::settingsChanged);
    connect(ui->stepSpinBox, &QSpinBox::valueChanged, this, &MainWindow::settingsChanged);
    connect(ui->durationSpinBox, &QSpinBox::valueChanged, this, &MainWindow::settingsChanged);
    connect(ui->thresholdSpinBox, &QSpinBox::valueChanged, this, &MainWindow::settingsChanged);
    updateChart();
}

MainWindow::~MainWindow()
{
    delete ui;
}

void MainWindow::settingsChanged()
{
    // Настройки во время опыта заблокированы. Новый порог можно задать после опыта.
    if (running) return;
    if (sender() == ui->thresholdSpinBox && measurements.size() >= 3) calculate();
    else updateChart();
}

void MainWindow::setRunning(bool value)
{
    // Во время опыта нельзя менять параметры нагрузки и очищать таблицу.
    running = value;
    ui->startButton->setEnabled(!value); ui->stopButton->setEnabled(value);
    ui->iterationsSpinBox->setEnabled(!value && measurements.isEmpty());
    ui->stepSpinBox->setEnabled(!value && measurements.isEmpty());
    ui->durationSpinBox->setEnabled(!value && measurements.isEmpty()); ui->thresholdSpinBox->setEnabled(!value);
    ui->clearButton->setEnabled(!value);
    ui->validateButton->setEnabled(!value && forecast > 0);
}

void MainWindow::startExperiment()
{
    // При продолжении опыта убираем проверочную точку: она не обучающий замер.
    if (!measurements.isEmpty() && measurements.last().validation) {
        measurements.removeLast();
        ui->measurementsTable->removeRow(ui->measurementsTable->rowCount() - 1);
    }
    if (ui->stepSpinBox->value() * (stage + 1) > 100) {
        ui->statusLabel->setText(QString::fromUtf8("Достигнут предел 100 запросов/с. Очистите данные для нового опыта."));
        return;
    }
    // Начинаем следующую ступень, сохранив предыдущие результаты.
    forecast = -1; modelReady = false;
    ui->resultLabel->setText(QString::fromUtf8("Собираем данные для модели..."));
    setRunning(true); startStage(ui->stepSpinBox->value() * (stage + 1), false); updateChart();
}

void MainWindow::clearMeasurements()
{
    // Полностью очищаем таблицу, точки графика и параметры прогноза.
    measurements.clear();
    ui->measurementsTable->setRowCount(0);
    stage = 0; forecast = -1; modelReady = false;
    setRunning(false);
    ui->validateButton->setEnabled(false);
    ui->statusLabel->setText(QString::fromUtf8("Данные очищены. Можно начать новый опыт."));
    ui->resultLabel->setText(QString::fromUtf8("После трёх успешных ступеней появится модель."));
    updateChart();
}

void MainWindow::startValidation()
{
    if (forecast <= 0) return;
    // Повторная проверка заменяет предыдущую точку, не влияя на модель.
    for (int i = measurements.size() - 1; i >= 0; --i)
        if (measurements[i].validation) { measurements.removeAt(i); ui->measurementsTable->removeRow(i); }
    // Отдельно измеряем время ответа при прогнозируемой нагрузке.
    setRunning(true); startStage(forecast, true); updateChart();
}

void MainWindow::startStage(double rps, bool validation)
{
    // Перед новой ступенью сбрасываем счётчики и запускаем секундомер.
    target = rps; validating = validation; draining = false;
    sent = errors = successes = consecutiveErrors = 0; totalMs = 0; nextSendMs = 0; lastError.clear();
    clock.restart(); timer->start();
    ui->statusLabel->setText(QString::fromUtf8("%1: задано %2 запросов/с")
        .arg(validation ? QString::fromUtf8("Проверка прогноза") : QString::fromUtf8("Ступень %1").arg(stage + 1))
        .arg(rps, 0, 'f', 2));
    sendDueRequests();
}

void MainWindow::sendDueRequests()
{
    if (!running || draining) return;
    const double elapsed = clock.elapsed();
    // Время ступени вышло: перестаём отправлять запросы и ждём ответы.
    if (elapsed >= ui->durationSpinBox->value() * 1000.0) {
        timer->stop(); draining = true;
        ui->statusLabel->setText(QString::fromUtf8("Ожидаем завершения отправленных запросов..."));
        if (active.isEmpty()) finishStage();
        return;
    }
    if (elapsed < nextSendMs) return;
    // Не накапливаем очередь пропущенных отправок: фактическая интенсивность измеряется отдельно.
    // При 5 запросах/с промежуток между запросами равен 1000 / 5 = 200 мс.
    nextSendMs += 1000.0 / target;
    if (nextSendMs <= elapsed) nextSendMs = elapsed + 1000.0 / target;
    if (active.size() >= maxConcurrent) return;
    // Формируем адрес CPU-теста с числом итераций, выбранным в окне.
    QUrl url(serverAddress + QString("/benchmark/cpu?iterations=%1").arg(ui->iterationsSpinBox->value()));
    QNetworkRequest request(url); request.setTransferTimeout(10000);
    // Отправляем POST-запрос асинхронно, выбирая менеджеры по кругу.
    QNetworkReply *reply = networks[sent % networks.size()]->post(request, QByteArray());
    reply->setProperty("started", clock.elapsed());
    active.insert(reply); ++sent;
}

void MainWindow::handleReply(QNetworkReply *reply)
{
    if (!active.remove(reply)) { reply->deleteLater(); return; }
    // Измеряем время между отправкой запроса и получением ответа.
    const double elapsed = clock.elapsed() - reply->property("started").toLongLong();
    QJsonParseError parseError;
    const auto doc = QJsonDocument::fromJson(reply->readAll(), &parseError);
    // Ответ считаем успешным, только если нет сетевой ошибки и сервер вернул status: success.
    if (reply->error() != QNetworkReply::NoError || parseError.error != QJsonParseError::NoError ||
        !doc.isObject() || doc.object().value("status").toString() != "success") {
        ++errors;
        ++consecutiveErrors;
        int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        lastError = code ? QString("HTTP %1: %2").arg(code).arg(reply->errorString()) : reply->errorString();
    } else { ++successes; consecutiveErrors = 0; totalMs += elapsed; }
    reply->deleteLater();
    // Три ошибки подряд останавливают опыт; завершённые ступени остаются в таблице.
    if (running && consecutiveErrors >= 3) {
        stopExperiment();
        ui->statusLabel->setText(QString::fromUtf8("Остановлено после 3 ошибок подряд: %1. Предыдущие замеры сохранены.").arg(lastError));
        return;
    }
    if (running && draining && active.isEmpty()) finishStage();
}

void MainWindow::finishStage()
{
    // Считаем отправленные запросы в секунду и среднее время успешных ответов.
    Measurement m{target, sent / double(ui->durationSpinBox->value()), successes ? totalMs / successes : 0.0, sent, errors, validating};
    measurements.append(m);
    // Добавляем результат ступени в таблицу и на график.
    int row = ui->measurementsTable->rowCount(); ui->measurementsTable->insertRow(row);
    const QStringList values = {m.validation ? QString::fromUtf8("Проверка") : QString::number(stage + 1),
        QString::number(m.target, 'f', 2), QString::number(m.actual, 'f', 2),
        successes ? QString::number(m.meanMs, 'f', 1) : QString::fromUtf8("нет данных"),
        QString::number(m.sent), QString::number(m.errors)};
    for (int i = 0; i < values.size(); ++i) ui->measurementsTable->setItem(row, i, new QTableWidgetItem(values[i]));
    updateChart();
    // Проверочная точка сравнивается с прогнозом, но не участвует в подборе модели.
    if (validating) {
        setRunning(false);
        if (m.errors || m.sent == 0 || m.actual < m.target * 0.9)
            ui->statusLabel->setText(QString::fromUtf8("Проверка ненадёжна: есть ошибки или не достигнута заданная интенсивность."));
        else ui->statusLabel->setText(QString::fromUtf8("Проверка: ожидалось %1 мс, измерено %2 мс при %3 запросов/с; разница %4 мс. Это одна проверочная точка.")
            .arg(forecastMs, 0, 'f', 1).arg(m.meanMs, 0, 'f', 1).arg(m.actual, 0, 'f', 2).arg(std::abs(m.meanMs - forecastMs), 0, 'f', 1));
        return;
    }
    // Останавливаем опыт при доле ошибок больше 5% или при недостаточной частоте отправки.
    if (m.sent == 0 || m.errors * 100.0 / m.sent > 5.0 || m.actual < m.target * 0.9) {
        setRunning(false);
        ui->statusLabel->setText(QString::fromUtf8("Ступень остановлена: задано %1, отправлено %2 запросов/с; ошибок %3 из %4. %5")
            .arg(m.target, 0, 'f', 1).arg(m.actual, 0, 'f', 1).arg(m.errors).arg(m.sent).arg(lastError));
        return;
    }
    // После трёх ступеней пробуем рассчитать модель; за один запуск проводим до пяти.
    ++stage;
    if (stage >= 3) calculate();
    if (stage % stages != 0 && ui->stepSpinBox->value() * (stage + 1) <= 100) startStage(ui->stepSpinBox->value() * (stage + 1), false);
    else { setRunning(false); calculate(); }
}

void MainWindow::stopExperiment()
{
    // Отменяем ожидающие ответы. Сервер может продолжить уже начатые вычисления.
    timer->stop(); setRunning(false); ui->validateButton->setEnabled(forecast > 0);
    const auto replies = active; active.clear();
    for (QNetworkReply *reply : replies) reply->abort();
    ui->statusLabel->setText(QString::fromUtf8("Остановлено. Сервер может ещё завершать уже полученные запросы."));
}

void MainWindow::calculate()
{
    // Основная модель: 1 / T = a + b * x, где T в секундах, x — запросы/с.
    // Метод наименьших квадратов (МНК) подбирает коэффициенты a и b.
    double sx = 0, sy = 0, sxx = 0, sxy = 0, maxMeasured = 0;
    double firstExceeded = -1;
    int n = 0;
    // Используем только пригодные обучающие ступени без ошибок.
    for (const auto &m : measurements) {
        if (m.validation || m.errors || m.actual < m.target * 0.9 || m.meanMs <= 0) continue;
        // Время хранится в миллисекундах: 1 / T(сек) = 1000 / T(мс).
        const double x = m.actual, y = 1000.0 / m.meanMs;
        maxMeasured = qMax(maxMeasured, x);
        if (m.meanMs >= ui->thresholdSpinBox->value() && (firstExceeded < 0 || x < firstExceeded)) firstExceeded = x;
        // Накапливаем суммы, необходимые для формул МНК.
        sx += x; sy += y; sxx += x*x; sxy += x*y; ++n;
    }
    const double denominator = n*sxx - sx*sx;
    if (n < 3 || std::abs(denominator) < 1e-9) {
        ui->statusLabel->setText(QString::fromUtf8("Недостаточно различных измерений для МНК.")); return;
    }
    // Находим наклон b и свободный коэффициент a прямой.
    slope = (n*sxy - sx*sy) / denominator; intercept = (sy - slope*sx) / n;
    modelReady = intercept + slope * maxMeasured > 0 && intercept > 0;
    // Если порог уже превышен, вместо прогноза предела оцениваем следующее время.
    if (firstExceeded >= 0) {
        forecastResponse(maxMeasured, firstExceeded);
        return;
    }
    const double limitSeconds = ui->thresholdSpinBox->value() / 1000.0;
    // Прогноз ограничен удвоенной последней ступенью и 100 запросами/с.
    double upper = qMin(100.0, maxMeasured * 2.0);
    if (slope < 0) upper = qMin(upper, -intercept / slope * 0.999999);
    // Ищем корень: модельное время минус заданный порог должно быть равно нулю.
    auto f = [this, limitSeconds](double x) { return 1.0 / (intercept + slope*x) - limitSeconds; };
    if (slope >= 0 || upper <= maxMeasured || intercept + slope*upper <= 0 || f(maxMeasured) >= 0 || f(upper) < 0) {
        forecastResponse(maxMeasured, firstExceeded);
        return;
    }
    // Метод половинного деления: сужаем интервал, в котором находится корень.
    double left = maxMeasured, right = upper; int count = 0;
    while (right - left > 0.001 && count < 100) {
        double middle = (left + right) / 2;
        if (f(middle) < 0) left = middle; else right = middle;
        ++count;
    }
    forecast = (left + right) / 2;
    forecastMs = ui->thresholdSpinBox->value();
    thresholdForecast = true;
    // RMSE показывает, насколько модель в среднем отклоняется от обучающих замеров.
    double squared = 0;
    for (const auto &m : measurements) {
        if (m.validation || m.errors || m.actual < m.target * 0.9 || m.meanMs <= 0) continue;
        double difference = 1000.0 / (intercept + slope*m.actual) - m.meanMs;
        squared += difference*difference;
    }
    double rmse = std::sqrt(squared / n);
    ui->resultLabel->setText(QString::fromUtf8("МНК: 1/T = %1 + (%2)·λ (T в секундах). RMSE: %3 мс. Половинное деление: %4 итераций. Прогноз порога: %5 запросов/с.")
        .arg(intercept, 0, 'f', 4).arg(slope, 0, 'f', 4).arg(rmse, 0, 'f', 1).arg(count).arg(forecast, 0, 'f', 2));
    ui->statusLabel->setText(QString::fromUtf8("Прогноз рассчитан по обучающим точкам. Нажмите «Проверить прогноз»."));
    ui->validateButton->setEnabled(!running); updateChart();
}

void MainWindow::forecastResponse(double maxMeasured, double firstExceeded)
{
    // Если прогноз нагрузки по порогу невозможен, подбираем прямую T = a + b*x.
    // Она оценивает время ответа на следующей ступени, а не предел сервера.
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    int n = 0;
    for (const auto &m : measurements) {
        if (m.validation || m.errors || m.actual < m.target * 0.9 || m.meanMs <= 0) continue;
        sx += m.actual;
        sy += m.meanMs;
        sxx += m.actual * m.actual;
        sxy += m.actual * m.meanMs;
        ++n;
    }
    const double divisor = n * sxx - sx * sx;
    if (n < 3 || std::abs(divisor) < 1e-9 || maxMeasured >= 100) {
        forecast = -1;
        ui->validateButton->setEnabled(false);
        ui->resultLabel->setText(QString::fromUtf8("Недостаточно данных или достигнут предел измерений: прогноз следующей ступени невозможен."));
        updateChart();
        return;
    }
    const double b = (n * sxy - sx * sy) / divisor;
    const double a = (sy - b * sx) / n;
    // Выбираем следующую нагрузку и вычисляем для неё время по прямой.
    forecast = qMin(100.0, maxMeasured + ui->stepSpinBox->value());
    forecastMs = a + b * forecast;
    bool usedAverage = forecastMs <= 0;
    if (usedAverage) forecastMs = sy / n; // Отрицательное время заменяем средним замеров.
    thresholdForecast = false;
    QString note = firstExceeded >= 0
        ? QString::fromUtf8("Порог уже достигнут при %1 запросов/с. ").arg(firstExceeded, 0, 'f', 2)
        : QString::fromUtf8("Прогноз нагрузки по порогу не найден. ");
    QString method = usedAverage ? QString::fromUtf8("среднее измеренное время") : QString::fromUtf8("линейный МНК");
    ui->resultLabel->setText(note + QString::fromUtf8("Оценка времени при %1 запросов/с: %2 мс (%3). Это не прогноз предельной нагрузки.")
        .arg(forecast, 0, 'f', 2).arg(forecastMs, 0, 'f', 1).arg(method));
    ui->statusLabel->setText(QString::fromUtf8("Оценка на следующей ступени приблизительная; её можно проверить после измерений."));
    ui->validateButton->setEnabled(!running);
    updateChart();
}

void MainWindow::updateChart()
{
    // При каждом обновлении заново строим точки, линии и оси графика.
    chart->removeAllSeries();
    auto *training = new QScatterSeries; training->setName(QString::fromUtf8("Измерения")); training->setMarkerSize(10);
    auto *validation = new QScatterSeries; validation->setName(QString::fromUtf8("Проверка")); validation->setMarkerSize(12);
    double maxX = 1, maxY = 0;
    // Обычные и проверочные измерения показываем разными наборами точек.
    for (const auto &m : measurements) {
        if (m.meanMs <= 0) continue;
        (m.validation ? validation : training)->append(m.actual, m.meanMs);
        maxX = qMax(maxX, m.actual * 1.1); maxY = qMax(maxY, m.meanMs * 1.15);
    }
    // При большом пороге масштабируем график по замерам, чтобы точки были видны.
    const double limitMs = ui->thresholdSpinBox->value();
    if (maxY == 0 || limitMs <= maxY * 2) maxY = qMax(maxY, limitMs * 1.15);
    if (forecast > 0) maxY = qMax(maxY, forecastMs * 1.15);
    chart->setTitle(limitMs > maxY ? QString::fromUtf8("Порог %1 мс выше масштаба графика").arg(limitMs) : QString());
    chart->addSeries(training); chart->addSeries(validation);
    if (forecast > 0) {
        maxX = qMax(maxX, forecast * 1.1);
    }
    // Кривую строим из отдельных точек, рассчитанных по основной модели.
    if (modelReady) {
        auto *model = new QLineSeries; model->setName(QString::fromUtf8("Модель МНК"));
        for (int i = 0; i <= 100; ++i) {
            double x = maxX * i / 100.0, d = intercept + slope*x;
            if (d > 0 && 1000.0 / d <= maxY) model->append(x, 1000.0 / d);
        }
        chart->addSeries(model);
    }
    // Прогноз отмечаем отдельной точкой; её название зависит от вида прогноза.
    if (forecast > 0) {
        auto *prediction = new QScatterSeries; prediction->setName(thresholdForecast ? QString::fromUtf8("Прогноз порога") : QString::fromUtf8("Прогноз времени")); prediction->setMarkerSize(12);
        prediction->append(forecast, forecastMs); chart->addSeries(prediction);
    }
    if (limitMs <= maxY) {
        auto *limit = new QLineSeries; limit->setName(QString::fromUtf8("Порог"));
        limit->append(0, limitMs); limit->append(maxX, limitMs); chart->addSeries(limit);
    }
    // Создаём подписанные оси и привязываем к ним все точки и линии.
    for (auto *axis : chart->axes()) { chart->removeAxis(axis); delete axis; }
    auto *x = new QValueAxis; x->setTitleText(QString::fromUtf8("Отправлено запросов/с")); x->setRange(0, maxX);
    auto *y = new QValueAxis; y->setTitleText(QString::fromUtf8("Среднее время ответа, мс")); y->setRange(0, maxY);
    chart->addAxis(x, Qt::AlignBottom); chart->addAxis(y, Qt::AlignLeft);
    for (auto *series : chart->series()) { series->attachAxis(x); series->attachAxis(y); }
}
