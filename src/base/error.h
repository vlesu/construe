// Errors and Result (spec/base.md §5).
#pragma once

#include <cassert>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace construe {

using ParamValue = std::variant<std::string, double, std::vector<std::string>>;

struct Error {
    std::string code;
    std::map<std::string, ParamValue> params;
    std::optional<std::string> path;  // JSON Pointer (RFC 6901) — only for errors in a file

    Error() = default;
    explicit Error(std::string c) : code(std::move(c)) {}

    Error& with(const std::string& key, ParamValue value) {
        params[key] = std::move(value);
        return *this;
    }
    Error& at(std::string jsonPointer) {
        path = std::move(jsonPointer);
        return *this;
    }

    // Convenient access for tests and messages.
    const std::string* stringParam(const std::string& key) const {
        auto it = params.find(key);
        if (it == params.end()) return nullptr;
        return std::get_if<std::string>(&it->second);
    }
    std::optional<double> numberParam(const std::string& key) const {
        auto it = params.find(key);
        if (it == params.end()) return std::nullopt;
        if (auto* d = std::get_if<double>(&it->second)) return *d;
        return std::nullopt;
    }
};

// Result: a value or an error. Exceptions do not cross the public API of modules.
template <typename T>
class Result {
public:
    Result(T value) : data_(std::move(value)) {}
    Result(Error error) : data_(std::move(error)) {}

    bool ok() const { return std::holds_alternative<T>(data_); }
    explicit operator bool() const { return ok(); }

    T& value() {
        assert(ok());
        return std::get<T>(data_);
    }
    const T& value() const {
        assert(ok());
        return std::get<T>(data_);
    }
    T& operator*() { return value(); }
    const T& operator*() const { return value(); }
    T* operator->() { return &value(); }
    const T* operator->() const { return &value(); }

    const Error& error() const {
        assert(!ok());
        return std::get<Error>(data_);
    }

private:
    std::variant<T, Error> data_;
};

struct Unit {};
using Status = Result<Unit>;
inline Status okStatus() { return Unit{}; }

}  // namespace construe
