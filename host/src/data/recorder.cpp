#include "data/recorder.h"

#include "codec.h"
#include "domain/commands.h"
#include "domain/frames.h"
#include "domain/units.h"

#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTextStream>
#include <QUuid>

#include <cstring>

namespace data {

namespace {

/// 数据列数（前缀 5 列 + 36 数据列 + 1 HEX = 42 列；改这里同时改 headerRow()）
constexpr int kDataCols = 36;

/// 数据列下标（与 headerRow() 的顺序一一对应）
enum DataCol {
    TaskName = 0,
    Phase,
    Progress,
    Checkpoint,
    FaultFlags,
    Pitch,
    Roll,
    Yaw,
    GyroX,
    LeftMmS,
    RightMmS,
    Pwm,
    Dist,
    Avoid,
    Mq2,
    Mq135,
    Temp,
    Hum,
    Gear,
    Volt,
    Page,
    Heap,
    OledRate,
    Bal,
    Vel,
    Turn,
    Moto1,
    Moto2,
    CcrA,
    CcrB,
    EncL,
    EncR,
    FwVersion,
    EventCode,
    Content,
    Note,
};

/// 表头（42 列，自描述：列名写清单位与口径，避免事后猜）
QStringList headerRow()
{
    return QStringList{
        QStringLiteral("本地时间ms"),     QStringLiteral("墙上时间"),    QStringLiteral("来源链路"),
        QStringLiteral("命令"),          QStringLiteral("名称"),       QStringLiteral("任务"),
        QStringLiteral("阶段"),          QStringLiteral("完成度%"),     QStringLiteral("巡检点"),
        QStringLiteral("异常标志"),      QStringLiteral("俯仰deg"),     QStringLiteral("横滚deg"),
        QStringLiteral("偏航deg"),       QStringLiteral("角速度Xdps"),  QStringLiteral("左轮速mmps"),
        QStringLiteral("右轮速mmps"),    QStringLiteral("PWM%"),        QStringLiteral("超声cm"),
        QStringLiteral("避障位"),        QStringLiteral("MQ2ppm"),      QStringLiteral("MQ135ppm"),
        QStringLiteral("温度C"),         QStringLiteral("湿度%RH"),     QStringLiteral("采样档位"),
        QStringLiteral("电压V"),         QStringLiteral("显示页"),      QStringLiteral("剩余堆KB"),
        QStringLiteral("OLED写入速率Bps"), QStringLiteral("平衡环Bal"), QStringLiteral("速度环Vel"),
        QStringLiteral("转向环Turn"),    QStringLiteral("左Moto"),      QStringLiteral("右Moto"),
        QStringLiteral("CCR_A"),         QStringLiteral("CCR_B"),       QStringLiteral("编码器左"),
        QStringLiteral("编码器右"),      QStringLiteral("固件版本"),    QStringLiteral("事件码"),
        QStringLiteral("内容"),          QStringLiteral("备注"),        QStringLiteral("原始帧HEX")};
}

/// 空的 36 个数据列（每行只填自己那几列，其余留空 ⇒ 单文件覆盖所有帧型，Excel 直接透视）
QStringList emptyDataCols()
{
    QStringList c;
    c.reserve(kDataCols);
    for (int i = 0; i < kDataCols; ++i) {
        c << QString();
    }
    return c;
}

/// CSV 转义：字段里有逗号/引号/换行时用双引号包起来（Excel 与脚本都能正确解析）
QString csvCell(const QString &s)
{
    if (s.contains(QLatin1Char(',')) || s.contains(QLatin1Char('"')) ||
        s.contains(QLatin1Char('\n'))) {
        QString t = s;
        t.replace(QLatin1Char('"'), QStringLiteral("\"\""));
        return QLatin1Char('"') + t + QLatin1Char('"');
    }
    return s;
}

/// 数值列：统一小数位（Excel 里列对齐、不抖）
QString num(double v, int decimals)
{
    return domain::units::number(v, decimals);
}

} // namespace

// ---------------------------------------------------------------- CsvRecorder

CsvRecorder::CsvRecorder(QObject *parent) : QObject(parent) {}

CsvRecorder::~CsvRecorder()
{
    stop();
}

bool CsvRecorder::start(const QString &path)
{
    stop();
    m_error.clear();
    QDir().mkpath(QFileInfo(path).absolutePath());

    auto *f = new QFile(path);
    if (!f->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        m_error = QStringLiteral("无法写入 %1（%2）").arg(path, f->errorString());
        delete f;
        emit writeFailed(m_error);
        return false;
    }
    m_file = f;
    m_path = path;
    m_out = new QTextStream(m_file);
    m_out->setEncoding(QStringConverter::Utf8);
    m_rows = 0;

    // ★ UTF-8 BOM：中文 Windows 的 Excel 没有 BOM 时会按 GBK 解析，表头变乱码
    m_file->write("\xEF\xBB\xBF");

    // 自描述表头（与 writeRow 用同一个列数常量，杜绝"表头与数据错位"）
    *m_out << headerRow().join(QLatin1Char(',')) << QLatin1Char('\n');
    m_out->flush();

    emit started(path);
    return true;
}

void CsvRecorder::writeRow(const QString &linkText, const QString &cmdText, const QString &name,
                           const QStringList &dataCols, const QByteArray &raw)
{
    if (m_file == nullptr || m_out == nullptr) {
        return;
    }
    QStringList cells;
    cells.reserve(5 + kDataCols + 1);
    cells << QString::number(QDateTime::currentMSecsSinceEpoch())
          << QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz"))
          << linkText << cmdText << name;
    for (int i = 0; i < kDataCols; ++i) {
        cells << (i < dataCols.size() ? dataCols.at(i) : QString());
    }
    cells << QString::fromLatin1(raw.toHex(' ').toUpper());

    QStringList escaped;
    escaped.reserve(cells.size());
    for (const QString &c : cells) {
        escaped << csvCell(c);
    }
    *m_out << escaped.join(QLatin1Char(',')) << QLatin1Char('\n');
    ++m_rows;
    if ((m_rows % 50) == 0) {
        m_out->flush(); // 定期落盘：演示中途断电也只丢最后几十行
    }
}

void CsvRecorder::onFrame(io::LinkKind kind, quint8 cmd, const QByteArray &data)
{
    if (!isRecording()) {
        return;
    }
    // 原始帧：与真实发送路径一致（数据段 + 命令字组帧），回放时直接喂解帧器
    const std::vector<uint8_t> payload(data.begin(), data.end());
    const auto frameBytes = proto::encodeFrame(cmd, payload);
    const QByteArray raw(reinterpret_cast<const char *>(frameBytes.data()),
                         static_cast<int>(frameBytes.size()));

    const QString link = QString::fromUtf8(io::linkName(kind));
    const QString cmdText = domain::hexByte(cmd);
    const QString name = QString::fromUtf8(domain::cmd::name(cmd));
    QStringList cols = emptyDataCols();

    switch (cmd) {
    case proto::up::kPose: {
        domain::PoseData v;
        if (domain::decodePose(data, v)) {
            cols[Pitch] = num(v.pitchDeg, 2);
            cols[Roll] = num(v.rollDeg, 2);
            cols[Yaw] = num(v.yawDeg, 2);
            cols[GyroX] = num(v.gyroXDps, 2);
        }
        break;
    }
    case proto::up::kMotion: {
        domain::MotionData v;
        if (domain::decodeMotion(data, v)) {
            cols[LeftMmS] = QString::number(v.leftMmS);
            cols[RightMmS] = QString::number(v.rightMmS);
            cols[Pwm] = num(v.pwmPercent, 1);
            cols[Dist] = num(domain::units::cmFromMm(v.distanceMm), 1);
            cols[Avoid] = QStringLiteral("0b%1").arg(v.avoidFlags, 4, 2, QLatin1Char('0'));
        }
        break;
    }
    case proto::up::kTask: {
        domain::TaskData v;
        if (domain::decodeTask(data, v)) {
            cols[TaskName] = QString::fromUtf8(domain::taskName(v.taskId));
            cols[Phase] = QString::fromUtf8(domain::phaseName(v.phase));
            cols[Progress] = QString::number(v.progressPct);
            cols[Checkpoint] = QString::number(v.checkpoint);
            cols[FaultFlags] = domain::faultFlagsText(v.faultFlags);
        }
        break;
    }
    case proto::up::kEnvGas: {
        domain::EnvData v;
        if (domain::decodeEnv(data, v)) {
            cols[Mq2] = num(v.mq2Ppm, 1);
            cols[Mq135] = num(v.mq135Ppm, 1);
            cols[Temp] = num(v.tempC, 1);
            cols[Hum] = num(v.humidityPct, 1);
            cols[Gear] = QString::fromUtf8(domain::sampleGearName(v.gear));
        }
        break;
    }
    case proto::up::kDiag: {
        domain::DiagData v;
        if (domain::decodeDiag(data, v)) {
            cols[Volt] = num(v.batteryV, 2);
            cols[Page] = QString::fromUtf8(domain::diagPageName(v.page));
            cols[Heap] = QString::number(v.heapKb);
            cols[OledRate] = QString::number(v.oledBytes);
        }
        break;
    }
    case proto::up::kLoop: {
        domain::LoopData v;
        if (domain::decodeLoop(data, v)) {
            cols[Bal] = QString::number(v.balancePwm);
            cols[Vel] = QString::number(v.velocityPwm);
            cols[Turn] = QString::number(v.turnPwm);
            cols[Moto1] = QString::number(v.moto1);
            cols[Moto2] = QString::number(v.moto2);
            cols[CcrA] = QString::number(v.ccrA);
            cols[CcrB] = QString::number(v.ccrB);
            cols[EncL] = QString::number(v.encLeft);
            cols[EncR] = QString::number(v.encRight);
            cols[FwVersion] = domain::firmwareVersionText(v.fwVersion);
        }
        break;
    }
    case proto::up::kEvent: {
        domain::EventData v;
        if (domain::decodeEvent(data, v)) {
            cols[EventCode] = domain::hexByte(static_cast<quint8>(v.code));
            cols[Content] = QString::fromUtf8(domain::eventName(v.code));
            cols[Note] = QStringLiteral("机器人时间戳 %1 ms").arg(v.timestampMs);
        }
        break;
    }
    default:
        break;
    }

    writeRow(link, cmdText, name, cols, raw);
}

void CsvRecorder::onEvent(const data::EventEntry &e)
{
    if (!isRecording()) {
        return;
    }
    QStringList cols = emptyDataCols();
    cols[EventCode] = e.local ? QStringLiteral("本机") : domain::hexByte(e.code);
    cols[Content] = e.title;
    cols[Note] = e.detail;
    writeRow(e.local ? QStringLiteral("本机") : QString::fromUtf8(io::linkName(e.kind)),
             e.local ? QStringLiteral("事件/本机") : QStringLiteral("事件"), e.title, cols, QByteArray());
    m_out->flush(); // 事件不常有，逐条刷盘
}

void CsvRecorder::stop()
{
    if (m_file == nullptr) {
        return;
    }
    m_out->flush();
    delete m_out;
    m_out = nullptr;
    m_file->close();
    delete m_file;
    m_file = nullptr;
    emit stopped(m_path, m_rows);
}

// ---------------------------------------------------------------- SessionStore

SessionStore::SessionStore(QObject *parent) : QObject(parent)
{
    m_connection = QStringLiteral("mcs_session_%1")
                       .arg(QUuid::createUuid().toString(QUuid::WithoutBraces).left(8));
}

SessionStore::~SessionStore()
{
    if (QSqlDatabase::contains(m_connection)) {
        {
            QSqlDatabase db = QSqlDatabase::database(m_connection, false);
            if (db.isOpen()) {
                db.close();
            }
        }
        QSqlDatabase::removeDatabase(m_connection);
    }
}

bool SessionStore::open(const QString &dbPath)
{
    m_dbPath = dbPath;
    QDir().mkpath(QFileInfo(dbPath).absolutePath());
    if (!QSqlDatabase::isDriverAvailable(QStringLiteral("QSQLITE"))) {
        m_error = QStringLiteral("SQLite 驱动不可用（Qt 未部署 sqldrivers/qsqlite）—— "
                                 "会话/事件/参数快照不可用，逐帧 CSV 不受影响");
        emit unavailable(m_error);
        return false;
    }
    QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connection);
    db.setDatabaseName(dbPath);
    if (!db.open()) {
        m_error = QStringLiteral("打开数据库失败：%1").arg(db.lastError().text());
        emit unavailable(m_error);
        return false;
    }
    // 三张表：会话 / 事件 / 参数快照（逐帧数据留在 CSV 里）
    bool ok = true;
    ok &= exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS sessions("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  started TEXT NOT NULL, ended TEXT, note TEXT, csv_path TEXT)"));
    ok &= exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS events("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT, session_id INTEGER, ts TEXT, kind TEXT,"
        "  code INTEGER, title TEXT, detail TEXT)"));
    ok &= exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS params("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT, session_id INTEGER, ts TEXT, param_id INTEGER,"
        "  value REAL, source TEXT)"));
    if (!ok) {
        return false;
    }
    m_available = true;
    m_error.clear();
    return true;
}

