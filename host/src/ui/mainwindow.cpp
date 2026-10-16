#include "mainwindow.h"

#include "data/event_log.h"
#include "data/param_store.h"
#include "data/recorder.h"
#include "domain/alarm.h"
#include "domain/keepalive.h"
#include "domain/units.h"
#include "domain/vehicle_state.h"
#include "ui/panels/curve_panel.h"
#include "ui/panels/event_panel.h"
#include "ui/panels/overview_panel.h"
#include "ui/panels/patrol_panel.h"
#include "ui/panels/tune_panel.h"
#include "ui/p0panel.h"
#include "ui/theme/theme.h"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QDateTime>
#include <QDir>
#include <QGuiApplication>
#include <QScreen>
#include <QScrollArea>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMenuBar>
#include <QShortcut>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include "codec.h"
#include "crc16.h"
#include "io/link_manager.h"

namespace {

/// 状态条里的常驻小标签（等宽，避免数字跳动时整条状态栏左右抖）
QLabel *statusLabel()
{
    auto *l = new QLabel;
    l->setFont(theme::monoFont(9));
    return l;
}

} // namespace

MainWindow::MainWindow(io::LinkManager *mgr, domain::VehicleState *state, domain::AlarmEngine *alarm,
                       domain::KeepaliveEngine *keepalive, data::EventLog *log,
                       data::ParamStore *params, data::CsvRecorder *recorder, data::SessionStore *store,
                       QWidget *parent)
    : QMainWindow(parent), m_mgr(mgr), m_state(state), m_alarm(alarm), m_keepalive(keepalive),
      m_log(log), m_params(params), m_recorder(recorder), m_store(store)
{
    setWindowTitle(QStringLiteral("仓储环境安全巡检机器人 · 上位机"));
    // ★ 2026-10-15 修"全屏时显示不全"：原先把窗口写死 1560×960，而常见笔记本屏是 1366×768
    //   ⇒ 窗口比屏幕还大，底部永远在屏外；又因为各页有"硬最小高"，窗口也缩不下去 ⇒ 内容被裁。
    //   现在：① 默认尺寸取"设计尺寸"与"可用屏幕"的较小者（留出任务栏与边框余量）；
    //        ② 每一页都套一层 QScrollArea（内容再大也能滚动看到，不会"看不见就当没有"）。
    {
        int w = 1560;
        int h = 960;
        if (QScreen *sc = QGuiApplication::primaryScreen()) {
            const QRect avail = sc->availableGeometry();
            w = qMin(w, avail.width() - 40);
            h = qMin(h, avail.height() - 60);
        }
        resize(qMax(w, 900), qMax(h, 600));
    }

    // ---------------- 左侧导航 + 中央堆叠 ----------------
    auto *central = new QWidget(this);
    auto *hl = new QHBoxLayout(central);
    hl->setContentsMargins(0, 0, 0, 0);
    hl->setSpacing(0);

    m_nav = new QListWidget(central);
    m_nav->setObjectName(QStringLiteral("nav"));
    m_nav->setFixedWidth(150);
    m_nav->addItem(QStringLiteral("① 总览"));
    m_nav->addItem(QStringLiteral("② 巡检与控制"));
    m_nav->addItem(QStringLiteral("③ 曲线与告警"));
    m_nav->addItem(QStringLiteral("④ 调参与记录"));
    m_nav->addItem(QStringLiteral("⑤ 事件流"));
    m_nav->addItem(QStringLiteral("⑥ 通信链路"));
    m_nav->addItem(QStringLiteral("⑦ 协议自检"));

    m_stack = new QStackedWidget(central);
    m_overview = new OverviewPanel(m_state, m_mgr, m_alarm, m_params, m_stack);
    m_patrol = new PatrolPanel(m_state, m_mgr, m_alarm, m_keepalive, m_log, m_stack);
    m_curves = new CurvePanel(m_state, m_mgr, m_alarm, m_stack);
    m_tune = new TunePanel(m_state, m_mgr, m_params, m_log, m_stack);
    m_events = new EventPanel(m_log, m_stack);
    m_p0 = new P0Panel(m_mgr, m_stack);
    // 每页套一层可滚动容器：小屏/全屏下"看不到"的内容可以滚出来（而不是被裁掉）
    for (QWidget *page : {static_cast<QWidget *>(m_overview), static_cast<QWidget *>(m_patrol),
                          static_cast<QWidget *>(m_curves), static_cast<QWidget *>(m_tune),
                          static_cast<QWidget *>(m_events), static_cast<QWidget *>(m_p0),
                          buildSelfTestPage()}) {
        // ★ objectName 让 QSS 能精确给"滚动区里的页面"定底色（见 theme.cpp 的 dshPage 规则）：
        //   裸 QWidget 在原生样式下会拿系统调色板 ⇒ 浅色主题下变白底、浅字看不清。
        page->setObjectName(QStringLiteral("dshPage"));
        page->setAutoFillBackground(false);
        auto *sa = new QScrollArea(m_stack);
        sa->setWidgetResizable(true);   // 内容随视口自适应宽度（横向不强制出滚动条）
        sa->setFrameShape(QFrame::NoFrame);
        sa->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        sa->setWidget(page);
        m_stack->addWidget(sa);
    }

    hl->addWidget(m_nav);
    hl->addWidget(m_stack, 1);
    setCentralWidget(central);

    connect(m_nav, &QListWidget::currentRowChanged, m_stack, &QStackedWidget::setCurrentIndex);
    m_nav->setCurrentRow(0);

    // ---- 界面内部连线：数据到了 → 曲线采样/参数回读；告警 → 曲线标记 ----
    // （跨层装配在 main.cpp；这里只连"界面自己的面板"）
    connect(m_state, &domain::VehicleState::dataUpdated, m_curves, &CurvePanel::onDataUpdated);
    connect(m_state, &domain::VehicleState::dataUpdated, m_tune, &TunePanel::onDataUpdated);
    connect(m_alarm, &domain::AlarmEngine::alarmRaised, m_curves, &CurvePanel::onAlarmRaised);
    connect(m_alarm, &domain::AlarmEngine::alarmCleared, m_curves, &CurvePanel::onAlarmCleared);

    // ---------------- 状态条 ----------------
    buildStatusBar();

    auto *stTimer = new QTimer(this);
    stTimer->setInterval(200); // 5 Hz：状态栏不需要 20 Hz
    connect(stTimer, &QTimer::timeout, this, &MainWindow::refreshStatusBar);
    stTimer->start();
    refreshStatusBar();

    buildMenus();

    // 链路状态变化时给一条临时提示（起停链路是"偶发动作"，适合用临时消息）
    connect(mgr, &io::LinkManager::linkStateChanged, this,
            [this](io::LinkKind k, io::LinkState st, const QString &detail) {
                statusBar()->showMessage(QStringLiteral("%1：%2 %3")
                                             .arg(QString::fromUtf8(io::linkName(k)),
                                                  QString::fromUtf8(io::stateName(st)), detail),
                                         3000);
            });
    connect(m_alarm, &domain::AlarmEngine::alarmRaised, this,
            [this](const domain::AlarmRecord &r) {
                statusBar()->showMessage(
                    QStringLiteral("⚠ 告警：%1（%2）").arg(r.title, domain::units::number(
                                                                     r.value, r.unit == QStringLiteral("V") ? 2 : 1)),
                    5000);
            });
    connect(m_alarm, &domain::AlarmEngine::alarmCleared, this,
            [this](const domain::AlarmRecord &r) {
                statusBar()->showMessage(QStringLiteral("✔ 解除：%1").arg(r.title), 4000);
            });
}

