#include "mainwindow.h"

#include "domain/units.h"
#include "domain/vehicle_state.h"
#include "ui/panels/overview_panel.h"
#include "ui/p0panel.h"
#include "ui/theme/theme.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMenuBar>
#include <QStackedWidget>
#include <QStatusBar>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include "codec.h"
#include "crc16.h"
#include "io/link_manager.h"

namespace {

/// 状态栏里的常驻小标签（等宽，避免数字跳动时整条状态栏左右抖）
QLabel *statusLabel()
{
    auto *l = new QLabel;
    l->setFont(theme::monoFont(9));
    return l;
}

} // namespace

MainWindow::MainWindow(io::LinkManager *mgr, domain::VehicleState *state, QWidget *parent)
    : QMainWindow(parent), m_mgr(mgr), m_state(state)
{
    setWindowTitle(QStringLiteral("仓储环境安全巡检机器人 · 上位机（P1 状态可见）"));
    resize(1500, 900);

    // ---------------- 左侧导航 + 中央堆叠 ----------------
    auto *central = new QWidget(this);
    auto *hl = new QHBoxLayout(central);
    hl->setContentsMargins(0, 0, 0, 0);
    hl->setSpacing(0);

    m_nav = new QListWidget(central);
    m_nav->setObjectName(QStringLiteral("nav"));
    m_nav->setFixedWidth(150);
    m_nav->addItem(QStringLiteral("① 总览"));
    m_nav->addItem(QStringLiteral("② 通信链路"));
    m_nav->addItem(QStringLiteral("③ 协议自检"));

    m_stack = new QStackedWidget(central);
    m_overview = new OverviewPanel(m_state, m_mgr, m_stack);
    m_p0 = new P0Panel(m_mgr, m_stack);
    m_stack->addWidget(m_overview);
    m_stack->addWidget(m_p0);
    m_stack->addWidget(buildSelfTestPage());

    hl->addWidget(m_nav);
    hl->addWidget(m_stack, 1);
    setCentralWidget(central);

    connect(m_nav, &QListWidget::currentRowChanged, m_stack, &QStackedWidget::setCurrentIndex);
    m_nav->setCurrentRow(0);

    // ---------------- 状态栏 ----------------
    m_stWire = statusLabel();
    m_stBt = statusLabel();
    m_stBattery = statusLabel();
    m_stSafety = statusLabel();
    m_stFrames = statusLabel();
    statusBar()->addPermanentWidget(m_stWire);
    statusBar()->addPermanentWidget(m_stBt);
    statusBar()->addPermanentWidget(m_stBattery);
    statusBar()->addPermanentWidget(m_stSafety);
    statusBar()->addPermanentWidget(m_stFrames);

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

void MainWindow::buildMenus()
{
    auto *viewMenu = menuBar()->addMenu(QStringLiteral("视图(&V)"));
    for (int i = 0; i < m_stack->count(); ++i) {
        const QString title = m_nav->item(i)->text();
        viewMenu->addAction(title, this, [this, i]() { m_nav->setCurrentRow(i); });
    }
    viewMenu->addSeparator();
    viewMenu->addAction(QStringLiteral("退出"), this, &QWidget::close);

    auto *helpMenu = menuBar()->addMenu(QStringLiteral("帮助(&H)"));
    helpMenu->addAction(QStringLiteral("关于本版本"), this, [this]() {
        statusBar()->showMessage(
            QStringLiteral("P1 状态可见：domain 层（单位/解析/聚合）+ 总览面板；"
                           "通信链路面板见左侧 ②；没有板子时用 --demo 跑假源"),
            8000);
    });
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

void MainWindow::refreshStatusBar()
{
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

    // ---- 安全（原样显示，不做平滑）----
    const domain::Severity sev = m_state->safetySeverity();
    m_stSafety->setText(m_state->safetySummary());
    m_stSafety->setStyleSheet(QStringLiteral("color:%1;")
                                  .arg(QString::fromLatin1(sev == domain::Severity::Alarm ? theme::kAlarm
                                                              : sev == domain::Severity::Warn ? theme::kWarn
                                                              : sev == domain::Severity::Ok   ? theme::kOk
                                                                                             : theme::kOffline)));

    m_stFrames->setText(QStringLiteral("格式错帧 %1").arg(m_state->malformedFrames()));
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
                        "状态聚合与安全判定在 domain/vehicle_state（界面不解析任何字节）。\n");

    const bool allOk = crcOk && rtOk && splitOk && dropOk && lenOk;
    s += QStringLiteral("\n结论：%1")
             .arg(allOk ? QStringLiteral("协议层与 domain 层工作正常，可对接固件。")
                        : QStringLiteral("存在失败项，需排查。"));
    return s;
}
