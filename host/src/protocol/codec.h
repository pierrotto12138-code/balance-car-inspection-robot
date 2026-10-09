#pragma once

#include <cstdint>
#include <vector>

#include "frame.h"

namespace proto {

/// 一帧解析后的结果。
struct Frame {
    uint8_t cmd = 0;
    std::vector<uint8_t> data;
};

/// 组帧：命令字 + 数据段 -> 完整字节流（含帧头与 CRC）。
std::vector<uint8_t> encodeFrame(uint8_t cmd, const std::vector<uint8_t> &data);

/// 流式解帧器。
///
/// 串口/蓝牙收到的是连续字节流，帧边界不保证与 read() 边界对齐，
/// 因此必须用状态机逐字节推进。校验失败时丢弃整帧并从下一个 0xAA 重新同步。
class FrameDecoder
{
public:
    /// 喂入一段字节，返回本次解析出的所有完整且校验通过的帧。
    std::vector<Frame> feed(const uint8_t *data, std::size_t len);
    std::vector<Frame> feed(const std::vector<uint8_t> &data);

    /// 复位状态机（例如链路重连后调用）。
    void reset();

    /// 统计信息，便于界面显示链路质量。
    uint64_t framesOk() const { return framesOk_; }
    uint64_t framesDropped() const { return framesDropped_; }
    void resetCounters() { framesOk_ = 0; framesDropped_ = 0; }

private:
    enum class State { Header0, Header1, Cmd, LenLo, LenHi, Data, CrcLo, CrcHi };

    State state_ = State::Header0;
    uint8_t cmd_ = 0;
    uint16_t need_ = 0;          // 数据段期望长度
    std::vector<uint8_t> buf_;   // 数据段 + 已收 CRC
    uint64_t framesOk_ = 0;
    uint64_t framesDropped_ = 0;
};

// ---- 小端读写辅助（协议统一小端）----
inline void putU16(std::vector<uint8_t> &out, uint16_t v)
{
    out.push_back(static_cast<uint8_t>(v & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
}
inline void putI16(std::vector<uint8_t> &out, int16_t v)
{
    putU16(out, static_cast<uint16_t>(v));
}
inline void putU32(std::vector<uint8_t> &out, uint32_t v)
{
    out.push_back(static_cast<uint8_t>(v & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
}
inline uint16_t getU16(const uint8_t *p)
{
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}
inline int16_t getI16(const uint8_t *p)
{
    return static_cast<int16_t>(getU16(p));
}
inline uint32_t getU32(const uint8_t *p)
{
    return static_cast<uint32_t>(p[0] | (p[1] << 8) | (p[2] << 16) |
                                 (static_cast<uint32_t>(p[3]) << 24));
}

} // namespace proto
