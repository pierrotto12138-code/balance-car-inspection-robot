// 协议交叉验证：用 **C 侧真实代码**（host/src/protocol/crc16.cpp + codec.cpp）
// 复算 tools/fw_prep/gen_test_vectors.py 生成的 test_vectors.tsv。
//
// 为什么要这么做：AGENTS 铁律 23 —— 别用同一套假设验自己。
//   Python 一份独立实现  +  C++ 一份真实实现  +  协议文档一份  三者一致，协议才可冻结。
//
// 编译（任意 C++17 编译器：g++ / clang++ / MinGW 均可）：
//   g++ -std=c++17 -I host/src/protocol tools/fw_prep/host_crosscheck.cpp
//       host/src/protocol/crc16.cpp host/src/protocol/codec.cpp -o build/host_crosscheck.exe
//
// 用法：
//   host_crosscheck.exe docs/protocol/test_vectors.tsv
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "codec.h"
#include "crc16.h"

static std::string toHex(const std::vector<uint8_t> &v)
{
    static const char *d = "0123456789abcdef";
    std::string s;
    s.reserve(v.size() * 2);
    for (uint8_t b : v) {
        s.push_back(d[b >> 4]);
        s.push_back(d[b & 0x0F]);
    }
    return s;
}

static std::vector<uint8_t> fromHex(const std::string &s)
{
    std::vector<uint8_t> v;
    for (std::size_t i = 0; i + 1 < s.size() + 1 && i + 1 < s.size() + 1; i += 2) {
        if (i + 1 >= s.size()) break;
        v.push_back(static_cast<uint8_t>(std::stoul(s.substr(i, 2), nullptr, 16)));
    }
    return v;
}

int main(int argc, char **argv)
{
    const std::string path = argc > 1 ? argv[1] : "docs/protocol/test_vectors.tsv";
    std::ifstream in(path);
    if (!in) {
        std::printf("打不开 %s\n", path.c_str());
        return 2;
    }

    int nFrame = 0, badFrame = 0, nCrc = 0, badCrc = 0;
    int nStream = 0, badStream = 0;
    std::string line;

    // 把 "aa55,aa55;aa55" 拆成 ["aa55","aa55"] 的多次 read
    auto splitStr = [](const std::string &s, char sep) {
        std::vector<std::string> out;
        std::string cur;
        for (char ch : s) {
            if (ch == sep) { out.push_back(cur); cur.clear(); }
            else cur.push_back(ch);
        }
        out.push_back(cur);
        return out;
    };

    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::vector<std::string> col;
        std::stringstream ss(line);
        std::string cell;
        while (std::getline(ss, cell, '\t')) col.push_back(cell);
        if (col.size() < 3) continue;

        // 只处理两种行，其余（例如 STREAM 流式用例、含中文名字段）一律跳过 ——
        // 免得向量文件以后加新分节时 stoul 抛异常把整个程序带走。
        auto isHex = [](const std::string &s) {
            if (s.empty()) return true;
            return s.find_first_not_of("0123456789abcdefABCDEF") == std::string::npos;
        };

        if (col[0] == "STREAM") {
            // STREAM\tname\tchunks(read内逗号, read间分号)\texpect_cmds
            // 注意：expect 为空时行尾是空的第 4 列，istream 的 getline 会把它丢掉，
            //       所以这里只要求 col.size() >= 3，缺的第 4 列按空串处理。
            if (col.size() < 3) continue;
            const std::string expectCol = (col.size() > 3) ? col[3] : std::string();
            proto::FrameDecoder dec;
            std::vector<uint8_t> gotCmds;
            for (const auto &readGrp : splitStr(col[2], ';')) {
                for (const auto &chunk : splitStr(readGrp, ',')) {
                    const auto bytes = fromHex(chunk);
                    if (bytes.empty()) continue;
                    for (const auto &f : dec.feed(bytes)) gotCmds.push_back(f.cmd);
                }
            }
            std::vector<uint8_t> wantCmds;
            for (const auto &c : splitStr(expectCol, ',')) {
                if (!c.empty()) {
                    wantCmds.push_back(static_cast<uint8_t>(std::stoul(c, nullptr, 16)));
                }
            }
            ++nStream;
            if (gotCmds != wantCmds) {
                ++badStream;
                std::printf("  [流式不符] %s：期望 %zu 帧，实得 %zu 帧\n",
                            col[1].c_str(), wantCmds.size(), gotCmds.size());
            } else if (wantCmds.empty() && dec.framesDropped() == 0) {
                ++badStream;
                std::printf("  [流式不符] %s：应有丢弃计数\n", col[1].c_str());
            }
            continue;
        }

        if (col[0] == "CRC") {                       // CRC\tinput_hex\texpected_crc
            if (!isHex(col[1]) || !isHex(col[2])) continue;
            const auto data = fromHex(col[1]);
            const uint16_t want = static_cast<uint16_t>(std::stoul(col[2], nullptr, 16));
            const uint16_t got = proto::crc16Modbus(data.data(), data.size());
            ++nCrc;
            if (got != want) {
                ++badCrc;
                std::printf("  [CRC 不符] %-14s 期望 %04x 实得 %04x\n",
                            col[1].c_str(), want, got);
            }
        } else {                                     // name\tcmd_hex\tdata_hex\tframe_hex
            if (col.size() < 4) continue;
            if (!isHex(col[1]) || !isHex(col[2]) || !isHex(col[3])) continue;
            const uint8_t cmd = static_cast<uint8_t>(std::stoul(col[1], nullptr, 16));
            const auto data = fromHex(col[2]);
            const std::string want = col[3];
            const auto got = toHex(proto::encodeFrame(cmd, data));
            ++nFrame;
            if (got != want) {
                ++badFrame;
                std::printf("  [帧不符] %-16s\n     期望 %s\n     实得 %s\n",
                            col[0].c_str(), want.c_str(), got.c_str());
            } else {
                // 反向：解出来必须还原成同一 cmd + data
                proto::FrameDecoder dec;
                const auto bytes = proto::encodeFrame(cmd, data);
                const auto frames = dec.feed(bytes);
                if (frames.size() != 1 || frames[0].cmd != cmd || frames[0].data != data) {
                    ++badFrame;
                    std::printf("  [往返失败] %s\n", col[0].c_str());
                }
            }
        }
    }

    std::printf("C 侧实现复算：CRC %d 条（不符 %d）、帧 %d 条（不符 %d）、流式 %d 条（不符 %d）\n",
                nCrc, badCrc, nFrame, badFrame, nStream, badStream);
    const bool allOk = (badCrc == 0 && badFrame == 0 && badStream == 0);
    std::printf("%s\n", allOk
                            ? "结论：C 侧实现与 Python 独立向量**完全一致** ✅"
                            : "结论：存在不一致 ❗");
    return allOk ? 0 : 1;
}
