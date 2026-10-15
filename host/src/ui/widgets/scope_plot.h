#pragma once

// ============================================================================
//  ScopePlot —— 实时曲线（本项目**唯一**直接使用 QCustomPlot 的地方）
//
//  为什么把它封成一层：
//    ① 将来若要换绘图实现（或改成自绘），只动这一个文件；
//    ② 界面的"纪律"（颜色只表达异常、网格用中性细线、不用渐变填充）在这里一次性落实，
//       而不是在每个面板里各写一遍；
//    ③ 交互（十字定标 / 滚轮缩放 / 拖动平移 / 双击复位 / 阈值线 / 事件标记 / 末端读数）
//       集中在一处，避免"每张图各有一套操作习惯"。
//
//  ★ 采样与重绘是**两条节奏**：
//    · 采样：每收到一帧数据 append 一个点（由面板接 `VehicleState::dataUpdated` 驱动）；
//    · 重绘：由面板的定时器（10~20 Hz）调用 `replot()`。
//    绝不能"来一帧就重绘" —— 这是计划 §五 定的规矩，也是界面不卡的前提。
//
//  ★ 时间轴用**会话相对秒**（面板给的时间戳），不用墙上时间：
//    曲线横轴只关心"相隔多久"，墙上时间留给事件流与落盘。
// ============================================================================

#include <QColor>
#include <QVector>
#include <QWidget>

class QCustomPlot;
class QCPGraph;
class QCPItemLine;
class QCPItemStraightLine;
class QCPItemText;
class QCPItemTracer;
class QMouseEvent;
class QWheelEvent;

class ScopePlot : public QWidget
{
    Q_OBJECT
public:
    explicit ScopePlot(const QString &title, QWidget *parent = nullptr);
    ~ScopePlot() override;

    /// 加一条曲线通道（返回通道号，供 addSample 用）
    int addChannel(const QString &name, const QColor &color);

    /// 采一个样：tSec = 会话相对秒，y = 值
    void addSample(int channel, double tSec, double y);

    /// 阈值参考线（横向虚线 + 右侧标签）；调用多次会替换原来那条
    void setThresholdLine(double y, const QString &label, const QColor &color);
    /// 事件/告警标记（纵向虚线 + 顶部小标签）
    void addMarker(double tSec, const QString &text, const QColor &color);
    void clearMarkers();
    void clearData();

    void setUnitLabel(const QString &unit);
    /// 固定 Y 量程（ISA-101：同一量始终同一量程，便于"看位置"）
    void setYRange(double lo, double hi);
    /// 自动 Y 量程（可按需开启；默认用固定量程更符合规范）
    void setAutoY(bool on);
    /// 时间窗（秒）—— 决定横轴显示多长
    void setWindowSeconds(double sec);
    double windowSeconds() const { return m_windowSec; }

    /// 是否在曲线末端标出当前值（厂商 PC 工具的"曲线上直接标数值"）
    void setEndValueVisible(bool on);

    /// 撤销缩放：回到"时间窗 + Y 量程"的默认视图
    void resetView();
    /// 当前是否被用户缩放/平移过（面板据此把"撤销缩放"标成可用）
    bool isViewModified() const { return m_viewModified; }

    QString cursorReadout() const { return m_readout; }
    /// 最近一个采样点在横轴上的位置（秒）；没有数据返回 -1
    double lastTime() const { return m_lastT; }
    /// 已采样点数（所有通道合计）——测试与"数据到没到"的诊断用
    int sampleCount() const;
    int markerCount() const { return m_markers.size(); }
    int channelCount() const { return m_channels.size(); }

public slots:
    /// 重绘（由面板定时器调用；暂停时不重绘）
    void replot();

signals:
    /// 十字定标读数（鼠标在图上移动时；离开图区发空串）
    void cursorMoved(const QString &readout);
    /// 视图被用户缩放/平移（面板据此提示"已缩放，可撤销"）
    void viewModified(bool modified);

protected:
    /// 只用于"鼠标离开图区收起十字定标"（子控件 QCustomPlot 吃掉了 Enter/Leave，
    /// 所以必须用事件过滤器，而不是重写 QWidget::leaveEvent）
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    struct Channel {
        QString name;
        QColor color;
        QVector<double> t;
        QVector<double> y;
        QCPGraph *graph = nullptr;
        QCPItemTracer *tracer = nullptr;
        QCPItemText *endLabel = nullptr;
    };

    void trim();
    void updateEndLabels();
    void applyDefaultRanges();
    void markViewModified();
    void updateCrosshair(double key, double value);
    void styleAxes();

    QCustomPlot *m_plot = nullptr;
    QVector<Channel> m_channels;
    QCPItemStraightLine *m_thresholdLine = nullptr;
    QCPItemText *m_thresholdLabel = nullptr;
    QVector<QCPItemLine *> m_markers;
    QVector<QCPItemText *> m_markerLabels;
    QCPItemStraightLine *m_crossV = nullptr;
    QCPItemText *m_crossLabel = nullptr;

    QString m_unit;
    double m_windowSec = 120.0;
    double m_yLo = 0.0;
    double m_yHi = 100.0;
    bool m_autoY = false;
    bool m_endLabels = true;
    bool m_viewModified = false;
    /// 正在程序化设置量程（此时 xAxis::rangeChanged 不算"用户缩放"）
    bool m_settingRange = false;
    double m_lastT = -1.0;
    QString m_readout;
    /// 数据点上限（防"窗口设得极大 + 高频帧"把内存吃满）
    static constexpr int kMaxPoints = 20000;
    /// 事件标记上限（防长时间运行后标记铺满整张图）
    static constexpr int kMaxMarkers = 40;
};
