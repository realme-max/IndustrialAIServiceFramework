#include "iaisf/net/tcp/buffer.hpp"

#include <algorithm>
#include <cstring>
#include <new>
#include <stdexcept>
#include <utility>

#include "iaisf/core/error.hpp"

namespace iaisf::net::tcp {

Result<Buffer> Buffer::create(
    const std::size_t initial_capacity,
    const std::size_t maximum_capacity) {
    if (maximum_capacity == 0U) {
        return Result<Buffer>::failure(make_error(
            ErrorCode::InvalidArgument,
            "Buffer maximum capacity must be greater than zero"));
    }
    if (initial_capacity > maximum_capacity) {
        return Result<Buffer>::failure(make_error(
            ErrorCode::InvalidArgument,
            "Buffer initial capacity exceeds maximum capacity"));
    }

    try {
        return Result<Buffer>::success(
            Buffer{std::vector<char>(initial_capacity), maximum_capacity});
    } catch (const std::bad_alloc&) {
        return Result<Buffer>::failure(make_error(
            ErrorCode::ResourceExhausted,
            "Buffer initial allocation failed"));
    } catch (const std::length_error&) {
        return Result<Buffer>::failure(make_error(
            ErrorCode::ResourceExhausted,
            "Buffer initial capacity is not representable"));
    }
}

Buffer::Buffer(
    std::vector<char> storage,
    const std::size_t maximum_capacity) noexcept
    : storage_(std::move(storage)), maximum_capacity_(maximum_capacity) {}

Buffer::Buffer(Buffer&& other) noexcept {
    move_from(std::move(other));
}

Buffer& Buffer::operator=(Buffer&& other) noexcept {
    if (this != &other) {
        move_from(std::move(other));
    }
    return *this;
}

void Buffer::move_from(Buffer&& other) noexcept {
    storage_ = std::move(other.storage_);
    reader_index_ = other.reader_index_;
    writer_index_ = other.writer_index_;
    maximum_capacity_ = other.maximum_capacity_;
    other.reader_index_ = 0U;
    other.writer_index_ = 0U;
    other.maximum_capacity_ = 0U;
}

std::size_t Buffer::readable_bytes() const noexcept {
    return writer_index_ - reader_index_;
}

std::size_t Buffer::writable_bytes() const noexcept {
    return storage_.size() - writer_index_;
}

std::size_t Buffer::prependable_bytes() const noexcept {
    return reader_index_;
}

std::size_t Buffer::capacity() const noexcept {
    return storage_.size();
}

std::size_t Buffer::maximum_capacity() const noexcept {
    return maximum_capacity_;
}

std::size_t Buffer::reader_index() const noexcept {
    return reader_index_;
}

std::size_t Buffer::writer_index() const noexcept {
    return writer_index_;
}

bool Buffer::empty() const noexcept {
    return readable_bytes() == 0U;
}

const char* Buffer::peek() const noexcept {
    return storage_.empty() ? nullptr : storage_.data() + reader_index_;
}

void Buffer::compact() noexcept {
    const std::size_t readable = readable_bytes();
    if (reader_index_ != 0U && readable != 0U) {
        std::memmove(storage_.data(), peek(), readable);
    }
    reader_index_ = 0U;
    writer_index_ = readable;
}

Result<void> Buffer::append(
    const void* const data,
    const std::size_t length) {
    if (length == 0U) {
        return Result<void>::success();
    }
    if (data == nullptr) {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidArgument,
            "Buffer append data must not be null"));
    }

    const std::size_t readable = readable_bytes();
    if (readable > maximum_capacity_ ||
        length > maximum_capacity_ - readable) {
        return Result<void>::failure(make_error(
            ErrorCode::ResourceExhausted,
            "Buffer maximum capacity exceeded"));
    }

    auto writable_result = ensure_writable(length);
    if (!writable_result) {
        return writable_result;
    }
    std::memcpy(storage_.data() + writer_index_, data, length);
    writer_index_ += length;
    return Result<void>::success();
}

Result<void> Buffer::append(const std::string_view bytes) {
    return append(bytes.data(), bytes.size());
}

Result<void> Buffer::retrieve(const std::size_t length) {
    if (length > readable_bytes()) {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidArgument,
            "Buffer retrieve length exceeds readable bytes"));
    }
    reader_index_ += length;
    if (reader_index_ == writer_index_) {
        reader_index_ = 0U;
        writer_index_ = 0U;
    }
    return Result<void>::success();
}

Result<std::string> Buffer::retrieve_as_string(const std::size_t length) {
    if (length > readable_bytes()) {
        return Result<std::string>::failure(make_error(
            ErrorCode::InvalidArgument,
            "Buffer string length exceeds readable bytes"));
    }
    if (length == 0U) {
        return Result<std::string>::success(std::string{});
    }

    try {
        std::string value{peek(), length};
        auto retrieve_result = retrieve(length);
        if (!retrieve_result) {
            return Result<std::string>::failure(retrieve_result.error());
        }
        return Result<std::string>::success(std::move(value));
    } catch (const std::bad_alloc&) {
        return Result<std::string>::failure(make_error(
            ErrorCode::ResourceExhausted,
            "Buffer string allocation failed"));
    } catch (const std::length_error&) {
        return Result<std::string>::failure(make_error(
            ErrorCode::ResourceExhausted,
            "Buffer string length is not representable"));
    }
}

void Buffer::retrieve_all() noexcept {
    reader_index_ = 0U;
    writer_index_ = 0U;
}

Result<void> Buffer::ensure_writable(const std::size_t length) {
    const std::size_t readable = readable_bytes();
    if (readable > maximum_capacity_ ||
        length > maximum_capacity_ - readable) {
        return Result<void>::failure(make_error(
            ErrorCode::ResourceExhausted,
            "Buffer maximum capacity exceeded"));
    }
    if (length <= writable_bytes()) {
        return Result<void>::success();
    }

    if (reader_index_ >= length - writable_bytes()) {
        compact();
        return Result<void>::success();
    }

    const std::size_t required_capacity = readable + length;
    std::size_t new_capacity = storage_.empty() ? 1U : storage_.size();
    while (new_capacity < required_capacity) {
        const std::size_t remaining = maximum_capacity_ - new_capacity;
        const std::size_t growth = std::min(new_capacity, remaining);
        if (growth == 0U) {
            new_capacity = maximum_capacity_;
            break;
        }
        new_capacity += growth;
    }
    if (new_capacity < required_capacity) {
        new_capacity = required_capacity;
    }

    try {
        std::vector<char> replacement(new_capacity);
        if (readable != 0U) {
            std::memcpy(replacement.data(), peek(), readable);
        }
        storage_.swap(replacement);
        reader_index_ = 0U;
        writer_index_ = readable;
        return Result<void>::success();
    } catch (const std::bad_alloc&) {
        return Result<void>::failure(make_error(
            ErrorCode::ResourceExhausted,
            "Buffer growth allocation failed"));
    } catch (const std::length_error&) {
        return Result<void>::failure(make_error(
            ErrorCode::ResourceExhausted,
            "Buffer growth capacity is not representable"));
    }
}

}  // namespace iaisf::net::tcp
