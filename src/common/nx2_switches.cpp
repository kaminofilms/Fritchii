// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstdlib>
#include <fstream>
#include <map>
#include <string>
#include <utility>

#include "common/nx2_switches.h"

namespace Common::Nx2Switches {

namespace {

// Built into the Frizti builds so players don't have to set anything: the switches tuned for
// Ocarina of Time. Either nx2_switches.txt overrides any of them (=0 turns one off).
// Not EDEN_NX2_WRITTEN_SSBO_MARK_KIB=64 (built in from v4 to v5.5): writes past its cap were not
// tracked, one of the gaps behind the huge glowing triangles (the other: native compute write
// extents, see BufferCache::ExtentTrust). Castle Town: 43.4/45.0 fps with it, 44.6/43.0 without;
// the triangles still showed without it alone (2026-11-09).
constexpr std::pair<const char*, const char*> BuiltinSwitches[] = {
    {"EDEN_NX2_GRAPHICS_SSBO_BARE", "0"},
    {"EDEN_NX2_ACQUIRE_PRECHECK", "2"},   {"EDEN_NX2_MACRO_SINK", "1"},
    {"EDEN_NX2_COMPUTE_SINK", "1"},       {"EDEN_NX2_RING_PAIRS", "1"},
    {"EDEN_NX2_UBO_INPLACE", "1"},        {"EDEN_NX2_PIPELINE_CLEAN", "1"},
    {"EDEN_NX2_LIST_WAIT_SKIP", "1"},     {"EDEN_NX2_PREFLUSH", "1"},
    {"EDEN_NX2_SEM_FAST", "2"},
};

void ParseSwitchFile(const std::filesystem::path& path,
                     std::map<std::string, std::string>& entries) {
    std::ifstream file{path};
    if (!file) {
        return;
    }
    constexpr const char* blank = " \t\r";
    std::string line;
    while (std::getline(file, line)) {
        line.erase(0, line.find_first_not_of(blank));
        line.erase(line.find_last_not_of(blank) + 1);
        if (line.empty() || line[0] == '#') {
            continue;
        }
        const auto eq = line.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        std::string name = line.substr(0, eq);
        std::string value = line.substr(eq + 1);
        name.erase(name.find_last_not_of(blank) + 1);
        value.erase(0, value.find_first_not_of(blank));
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
            value = value.substr(1, value.size() - 2);
        }
        if (!name.empty()) {
            entries.insert_or_assign(std::move(name), std::move(value));
        }
    }
}

} // Anonymous namespace

void Apply(std::span<const std::filesystem::path> files) {
    // Later files overwrite earlier entries; the real environment is checked only at the end.
    std::map<std::string, std::string> entries;
    for (const auto& [name, value] : BuiltinSwitches) {
        entries.emplace(name, value);
    }
    for (const auto& file : files) {
        ParseSwitchFile(file, entries);
    }
    for (const auto& [name, value] : entries) {
        if (std::getenv(name.c_str()) != nullptr) {
            continue;
        }
#ifdef _WIN32
        _putenv_s(name.c_str(), value.c_str());
#else
        setenv(name.c_str(), value.c_str(), 1);
#endif
    }
}

} // namespace Common::Nx2Switches