void MainWindow::buildStatusBar()
{
    m_stDemo = statusLabel();
    m_stWire = statusLabel();
    m_stBt = statusLabel();
    m_stBattery = statusLabel();
    m_stSafety = statusLabel();
    m_stAlarm = statusLabel();
    m_stKeepalive = statusLabel();
    m_stRecord = statusLabel();
    m_stFw = statusLabel();
    m_stFrames = statusLabel();
    // 假源水印放在最左边：**演示数据必须一眼可辨**（计划 §11.2 第 10 条）
    statusBar()->addWidget(m_stDemo);
    statusBar()->addPermanentWidget(m_stWire);
    statusBar()->addPermanentWidget(m_stBt);
    statusBar()->addPermanentWidget(m_stBattery);
    statusBar()->addPermanentWidget(m_stAlarm);
    statusBar()->addPermanentWidget(m_stSafety);
    statusBar()->addPermanentWidget(m_stKeepalive);
    statusBar()->addPermanentWidget(m_stRecord);
    statusBar()->addPermanentWidget(m_stFw);
    statusBar()->addPermanentWidget(m_stFrames);
}

void MainWindow::showPage(int index)
{
    if (index >= 0 && index < m_nav->count()) {
        m_nav->setCurrentRow(index);
    }
}

int MainWindow::currentPage() const
{
    return m_stack->currentIndex();
}