bool SessionStore::exec(const QString &sql)
{
    QSqlDatabase db = QSqlDatabase::database(m_connection, false);
    QSqlQuery q(db);
    if (!q.exec(sql)) {
        m_error = QStringLiteral("SQL 失败：%1（%2）").arg(q.lastError().text(), sql.left(60));
        m_available = false;
        emit unavailable(m_error);
        return false;
    }
    return true;
}

qint64 SessionStore::beginSession(const QString &note, const QString &csvPath)
{
    if (!m_available) {
        return -1;
    }
    QSqlDatabase db = QSqlDatabase::database(m_connection, false);
    QSqlQuery q(db);
    q.prepare(QStringLiteral("INSERT INTO sessions(started, note, csv_path) VALUES(?,?,?)"));
    q.addBindValue(QDateTime::currentDateTime().toString(Qt::ISODate));
    q.addBindValue(note);
    q.addBindValue(csvPath);
    if (!q.exec()) {
        m_error = q.lastError().text();
        return -1;
    }
    m_sessionId = q.lastInsertId().toLongLong();
    return m_sessionId;
}

qint64 SessionStore::endSession()
{
    if (!m_available || m_sessionId < 0) {
        return -1;
    }
    QSqlDatabase db = QSqlDatabase::database(m_connection, false);
    QSqlQuery q(db);
    q.prepare(QStringLiteral("UPDATE sessions SET ended=? WHERE id=?"));
    q.addBindValue(QDateTime::currentDateTime().toString(Qt::ISODate));
    q.addBindValue(m_sessionId);
    if (!q.exec()) {
        m_error = q.lastError().text();
        return -1;
    }
    const qint64 id = m_sessionId;
    m_sessionId = -1;
    return id;
}

