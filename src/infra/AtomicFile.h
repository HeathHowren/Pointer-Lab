#pragma once

#include "infra/Result.h"

#include <filesystem>
#include <functional>
#include <ios>
#include <ostream>

namespace ire::infra {

// Writes a file so that it is either wholly replaced or not touched.
//
// The contents go to a sibling temporary file first, and only once that has
// been flushed and closed successfully is it moved over `path`. Opening the
// real file with std::ios::trunc -- which is what both the settings and the
// project store used to do -- empties it before the first byte is written, so a
// crash, a full disk or a power cut mid-way left a zero-length or partial file
// and the previous contents gone. For settings that meant a silent reset to
// defaults; for a project file it meant losing the user's work.
//
// `fill` writes the contents. `mode` is or-ed into the open mode, so a caller
// that wants binary output says so the way it would with std::ofstream.
[[nodiscard]] Result<void> writeFileAtomically(const std::filesystem::path& path, std::ios::openmode mode,
                                               const std::function<void(std::ostream&)>& fill);

} // namespace ire::infra
