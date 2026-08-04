#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "iaisf/core/result.hpp"
#include "iaisf/http/http_limits.hpp"
#include "iaisf/http/http_request.hpp"
#include "iaisf/http/http_status.hpp"

namespace iaisf::http {

enum class ParseDisposition {
    NeedMore,
    Complete,
    Error,
};

enum class ParsePhase {
    Headers,
    Body,
    Complete,
    Error,
};

struct ParseProgress {
    ParseDisposition disposition{ParseDisposition::NeedMore};
    std::size_t consumed{0U};
    HttpStatus error_status{HttpStatus::BadRequest};
    ParsePhase phase{ParsePhase::Headers};
    std::size_t body_bytes_consumed{0U};
};

/**
 * Incremental, strict-CRLF HTTP/1.1 request parser.
 *
 * A parser belongs to exactly one connection and is not thread-safe. parse()
 * consumes only the returned byte count. take_request() moves out a complete
 * request and resets the parser for the next sequential request.
 */
class HttpParser final {
public:
    explicit HttpParser(HttpLimits limits = HttpLimits::defaults());

    [[nodiscard]] Result<ParseProgress> parse(std::string_view bytes);
    [[nodiscard]] Result<HttpRequest> take_request();
    [[nodiscard]] ParseDisposition disposition() const noexcept;
    [[nodiscard]] ParsePhase phase() const noexcept;
    [[nodiscard]] HttpStatus error_status() const noexcept;

private:
    enum class State {
        RequestLine,
        Headers,
        Body,
        Complete,
        Error,
    };

    [[nodiscard]] Result<void> consume_line_byte(char byte);
    [[nodiscard]] Result<void> finish_line();
    [[nodiscard]] Result<void> process_request_line(const std::string& line);
    [[nodiscard]] Result<void> process_header_line(const std::string& line);
    [[nodiscard]] Result<void> finish_headers();
    [[nodiscard]] Result<void> append_body(std::string_view bytes);
    void set_protocol_error(HttpStatus status) noexcept;
    void reset_for_next_request() noexcept;

    HttpLimits limits_;
    State state_{State::RequestLine};
    HttpStatus error_status_{HttpStatus::BadRequest};
    std::string line_;
    bool saw_carriage_return_{false};
    std::size_t header_bytes_{0U};
    std::size_t header_count_{0U};
    std::optional<std::size_t> content_length_;
    std::size_t host_count_{0U};
    bool transfer_encoding_seen_{false};
    bool expect_seen_{false};
    bool upgrade_seen_{false};
    bool connection_close_{false};
    std::string method_;
    std::string target_;
    std::unordered_set<std::string> header_names_;
    HttpRequest::Headers headers_;
    std::string body_;
};

}  // namespace iaisf::http
