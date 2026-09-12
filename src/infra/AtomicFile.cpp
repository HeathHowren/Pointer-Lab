#include "infra/AtomicFile.h"

#include <Windows.h>

#include <fstream>
#include <string>
#include <system_error>

namespace ire::infra {

Result<void> writeFileAtomically(const std::filesystem::path& path, std::ios::openmode mode,
                                 const std::function<void(std::ostream&)>& fill) {
    std::error_code ignored;
    std::filesystem::create_directories(path.parent_path(), ignored);

    // Beside the target rather than in the temp directory, because a move
    // across volumes is a copy followed by a delete, and a copy is exactly the
    // non-atomic write this exists to avoid.
    const std::filesystem::path temporary = path.native() + L".tmp";
    {
        std::ofstream out(temporary, mode | std::ios::trunc);
        if (!out) {
            return Result<void>::fail("Could not create " + temporary.string() + " for writing.");
        }
        fill(out);
        out.flush();
        if (!out) {
            out.close();
            std::filesystem::remove(temporary, ignored);
            return Result<void>::fail("Could not write " + temporary.string() +
                                      " (the disk may be full or read-only).");
        }
    }

    // MoveFileEx rather than std::filesystem::rename: rename() is implemented
    // over the same call, but this spells out the two flags that matter.
    // REPLACE_EXISTING is the swap; WRITE_THROUGH makes the call return only
    // once the rename has reached the disk, so a power cut a moment later does
    // not undo it.
    if (MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
        const auto error = GetLastError();
        std::filesystem::remove(temporary, ignored);
        return Result<void>::fail("Could not replace " + path.string() + " (Windows error " +
                                      std::to_string(error) + ").",
                                  static_cast<ErrorCode>(error));
    }
    return Result<void>::ok();
}

} // namespace ire::infra
