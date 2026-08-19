#include "engine_pointer/PointerScanner.h"

#include "infra/Logger.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <cwctype>
#include <numeric>
#include <unordered_set>

namespace ire::engine_pointer {

namespace {

constexpr std::size_t pointerChunkSize = 1024 * 1024;

bool contains(const domain::ModuleInfo& module, std::uintptr_t address) {
    return address >= module.base && address < module.base + module.size;
}

// Windows module names are case-insensitive, and the case a project file was
// saved with need not match what the loader reports on the next run.
bool sameName(const std::wstring& left, const std::wstring& right) {
    return left.size() == right.size() &&
           std::equal(left.begin(), left.end(), right.begin(),
                      [](wchar_t a, wchar_t b) { return std::towlower(a) == std::towlower(b); });
}

std::uint64_t eligibleBytes(const std::vector<domain::MemoryRegion>& regions) {
    std::uint64_t total{};
    for (const auto& region : regions) {
        if (region.readable) {
            total += region.size;
        }
    }
    return total == 0 ? 1 : total;
}

} // namespace

PointerScanJob::PointerScanJob(domain::TargetSession& session) : session_(session) {}

PointerScanJob::~PointerScanJob() {
    cancel();
}

void PointerScanJob::start(PointerScanOptions options) {
    cancel();
    cancel_ = false;
    running_ = true;
    fraction_ = 0.0;
    {
        std::scoped_lock lock(mutex_);
        results_.clear();
        status_ = "Starting pointer scan";
    }
    worker_ = std::jthread([this, options] { run(options); });
}

void PointerScanJob::cancel() {
    cancel_ = true;
    if (worker_.joinable()) {
        worker_.join();
    }
}

PointerScanProgress PointerScanJob::progress() const {
    std::scoped_lock lock(mutex_);
    return {running_, fraction_.load(), results_.size(), status_};
}

std::vector<domain::PointerChain> PointerScanJob::results() const {
    std::scoped_lock lock(mutex_);
    return results_;
}

void PointerScanJob::run(PointerScanOptions options) {
    struct Candidate {
        std::uintptr_t address{};
        std::vector<std::ptrdiff_t> offsets;
    };

    const auto started = std::chrono::steady_clock::now();
    const auto modules = session_.modules();
    const auto regions = session_.regions();
    const auto total = eligibleBytes(regions) * std::max<std::uint32_t>(1, options.maxDepth);
    constexpr std::size_t ptrSize = sizeof(void*);
    std::uint64_t visited{};

    infra::Logger::instance().info(
        "Pointer scan: target=" + domain::toHex(options.target) + " depth=" + std::to_string(options.maxDepth) +
        " maxOffset=" + domain::toHex(options.maxOffset) + " modules=" + std::to_string(modules.size()) +
        " regions=" + std::to_string(regions.size()));

    std::vector<Candidate> frontier = {{options.target, {}}};

    // Every address ever placed in a frontier. Without it the scan revisits the
    // same addresses at every depth, and a structure that points back at itself
    // makes the frontier grow without bound.
    std::unordered_set<std::uintptr_t> seen;
    seen.insert(options.target);

    bool truncated = false;

    for (std::uint32_t depth = 0; depth < options.maxDepth && !cancel_; ++depth) {
        // Sorted target addresses, with each one's chain alongside. A pointer
        // value P matches any target in [P, P + maxOffset], which is a short
        // range scan from a binary search. The previous code compared every
        // pointer in the address space against every frontier entry in turn,
        // which is why anything past depth 1 never finished.
        std::vector<std::uintptr_t> targets;
        targets.reserve(frontier.size());
        for (const auto& item : frontier) {
            targets.push_back(item.address);
        }
        std::vector<std::size_t> order(frontier.size());
        std::iota(order.begin(), order.end(), std::size_t{0});
        std::sort(order.begin(), order.end(),
                  [&targets](std::size_t a, std::size_t b) { return targets[a] < targets[b]; });
        std::vector<std::uintptr_t> sortedTargets;
        sortedTargets.reserve(order.size());
        for (const auto index : order) {
            sortedTargets.push_back(targets[index]);
        }

        std::vector<Candidate> next;

        for (const auto& region : regions) {
            if (cancel_ || !region.readable) {
                continue;
            }
            for (std::size_t offset = 0; offset < region.size && !cancel_; offset += pointerChunkSize) {
                const std::size_t readSize = std::min(pointerChunkSize, region.size - offset);
                auto bytes = session_.readBytes(region.base + offset, readSize);
                visited += readSize;
                fraction_ = std::min(1.0, static_cast<double>(visited) / static_cast<double>(total));
                if (!bytes || bytes.value().size() < ptrSize) {
                    continue;
                }

                const auto& buffer = bytes.value();
                for (std::size_t i = 0; i + ptrSize <= buffer.size() && !cancel_; i += ptrSize) {
                    std::uintptr_t pointerValue{};
                    std::memcpy(&pointerValue, buffer.data() + i, ptrSize);
                    if (pointerValue == 0) {
                        continue;
                    }

                    auto match = std::lower_bound(sortedTargets.begin(), sortedTargets.end(), pointerValue);
                    for (; match != sortedTargets.end() && *match - pointerValue <= options.maxOffset; ++match) {
                        const auto& source = frontier[order[static_cast<std::size_t>(match - sortedTargets.begin())]];
                        const auto found = region.base + offset + i;
                        if (!seen.insert(found).second) {
                            continue;
                        }

                        Candidate candidate;
                        candidate.address = found;
                        candidate.offsets = source.offsets;
                        candidate.offsets.insert(candidate.offsets.begin(),
                                                 static_cast<std::ptrdiff_t>(source.address - pointerValue));

                        // A chain is only useful if its base is a fixed point in
                        // a module; anything else moves on the next run.
                        for (const auto& module : modules) {
                            if (!contains(module, found)) {
                                continue;
                            }
                            domain::PointerChain chain;
                            chain.moduleName = module.name;
                            chain.moduleBase = module.base;
                            chain.moduleOffset = found - module.base;
                            chain.offsets = candidate.offsets;

                            std::scoped_lock lock(mutex_);
                            if (results_.size() < options.maxResults) {
                                results_.push_back(std::move(chain));
                                status_ = "Pointer scan: " + std::to_string(results_.size()) + " chains";
                            } else {
                                truncated = true;
                            }
                            // One module owns any given address.
                            break;
                        }

                        if (next.size() < options.maxFrontier) {
                            next.push_back(std::move(candidate));
                        } else {
                            truncated = true;
                        }
                    }
                }
            }
        }

        frontier = std::move(next);
        infra::Logger::instance().trace("Pointer scan depth " + std::to_string(depth + 1) + ": frontier=" +
                                        std::to_string(frontier.size()));
        if (frontier.empty()) {
            break;
        }
    }

    std::size_t chains{};
    {
        std::scoped_lock lock(mutex_);
        chains = results_.size();
        if (cancel_) {
            status_ = "Pointer scan cancelled";
        } else if (truncated) {
            status_ = "Stopped at the " + std::to_string(options.maxResults) +
                      " chain limit; lower the depth or the maximum offset";
        } else {
            status_ = "Pointer scan complete: " + std::to_string(results_.size()) + " chain(s)";
        }
    }

    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
    infra::Logger::instance().info("Pointer scan finished: " + std::to_string(chains) + " chain(s) in " +
                                   std::to_string(elapsed) + " ms" + (cancel_ ? " (cancelled)" : "") +
                                   (truncated ? " (truncated)" : ""));
    running_ = false;
}

infra::Result<std::uintptr_t> resolveChain(domain::TargetSession& session, const domain::PointerChain& chain) {
    using Address = infra::Result<std::uintptr_t>;

    if (!session.attached()) {
        return Address::fail("No target process is attached.");
    }
    if (!chain.valid()) {
        return Address::fail("The pointer chain is incomplete.");
    }

    // Look the module up by name every time. After a restart it is almost
    // certainly somewhere else, which is exactly what the chain exists to
    // survive.
    std::uintptr_t moduleBase{};
    for (const auto& module : session.modules()) {
        if (sameName(module.name, chain.moduleName)) {
            moduleBase = module.base;
            break;
        }
    }
    if (moduleBase == 0) {
        return Address::fail(domain::narrow(chain.moduleName) + " is not loaded in the target.");
    }

    std::uintptr_t address = moduleBase + chain.moduleOffset;
    for (const auto offset : chain.offsets) {
        auto bytes = session.readBytes(address, sizeof(std::uintptr_t));
        if (!bytes || bytes.value().size() != sizeof(std::uintptr_t)) {
            return Address::fail("The chain broke: nothing readable at " + domain::toHex(address) + ".");
        }
        std::uintptr_t pointer{};
        std::memcpy(&pointer, bytes.value().data(), sizeof(pointer));
        if (pointer == 0) {
            return Address::fail("The chain broke: a null pointer at " + domain::toHex(address) + ".");
        }
        address = pointer + static_cast<std::uintptr_t>(offset);
    }
    return Address::ok(address);
}

} // namespace ire::engine_pointer

