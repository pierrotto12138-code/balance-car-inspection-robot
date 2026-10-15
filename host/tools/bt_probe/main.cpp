// 蓝牙 RFCOMM 连通性探针（独立小程序，不属于上位机主工程）
//
// 为什么单独写它：Windows 上的蓝牙串口有两条路——
//   ① SPP COM 口：要靠系统"配对+建口"，本项目实测**能配对却建不起口**（Windows 蓝牙栈抽风）；
//   ② RFCOMM 直连：按模块 MAC 直连通道，**绕开 COM 口这一层**。
// 而 Python 做不到 ②（CPython 的 AF_BLUETOOTH 只有 Linux/Android 实现，Windows 会报
// "bad bluetooth address"/WinError 10022）—— 所以用 **Qt 的 QBluetoothSocket** 探（Qt 有 Windows 后端）。
//
// 用法：bt_probe.exe [地址]      地址默认 2A:A2:19:07:29:F4（JDY-33 实测 AT+LADDR 读出）
// 判据：连上 + 3 秒内读到 AA 55 帧头 ⇒ 蓝牙这条链路可用，值得给上位机实装 BluetoothLink。

#include <QCoreApplication>
#include <QBluetoothAddress>
#include <QBluetoothLocalDevice>
#include <QBluetoothServiceInfo>
#include <QBluetoothSocket>
#include <QBluetoothUuid>
#include <QDebug>
#include <QElapsedTimer>

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    const QString addr = (argc > 1) ? QString::fromLatin1(argv[1])
                                    : QStringLiteral("2A:A2:19:07:29:F4");

    QBluetoothLocalDevice local;
    qInfo() << "本机蓝牙适配器:" << local.name() << "状态:" << local.hostMode();
    qInfo() << "已连接设备:" << local.connectedDevices();

    QBluetoothSocket sock(QBluetoothServiceInfo::RfcommProtocol);
    qInfo() << "尝试 RFCOMM 连接" << addr << "（SPP 服务 UUID）…";

    sock.connectToService(QBluetoothAddress(addr),
                          QBluetoothUuid(QBluetoothUuid::ServiceClassUuid::SerialPort));

    if (!sock.waitForConnected(15000)) {
        qWarning() << "❌ 连接失败:" << sock.errorString();
        return 2;
    }
    qInfo() << "✅ 连接成功，读 3 秒（看有没有板子发来的 AA 55 帧）…";

    QByteArray buf;
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < 3000) {
        if (sock.waitForReadyRead(500)) {
            buf += sock.readAll();
        }
    }

    int heads = 0;
    for (int i = 0; i + 1 < buf.size(); ++i) {
        if (static_cast<quint8>(buf.at(i)) == 0xAA &&
            static_cast<quint8>(buf.at(i + 1)) == 0x55) {
            ++heads;
        }
    }
    qInfo() << "收到" << buf.size() << "字节；AA55 帧头" << heads << "个";
    sock.disconnectFromService();

    if (heads > 0) {
        qInfo() << "结论：RFCOMM 可用 ⇒ 值得给上位机实装 BluetoothLink";
        return 0;
    }
    qInfo() << "结论：能连上但没收到帧（模块可能与板子未连通，或板子没在发）";
    return 1;
}
