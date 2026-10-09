#include "codec.h"

#include "crc16.h"

namespace proto {

std::vector<uint8_t> encodeFrame(uint8_t cmd, const std::vector<uint8_t> &data)
{
    std::vector<uint8_t> out;
    out.reserve(kOverhead + data.size());
    out.push_back(kHeader0);
    out.push_back(kHeader1);
    out.push_back(cmd);
    putU16(out, static_cast<uint16_t>(data.size()));
    out.insert(out.end(), data.begin(), data.end());

    // CRC 覆盖范围：命令字 -> 数据段末尾（偏移 2 起）
    const uint16_t crc = crc16Modbus(out.data() + kHeaderLen, out.size() - kHeaderLen);
    putU16(out, crc);
    return out;
}

std::vector<Frame> FrameDecoder::feed(const std::vector<uint8_t> &data)
{
    return feed(data.data(), data.size());
}

void FrameDecoder::reset()
{
    state_ = State::Header0;
    cmd_ = 0;
    need_ = 0;
    buf_.clear();
}

std::vector<Frame> FrameDecoder::feed(const uint8_t *data, std::size_t len)
{
    std::vector<Frame> frames;
    for (std::size_t i = 0; i < len; ++i) {
        const uint8_t b = data[i];
        switch (state_) {
        case State::Header0:
            if (b == kHeader0) {
                state_ = State::Header1;
            }
            break;

        case State::Header1:
            if (b == kHeader1) {
                state_ = State::Cmd;
            } else if (b == kHeader0) {
                // 连续 0xAA：保持在等待第二字节，兼容 AA AA 55 的情形
                state_ = State::Header1;
            } else {
                state_ = State::Header0;
            }
            break;

        case State::Cmd:
            cmd_ = b;
            state_ = State::LenLo;
            break;

        case State::LenLo:
            need_ = b;
            state_ = State::LenHi;
            break;

        case State::LenHi:
            need_ = static_cast<uint16_t>(need_ | (b << 8));
            buf_.clear();
            if (need_ > kMaxDataLen) {
                // 长度异常：整帧作废，重新同步
                ++framesDropped_;
                state_ = State::Header0;
            } else if (need_ == 0) {
                state_ = State::CrcLo; // 无数据段（心跳 / 急停）
            } else {
                state_ = State::Data;
            }
            break;

        case State::Data:
            buf_.push_back(b);
            if (buf_.size() == need_) {
                state_ = State::CrcLo;
            }
            break;

        case State::CrcLo:
            if (buf_.size() == need_) {
                buf_.push_back(b);
            } else {
                buf_[need_] = b; // 数据段之后即 CRC 低字节
            }
            state_ = State::CrcHi;
            break;

        case State::CrcHi: {
            // buf_ = [数据段 N 字节][CRC 低][CRC 高]
            uint16_t got = static_cast<uint16_t>(buf_[need_] | (b << 8));

            // 重算：命令字 + 长度(2) + 数据段
            std::vector<uint8_t> calc;
            calc.reserve(3 + need_);
            calc.push_back(cmd_);
            calc.push_back(static_cast<uint8_t>(need_ & 0xFF));
            calc.push_back(static_cast<uint8_t>((need_ >> 8) & 0xFF));
            calc.insert(calc.end(), buf_.begin(), buf_.begin() + need_);
            const uint16_t want = crc16Modbus(calc.data(), calc.size());

            if (got == want) {
                Frame f;
                f.cmd = cmd_;
                f.data.assign(buf_.begin(), buf_.begin() + need_);
                frames.push_back(std::move(f));
                ++framesOk_;
                state_ = State::Header0;
            } else {
                ++framesDropped_;
                // 该字节本身可能是下一帧的帧头，避免漏帧
                state_ = (b == kHeader0) ? State::Header1 : State::Header0;
            }
            buf_.clear();
            break;
        }
        }
    }
    return frames;
}

} // namespace proto
