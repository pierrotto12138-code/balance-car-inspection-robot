#pragma once

// ============================================================================
//  记录与落盘（data 层，P4）
//
//  两件东西，各有各的用途：
//    ① `CsvRecorder`：**逐帧一行**的 CSV（含原始帧的十六进制列）——
//       · 给 Excel 直接看（同事/老师要的就是这个）；
//       · 同时**自足可回放**：十六进制列就是原始帧，`--play` 直接拿它驱动整个界面
//         （不需要板子，也不需要另存一份二进格式，避免"记录与回放两套格式"）。
//    ② `SessionStore`：SQLite 里存**会话 / 事件 / 参数快照**三张表 ——
//       回答"哪一次实验、当时改了什么参数、报了哪些警"这类**跨会话**的问题；
//       逐帧数据留在 CSV 里（SQLite 存 20 kHz 级别的逐帧数据得不偿失）。
//
//  三条实现纪律：
//    · **写盘失败要能被看见**：`start()` 返回 false 并带上原因，界面/自检都要报出来 ——
//      "以为在录，其实没录"比"没录"更危险（现场演示常常靠这份数据复现问题）；
//    · **CSV 用 UTF-8 + BOM**：中文 Windows 的 Excel 默认按本地编码解析，
//      没有 BOM 的表头会变成乱码（这是"记录文件能被 Excel 打开"的实际含义）；
//    · **SQLite 缺失/打开失败不许崩**：`isAvailable()` 为假时全部操作变空操作。
// ============================================================================

#include "data/event_log.h"
#include "io/link.h"

#include <QObject>
#include <QString>
#include <QVector>

class QFile;
class QTextStream;

namespace data {

/// 逐帧 CSV 记录器
class CsvRecorder : public QObject
{
    Q_OBJECT
public:
    explicit CsvRecorder(QObject *parent = nullptr);
    ~CsvRecorder() override;

    /// 开始记录（覆盖同名文件）。失败时 `lastError()` 有原因
    bool start(const QString &path);
    void stop();
    bool isRecording() const { return m_file != nullptr; }
    QString path() const { return m_path; }
    QString lastError() const { return m_error; }
    quint64 rowsWritten() const { return m_rows; }

public slots:
    /// 接 `io::LinkManager::frameReady`：每帧写一行
    void onFrame(io::LinkKind kind, quint8 cmd, const QByteArray &data);
    /// 接 `data::EventLog::entryAdded`：事件/告警/操作员动作也写进同一根时间轴
    void onEvent(const data::EventEntry &e);

signals:
    void started(const QString &path);
    void stopped(const QString &path, quint64 rows);
    void writeFailed(const QString &why);

private:
    /// 写一行：前缀 5 列（本地时间/墙上时间/来源链路/命令/名称）+ 数据列 + HEX
    ///
    /// ★ 列数与 `headerRow()` **同源**（都由 `kDataCols` 决定）：逐帧与逐事件走同一个
    ///   写入口，避免"表头 42 列、数据 41 列"这种 Excel 里才会暴露的错位。
    void writeRow(const QString &linkText, const QString &cmdText, const QString &name,
                  const QStringList &dataCols, const QByteArray &raw);

    QFile *m_file = nullptr;
    QTextStream *m_out = nullptr;
    QString m_path;
    QString m_error;
    quint64 m_rows = 0;
};

/// 会话 / 事件 / 参数快照（SQLite）
class SessionStore : public QObject
{
    Q_OBJECT
public:
    explicit SessionStore(QObject *parent = nullptr);
    ~SessionStore() override;

    /// 打开（或新建）数据库；返回是否可用。驱动缺失/路径不可写时返回 false 但不崩
    bool open(const QString &dbPath);
    bool isAvailable() const { return m_available; }
    QString lastError() const { return m_error; }
    QString databasePath() const { return m_dbPath; }

    /// 开一次会话（记录开始时间、CSV 路径与备注），返回会话 id（不可用时返回 -1）
    qint64 beginSession(const QString &note, const QString &csvPath);
    /// 结束当前会话（写结束时间；返回会话 id，不可用时 -1）
    qint64 endSession();
    qint64 currentSession() const { return m_sessionId; }

    bool addEvent(const data::EventEntry &e);
    bool addParam(qint64 atMs, quint8 paramId, double value, const QString &source);

    /// 最近若干次会话（倒序）；不可用时返回空
    struct SessionRow {
        qint64 id = -1;
        QString started;
        QString ended;
        QString note;
        QString csvPath;
    };
    QVector<SessionRow> recentSessions(int limit = 20) const;
    int eventCount() const;
    int paramCount() const;

signals:
    void unavailable(const QString &why);

private:
    bool exec(const QString &sql);

    QString m_dbPath;
    QString m_error;
    QString m_connection;
    bool m_available = false;
    qint64 m_sessionId = -1;
};

} // namespace data