bool SessionStore::addEvent(const data::EventEntry &e)
{
    if (!m_available) {
        return false;
    }
    QSqlDatabase db = QSqlDatabase::database(m_connection, false);
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "INSERT INTO events(session_id, ts, kind, code, title, detail) VALUES(?,?,?,?,?,?)"));
    q.addBindValue(m_sessionId);
    q.addBindValue(e.wall.toString(Qt::ISODateWithMs));
    q.addBindValue(e.local ? QStringLiteral("本机") : QString::fromUtf8(io::linkName(e.kind)));
    q.addBindValue(int(e.code));
    q.addBindValue(e.title);
    q.addBindValue(e.detail);
    if (!q.exec()) {
        m_error = q.lastError().text();
        return false;
    }
    return true;
}

bool SessionStore::addParam(qint64 atMs, quint8 paramId, double value, const QString &source)
{
    if (!m_available) {
        return false;
    }
    QSqlDatabase db = QSqlDatabase::database(m_connection, false);
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "INSERT INTO params(session_id, ts, param_id, value, source) VALUES(?,?,?,?,?)"));
    q.addBindValue(m_sessionId);
    q.addBindValue(QDateTime::fromMSecsSinceEpoch(atMs).toString(Qt::ISODateWithMs));
    q.addBindValue(int(paramId));
    q.addBindValue(value);
    q.addBindValue(source);
    if (!q.exec()) {
        m_error = q.lastError().text();
        return false;
    }
    return true;
}

