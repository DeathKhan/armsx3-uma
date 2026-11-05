#include "segment.hpp"
#include <iostream>
#include <stdexcept>

namespace
{
std::size_t checks = 0;
void check(bool condition, const char* message) { checks++; if (!condition) throw std::runtime_error(message); }
void set32(std::vector<cell_replay::byte>& data, std::size_t offset, std::uint32_t value)
{ for (unsigned i = 0; i < 4; i++) data[offset + i] = value >> (i * 8); }
std::vector<cell_replay::byte> fixture()
{
    std::vector<cell_replay::byte> data(cell_replay::fixed_bytes);
    std::memcpy(data.data(), "SSEG", 4);
    set32(data, 4, 2); // zero-instruction segment ending at STOP0 is valid
    return data;
}
}
int main()
{
    try
    {
        cell_replay::segment segment;
        std::string error;
        auto good = fixture();
        check(cell_replay::parse(good, segment, error), "valid minimal fixture rejected");
        const auto reject = [&](auto data, const char* message)
        { check(!cell_replay::parse(data, segment, error), message); check(!error.empty(), "missing diagnostic"); };
        for (std::size_t length : {std::size_t(0), std::size_t(31), cell_replay::fixed_bytes - 1})
        { auto bad = good; bad.resize(length); reject(bad, "truncation accepted"); }
        { auto bad = good; bad[0] = 'B'; reject(bad, "bad magic accepted"); }
        { auto bad = good; set32(bad, 4, 3); reject(bad, "future version accepted"); }
        { auto bad = good; set32(bad, 8, 1); reject(bad, "misaligned PC accepted"); }
        { auto bad = good; set32(bad, 12, cell_replay::ls_size); reject(bad, "out-of-range PC accepted"); }
        { auto bad = good; set32(bad, 20, 1); reject(bad, "unbounded instruction count accepted"); }
        { auto bad = good; set32(bad, 24, 0x40200000); reject(bad, "nonboundary endpoint accepted"); }
        { auto bad = good; set32(bad, 28, cell_replay::ls_size / 16 + 1); reject(bad, "unbounded diff count accepted"); }
        { auto bad = good; bad.push_back(0); reject(bad, "trailing byte accepted"); }
        { auto bad = good; set32(bad, 28, 1); reject(bad, "missing diff accepted"); }
        { auto bad = good; bad.resize(good.size() + 20); set32(bad, 28, 1); set32(bad, good.size(), 1); reject(bad, "misaligned diff accepted"); }
        { auto bad = good; bad.resize(good.size() + 20); set32(bad, 28, 1); set32(bad, good.size(), cell_replay::ls_size); reject(bad, "out-of-range diff accepted"); }
        { auto bad = good; bad.resize(good.size() + 40); set32(bad, 28, 2); reject(bad, "duplicate diff accepted"); }
        {
            auto changed = good;
            changed.resize(good.size() + 20);
            set32(changed, 28, 1); set32(changed, good.size(), cell_replay::ls_size - 16);
            changed[good.size() + 4] = 0x5a;
            check(cell_replay::parse(changed, segment, error), "valid final-quadword difference rejected");
            check(segment.ls_end[cell_replay::ls_size - 16] == 0x5a && segment.ls_start[cell_replay::ls_size - 16] == 0,
                  "LS reconstruction changed initial snapshot");
        }
        {
            auto changed = good;
            changed[32 + cell_replay::register_bytes] = 0x35;
            const auto fp_end = 32 + cell_replay::register_bytes + cell_replay::fpscr_bytes + cell_replay::ls_size + cell_replay::register_bytes;
            changed[fp_end] = 0x7a;
            check(cell_replay::parse(changed, segment, error), "valid FPSCR fixture rejected");
            check(segment.fpscr_start[0] == 0x35 && segment.fpscr_end[0] == 0x7a, "FPSCR snapshots omitted");
        }
        {
            auto wrong = good;
            wrong[32 + cell_replay::register_bytes + cell_replay::fpscr_bytes] = 0x40;
            reject(wrong, "inconsistent final opcode accepted");
        }
        check(cell_replay::boundary(0x1a8u << 21 | 1 << 18, 2), "v2 IRQ boundary missing");
        check(!cell_replay::boundary(0x1a8u << 21 | 1 << 18, 1), "v1 boundary semantics changed");
        std::cout << "SSEG parser: " << checks << " assertions passed\n";
        return 0;
    }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
