#include "segment.hpp"
#include <algorithm>
#include <filesystem>
#include <iostream>
#include <map>

int main(int argc, char** argv)
{
    if (argc != 2) { std::cerr << "usage: segment_audit <capture-directory>\n"; return 2; }
    std::vector<std::filesystem::path> paths;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(argv[1], ec))
    {
        const auto name = entry.path().filename().string();
        if (entry.is_regular_file() && name.starts_with("seg-") && name.ends_with(".bin")) paths.push_back(entry.path());
    }
    if (ec) { std::cerr << "cannot enumerate capture directory\n"; return 2; }
    std::sort(paths.begin(), paths.end());
    std::map<std::uint32_t, std::size_t> versions, boundaries;
    std::size_t failed = 0, fp_changes = 0, ls_changes = 0;
    std::uint64_t total_insns = 0, max_insns = 0;
    for (const auto& path : paths)
    {
        cell_replay::segment segment;
        std::string error;
        if (!cell_replay::read(path.string(), segment, error))
        {
            failed++;
            std::cerr << path.filename().string() << ": " << error << '\n';
            continue;
        }
        versions[segment.version]++;
        boundaries[segment.end_opcode >> 21]++;
        fp_changes += segment.fpscr_start != segment.fpscr_end;
        ls_changes += !segment.changed_quadwords.empty();
        total_insns += segment.instructions;
        max_insns = std::max(max_insns, segment.instructions);
    }
    std::cout << "captures=" << paths.size() << " valid=" << paths.size() - failed << " invalid=" << failed
              << " fpscr_changed=" << fp_changes << " ls_changed=" << ls_changes
              << " total_instructions=" << total_insns << " max_instructions=" << max_insns << '\n';
    for (const auto& [version, count] : versions) std::cout << "version=" << version << " count=" << count << '\n';
    for (const auto& [opcode, count] : boundaries) std::cout << "boundary_major=" << std::hex << opcode << std::dec << " count=" << count << '\n';
    return failed || paths.empty() ? 1 : 0;
}
