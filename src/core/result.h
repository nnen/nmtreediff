#pragma once

// A minimal stand-in for std::expected, which is C++23 while this project
// targets C++20. The shape is deliberately the same, so moving to
// std::expected later is a mechanical substitution rather than a redesign.

#include <optional>
#include <type_traits>
#include <utility>
#include <variant>

namespace nmxd {

// Wrapper used to disambiguate constructing a failed Result, in the same role
// as std::unexpected.
template <class E>
struct Failure {
    E value;
};

template <class E>
[[nodiscard]] Failure<std::decay_t<E>> fail(E&& error) {
    return Failure<std::decay_t<E>>{std::forward<E>(error)};
}

template <class T, class E>
class Result {
public:
    using value_type = T;
    using error_type = E;

    Result(T value) : data_(std::in_place_index<0>, std::move(value)) {}
    Result(Failure<E> failure) : data_(std::in_place_index<1>, std::move(failure.value)) {}

    [[nodiscard]] bool ok() const noexcept { return data_.index() == 0; }
    explicit operator bool() const noexcept { return ok(); }

    [[nodiscard]] T& value() & { return std::get<0>(data_); }
    [[nodiscard]] const T& value() const& { return std::get<0>(data_); }
    [[nodiscard]] T&& value() && { return std::get<0>(std::move(data_)); }

    [[nodiscard]] const E& error() const& { return std::get<1>(data_); }
    [[nodiscard]] E&& error() && { return std::get<1>(std::move(data_)); }

    template <class U>
    [[nodiscard]] T valueOr(U&& fallback) const& {
        return ok() ? value() : static_cast<T>(std::forward<U>(fallback));
    }

private:
    std::variant<T, E> data_;
};

// Specialisation for operations that either succeed with no value or fail.
template <class E>
class Result<void, E> {
public:
    using value_type = void;
    using error_type = E;

    Result() = default;
    Result(Failure<E> failure) : error_(std::in_place, std::move(failure.value)) {}

    [[nodiscard]] bool ok() const noexcept { return !error_.has_value(); }
    explicit operator bool() const noexcept { return ok(); }

    [[nodiscard]] const E& error() const& { return *error_; }
    [[nodiscard]] E&& error() && { return std::move(*error_); }

private:
    std::optional<E> error_;
};

}  // namespace nmxd
