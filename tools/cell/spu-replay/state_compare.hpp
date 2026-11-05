#pragma once
#include "segment.hpp"

namespace cell_replay
{
struct state_view
{
    std::span<const byte> gpr;
    std::span<const byte> fpscr;
    std::span<const byte> canonical_ls;
    std::uint32_t pc;
};
struct state_comparison
{
    std::uint32_t bad_registers = 0;
    std::uint32_t bad_ls_quadwords = 0;
    bool bad_fpscr = false;
    bool bad_pc = false;
    bool bad_opcode = false;
    bool bad_shape = false;
    bool exact() const { return !bad_registers && !bad_ls_quadwords && !bad_fpscr && !bad_pc && !bad_opcode && !bad_shape; }
};
inline state_comparison compare_state(const state_view& state, const segment& capture)
{
    state_comparison result;
    if (state.gpr.size() != register_bytes || state.fpscr.size() != fpscr_bytes ||
        state.canonical_ls.size() != ls_size || capture.ls_end.size() != ls_size)
    {
        result.bad_shape = true;
        return result;
    }
    for (std::uint32_t i = 0; i < 128; i++)
        result.bad_registers += std::memcmp(state.gpr.data() + i * 16, capture.gpr_end.data() + i * 16, 16) != 0;
    // Compare every quadword, including those absent from the difference records.
    for (std::uint32_t address = 0; address < ls_size; address += 16)
        result.bad_ls_quadwords += std::memcmp(state.canonical_ls.data() + address, capture.ls_end.data() + address, 16) != 0;
    result.bad_fpscr = std::memcmp(state.fpscr.data(), capture.fpscr_end.data(), fpscr_bytes) != 0;
    result.bad_pc = state.pc != capture.pc_end;
    result.bad_opcode = state.pc >= ls_size || (state.pc & 3) || big32(state.canonical_ls.data() + state.pc) != capture.end_opcode;
    return result;
}
}
