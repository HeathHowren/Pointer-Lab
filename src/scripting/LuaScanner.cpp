#include "scripting/LuaScanner.h"

#include "infra/Logger.h"

#include <lua.hpp>

#include <algorithm>
#include <cstring>
#include <memory>

namespace ire::scripting {

namespace {

constexpr std::size_t chunkSize = 1024 * 1024;

struct LuaStateDeleter {
    void operator()(lua_State* state) const {
        if (state) {
            lua_close(state);
        }
    }
};

template <typename T>
T unpack(const std::vector<std::uint8_t>& bytes) {
    T value{};
    if (bytes.size() >= sizeof(T)) {
        std::memcpy(&value, bytes.data(), sizeof(T));
    }
    return value;
}

bool eligibleRegion(const domain::MemoryRegion& region, const LuaScanOptions& options) {
    if (!region.readable || region.size == 0) {
        return false;
    }
    if (options.writableOnly && !region.writable) {
        return false;
    }
    if (options.executableOnly && !region.executable) {
        return false;
    }
    return true;
}

std::uint64_t totalBytes(const std::vector<domain::MemoryRegion>& regions, const LuaScanOptions& options) {
    std::uint64_t total{};
    for (const auto& region : regions) {
        if (eligibleRegion(region, options)) {
            total += region.size;
        }
    }
    return total == 0 ? 1 : total;
}

void pushValue(lua_State* state, domain::ValueType type, const std::vector<std::uint8_t>& bytes) {
    switch (type) {
    case domain::ValueType::Int8:
        lua_pushinteger(state, unpack<std::int8_t>(bytes));
        break;
    case domain::ValueType::UInt8:
        lua_pushinteger(state, unpack<std::uint8_t>(bytes));
        break;
    case domain::ValueType::Int16:
        lua_pushinteger(state, unpack<std::int16_t>(bytes));
        break;
    case domain::ValueType::UInt16:
        lua_pushinteger(state, unpack<std::uint16_t>(bytes));
        break;
    case domain::ValueType::Int32:
        lua_pushinteger(state, unpack<std::int32_t>(bytes));
        break;
    case domain::ValueType::UInt32:
        lua_pushinteger(state, unpack<std::uint32_t>(bytes));
        break;
    case domain::ValueType::Int64:
        lua_pushinteger(state, unpack<std::int64_t>(bytes));
        break;
    case domain::ValueType::UInt64:
        lua_pushnumber(state, static_cast<lua_Number>(unpack<std::uint64_t>(bytes)));
        break;
    case domain::ValueType::Float:
        lua_pushnumber(state, unpack<float>(bytes));
        break;
    case domain::ValueType::Double:
        lua_pushnumber(state, unpack<double>(bytes));
        break;
    case domain::ValueType::Bytes:
        lua_pushstring(state, domain::bytesToHex(bytes).c_str());
        break;
    }
}

void setIntegerField(lua_State* state, const char* name, std::uint64_t value) {
    lua_pushinteger(state, static_cast<lua_Integer>(value));
    lua_setfield(state, -2, name);
}

void setStringField(lua_State* state, const char* name, const std::string& value) {
    lua_pushstring(state, value.c_str());
    lua_setfield(state, -2, name);
}

void pushContext(lua_State* state, domain::ValueType type, std::uintptr_t address, const domain::MemoryRegion& region, const std::vector<std::uint8_t>& bytes) {
    lua_newtable(state);
    setIntegerField(state, "address", address);
    setIntegerField(state, "region_base", region.base);
    setIntegerField(state, "region_size", region.size);
    setStringField(state, "type", domain::valueTypeName(type));
    setStringField(state, "bytes", domain::bytesToHex(bytes));
    setStringField(state, "hex", domain::bytesToHex(bytes, false));
    pushValue(state, type, bytes);
    lua_setfield(state, -2, "value");
}

} // namespace

LuaScanJob::LuaScanJob(domain::TargetSession& session) : session_(session) {}

LuaScanJob::~LuaScanJob() {
    cancel();
}

void LuaScanJob::start(LuaScanOptions options) {
    cancel();
    cancel_ = false;
    running_ = true;
    fraction_ = 0.0;
    valueType_ = options.type;
    {
        std::scoped_lock lock(mutex_);
        results_.clear();
        error_.clear();
        status_ = "Starting Lua scan";
    }
    worker_ = std::jthread([this, options = std::move(options)]() mutable { run(std::move(options)); });
}

void LuaScanJob::cancel() {
    cancel_ = true;
    if (worker_.joinable()) {
        worker_.join();
    }
}

LuaScanProgress LuaScanJob::progress() const {
    std::scoped_lock lock(mutex_);
    return {running_, fraction_.load(), results_.size(), status_, error_};
}

std::vector<domain::ScanResult> LuaScanJob::results() const {
    std::scoped_lock lock(mutex_);
    return results_;
}

void LuaScanJob::run(LuaScanOptions options) {
    const auto valueSize = domain::valueTypeSize(options.type);
    if (valueSize == 0) {
        std::scoped_lock lock(mutex_);
        error_ = "Lua Scanner currently requires a fixed-size primitive type.";
        status_ = "Lua scan failed";
        running_ = false;
        return;
    }

    std::unique_ptr<lua_State, LuaStateDeleter> state(luaL_newstate());
    luaL_openlibs(state.get());
    const std::string wrapped = "return (function()\n" + options.script + "\nend)()";
    if (luaL_loadstring(state.get(), wrapped.c_str()) != LUA_OK || lua_pcall(state.get(), 0, 1, 0) != LUA_OK) {
        std::scoped_lock lock(mutex_);
        error_ = lua_tostring(state.get(), -1) ? lua_tostring(state.get(), -1) : "Could not compile Lua predicate.";
        status_ = "Lua scan failed";
        running_ = false;
        return;
    }
    if (!lua_isfunction(state.get(), -1)) {
        std::scoped_lock lock(mutex_);
        error_ = "Lua script must return function(ctx) ... end.";
        status_ = "Lua scan failed";
        running_ = false;
        return;
    }
    const int predicateRef = luaL_ref(state.get(), LUA_REGISTRYINDEX);

    const auto regions = session_.regions();
    const auto total = totalBytes(regions, options);
    const auto stride = options.stride == 0 ? valueSize : std::max<std::size_t>(1, options.stride);
    std::uint64_t visited{};
    std::vector<domain::ScanResult> batch;
    batch.reserve(2048);

    for (const auto& region : regions) {
        if (cancel_) {
            break;
        }
        if (!eligibleRegion(region, options)) {
            continue;
        }

        for (std::size_t offset = 0; offset < region.size && !cancel_; offset += chunkSize) {
            const std::size_t readSize = std::min(chunkSize + valueSize, region.size - offset);
            auto bytes = session_.readBytes(region.base + offset, readSize);
            visited += readSize;
            fraction_ = std::min(1.0, static_cast<double>(visited) / static_cast<double>(total));
            if (!bytes || bytes.value().size() < valueSize) {
                continue;
            }

            const auto& buffer = bytes.value();
            for (std::size_t i = 0; i + valueSize <= buffer.size() && !cancel_; i += stride) {
                std::vector<std::uint8_t> current(buffer.begin() + static_cast<std::ptrdiff_t>(i), buffer.begin() + static_cast<std::ptrdiff_t>(i + valueSize));

                lua_rawgeti(state.get(), LUA_REGISTRYINDEX, predicateRef);
                pushContext(state.get(), options.type, region.base + offset + i, region, current);
                if (lua_pcall(state.get(), 1, 1, 0) != LUA_OK) {
                    std::scoped_lock lock(mutex_);
                    error_ = lua_tostring(state.get(), -1) ? lua_tostring(state.get(), -1) : "Lua predicate failed.";
                    status_ = "Lua scan failed";
                    running_ = false;
                    return;
                }
                const bool matched = lua_toboolean(state.get(), -1) != 0;
                lua_pop(state.get(), 1);

                if (matched) {
                    batch.push_back({region.base + offset + i, current, current});
                    if (batch.size() >= 2048) {
                        std::scoped_lock lock(mutex_);
                        results_.insert(results_.end(), batch.begin(), batch.end());
                        if (results_.size() > options.maxResults) {
                            results_.resize(options.maxResults);
                            cancel_ = true;
                        }
                        status_ = "Lua scanning: " + std::to_string(results_.size()) + " matches";
                        batch.clear();
                    }
                }
            }
        }
    }

    {
        std::scoped_lock lock(mutex_);
        if (!batch.empty()) {
            results_.insert(results_.end(), batch.begin(), batch.end());
            if (results_.size() > options.maxResults) {
                results_.resize(options.maxResults);
            }
        }
        status_ = cancel_ ? "Lua scan cancelled or capped" : "Lua scan complete";
    }
    luaL_unref(state.get(), LUA_REGISTRYINDEX, predicateRef);
    running_ = false;
    infra::Logger::instance().info("Lua scan finished.");
}

} // namespace ire::scripting

