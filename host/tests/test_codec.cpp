#include <QtTest>

#include "codec.h"
#include "crc16.h"

using namespace proto;

/// 协议编解码单元测试。
/// 校验用例取自 docs/protocol/communication_protocol.md 第 2.1 节。
class TestCodec : public QObject
{
    Q_OBJECT

private slots:
    // ---- CRC-16/MODBUS ----
    void crc_standardVector()
    {
        const char *s = "123456789";
        QCOMPARE(crc16Modbus(reinterpret_cast<const uint8_t *>(s), 9),
                 static_cast<uint16_t>(0x4B37));
    }

    void crc_documentedCases()
    {
        const uint8_t a[] = {0x10};
        const uint8_t b[] = {0x10, 0x0C};
        const uint8_t c[] = {0x10, 0x0C, 0x00};
        QCOMPARE(crc16Modbus(a, 1), static_cast<uint16_t>(0x8CBE));
        QCOMPARE(crc16Modbus(b, 2), static_cast<uint16_t>(0x750C));
        QCOMPARE(crc16Modbus(c, 3), static_cast<uint16_t>(0x0575));
    }

    // ---- 组帧 ----
    void encode_headerAndLength()
    {
        std::vector<uint8_t> data{0x01, 0x02, 0x03};
        auto f = encodeFrame(up::kTask, data);
        QCOMPARE(f.size(), static_cast<size_t>(kOverhead + 3));
        QCOMPARE(f[0], kHeader0);
        QCOMPARE(f[1], kHeader1);
        QCOMPARE(f[2], up::kTask);
        QCOMPARE(getU16(&f[3]), static_cast<uint16_t>(3)); // 小端长度
    }

    void encode_zeroLength()
    {
        auto f = encodeFrame(up::kHeartbeat, {});
        QCOMPARE(f.size(), static_cast<size_t>(kOverhead));
        QCOMPARE(getU16(&f[3]), static_cast<uint16_t>(0));
    }

    // ---- 往返 ----
    void roundTrip_singleFrame()
    {
        // 构造一帧 0x14 环境与气体（N=16）
        std::vector<uint8_t> d;
        putU16(d, 1234);   // MQ-2 原始
        putU16(d, 2345);   // MQ-135 原始
        putU16(d, 567);    // MQ-2 ppm*10
        putU16(d, 890);    // MQ-135 ppm*10
        putI16(d, 256);    // 25.6 C
        putU16(d, 610);    // 61.0 %RH
        d.push_back(0x0F); // 就绪位
        d.push_back(1);    // 档位
        d.push_back(0);
        d.push_back(0);
        QCOMPARE(d.size(), static_cast<size_t>(16));

        auto bytes = encodeFrame(up::kEnvGas, d);
        FrameDecoder dec;
        auto frames = dec.feed(bytes);

        QCOMPARE(frames.size(), static_cast<size_t>(1));
        QCOMPARE(frames[0].cmd, up::kEnvGas);
        QCOMPARE(frames[0].data, d);
        QCOMPARE(dec.framesOk(), static_cast<uint64_t>(1));
        QCOMPARE(dec.framesDropped(), static_cast<uint64_t>(0));
    }

    // ---- 流式：帧被切成任意片段 ----
    void stream_splitAcrossReads()
    {
        auto bytes = encodeFrame(up::kPose, std::vector<uint8_t>(12, 0x11));
        FrameDecoder dec;
        std::vector<Frame> all;

        // 逐字节喂入，模拟最恶劣的分片
        for (uint8_t b : bytes) {
            auto got = dec.feed(&b, 1);
            for (auto &f : got) {
                all.push_back(std::move(f));
            }
        }
        QCOMPARE(all.size(), static_cast<size_t>(1));
        QCOMPARE(all[0].cmd, up::kPose);
        QCOMPARE(all[0].data.size(), static_cast<size_t>(12));
    }

    void stream_twoFramesInOneChunk()
    {
        auto a = encodeFrame(up::kHeartbeat, {});
        auto b = encodeFrame(down::kEstop, {});
        std::vector<uint8_t> buf = a;
        buf.insert(buf.end(), b.begin(), b.end());

        FrameDecoder dec;
        auto frames = dec.feed(buf);
        QCOMPARE(frames.size(), static_cast<size_t>(2));
        QCOMPARE(frames[0].cmd, up::kHeartbeat);
        QCOMPARE(frames[1].cmd, down::kEstop);
    }

    // ---- 容错：坏校验应丢弃 ----
    void corrupt_crcIsDropped()
    {
        auto bytes = encodeFrame(up::kTask, std::vector<uint8_t>(5, 0x22));
        bytes.back() ^= 0xFF; // 破坏 CRC 高字节

        FrameDecoder dec;
        auto frames = dec.feed(bytes);
        QCOMPARE(frames.size(), static_cast<size_t>(0));
        QCOMPARE(dec.framesDropped(), static_cast<uint64_t>(1));
    }

    // ---- 容错：帧前有垃圾字节，应能重新同步 ----
    void resync_afterGarbage()
    {
        std::vector<uint8_t> buf{0x00, 0xFF, 0xAA, 0x13, 0x77, 0xAA}; // 伪帧头 + 垃圾
        auto good = encodeFrame(up::kEvent, std::vector<uint8_t>{0x06, 1, 2, 3, 4});
        buf.insert(buf.end(), good.begin(), good.end());

        FrameDecoder dec;
        auto frames = dec.feed(buf);
        QCOMPARE(frames.size(), static_cast<size_t>(1));
        QCOMPARE(frames[0].cmd, up::kEvent);
        QCOMPARE(frames[0].data[0], static_cast<uint8_t>(0x06)); // 超阈值事件
    }

    // ---- 容错：异常长度应整帧作废，不越界 ----
    void oversizeLength_isRejected()
    {
        std::vector<uint8_t> buf{kHeader0, kHeader1, 0x14, 0xFF, 0x7F}; // 长度 32767
        FrameDecoder dec;
        auto frames = dec.feed(buf);
        QCOMPARE(frames.size(), static_cast<size_t>(0));
        QCOMPARE(dec.framesDropped(), static_cast<uint64_t>(1));
    }

    // ---- 复位 ----
    void reset_clearsPartialState()
    {
        auto bytes = encodeFrame(up::kPose, std::vector<uint8_t>(12, 0x33));
        FrameDecoder dec;
        dec.feed(bytes.data(), 5); // 只喂一半
        dec.reset();
        auto frames = dec.feed(bytes.data() + 5, bytes.size() - 5);
        QCOMPARE(frames.size(), static_cast<size_t>(0)); // 残帧已清，不应误判
    }
};

QTEST_APPLESS_MAIN(TestCodec)
#include "test_codec.moc"