QString MainWindow::pageTitle(int index) const
{
    if (index < 0 || index >= m_nav->count()) {
        return QString();
    }
    return m_nav->item(index)->text();
}

void MainWindow::buildMenus()
{
    // ---------------- 文件：记录与回放 ----------------
    auto *fileMenu = menuBar()->addMenu(QStringLiteral("文件(&F)"));
    auto *rec = fileMenu->addAction(QStringLiteral("开始录制（CSV 逐帧 + 会话入库）…"));
    rec->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_R));
    connect(rec, &QAction::triggered, this, [this]() {
        const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
                            + QStringLiteral("/records");
        QDir().mkpath(dir);
        const QString suggested =
            dir + QStringLiteral("/巡检记录_%1.csv")
                      .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss")));
        const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("开始录制"), suggested,
                                                         QStringLiteral("CSV (*.csv)"));
        if (!path.isEmpty()) {
            startRecording(path);
        }
    });
    auto *stopRec = fileMenu->addAction(QStringLiteral("停止录制"));
    stopRec->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_R));
    connect(stopRec, &QAction::triggered, this, &MainWindow::stopRecording);
    fileMenu->addSeparator();
    auto *play = fileMenu->addAction(QStringLiteral("打开记录文件回放（无硬件驱动界面）…"));
    connect(play, &QAction::triggered, this, &MainWindow::openReplayDialog);
    fileMenu->addSeparator();
    fileMenu->addAction(QStringLiteral("退出"), this, &QWidget::close);

    auto *viewMenu = menuBar()->addMenu(QStringLiteral("视图(&V)"));
    for (int i = 0; i < m_stack->count(); ++i) {
        const QString title = pageTitle(i);
        viewMenu->addAction(title, this, [this, i]() { showPage(i); });
    }
    viewMenu->addSeparator();

    auto *pause = viewMenu->addAction(QStringLiteral("暂停界面更新"));
    pause->setCheckable(true);
    pause->setShortcut(QKeySequence(Qt::Key_Space));
    connect(pause, &QAction::toggled, this, [this](bool on) {
        m_curves->setPaused(on);
        m_overview->setUpdatesPaused(on);
        statusBar()->showMessage(on ? QStringLiteral("界面已暂停更新（数据仍在接收与判定）")
                                    : QStringLiteral("界面恢复更新"),
                                 3000);
    });

    auto *topmost = viewMenu->addAction(QStringLiteral("窗口置顶"));
    topmost->setCheckable(true);
    connect(topmost, &QAction::toggled, this, [this](bool on) {
        const bool visible = isVisible();
        setWindowFlag(Qt::WindowStaysOnTopHint, on);
        if (visible) {
            show(); // 改窗口标志后必须重新 show，否则窗口会消失
        }
    });

    viewMenu->addSeparator();
    auto *shot = viewMenu->addAction(QStringLiteral("快速截图（F11）"));
    shot->setShortcut(QKeySequence(Qt::Key_F11));
    connect(shot, &QAction::triggered, this, &MainWindow::quickScreenshot);
    viewMenu->addAction(QStringLiteral("退出"), this, &QWidget::close);

    auto *helpMenu = menuBar()->addMenu(QStringLiteral("帮助(&H)"));
    helpMenu->addAction(QStringLiteral("关于本版本"), this, [this]() {
        statusBar()->showMessage(QStringLiteral("P4：调参与记录（0x24 全流程 + Flash + 观测模式 + "
                                                "三环曲线 + CSV/SQLite + 回放）；协议 v1.2；"
                                                "没有板子时用 --demo 跑假源"),
                                 8000);
    });
}

