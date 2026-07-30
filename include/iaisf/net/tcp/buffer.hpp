#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "iaisf/core/result.hpp"

namespace iaisf::net::tcp {

/**
 * Owner-thread-only bounded byte buffer.
 *
 * Bytes before reader_index are reclaimed lazily. retrieve() advances the
 * cursor without moving bytes; append() compacts only when the reclaimed front
 * is required. peek() remains valid only until the next mutating operation.
 */
class Buffer final {
public:
    [[nodiscard]] static Result<Buffer> create(
        std::size_t initial_capacity,
        std::size_t maximum_capacity);

    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;
    Buffer(Buffer&& other) noexcept;
    Buffer& operator=(Buffer&& other) noexcept;
    ~Buffer() = default;

    [[nodiscard]] std::size_t readable_bytes() const noexcept;
    [[nodiscard]] std::size_t writable_bytes() const noexcept;
    [[nodiscard]] std::size_t prependable_bytes() const noexcept;
    [[nodiscard]] std::size_t capacity() const noexcept;
    [[nodiscard]] std::size_t maximum_capacity() const noexcept;
    [[nodiscard]] std::size_t reader_index() const noexcept;
    [[nodiscard]] std::size_t writer_index() const noexcept;
    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] const char* peek() const noexcept;

    [[nodiscard]] Result<void> ensure_writable(std::size_t length);
    void compact() noexcept;
    [[nodiscard]] Result<void> append(
        const void* data,
        std::size_t length);
    [[nodiscard]] Result<void> append(std::string_view bytes);
    [[nodiscard]] Result<void> retrieve(std::size_t length);
    [[nodiscard]] Result<std::string> retrieve_as_string(std::size_t length);
    void retrieve_all() noexcept;

private:
    Buffer(
        std::vector<char> storage,
        std::size_t maximum_capacity) noexcept;

    void move_from(Buffer&& other) noexcept;

    std::vector<char> storage_;
    std::size_t reader_index_{0U};
    std::size_t writer_index_{0U};
    std::size_t maximum_capacity_{0U};
};

}  // namespace iaisf::net::tcp
