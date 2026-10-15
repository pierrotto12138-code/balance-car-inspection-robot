#include "mainwindow.h"

#include "data/event_log.h"
#include "domain/alarm.h"
#include "domain/units.h"
#include "domain/vehicle_state.h"
#include "ui/panels/curve_panel.h"
#include "ui/panels/event_panel.h"
#include "ui/panels/overview_panel.h"
#include "ui/p0panel.h"
#include "ui/theme/theme.h"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QDateTime>
#include <QDir>
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
                       data::EventLog *log, QWidget *parent)
    : QMainWindow(parent), m_mgr(mgr), m_state(state), m_alarm(alarm), m_log(log)
{
    setWindowTitle(QStringLiteral("仓储环境安全巡检机器人 · 上位机"));
    resize(1500, 940);

    // ---------------- 左侧导航 + 中央堆叠 ----------------
    auto *central = new QWidget(this);
    auto *hl = new QHBoxLayout(central);
    hl->setContentsMargins(0, 0, 0, 0);
    hl->setSpacing(0);

    m_nav = new QListWidget(central);
    m_nav->setObjectName(QStringLiteral("nav"));
    m_nav->setFixedWidth(150);
    m_nav->addItem(QStringLiteral("① 总览"));
    m_nav->addItem(QStringLiteral("② 曲线与告警"));
    m_nav->addItem(QStringLiteral("③ 事件流"));
    m_nav->addItem(QStringLiteral("④ 通信链路"));
    m_nav->addItem(QStringLiteral("⑤ 协议自检"));

    m_stack = new QStackedWidget(central);
    m_overview = new OverviewPanel(m_state, m_mgr, m_alarm, m_stack);
    m_curves = new CurvePanel(m_state, m_mgr, m_alarm, m_stack);
    m_events = new EventPanel(m_log, m_stack);
    m_p0 = new P0Panel(m_mgr, m_stack);
    m_stack->addWidget(m_overview);
    m_stack->addWidget(m_curves);
    m_stack->addWidget(m_events);
    m_stack->addWidget(m_p0);
    m_stack->addWidget(buildSelfTestPage());

    hl->addWidget(m_nav);
    hl->addWidget(m_stack, 1);
    setCentralWidget(central);

    connect(m_nav, &QListWidget::currentRowChanged, m_stack, &QStackedWidget::setCurrentIndex);
    m_nav->setCurrentRow(0);

    // ---- 界面内部连线：数据到了 → 曲线采样；告警 → 曲线标记 ----
    // （跨层装配在 main.cpp；这里只连"界面自己的面板"）
    connect(m_state, &domain::VehicleState::dataUpdated, m_curves, &CurvePanel::onDataUpdated);
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
    m_stFrames = statusLabel();
    // 假源水印放在最左边：**演示数据必须一眼可辨**（计划 §11.2 第 10 条）
    statusBar()->addWidget(m_stDemo);
    statusBar()->addPermanentWidget(m_stWire);
    statusBar()->addPermanentWidget(m_stBt);
    statusBar()->addPermanentWidget(m_stBattery);
    statusBar()->addPermanentWidget(m_stAlarm);
    statusBar()->addPermanentWidget(m_stSafety);
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
        statusBar()->showMessage(QStringLiteral("P2：曲线与告警（四张通道独立曲线 + 阈值线 + 告警标记 + "
                                                "事件流）；协议 v1.2；没有板子时用 --demo 跑假源"),
                                 8000);
    });
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

    m_stFrames->setText(QStringLiteral("事件 %1 · 格式错帧 %2")
                            .arg(m_log->size())
                            .arg(m_state->malformedFrames()));
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
