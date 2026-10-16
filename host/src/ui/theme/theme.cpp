#include "ui/theme/theme.h"

#include <QStringList>

namespace theme {

QString styleSheet()
{
    // 说明：QSS 里用 %1..%8 注入配色变量 —— 改配色只改 theme.h 一处。
    // 这里显式 fromLatin1 转 QString：QSS 模板里 %n 的出现顺序与参数顺序无关，
    // 用显式类型可以避免"隐式转换 + 多参 arg"带来的意外重载歧义。
    auto s = [](const char *hex) { return QString::fromLatin1(hex); };
    static const QString qss = QStringLiteral(R"QSS(
QMainWindow, QDialog { background: %1; }
QWidget { color: %4; }
QStackedWidget, QScrollArea { background: %1; }
/* ★ 2026-10-15 修"背景变白、浅字看不清"：把每页套进 QScrollArea 之后，
   滚动区里的页面与它的 viewport 都是**裸 QWidget** —— 之前 QSS 只给
   QStackedWidget/QScrollArea 定了底色，于是**原生 Windows 样式**下这些裸 QWidget
   去用系统调色板（浅色主题 ⇒ 白底），而文字仍是我设的浅灰 ⇒ 看不见。
   这里把三条都显式定色：滚动区本身、它的 viewport、以及 viewport 里的页面容器。*/
QScrollArea { border: none; }
QScrollArea > QWidget#qt_scrollarea_viewport { background: %1; }
QWidget#dshPage { background: %1; }

/* ---- 分组框（每个信息区一个）---- */
QGroupBox {
    background: %2;
    border: 1px solid %3;
    border-radius: 6px;
    margin-top: 20px;
    padding: 8px;
}
QGroupBox::title {
    subcontrol-origin: margin;
    subcontrol-position: top left;
    left: 10px;
    padding: 0 4px;
    color: %5;
}

/* ---- 卡片（数值牌）---- */
QFrame[role="card"] {
    background: %6;
    border: 1px solid %3;
    border-radius: 6px;
}
QLabel[role="cardTitle"] { color: %5; font-size: 11px; }
QLabel[role="sub"] { color: %5; font-size: 10px; }
QLabel[role="banner"] {
    background: %2;
    border: 1px solid %3;
    border-radius: 6px;
    padding: 6px 10px;
}

/* ---- 左侧导航 ---- */
QListWidget#nav {
    background: %2;
    border: none;
    outline: none;
    padding: 6px;
}
QListWidget#nav::item {
    padding: 9px 10px;
    border-radius: 4px;
    color: %5;
}
QListWidget#nav::item:selected { background: %3; color: %4; }
QListWidget#nav::item:hover { color: %4; }

/* ---- 输入控件 ---- */
QComboBox, QLineEdit, QSpinBox, QDoubleSpinBox, QPlainTextEdit, QTextEdit, QListWidget, QTableView, QTableWidget {
    background: %1;
    color: %4;
    border: 1px solid %3;
    border-radius: 4px;
    padding: 3px 6px;
    selection-background-color: %3;
}
QComboBox QAbstractItemView { background: %2; color: %4; }
QCheckBox, QRadioButton { color: %4; }
QPushButton {
    background: %3;
    color: %4;
    border: 1px solid %3;
    border-radius: 4px;
    padding: 5px 12px;
}
QPushButton:hover { background: %6; }
QPushButton:pressed { background: %2; }
QPushButton:disabled { color: %7; }
QPushButton:checked { border: 1px solid %8; }

/* ---- 进度条 ---- */
QProgressBar {
    background: %1;
    border: 1px solid %3;
    border-radius: 4px;
    text-align: center;
    color: %4;
    min-height: 16px;
}
QProgressBar::chunk { background: %8; border-radius: 3px; }

/* ---- 表格/列表头 ---- */
QHeaderView::section { background: %2; color: %5; border: none; padding: 4px; }

/* ---- 标签页与状态条 ---- */
QTabWidget::pane { border: 1px solid %3; }
QTabBar::tab { background: %2; color: %5; padding: 6px 14px; }
QTabBar::tab:selected { color: %4; border-bottom: 2px solid %8; }
QStatusBar { background: %2; color: %5; }
QStatusBar::item { border: none; }
QMenuBar { background: %2; color: %4; }
QMenuBar::item:selected { background: %3; }
QMenu { background: %2; color: %4; border: 1px solid %3; }
QMenu::item:selected { background: %3; }
QToolTip { background: %2; color: %4; border: 1px solid %3; }
)QSS")
                                   .arg(s(kBg),      // %1
                                        s(kPanel),   // %2
                                        s(kLine),    // %3
                                        s(kText),    // %4
                                        s(kTextDim), // %5
                                        s(kPanel2),  // %6
                                        s(kOffline), // %7
                                        s(kOk));     // %8
    return qss;
}

QFont monoFont(int pointSize, bool bold)
{
    QFont f(QStringLiteral("Consolas"));
    f.setStyleHint(QFont::Monospace);
    // 注意：这里用 << 逐个追加，而不是 `QStringList{a, b, c}` ——
    // QStringLiteral 的宏展开里带有顶层逗号，放进**花括号初始化列表**会被当成多个元素
    // （报"too many initializers for QStringList"，很容易误判成 Qt 的 bug）。
    QStringList families;
    families << QStringLiteral("Consolas") << QStringLiteral("Cascadia Mono")
             << QStringLiteral("JetBrains Mono") << QStringLiteral("Courier New");
    f.setFamilies(families);
    f.setPointSize(pointSize);
    f.setBold(bold);
    return f;
}

} // namespace theme
