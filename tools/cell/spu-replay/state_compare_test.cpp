#include "state_compare.hpp"
#include <iostream>
#include <stdexcept>

int main()
{
    std::size_t assertions = 0;
    const auto check = [&](bool condition) { assertions++; if (!condition) throw std::runtime_error("state comparison contract failed"); };
    try
    {
        cell_replay::segment expected;
        expected.ls_end.resize(cell_replay::ls_size);
        auto registers = expected.gpr_end;
        auto fpscr = expected.fpscr_end;
        auto ls = expected.ls_end;
        cell_replay::state_view state{registers, fpscr, ls, 0};
        check(cell_replay::compare_state(state, expected).exact());
        registers.back() = 1;
        check(cell_replay::compare_state(state, expected).bad_registers == 1);
        check(!cell_replay::compare_state(state, expected).exact());
        registers.back() = 0;
        fpscr.back() = 1;
        check(cell_replay::compare_state(state, expected).bad_fpscr);
        check(!cell_replay::compare_state(state, expected).exact());
        fpscr.back() = 0;
        ls.back() = 1; // Not listed in any diff record: full-LS comparison must see it.
        check(cell_replay::compare_state(state, expected).bad_ls_quadwords == 1);
        check(!cell_replay::compare_state(state, expected).exact());
        ls.back() = 0;
        state.pc = 4;
        check(cell_replay::compare_state(state, expected).bad_pc);
        check(!cell_replay::compare_state(state, expected).bad_opcode);
        state.pc = cell_replay::ls_size;
        check(cell_replay::compare_state(state, expected).bad_opcode);
        state.pc = 1;
        check(cell_replay::compare_state(state, expected).bad_opcode);
        state.pc = 0;
        ls[0] = 1;
        check(cell_replay::compare_state(state, expected).bad_opcode);
        ls[0] = 0;
        state.gpr = state.gpr.first(cell_replay::register_bytes - 1);
        check(cell_replay::compare_state(state, expected).bad_shape);
        check(!cell_replay::compare_state(state, expected).exact());
        state.gpr = registers;
        state.fpscr = state.fpscr.first(cell_replay::fpscr_bytes - 1);
        check(cell_replay::compare_state(state, expected).bad_shape);
        state.fpscr = fpscr;
        state.canonical_ls = state.canonical_ls.first(cell_replay::ls_size - 1);
        check(cell_replay::compare_state(state, expected).bad_shape);
        state.canonical_ls = ls;
        expected.ls_end.clear();
        check(cell_replay::compare_state(state, expected).bad_shape);
        std::cout << "SPU state comparator: " << assertions << " assertions passed\n";
        return 0;
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
