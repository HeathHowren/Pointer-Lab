#pragma once

#include <cassert>
#include <string>
#include <utility>

namespace ire::infra {

// Win32 error code carried alongside the message so callers can tell an
// actionable failure (ERROR_ACCESS_DENIED - needs elevation) from an expected
// one (ERROR_PARTIAL_COPY - unmapped page during a scan). 0 means "no code".
using ErrorCode = unsigned long;

// [[nodiscard]] is on the class, not just the accessors: the whole point is
// that ignoring a Result is a bug, and several call sites used to drop write
// failures on the floor silently.
template <typename T>
class [[nodiscard]] Result {
public:
    static Result ok(T value) { return Result(true, std::move(value), {}, 0); }
    static Result fail(std::string message, ErrorCode code = 0) { return Result(false, T{}, std::move(message), code); }

    [[nodiscard]] bool has_value() const { return ok_; }
    [[nodiscard]] explicit operator bool() const { return ok_; }

    [[nodiscard]] T& value() {
        assert(ok_ && "Result::value() on a failed Result");
        return value_;
    }
    [[nodiscard]] const T& value() const {
        assert(ok_ && "Result::value() on a failed Result");
        return value_;
    }

    [[nodiscard]] const std::string& error() const { return error_; }
    [[nodiscard]] ErrorCode code() const { return code_; }

private:
    Result(bool ok, T value, std::string error, ErrorCode code)
        : ok_(ok), value_(std::move(value)), error_(std::move(error)), code_(code) {}

    bool ok_;
    T value_;
    std::string error_;
    ErrorCode code_{};
};

template <>
class [[nodiscard]] Result<void> {
public:
    static Result ok() { return Result(true, {}, 0); }
    static Result fail(std::string message, ErrorCode code = 0) { return Result(false, std::move(message), code); }

    [[nodiscard]] bool has_value() const { return ok_; }
    [[nodiscard]] explicit operator bool() const { return ok_; }
    [[nodiscard]] const std::string& error() const { return error_; }
    [[nodiscard]] ErrorCode code() const { return code_; }

private:
    Result(bool ok, std::string error, ErrorCode code) : ok_(ok), error_(std::move(error)), code_(code) {}

    bool ok_;
    std::string error_;
    ErrorCode code_{};
};

} // namespace ire::infra
