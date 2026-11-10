// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <filesystem>
#include <span>

namespace Common::Nx2Switches {

/// The NX2 switches are read from the environment (EDEN_NX2_*), which a GUI launch (or an Android
/// app) has no way to set. Puts Fritchii's built-in tuned switches, then each nx2_switches.txt in
/// `files` ("NAME=VALUE" lines, '#' comments; a later file wins), into the environment. A
/// variable already in the real environment always wins. Call before the emulator reads any of
/// them: they are read on first use.
void Apply(std::span<const std::filesystem::path> files);

} // namespace Common::Nx2Switches
