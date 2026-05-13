#pragma once

#include <string>
#include <utility>

namespace ire::infra {

template <typename T>
class Result {
public:
    static Result ok(T value) { return Result(true, std::move(value), {}); }
    static Result fail(std::string message) { return Result(false, T{}, std::move(message)); }

    [[nodiscard]] bool has_value() const { return ok_; }
    [[nodiscard]] explicit operator bool() const { return ok_; }
    [[nodiscard]] T& value() { return value_; }
    [[nodiscard]] const T& value() const { return value_; }
    [[nodiscard]] const std::string& error() const { return error_; }

private:
    Result(bool ok, T value, std::string error) : ok_(ok), value_(std::move(value)), error_(std::move(error)) {}

    bool ok_;
    T value_;
    std::string error_;
};

template <>
class Result<void> {
public:
    static Result ok() { return Result(true, {}); }
    static Result fail(std::string message) { return Result(false, std::move(message)); }

    [[nodiscard]] bool has_value() const { return ok_; }
    [[nodiscard]] explicit operator bool() const { return ok_; }
    [[nodiscard]] const std::string& error() const { return error_; }

private:
    Result(bool ok, std::string error) : ok_(ok), error_(std::move(error)) {}

    bool ok_;
    std::string error_;
};

} // namespace ire::infra

