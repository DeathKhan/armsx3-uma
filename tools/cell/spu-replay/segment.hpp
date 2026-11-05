// Strict reader for the archived SSEG v1/v2 capture format.
// This header has no emulator dependencies and never executes captured instructions.
#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <span>
#include <string>
#include <vector>

namespace cell_replay
{
using byte = std::uint8_t;
constexpr std::uint32_t ls_size = 0x40000;
constexpr std::size_t register_bytes = 128 * 16;
constexpr std::size_t fpscr_bytes = 16;
constexpr std::size_t fixed_bytes = 32 + 2 * (register_bytes + fpscr_bytes) + ls_size;
constexpr std::size_t max_bytes = fixed_bytes + (ls_size / 16) * 20;
constexpr std::uint64_t max_instructions = 50'000'000;

inline std::uint32_t little32(const byte* p)
{
    return std::uint32_t(p[0]) | std::uint32_t(p[1]) << 8 |
           std::uint32_t(p[2]) << 16 | std::uint32_t(p[3]) << 24;
}
inline std::uint64_t little64(const byte* p)
{
    return little32(p) | std::uint64_t(little32(p + 4)) << 32;
}
inline std::uint32_t big32(const byte* p)
{
    return std::uint32_t(p[0]) << 24 | std::uint32_t(p[1]) << 16 |
           std::uint32_t(p[2]) << 8 | std::uint32_t(p[3]);
}
inline bool boundary(std::uint32_t opcode, std::uint32_t version = 2)
{
    switch (opcode >> 21)
    {
    case 0x000: case 0x140: case 0x00d: case 0x00f:
    case 0x10d: case 0x1aa: case 0x1ab:
        return true;
    case 0x1a8: case 0x1a9: case 0x128: case 0x129: case 0x12a: case 0x12b:
        return version >= 2 && ((opcode >> 18) & 3);
    default:
        return false;
    }
}

struct segment
{
    std::uint32_t version = 0;
    std::uint32_t pc_start = 0;
    std::uint32_t pc_end = 0;
    std::uint32_t end_opcode = 0;
    std::uint64_t instructions = 0;
    // Captures store register/FPSCR bytes in RPCS3's little-host layout.
    // LS images are canonical guest-order bytes. Keep these contracts separate.
    std::array<byte, register_bytes> gpr_start{}, gpr_end{};
    std::array<byte, fpscr_bytes> fpscr_start{}, fpscr_end{};
    std::vector<byte> ls_start, ls_end;
    std::vector<std::uint32_t> changed_quadwords;
};

inline bool parse(std::span<const byte> data, segment& output, std::string& error)
{
    const auto fail = [&](const char* reason) { error = reason; return false; };
    if (data.size() < fixed_bytes || data.size() > max_bytes)
        return fail("capture size outside bounded SSEG range");
    if (std::memcmp(data.data(), "SSEG", 4)) return fail("invalid SSEG magic");
    segment parsed;
    parsed.version = little32(data.data() + 4);
    if (parsed.version != 1 && parsed.version != 2) return fail("unsupported SSEG version");
    parsed.pc_start = little32(data.data() + 8);
    parsed.pc_end = little32(data.data() + 12);
    if ((parsed.pc_start | parsed.pc_end) & 3 || parsed.pc_start >= ls_size || parsed.pc_end >= ls_size)
        return fail("PC outside aligned local store");
    parsed.instructions = little64(data.data() + 16);
    if (parsed.instructions > max_instructions) return fail("capture instruction budget exceeded");
    parsed.end_opcode = little32(data.data() + 24);
    if (!boundary(parsed.end_opcode, parsed.version)) return fail("recorded end opcode is not a segment boundary");
    const std::uint32_t count = little32(data.data() + 28);
    if (count > ls_size / 16) return fail("too many local-store differences");
    if (data.size() != fixed_bytes + std::size_t(count) * 20)
        return fail("truncated difference records or trailing bytes");
    std::size_t offset = 32;
    std::memcpy(parsed.gpr_start.data(), data.data() + offset, register_bytes); offset += register_bytes;
    std::memcpy(parsed.fpscr_start.data(), data.data() + offset, fpscr_bytes); offset += fpscr_bytes;
    parsed.ls_start.assign(data.begin() + offset, data.begin() + offset + ls_size); offset += ls_size;
    std::memcpy(parsed.gpr_end.data(), data.data() + offset, register_bytes); offset += register_bytes;
    std::memcpy(parsed.fpscr_end.data(), data.data() + offset, fpscr_bytes); offset += fpscr_bytes;
    parsed.ls_end = parsed.ls_start;
    std::array<bool, ls_size / 16> seen{};
    parsed.changed_quadwords.reserve(count);
    for (std::uint32_t i = 0; i < count; i++, offset += 20)
    {
        const std::uint32_t address = little32(data.data() + offset);
        if ((address & 15) || address > ls_size - 16) return fail("difference outside aligned local store");
        if (seen[address / 16]) return fail("duplicate local-store difference");
        seen[address / 16] = true;
        std::memcpy(parsed.ls_end.data() + address, data.data() + offset + 4, 16);
        parsed.changed_quadwords.push_back(address);
    }
    if (big32(parsed.ls_end.data() + parsed.pc_end) != parsed.end_opcode)
        return fail("end opcode does not match reconstructed final local store");
    output = std::move(parsed);
    error.clear();
    return true;
}

inline bool read(const std::string& path, segment& output, std::string& error)
{
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) { error = "cannot open capture"; return false; }
    const auto size = input.tellg();
    if (size < 0 || static_cast<std::uint64_t>(size) < fixed_bytes || static_cast<std::uint64_t>(size) > max_bytes)
    { error = "capture size outside bounded SSEG range"; return false; }
    std::vector<byte> data(static_cast<std::size_t>(size));
    input.seekg(0);
    if (!input.read(reinterpret_cast<char*>(data.data()), data.size()))
    { error = "cannot read complete capture"; return false; }
    return parse(data, output, error);
}
} // namespace cell_replay
