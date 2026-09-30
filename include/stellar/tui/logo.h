// SPDX-License-Identifier: MIT
// The canonical STELLAR ASCII banner.
//
// This is project branding, transcribed verbatim from the design specification
// (TODO.md). Do not regenerate, reflow, re-encode or "tidy" these lines: the
// exact characters and spacing ARE the mark, and every screen that shows the
// banner must show it unchanged.
//
// The strings are emitted as explicit escaped char arrays rather than a raw
// string literal so that no future edit can trip over a delimiter or silently
// change the trailing whitespace.
#pragma once

#include <array>
#include <cstddef>
#include <string>
#include <string_view>

#include "stellar/tui/theme.h"

namespace stellar::tui {

/// Number of rows in the banner.
inline constexpr std::size_t kLogoRows = 6;

/// Display width of the widest banner row, in terminal columns.
inline constexpr std::size_t kLogoWidth = 57;

inline constexpr std::array<std::string_view, kLogoRows> kLogoLines = {{
    "███████╗████████╗███████╗██╗     ██╗      █████╗ ██████╗",
    "██╔════╝╚══██╔══╝██╔════╝██║     ██║     ██╔══██╗██╔══██╗",
    "███████╗   ██║   █████╗  ██║     ██║     ███████║██████╔╝",
    "╚════██║   ██║   ██╔══╝  ██║     ██║     ██╔══██║██╔══██╗",
    "███████║   ██║   ███████╗███████╗███████╗██║  ██║██║  ██║",
    "╚══════╝   ╚═╝   ╚══════╝╚══════╝╚══════╝╚═╝  ╚═╝╚═╝  ╚═╝",
}};

/// True when the banner is present and matches the canonical artwork. Used by
/// the tests to prove the artwork was not corrupted.
[[nodiscard]] bool logo_is_canonical();

/// The banner as a single string: the letter mass in light blue and the
/// box-drawing structure in Stellar blue, both bold. With colour unavailable
/// the rows are returned unchanged, with no escape sequences at all.
[[nodiscard]] std::string render_logo(const Theme& theme);

}  // namespace stellar::tui
