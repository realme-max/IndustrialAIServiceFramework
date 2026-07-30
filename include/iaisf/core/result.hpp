#pragma once

#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>

#include "iaisf/core/error.hpp"

namespace iaisf {

/**
 * Holds either a value or an Error.
 *
 * Calling value() on a failed result, or error() on a successful result,
 * throws std::logic_error. Those exceptions indicate programmer API misuse;
 * expected runtime failures remain ordinary Result values.
 */
template <typename T>
class Result {
    static_assert(!std::is_void_v<T>, "Use Result<void> for operations without a value");
    static_assert(!std::is_reference_v<T>, "Result<T> cannot store reference types");

public:
    [[nodiscard]] static Result success(T value) {
        return Result{ValueStorage{std::move(value)}};
    }

    [[nodiscard]] static Result failure(Error error) {
        return Result{ErrorStorage{normalize_error(std::move(error))}};
    }

    [[nodiscard]] bool has_value() const noexcept {
        return std::holds_alternative<ValueStorage>(storage_);
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return has_value();
    }

    [[nodiscard]] T& value() & {
        ensure_has_value();
        return std::get<ValueStorage>(storage_).value;
    }

    [[nodiscard]] const T& value() const& {
        ensure_has_value();
        return std::get<ValueStorage>(storage_).value;
    }

    [[nodiscard]] T&& value() && {
        ensure_has_value();
        return std::move(std::get<ValueStorage>(storage_).value);
    }

    [[nodiscard]] Error& error() & {
        ensure_has_error();
        return std::get<ErrorStorage>(storage_).error;
    }

    [[nodiscard]] const Error& error() const& {
        ensure_has_error();
        return std::get<ErrorStorage>(storage_).error;
    }

    [[nodiscard]] Error&& error() && {
        ensure_has_error();
        return std::move(std::get<ErrorStorage>(storage_).error);
    }

private:
    struct ValueStorage {
        T value;
    };

    struct ErrorStorage {
        Error error;
    };

    using Storage = std::variant<ValueStorage, ErrorStorage>;

    explicit Result(ValueStorage value) : storage_(std::move(value)) {}
    explicit Result(ErrorStorage error) : storage_(std::move(error)) {}

    [[nodiscard]] static Error normalize_error(Error error) {
        if (error.message.empty()) {
            error.message = "unspecified error";
        }
        return error;
    }

    void ensure_has_value() const {
        if (!has_value()) {
            throw std::logic_error{"Result does not contain a value"};
        }
    }

    void ensure_has_error() const {
        if (has_value()) {
            throw std::logic_error{"Result does not contain an error"};
        }
    }

    Storage storage_;
};

/**
 * Result specialization for operations that do not return a value.
 *
 * API misuse follows the same std::logic_error convention as Result<T>.
 */
template <>
class Result<void> {
public:
    [[nodiscard]] static Result success() {
        return Result{std::monostate{}};
    }

    [[nodiscard]] static Result failure(Error error) {
        return Result{normalize_error(std::move(error))};
    }

    [[nodiscard]] bool has_value() const noexcept {
        return std::holds_alternative<std::monostate>(storage_);
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return has_value();
    }

    void value() const {
        if (!has_value()) {
            throw std::logic_error{"Result does not contain a value"};
        }
    }

    [[nodiscard]] Error& error() & {
        ensure_has_error();
        return std::get<Error>(storage_);
    }

    [[nodiscard]] const Error& error() const& {
        ensure_has_error();
        return std::get<Error>(storage_);
    }

    [[nodiscard]] Error&& error() && {
        ensure_has_error();
        return std::move(std::get<Error>(storage_));
    }

private:
    using Storage = std::variant<std::monostate, Error>;

    explicit Result(std::monostate value) : storage_(value) {}
    explicit Result(Error error) : storage_(std::move(error)) {}

    [[nodiscard]] static Error normalize_error(Error error) {
        if (error.message.empty()) {
            error.message = "unspecified error";
        }
        return error;
    }

    void ensure_has_error() const {
        if (has_value()) {
            throw std::logic_error{"Result does not contain an error"};
        }
    }

    Storage storage_;
};

}  // namespace iaisf

