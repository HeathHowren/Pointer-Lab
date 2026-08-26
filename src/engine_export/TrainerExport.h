#pragma once

#include "domain/Domain.h"
#include "infra/Result.h"

#include <filesystem>
#include <string>
#include <vector>

namespace ire::engine_export {

struct TrainerOptions {
    // Becomes the project name, the window title and the file names, so it is
    // sanitised to something a compiler and a file system will both accept.
    std::string name{"Trainer"};
    std::wstring processName;
    domain::Bitness bitness{domain::Bitness::X64};
    std::vector<domain::AddressEntry> entries;
};

struct GeneratedFile {
    std::string name;
    std::string contents;
};

// Exports an address list as a trainer -- as C++ source and a CMake project,
// not as an executable.
//
// The obvious thing to generate is a finished .exe. This deliberately does not,
// for two reasons.
//
// The first is pedagogical, and it is the important one. A generated binary is
// a black box: it works, and the person holding it has learned nothing about
// why. Generated *source* is the same trainer with its reasoning visible --
// here is how a process is found by name, here is how a module base is looked
// up, here is the loop that walks a pointer chain one dereference at a time.
// The book teaches exactly this by hand; a generator that produced an opaque
// executable would be replacing that chapter rather than reinforcing it.
//
// The second is practical. Nobody should download an executable produced by
// somebody else's tool from somebody else's table and run it against their own
// machine. Source can be read first.
class TrainerExport {
public:
    // The files, in memory. Separated from writing them so the generator can be
    // tested without a temporary directory, and so the UI can preview.
    [[nodiscard]] static std::vector<GeneratedFile> generate(const TrainerOptions& options);

    // Writes them into `directory`, creating it if needed. Refuses to overwrite
    // a file that is already there: the likeliest reason for a name collision
    // is that the user is exporting over a trainer they have since edited.
    static infra::Result<std::size_t> write(const std::filesystem::path& directory,
                                            const TrainerOptions& options);

    // What `name` becomes after sanitising: letters, digits and underscores,
    // never starting with a digit. Exposed because the UI shows it.
    [[nodiscard]] static std::string identifier(const std::string& name);
};

} // namespace ire::engine_export