void MainWindow::startRecording(const QString &path)
{
    if (m_recorder == nullptr) {
        return;
    }
    if (!m_recorder->start(path)) {
        statusBar()->showMessage(QStringLiteral("✘ 录制未能开始：%1").arg(m_recorder->lastError()), 8000);
        return;
    }
    // 会话入库（SQLite 不可用时只是不记会话，逐帧 CSV 照常）
    if (m_store != nullptr && m_store->isAvailable()) {
        m_store->beginSession(QStringLiteral("上位机会话"), path);
    }
    statusBar()->showMessage(QStringLiteral("⏺ 录制中：%1（%2）")
                                 .arg(path, m_store != nullptr && m_store->isAvailable()
                                                ? QStringLiteral("会话已入库")
                                                : QStringLiteral("会话库不可用，仅 CSV")),
                             6000);
}

void MainWindow::stopRecording()
{
    if (m_recorder == nullptr || !m_recorder->isRecording()) {
        return;
    }
    m_recorder->stop();
    if (m_store != nullptr && m_store->isAvailable()) {
        m_store->endSession();
    }
    statusBar()->showMessage(QStringLiteral("⏹ 录制已停止：%1（%2 行）")
                                 .arg(m_recorder->path())
                                 .arg(m_recorder->rowsWritten()),
                             8000);
}

void MainWindow::openReplayDialog()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
                        + QStringLiteral("/records");
    const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("打开记录文件回放"), dir,
                                                      QStringLiteral("CSV (*.csv)"));
    if (path.isEmpty()) {
        return;
    }
    // 回放走**假源链路**（同一条数据通路）：关掉真实链路 → 切 Replay → 载入记录
    m_mgr->closeAll();
    m_mgr->setMode(io::LinkKind::Wire, io::LinkManager::Mode::Replay);
    const int n = m_mgr->loadCapture(io::LinkKind::Wire, path);
    if (n <= 0) {
        statusBar()->showMessage(QStringLiteral("✘ 记录文件里没有可用帧：%1").arg(path), 8000);
        return;
    }
    m_mgr->open(io::LinkKind::Wire);
    showPage(0);
    statusBar()->showMessage(QStringLiteral("▶ 回放中：%1（%2 帧，循环播放；"
                                            "切到「通信链路」页把实现方式改回真实串口即可退出回放）")
                                 .arg(path)
                                 .arg(n),
                             12000);
}

void MainWindow::quickScreenshot()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
                        + QStringLiteral("/shots");
    QDir().mkpath(dir);
    const QString name = QStringLiteral("上位机_%1_%2.png")
                             .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss")))
                             .arg(++m_shotCount);
    const QString path = dir + QLatin1Char('/') + name;
    if (grab().save(path)) {
        // 顺手把路径放进剪贴板：写实验报告时直接粘贴
        QApplication::clipboard()->setText(path);
        statusBar()->showMessage(QStringLiteral("截图已保存（路径已复制到剪贴板）：%1").arg(path), 8000);
    } else {
        statusBar()->showMessage(QStringLiteral("截图保存失败：%1").arg(path), 8000);
    }
}

