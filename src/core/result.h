#pragma once

/// \file
/// \brief A minimal stand-in for `std::expected`, which is C++23 while this
///        project targets C++20.

#include <optional>
#include <type_traits>
#include <utility>
#include <variant>

namespace nmtreediff {

/// \brief Wraps an error value so that a failed Result can be constructed
///        without ambiguity.
///
/// \remarks Fills the same role as `std::unexpected`. Prefer the fail() helper
///          over naming this type directly.
template <class E>
struct Failure {
    /// \brief The error being reported.
    E value;
};

/// \brief Builds a Failure from an error value, deducing its type.
///
/// \param error The error to report.
///
/// \returns A Failure that converts to any Result whose error type matches.
template <class E>
[[nodiscard]] Failure<std::decay_t<E>> fail(E&& error) {
    return Failure<std::decay_t<E>>{std::forward<E>(error)};
}

/// \brief Holds either a value or an error, never both and never neither.
///
/// \tparam T The value type produced on success.
/// \tparam E The error type produced on failure.
///
/// \remarks The shape is deliberately the same as `std::expected`, so moving to
///          it once the project adopts C++23 is a mechanical substitution
///          rather than a redesign. The value and the error may be the same
///          type: the underlying variant is addressed by index, not by type.
template <class T, class E>
class Result {
public:
    /// \brief The value type produced on success.
    using value_type = T;
    /// \brief The error type produced on failure.
    using error_type = E;

    /// \brief Constructs a successful result.
    ///
    /// \param value The value to hold.
    Result(T value) : data_(std::in_place_index<0>, std::move(value)) {}

    /// \brief Constructs a failed result.
    ///
    /// \param failure The error to hold, usually produced by fail().
    Result(Failure<E> failure) : data_(std::in_place_index<1>, std::move(failure.value)) {}

    /// \brief Reports whether the operation succeeded.
    ///
    /// \returns `true` when a value is held, `false` when an error is held.
    [[nodiscard]] bool ok() const noexcept { return data_.index() == 0; }

    /// \brief Reports whether the operation succeeded.
    ///
    /// \returns `true` when a value is held, `false` when an error is held.
    explicit operator bool() const noexcept { return ok(); }

    /// \brief Accesses the held value.
    ///
    /// \returns A reference to the value.
    ///
    /// \remarks Undefined unless ok() is `true`.
    [[nodiscard]] T& value() & { return std::get<0>(data_); }

    /// \copydoc value()
    [[nodiscard]] const T& value() const& { return std::get<0>(data_); }

    /// \brief Moves the held value out of the result.
    ///
    /// \returns The value, moved.
    ///
    /// \remarks Undefined unless ok() is `true`.
    [[nodiscard]] T&& value() && { return std::get<0>(std::move(data_)); }

    /// \brief Accesses the held error.
    ///
    /// \returns A reference to the error.
    ///
    /// \remarks Undefined unless ok() is `false`.
    [[nodiscard]] const E& error() const& { return std::get<1>(data_); }

    /// \brief Moves the held error out of the result.
    ///
    /// \returns The error, moved.
    ///
    /// \remarks Undefined unless ok() is `false`.
    [[nodiscard]] E&& error() && { return std::get<1>(std::move(data_)); }

    /// \brief Returns the held value, or a fallback when the result failed.
    ///
    /// \param fallback The value to return when no value is held.
    ///
    /// \returns The held value, or \p fallback converted to T.
    template <class U>
    [[nodiscard]] T valueOr(U&& fallback) const& {
        return ok() ? value() : static_cast<T>(std::forward<U>(fallback));
    }

private:
    std::variant<T, E> data_;
};

/// \brief Specialisation for operations that either succeed with no value or
///        fail with an error.
///
/// \tparam E The error type produced on failure.
template <class E>
class Result<void, E> {
public:
    /// \brief No value is produced on success.
    using value_type = void;
    /// \brief The error type produced on failure.
    using error_type = E;

    /// \brief Constructs a successful result.
    Result() = default;

    /// \brief Constructs a failed result.
    ///
    /// \param failure The error to hold, usually produced by fail().
    Result(Failure<E> failure) : error_(std::in_place, std::move(failure.value)) {}

    /// \brief Reports whether the operation succeeded.
    ///
    /// \returns `true` when no error is held.
    [[nodiscard]] bool ok() const noexcept { return !error_.has_value(); }

    /// \brief Reports whether the operation succeeded.
    ///
    /// \returns `true` when no error is held.
    explicit operator bool() const noexcept { return ok(); }

    /// \brief Accesses the held error.
    ///
    /// \returns A reference to the error.
    ///
    /// \remarks Undefined unless ok() is `false`.
    [[nodiscard]] const E& error() const& { return *error_; }

    /// \brief Moves the held error out of the result.
    ///
    /// \returns The error, moved.
    ///
    /// \remarks Undefined unless ok() is `false`.
    [[nodiscard]] E&& error() && { return std::move(*error_); }

private:
    std::optional<E> error_;
};

}  // namespace nmtreediff