QVector<SessionStore::SessionRow> SessionStore::recentSessions(int limit) const
{
    QVector<SessionRow> out;
    if (!m_available) {
        return out;
    }
    QSqlDatabase db = QSqlDatabase::database(m_connection, false);
    QSqlQuery q(db);
    q.prepare(QStringLiteral("SELECT id, started, ended, note, csv_path FROM sessions "
                             "ORDER BY id DESC LIMIT ?"));
    q.addBindValue(limit);
    if (!q.exec()) {
        return out;
    }
    while (q.next()) {
        SessionRow r;
        r.id = q.value(0).toLongLong();
        r.started = q.value(1).toString();
        r.ended = q.value(2).toString();
        r.note = q.value(3).toString();
        r.csvPath = q.value(4).toString();
        out.append(r);
    }
    return out;
}

int SessionStore::eventCount() const
{
    if (!m_available) {
        return -1;
    }
    QSqlDatabase db = QSqlDatabase::database(m_connection, false);
    QSqlQuery q(db);
    if (!q.exec(QStringLiteral("SELECT COUNT(*) FROM events")) || !q.next()) {
        return -1;
    }
    return q.value(0).toInt();
}

int SessionStore::paramCount() const
{
    if (!m_available) {
        return -1;
    }
    QSqlDatabase db = QSqlDatabase::database(m_connection, false);
    QSqlQuery q(db);
    if (!q.exec(QStringLiteral("SELECT COUNT(*) FROM params")) || !q.next()) {
        return -1;
    }
    return q.value(0).toInt();
}

} // namespace data