void MainWindow::refreshStatusBar()
{
    // ---- 假源水印：只要有一条链路在跑假源，就必须明确标出来 ----
    bool demo = false;
    for (int i = 0; i < static_cast<int>(io::LinkKind::Count); ++i) {
        if (m_mgr->mode(static_cast<io::LinkKind>(i)) == io::LinkManager::Mode::Replay &&
            m_mgr->isOpen(static_cast<io::LinkKind>(i))) {
            demo = true;
        }
    }
    m_stDemo->setText(demo ? QStringLiteral("⚠ 演示数据（假源）—— 非真实测量")
                           : QStringLiteral("真实串口数据"));
    m_stDemo->setStyleSheet(QStringLiteral("color:%1; padding-right:10px;")
                                .arg(QString::fromLatin1(demo ? theme::kWarn : theme::kOffline)));

    // ---- 两条链路：状态 + 解帧（同一来源：链路自身维护的统计）----
    auto linkText = [this](io::LinkKind k) {
        const io::LinkStats s = m_mgr->stats(k);
        const bool open = m_mgr->isOpen(k);
        const qint64 age = m_state->linkAgeMs(k);
        const QString ageText = (age < 0) ? QStringLiteral("--") : QStringLiteral("%1ms").arg(age);
        return QStringLiteral("%1 %2 · 帧 %3 · %4")
            .arg(QString::fromUtf8(io::linkName(k)),
                 QString::fromUtf8(io::stateName(open ? io::LinkState::Open : io::LinkState::Closed)))
            .arg(s.framesOk)
            .arg(ageText);
    };
    m_stWire->setText(linkText(io::LinkKind::Wire));
    m_stBt->setText(linkText(io::LinkKind::Bluetooth));
    m_stWire->setStyleSheet(QStringLiteral("color:%1;")
                                .arg(QString::fromLatin1(m_mgr->isOpen(io::LinkKind::Wire)
                                                             ? theme::kOk
                                                             : theme::kOffline)));
    m_stBt->setStyleSheet(QStringLiteral("color:%1;")
                              .arg(QString::fromLatin1(m_mgr->isOpen(io::LinkKind::Bluetooth)
                                                           ? theme::kOk
                                                           : theme::kOffline)));

    // ---- 电池（★ 只有 0x15 带电压；没有它就只能显示"无数据"，不许编一个值）----
    if (m_state->batteryValid()) {
        m_stBattery->setText(QStringLiteral("🔋 %1 V")
                                 .arg(domain::units::number(m_state->diag().batteryV, 2)));
        const domain::Severity s = m_state->batterySeverity();
        m_stBattery->setStyleSheet(
            QStringLiteral("color:%1;")
                .arg(QString::fromLatin1(s == domain::Severity::Ok        ? theme::kText
                                         : s == domain::Severity::Warn    ? theme::kWarn
                                                                          : theme::kAlarm)));
    } else {
        m_stBattery->setText(QStringLiteral("🔋 -- V"));
        m_stBattery->setStyleSheet(QStringLiteral("color:%1;").arg(QString::fromLatin1(theme::kOffline)));
    }

    // ---- 报警与安全（原样显示，不做平滑）----
    const int active = m_alarm->activeCount();
    m_stAlarm->setText(active > 0 ? QStringLiteral("⚠ 报警 %1 项").arg(active)
                                  : QStringLiteral("无报警"));
    m_stAlarm->setStyleSheet(
        QStringLiteral("color:%1;")
            .arg(QString::fromLatin1(active > 0 ? theme::kAlarm : theme::kOffline)));
    m_stAlarm->setToolTip(m_alarm->activeSummary());

    const domain::Severity sev = m_state->safetySeverity();
    m_stSafety->setText(m_state->safetySummary());
    m_stSafety->setStyleSheet(QStringLiteral("color:%1;")
                                  .arg(QString::fromLatin1(sev == domain::Severity::Alarm ? theme::kAlarm
                                                              : sev == domain::Severity::Warn ? theme::kWarn
                                                              : sev == domain::Severity::Ok   ? theme::kOk
                                                                                             : theme::kOffline)));

    // ---- 固件版本（0x16 偏移 19）：B 侧接口文档 §1 点名"建议界面直接显示" ——
    //      "当前现象与预期版本不符"曾是无法定位的争议，一读版本号即可排除。
    if (m_state->received(proto::up::kLoop)) {
        const quint8 fw = m_state->loop().fwVersion;
        // ★ 特性位（0x16 状态位 bit3）：**一眼判断板上是不是新固件** ——
        //   同一天多次构建时 buildId 只到"日"，分辨不出来（本项目为此猜过一轮）。
        const bool fresh = m_state->loop().featureBits();
        m_stFw->setText(QStringLiteral("固件 %1/%2%3")
                            .arg(domain::hexByte(fw), domain::firmwareVersionText(fw),
                                 fresh ? QStringLiteral(" · 新特性✔")
                                       : QStringLiteral(" · ⚠旧构建")));
        m_stFw->setStyleSheet(QStringLiteral("color:%1;")
                                  .arg(QString::fromLatin1(fresh ? theme::kOk : theme::kWarn)));
        m_stFw->setToolTip(
            QStringLiteral("0x16 偏移 19 = (月<<4)|日；其状态位 bit3 = **固件特性位**。\n"
                           "未置位 ⇒ 板上是**旧构建**（缺 10-15 晚的改动：左右倾角上报 /\n"
                           "转向环启用 / 前进档 150 / MQ-135 角色指派）⇒ 请重烧 "
                           "firmware/build/MiniBalance.hex。"));
    } else {
        m_stFw->setText(QStringLiteral("固件 --"));
        m_stFw->setStyleSheet(QStringLiteral("color:%1;").arg(QString::fromLatin1(theme::kOffline)));
    }

    // ---- 录制状态：现场最怕"以为在录、其实没录"，所以常显 ----
    if (m_recorder != nullptr) {
        const bool rec = m_recorder->isRecording();
        m_stRecord->setText(rec ? QStringLiteral("⏺ 录制 %1 行").arg(m_recorder->rowsWritten())
                                : QStringLiteral("未录制"));
        m_stRecord->setStyleSheet(QStringLiteral("color:%1;")
                                      .arg(QString::fromLatin1(rec ? theme::kAlarm
                                                                   : theme::kOffline)));
        m_stRecord->setToolTip(rec ? m_recorder->path()
                                   : QStringLiteral("文件菜单 → 开始录制（CSV 逐帧 + 会话入库）；"
                                                    "Ctrl+R 快捷键"));
    }

    m_stFrames->setText(QStringLiteral("事件 %1 · 格式错帧 %2")
                            .arg(m_log->size())
                            .arg(m_state->malformedFrames()));

    // ---- 保活：它是"车会不会因为上位机安静半秒而锁存"的关键，必须常显 ----
    if (m_keepalive != nullptr) {
        const bool on = m_keepalive->enabled();
        const bool ready = m_keepalive->linkReady();
        const bool risky = (m_keepalive->mode() == domain::KeepaliveMode::EnableZeroSpeed);
        QString t = QStringLiteral("保活 ");
        if (!on) {
            t += QStringLiteral("关闭 ⚠");
        } else if (!ready) {
            t += QStringLiteral("待链路");
        } else {
            t += QStringLiteral("%1 ms · 已发 %2").arg(m_keepalive->intervalMs()).arg(m_keepalive->sentCount());
            if (risky) {
                t += QStringLiteral(" · 使能模式⚠");
            }
        }
        m_stKeepalive->setText(t);
        m_stKeepalive->setStyleSheet(
            QStringLiteral("color:%1;")
                .arg(QString::fromLatin1(!on ? theme::kWarn
                                         : risky ? theme::kAlarm
                                                 : (ready ? theme::kOk : theme::kWarn))));
        m_stKeepalive->setToolTip(
            QStringLiteral("固件规定：500 ms 没收到任何下行帧 ⇒ 安全停车并锁存（硬件按键也解不开）。\n"
                           "所以上位机要周期发合法下行帧。\n"
                           "★ 默认用心跳 0x1F：只声明在线，不使能电机、不解除锁存。\n"
                           "★ 使能 0x20 保活会**周期性解除安全锁存**，等于绕过急停保护（仅在确需保持使能时用）。"));
    }
}

