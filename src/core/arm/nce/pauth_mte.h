// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include "common/common_types.h"

namespace Core::NCE {

/// Host CPU features that decide whether Switch 2 code can run its pointer authentication and
/// memory tagging instructions natively. Without them those instructions are UNDEFINED (SIGILL).
struct HostArmFeatures {
    bool pauth_address{}; ///< FEAT_PAuth address keys (HWCAP_PACA): RETAA/B, BRA*, LDRA*, PAC*/AUT*.
    bool pauth_generic{}; ///< FEAT_PAuth generic key (HWCAP_PACG): PACGA.
    bool mte{};           ///< FEAT_MTE2 (HWCAP2_MTE): STG/STZG/STGP/LDG/IRG/GMI/ADDG/SUBG/SUBP.
};

/// Reads the host features from the auxiliary vector. EDEN_NCE_EMULATE_PAUTH=1 and
/// EDEN_NCE_EMULATE_MTE=1 force the rewrites on hosts that have the features (for testing).
HostArmFeatures DetectHostArmFeatures();

/// Replacement for one guest instruction. With the features missing these are treated like the
/// JIT does (dynarmic): pointer authentication is the identity, PACGA is (Xn ^ Xm) & 0xFFFFFFFF00000000,
/// allocation tags are always 0 and tag stores are no-ops (STZG/STGP still write their data).
struct PauthMteRewrite {
    enum class Kind : u8 {
        None,       ///< Not a pointer authentication or memory tagging instruction, or runs natively.
        Inline,     ///< Replace the word in place with words[0].
        Trampoline, ///< Branch to the patch section, run words[0..count), return to the next word.
        Unhandled,  ///< Needs a rewrite this translator has no sequence for (logged; left as is).
    };
    Kind kind{Kind::None};
    u8 count{};
    std::array<u32, 6> words{};
};

PauthMteRewrite TranslatePauthMte(u32 inst, const HostArmFeatures& host);

} // namespace Core::NCE
