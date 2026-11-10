// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstdlib>
#include <cstring>
#include <optional>

#if defined(__linux__) || defined(__ANDROID__)
#include <sys/auxv.h>
#endif

#include "core/arm/nce/pauth_mte.h"

namespace Core::NCE {

namespace {

constexpr u32 Nop = 0xD503201F;
constexpr u32 Ret = 0xD65F03C0;

constexpr u32 Rd(u32 inst) {
    return inst & 0x1F;
}
constexpr u32 Rn(u32 inst) {
    return (inst >> 5) & 0x1F;
}
constexpr u32 Rm(u32 inst) {
    return (inst >> 16) & 0x1F;
}

constexpr s32 SignExtend(u32 value, u32 bits) {
    const u32 shift = 32 - bits;
    return static_cast<s32>(value << shift) >> shift;
}

/// ADD/SUB (immediate) Xd|SP, Xn|SP, #value; nothing when value is 0.
std::optional<u32> AddImm(u32 d, u32 n, s32 value) {
    const bool sub = value < 0;
    const u32 magnitude = static_cast<u32>(sub ? -value : value);
    const u32 base = sub ? 0xD1000000 : 0x91000000;
    if (magnitude <= 0xFFF) {
        return base | (magnitude << 10) | (n << 5) | d;
    }
    if ((magnitude & 0xFFF) == 0 && (magnitude >> 12) <= 0xFFF) {
        return base | (1U << 22) | ((magnitude >> 12) << 10) | (n << 5) | d;
    }
    return std::nullopt;
}

enum class IndexMode : u32 { Post = 1, Offset = 2, Pre = 3 };

/// STP (64-bit) Xt1, Xt2, [Xn|SP, #offset] in the given index mode, when the offset fits.
std::optional<u32> Stp(u32 t1, u32 t2, u32 n, s32 offset, IndexMode mode) {
    if (offset % 8 != 0 || offset < -512 || offset > 504) {
        return std::nullopt;
    }
    const u32 base = mode == IndexMode::Post ? 0xA8800000 : mode == IndexMode::Pre ? 0xA9800000 : 0xA9000000;
    const u32 imm7 = static_cast<u32>(offset / 8) & 0x7F;
    return base | (imm7 << 15) | (t2 << 10) | (n << 5) | t1;
}

/// STR or STUR Xt, [Xn|SP, #offset] without writeback, when the offset fits one of them.
std::optional<u32> StoreX(u32 t, u32 n, s32 offset) {
    if (offset >= 0 && offset % 8 == 0 && offset <= 32760) {
        return 0xF9000000 | (static_cast<u32>(offset / 8) << 10) | (n << 5) | t;
    }
    if (offset >= -256 && offset <= 255) {
        return 0xF8000000 | ((static_cast<u32>(offset) & 0x1FF) << 12) | (n << 5) | t;
    }
    return std::nullopt;
}

class Builder {
public:
    PauthMteRewrite Inline(u32 word) const {
        PauthMteRewrite rewrite{};
        rewrite.kind = PauthMteRewrite::Kind::Inline;
        rewrite.count = 1;
        rewrite.words[0] = word;
        return rewrite;
    }

    bool Push(std::optional<u32> word) {
        if (!word || count >= words.size()) {
            ok = false;
            return false;
        }
        words[count++] = *word;
        return true;
    }

    PauthMteRewrite Finish() const {
        PauthMteRewrite rewrite{};
        if (!ok) {
            rewrite.kind = PauthMteRewrite::Kind::Unhandled;
            return rewrite;
        }
        if (count == 0) {
            return Inline(Nop);
        }
        if (count == 1) {
            return Inline(words[0]);
        }
        rewrite.kind = PauthMteRewrite::Kind::Trampoline;
        rewrite.count = static_cast<u8>(count);
        rewrite.words = words;
        return rewrite;
    }