QWidget *MainWindow::buildSelfTestPage()
{
    auto *page = new QWidget(this);
    auto *vl = new QVBoxLayout(page);
    auto *lbl = new QLabel(page);
    lbl->setWordWrap(true);
    lbl->setTextInteractionFlags(Qt::TextSelectableByMouse);
    lbl->setFont(theme::monoFont(10));
    lbl->setText(runProtocolSelfTest());
    vl->addWidget(lbl);
    vl->addStretch();
    return page;
}

QString MainWindow::runProtocolSelfTest()
{
    using namespace proto;

    // 1) CRC 自检（标准向量）
    const char *vec = "123456789";
    const uint16_t crc = crc16Modbus(reinterpret_cast<const uint8_t *>(vec), 9);
    const bool crcOk = (crc == 0x4B37);

    // 2) 组帧 → 解帧 → 往返比对（用 0x14 环境与气体帧，N=16）
    std::vector<uint8_t> d;
    putU16(d, 1234);
    putU16(d, 2345);
    putU16(d, 567);
    putU16(d, 890);
    putI16(d, 256);
    putU16(d, 610);
    d.push_back(0x0F);
    d.push_back(1);
    d.push_back(0);
    d.push_back(0);

    const auto bytes = encodeFrame(up::kEnvGas, d);
    FrameDecoder dec;
    const auto frames = dec.feed(bytes);
    const bool rtOk = (frames.size() == 1 && frames[0].cmd == up::kEnvGas && frames[0].data == d);

    // 3) 分片容错：逐字节喂入
    FrameDecoder dec2;
    std::size_t got2 = 0;
    for (uint8_t b : bytes) {
        got2 += dec2.feed(&b, 1).size();
    }
    const bool splitOk = (got2 == 1);

    // 4) 容错：坏 CRC 应被丢弃
    auto bad = bytes;
    bad.back() ^= 0xFF;
    FrameDecoder dec3;
    const bool dropOk = (dec3.feed(bad).empty() && dec3.framesDropped() == 1);

    // 5) 关键长度口径（协议 v1.1/v1.2）：0x11 = 9、0x14 = 16、0x15 = 16、0x16 = 20
    const bool lenOk = (domain::expectedUpLen(up::kMotion) == 9) &&
                       (domain::expectedUpLen(up::kEnvGas) == 16) &&
                       (domain::expectedUpLen(up::kDiag) == 16) &&
                       (domain::expectedUpLen(up::kLoop) == 20);

    QString s;
    s += QStringLiteral("协议层与 domain 层自检（与 docs/protocol/communication_protocol.md 一致）\n\n");
    s += QStringLiteral("① CRC-16 标准向量 \"123456789\" → 0x%1  %2\n")
             .arg(crc, 4, 16, QLatin1Char('0'))
             .arg(crcOk ? QStringLiteral("✔ 通过") : QStringLiteral("✘ 失败"));
    s += QStringLiteral("② 组帧/解帧往返（0x14，16 字节）  %1\n")
             .arg(rtOk ? QStringLiteral("✔ 通过") : QStringLiteral("✘ 失败"));
    s += QStringLiteral("③ 逐字节分片重组  %1\n")
             .arg(splitOk ? QStringLiteral("✔ 通过") : QStringLiteral("✘ 失败"));
    s += QStringLiteral("④ 坏校验丢弃  %1\n")
             .arg(dropOk ? QStringLiteral("✔ 通过") : QStringLiteral("✘ 失败"));
    s += QStringLiteral("⑤ 数据段长度口径（0x11=9 / 0x14=16 / 0x15=16 / 0x16=20）  %1\n")
             .arg(lenOk ? QStringLiteral("✔ 通过") : QStringLiteral("✘ 失败"));
    s += QStringLiteral("\n协议版本：v1.2（v1.1 冻结 + 0x15 诊断 + 0x16 三环实况）\n");
    s += QStringLiteral("domain 层：单位换算集中在 domain/units，帧解析在 domain/frames，\n"
                        "状态聚合在 domain/vehicle_state，告警判定与迟滞在 domain/alarm（界面不解析任何字节）。\n");

    const bool allOk = crcOk && rtOk && splitOk && dropOk && lenOk;
    s += QStringLiteral("\n结论：%1")
             .arg(allOk ? QStringLiteral("协议层与 domain 层工作正常，可对接固件。")
                        : QStringLiteral("存在失败项，需排查。"));
    return s;
}
