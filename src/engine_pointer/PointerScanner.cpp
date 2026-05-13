#include "engine_pointer/PointerScanner.h"

#include "infra/Logger.h"

#include <algorithm>
#include <cstring>
#include <unordered_set>

namespace ire::engine_pointer {

namespace {

constexpr std::size_t pointerChunkSize = 1024 * 1024;

bool contains(const domain::ModuleInfo& module, std::uintptr_t address) {
    return address >= module.base && address < module.base + module.size;
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

    const auto modules = session_.modules();
    const auto regions = session_.regions();
    const auto total = eligibleBytes(regions) * std::max<std::uint32_t>(1, options.maxDepth);
    const std::size_t ptrSize = sizeof(void*);
    std::uint64_t visited{};
    std::vector<Candidate> frontier = {{options.target, {}}};

    for (std::uint32_t depth = 0; depth < options.maxDepth && !cancel_; ++depth) {
        std::vector<Candidate> next;
        std::unordered_set<std::uintptr_t> targets;
        for (const auto& item : frontier) {
            targets.insert(item.address);
        }

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

                    for (const auto& source : frontier) {
                        if (pointerValue <= source.address && source.address - pointerValue <= options.maxOffset) {
                            Candidate found;
                            found.address = region.base + offset + i;
                            found.offsets = source.offsets;
                            found.offsets.insert(found.offsets.begin(), static_cast<std::ptrdiff_t>(source.address - pointerValue));

                            for (const auto& module : modules) {
                                if (contains(module, found.address)) {
                                    domain::PointerChain chain;
                                    chain.moduleName = module.name;
                                    chain.moduleBase = module.base;
                                    chain.baseAddress = found.address;
                                    chain.offsets = found.offsets;
                                    std::scoped_lock lock(mutex_);
                                    if (results_.size() < options.maxResults) {
                                        results_.push_back(chain);
                                        status_ = "Pointer scan: " + std::to_string(results_.size()) + " chains";
                                    }
                                }
                            }

                            if (next.size() < options.maxResults) {
                                next.push_back(std::move(found));
                            }
                        }
                    }
                }
            }
        }

        frontier = std::move(next);
        if (frontier.empty()) {
            break;
        }
    }

    {
        std::scoped_lock lock(mutex_);
        status_ = cancel_ ? "Pointer scan cancelled" : "Pointer scan complete";
    }
    running_ = false;
}

} // namespace ire::engine_pointer