    /// Xn += offset (nothing for 0).
    void Adjust(u32 n, s32 offset) {
        if (offset != 0) {
            Push(AddImm(n, n, offset));
        }
    }

private:
    std::array<u32, 6> words{};
    size_t count{};
    bool ok{true};
};

PauthMteRewrite Unhandled() {
    PauthMteRewrite rewrite{};
    rewrite.kind = PauthMteRewrite::Kind::Unhandled;
    return rewrite;
}

std::optional<PauthMteRewrite> TranslatePauth(u32 inst) {
    Builder b;

    // RETAA, RETAB
    if (inst == 0xD65F0BFF || inst == 0xD65F0FFF) {
        return b.Inline(Ret);
    }
    // BRAAZ/BRABZ Xn -> BR Xn; BLRAAZ/BLRABZ Xn -> BLR Xn
    if ((inst & 0xFFFFF81F) == 0xD61F081F) {
        return b.Inline(0xD61F0000 | (Rn(inst) << 5));
    }
    if ((inst & 0xFFFFF81F) == 0xD63F081F) {
        return b.Inline(0xD63F0000 | (Rn(inst) << 5));
    }
    // BRAA/BRAB Xn, Xm|SP -> BR Xn; BLRAA/BLRAB -> BLR Xn
    if ((inst & 0xFFFFF800) == 0xD71F0800) {
        return b.Inline(0xD61F0000 | (Rn(inst) << 5));
    }
    if ((inst & 0xFFFFF800) == 0xD73F0800) {
        return b.Inline(0xD63F0000 | (Rn(inst) << 5));
    }
    // PACIA/PACIB/PACDA/PACDB/AUTIA/AUTIB/AUTDA/AUTDB and their Z forms: the pointer is unchanged.
    if ((inst & 0xFFFFC000) == 0xDAC10000) {
        return b.Inline(Nop);
    }
    // XPACI/XPACD: nothing to strip.
    if ((inst & 0xFFFFFBE0) == 0xDAC143E0) {
        return b.Inline(Nop);
    }
    // LDRAA/LDRAB Xt, [Xn|SP, #simm10*8]{!}
    if ((inst & 0xFF200400) == 0xF8200400) {
        const u32 t = Rd(inst);
        const u32 n = Rn(inst);
        const bool writeback = (inst >> 11) & 1;
        const u32 s_imm9 = (((inst >> 22) & 1) << 9) | ((inst >> 12) & 0x1FF);
        const s32 offset = SignExtend(s_imm9, 10) * 8;
        if (writeback) {
            if (offset >= -256 && offset <= 255) {
                return b.Inline(0xF8400C00 | ((static_cast<u32>(offset) & 0x1FF) << 12) | (n << 5) | t);
            }
            b.Adjust(n, offset);
            b.Push(0xF9400000 | (n << 5) | t);
            return b.Finish();
        }
        if (offset >= 0) {
            return b.Inline(0xF9400000 | (static_cast<u32>(offset / 8) << 10) | (n << 5) | t);
        }
        if (offset >= -256) {
            return b.Inline(0xF8400000 | ((static_cast<u32>(offset) & 0x1FF) << 12) | (n << 5) | t);
        }
        if (t == 31) {
            return b.Inline(Nop); // load into XZR
        }
        // Xt is overwritten anyway, so it can hold the address.
        b.Push(AddImm(t, n, offset));
        b.Push(0xF9400000 | (t << 5) | t);
        return b.Finish();
    }
    return std::nullopt;
}

std::optional<PauthMteRewrite> TranslatePacga(u32 inst) {
    // PACGA Xd, Xn, Xm|SP -> (Xn ^ Xm) & 0xFFFFFFFF00000000, as the JIT computes it.
    if ((inst & 0xFFE0FC00) != 0x9AC03000) {
        return std::nullopt;
    }
    Builder b;
    const u32 d = Rd(inst);
    const u32 n = Rn(inst);
    const u32 m = Rm(inst);
    if (d == 31) {
        return b.Inline(Nop);
    }
    constexpr u32 AndHigh32 = 0x92607C00; // AND Xd, Xn, #0xFFFFFFFF00000000
    if (m == 31) {
        // Modifier SP: any deterministic value works, since signing and checking both run here.
        return b.Inline(AndHigh32 | (n << 5) | d);
    }
    b.Push(0xCA000000 | (m << 16) | (n << 5) | d); // EOR Xd, Xn, Xm
    b.Push(AndHigh32 | (d << 5) | d);
    return b.Finish();
}

std::optional<PauthMteRewrite> TranslateMte(u32 inst) {
    Builder b;

    // LDG Xt, [Xn|SP, #simm9*16]: tags are 0, Xt keeps its value.
    if ((inst & 0xFFE00C00) == 0xD9600000) {
        return b.Inline(Nop);
    }
    // STG/STZG/ST2G/STZ2G Xt|SP, [Xn|SP, #simm9*16] (post-index, offset, pre-index)
    if ((inst & 0xFF200000) == 0xD9200000 && ((inst >> 10) & 3) != 0) {
        const u32 opc = (inst >> 22) & 3;
        const auto mode = static_cast<IndexMode>((inst >> 10) & 3);
        const u32 n = Rn(inst);
        const s32 offset = SignExtend((inst >> 12) & 0x1FF, 9) * 16;
        const u32 zero_bytes = opc == 1 ? 16 : opc == 3 ? 32 : 0;
        if (zero_bytes == 0) {
            if (mode == IndexMode::Offset) {
                return b.Inline(Nop);
            }
            b.Adjust(n, offset);
            return b.Finish();
        }
        const auto zero_at = [&](s32 at) {
            for (u32 done = 0; done < zero_bytes; done += 16) {
                b.Push(Stp(31, 31, n, at + static_cast<s32>(done), IndexMode::Offset));
            }
        };
        if (mode == IndexMode::Pre) {
            b.Adjust(n, offset);
            zero_at(0);
        } else if (mode == IndexMode::Post) {
            zero_at(0);
            b.Adjust(n, offset);
        } else if (Stp(31, 31, n, offset, IndexMode::Offset) &&
                   Stp(31, 31, n, offset + static_cast<s32>(zero_bytes) - 16, IndexMode::Offset)) {
            zero_at(offset);
        } else {
            b.Adjust(n, offset);
            zero_at(0);
            b.Adjust(n, -offset);
        }
        return b.Finish();
    }
    // STGP Xt1, Xt2, [Xn|SP, #simm7*16] -> STP of the data
    if (const u32 top = inst & 0xFFC00000; top == 0x68800000 || top == 0x69000000 || top == 0x69800000) {
        const auto mode = static_cast<IndexMode>((inst >> 23) & 3);
        const u32 t1 = Rd(inst);
        const u32 t2 = (inst >> 10) & 0x1F;
        const u32 n = Rn(inst);
        const s32 offset = SignExtend((inst >> 15) & 0x7F, 7) * 16;
        if (const auto stp = Stp(t1, t2, n, offset, mode)) {
            return b.Inline(*stp);
        }
        if (mode == IndexMode::Pre) {
            b.Adjust(n, offset);
            b.Push(Stp(t1, t2, n, 0, IndexMode::Offset));
        } else if (mode == IndexMode::Post) {
            b.Push(Stp(t1, t2, n, 0, IndexMode::Offset));
            b.Adjust(n, offset);
        } else if (StoreX(t1, n, offset) && StoreX(t2, n, offset + 8)) {
            b.Push(StoreX(t1, n, offset));
            b.Push(StoreX(t2, n, offset + 8));
        } else if (t1 != n && t2 != n) {
            b.Adjust(n, offset);
            b.Push(Stp(t1, t2, n, 0, IndexMode::Offset));
            b.Adjust(n, -offset);
        } else {
            return Unhandled();
        }
        return b.Finish();
    }
    // IRG Xd|SP, Xn|SP, Xm -> Xd = Xn (tag 0)
    if ((inst & 0xFFE0FC00) == 0x9AC01000) {
        return b.Inline(*AddImm(Rd(inst), Rn(inst), 0));
    }
    // GMI Xd, Xn|SP, Xm -> Xd = Xm | 1 (tag 0 of Xn)
    if ((inst & 0xFFE0FC00) == 0x9AC01400) {
        if (Rd(inst) == 31) {
            return b.Inline(Nop);
        }
        return b.Inline(0xB2400000 | (Rm(inst) << 5) | Rd(inst)); // ORR Xd, Xm, #1
    }
    // ADDG/SUBG Xd|SP, Xn|SP, #uimm6*16, #uimm4 -> ADD/SUB Xd|SP, Xn|SP, #uimm6*16
    if ((inst & 0xFFC0C000) == 0x91800000 || (inst & 0xFFC0C000) == 0xD1800000) {
        const s32 value = static_cast<s32>(((inst >> 16) & 0x3F) * 16);
        const bool sub = (inst >> 30) & 1;
        if (value == 0) {
            return b.Inline(*AddImm(Rd(inst), Rn(inst), 0));
        }
        return b.Inline(*AddImm(Rd(inst), Rn(inst), sub ? -value : value));
    }
    // SUBP/SUBPS Xd, Xn|SP, Xm|SP -> SUB/SUBS (extended register, UXTX) Xd, Xn|SP, Xm
    if ((inst & 0xFFE0FC00) == 0x9AC00000 || (inst & 0xFFE0FC00) == 0xBAC00000) {
        const bool set_flags = (inst >> 29) & 1;
        const u32 d = Rd(inst);
        if (Rm(inst) == 31) {
            return Unhandled(); // Xm is SP, which SUB (extended) cannot take as its second operand
        }
        if (!set_flags && d == 31) {
            return b.Inline(Nop); // SUBP writes XZR; SUB (extended) would write SP
        }
        const u32 base = set_flags ? 0xEB206000 : 0xCB206000;
        return b.Inline(base | (Rm(inst) << 16) | (Rn(inst) << 5) | d);
    }
    // DC GVA / DC GBVA set tags only; DC GZVA also zeroes the block, like DC ZVA.
    if ((inst & 0xFFFFFFE0) == 0xD50B7460 || (inst & 0xFFFFFFE0) == 0xD50B74A0) {
        return b.Inline(Nop);
    }
    if ((inst & 0xFFFFFFE0) == 0xD50B7480) {
        return b.Inline(0xD50B7420 | Rd(inst));
    }
    return std::nullopt;
}

bool EnvSet(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr && value[0] != '\0' && std::strcmp(value, "0") != 0;
}

} // namespace

HostArmFeatures DetectHostArmFeatures() {
    HostArmFeatures features{};
#if (defined(__linux__) || defined(__ANDROID__)) && defined(__aarch64__)
    const unsigned long hwcap = getauxval(AT_HWCAP);
    const unsigned long hwcap2 = getauxval(AT_HWCAP2);
    features.pauth_address = (hwcap & (1UL << 30)) != 0; // HWCAP_PACA
    features.pauth_generic = (hwcap & (1UL << 31)) != 0; // HWCAP_PACG
    features.mte = (hwcap2 & (1UL << 18)) != 0;          // HWCAP2_MTE
#endif
    if (EnvSet("EDEN_NCE_EMULATE_PAUTH")) {
        features.pauth_address = false;
        features.pauth_generic = false;
    }
    if (EnvSet("EDEN_NCE_EMULATE_MTE")) {
        features.mte = false;
    }
    return features;
}

PauthMteRewrite TranslatePauthMte(u32 inst, const HostArmFeatures& host) {
    if (!host.pauth_address) {
        if (auto rewrite = TranslatePauth(inst)) {
            return *rewrite;
        }
    }
    if (!host.pauth_generic) {
        if (auto rewrite = TranslatePacga(inst)) {
            return *rewrite;
        }
    }
    if (!host.mte) {
        if (auto rewrite = TranslateMte(inst)) {
            return *rewrite;
        }
    }
    return {};
}

} // namespace Core::NCE
