#include "iaisf/http/http_parser.hpp"

#include <algorithm>
#include <limits>
#include <new>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace iaisf::http {
namespace {

[[nodiscard]] bool is_token_character(unsigned char value) noexcept {
    if ((value >= static_cast<unsigned char>('a') &&
         value <= static_cast<unsigned char>('z')) ||
        (value >= static_cast<unsigned char>('A') &&
         value <= static_cast<unsigned char>('Z')) ||
        (value >= static_cast<unsigned char>('0') &&
         value <= static_cast<unsigned char>('9'))) {
        return true;
    }
    constexpr std::string_view kPunctuation{"!#$%&'*+-.^_`|~"};
    return kPunctuation.find(static_cast<char>(value)) != std::string_view::npos;
}

[[nodiscard]] std::string lowercase(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (const char character : value) {
        const auto byte = static_cast<unsigned char>(character);
        result.push_back(
            byte >= static_cast<unsigned char>('A') &&
                    byte <= static_cast<unsigned char>('Z')
                ? static_cast<char>(
                      byte + static_cast<unsigned char>('a' - 'A'))
                : character);
    }
    return result;
}

[[nodiscard]] std::string_view trim_ows(std::string_view value) noexcept {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
        value.remove_prefix(1U);
    }
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
        value.remove_suffix(1U);
    }
    return value;
}

[[nodiscard]] bool valid_field_value(std::string_view value) noexcept {
    return std::none_of(value.begin(), value.end(), [](char character) {
        const auto byte = static_cast<unsigned char>(character);
        return (byte < 0x20U && byte != 0x09U) || byte == 0x7FU;
    });
}

[[nodiscard]] Result<std::size_t> parse_content_length(
    std::string_view value) {
    if (value.empty()) {
        return Result<std::size_t>::failure(make_error(
            ErrorCode::InvalidArgument,
            "Content-Length is empty"));
    }
    std::size_t parsed = 0U;
    for (const char character : value) {
        if (character < '0' || character > '9') {
            return Result<std::size_t>::failure(make_error(
                ErrorCode::InvalidArgument,
                "Content-Length is not decimal"));
        }
        const auto digit =
            static_cast<std::size_t>(character - '0');
        if (parsed > (std::numeric_limits<std::size_t>::max() - digit) / 10U) {
            return Result<std::size_t>::failure(make_error(
                ErrorCode::ResourceExhausted,
                "Content-Length overflows"));
        }
        parsed = parsed * 10U + digit;
    }
    return Result<std::size_t>::success(parsed);
}

[[nodiscard]] bool valid_origin_target(std::string_view target) noexcept {
    if (target.empty() || target.front() != '/' ||
        target.find('#') != std::string_view::npos ||
        target.find('\\') != std::string_view::npos) {
        return false;
    }
    return std::none_of(target.begin(), target.end(), [](char character) {
        const auto byte = static_cast<unsigned char>(character);
        return byte <= 0x20U || byte == 0x7FU;
    });
}

}  // namespace

HttpParser::HttpParser(HttpLimits limits) : limits_(std::move(limits)) {}

Result<ParseProgress> HttpParser::parse(std::string_view bytes) {
    if (state_ == State::Complete) {
        return Result<ParseProgress>::success(
            {ParseDisposition::Complete, 0U, error_status_, phase(), 0U});
    }
    if (state_ == State::Error) {
        return Result<ParseProgress>::success(
            {ParseDisposition::Error, 0U, error_status_, phase(), 0U});
    }

    std::size_t consumed = 0U;
    std::size_t body_bytes_consumed = 0U;
    try {
        while (consumed < bytes.size()) {
            if (state_ == State::Body) {
                const auto expected = content_length_.value_or(0U);
                const auto remaining = expected - body_.size();
                const auto available = bytes.size() - consumed;
                const auto count = std::min(remaining, available);
                auto append_result =
                    append_body(bytes.substr(consumed, count));
                if (!append_result) {
                    return Result<ParseProgress>::failure(
                        std::move(append_result).error());
                }
                consumed += count;
                body_bytes_consumed += count;
                if (state_ == State::Complete) {
                    return Result<ParseProgress>::success(
                        {ParseDisposition::Complete,
                         consumed,
                         error_status_,
                         phase(),
                         body_bytes_consumed});
                }
                continue;
            }

            if (state_ == State::Headers) {
                if (header_bytes_ == limits_.max_header_bytes()) {
                    set_protocol_error(
                        HttpStatus::RequestHeaderFieldsTooLarge);
                    ++consumed;
                    return Result<ParseProgress>::success(
                        {ParseDisposition::Error,
                         consumed,
                         error_status_,
                         phase(),
                         body_bytes_consumed});
                }
                ++header_bytes_;
            }

            auto byte_result = consume_line_byte(bytes[consumed]);
            ++consumed;
            if (!byte_result) {
                return Result<ParseProgress>::failure(
                    std::move(byte_result).error());
            }
            if (state_ == State::Error) {
                return Result<ParseProgress>::success(
                    {ParseDisposition::Error,
                     consumed,
                     error_status_,
                     phase(),
                     body_bytes_consumed});
            }
            if (state_ == State::Complete) {
                return Result<ParseProgress>::success(
                    {ParseDisposition::Complete,
                     consumed,
                     error_status_,
                     phase(),
                     body_bytes_consumed});
            }
        }
    } catch (const std::bad_alloc&) {
        return Result<ParseProgress>::failure(make_error(
            ErrorCode::ResourceExhausted,
            "HTTP parser allocation failed"));
    } catch (const std::length_error&) {
        return Result<ParseProgress>::failure(make_error(
            ErrorCode::ResourceExhausted,
            "HTTP parser length limit failed"));
    }

    return Result<ParseProgress>::success(
        {ParseDisposition::NeedMore,
         consumed,
         error_status_,
         phase(),
         body_bytes_consumed});
}

Result<HttpRequest> HttpParser::take_request() {
    if (state_ != State::Complete) {
        return Result<HttpRequest>::failure(make_error(
            ErrorCode::InvalidState,
            "HTTP request is not complete"));
    }
    auto request = HttpRequest::create(
        std::move(method_),
        std::move(target_),
        std::move(headers_),
        std::move(body_),
        !connection_close_);
    if (!request) {
        set_protocol_error(HttpStatus::InternalServerError);
        return request;
    }
    reset_for_next_request();
    return request;
}

ParseDisposition HttpParser::disposition() const noexcept {
    if (state_ == State::Complete) {
        return ParseDisposition::Complete;
    }
    if (state_ == State::Error) {
        return ParseDisposition::Error;
    }
    return ParseDisposition::NeedMore;
}

ParsePhase HttpParser::phase() const noexcept {
    switch (state_) {
        case State::RequestLine:
        case State::Headers:
            return ParsePhase::Headers;
        case State::Body:
            return ParsePhase::Body;
        case State::Complete:
            return ParsePhase::Complete;
        case State::Error:
            return ParsePhase::Error;
    }
    return ParsePhase::Error;
}

HttpStatus HttpParser::error_status() const noexcept {
    return error_status_;
}

Result<void> HttpParser::consume_line_byte(char byte) {
    if (saw_carriage_return_) {
        saw_carriage_return_ = false;
        if (byte != '\n') {
            set_protocol_error(HttpStatus::BadRequest);
            return Result<void>::success();
        }
        return finish_line();
    }
    if (byte == '\n') {
        set_protocol_error(HttpStatus::BadRequest);
        return Result<void>::success();
    }
    if (byte == '\r') {
        const auto limit = state_ == State::RequestLine
                               ? limits_.max_request_line_bytes()
                               : limits_.max_header_line_bytes();
        if (limit < 2U || line_.size() > limit - 2U) {
            set_protocol_error(
                state_ == State::RequestLine
                    ? HttpStatus::UriTooLong
                    : HttpStatus::RequestHeaderFieldsTooLarge);
            return Result<void>::success();
        }
        saw_carriage_return_ = true;
        return Result<void>::success();
    }

    const auto limit = state_ == State::RequestLine
                           ? limits_.max_request_line_bytes()
                           : limits_.max_header_line_bytes();
    if (limit < 2U || line_.size() >= limit - 2U) {
        set_protocol_error(
            state_ == State::RequestLine
                ? HttpStatus::UriTooLong
                : HttpStatus::RequestHeaderFieldsTooLarge);
        return Result<void>::success();
    }
    line_.push_back(byte);
    return Result<void>::success();
}

Result<void> HttpParser::finish_line() {
    Result<void> result = Result<void>::success();
    if (state_ == State::RequestLine) {
        result = process_request_line(line_);
    } else if (state_ == State::Headers) {
        result = process_header_line(line_);
    } else {
        set_protocol_error(HttpStatus::BadRequest);
    }
    line_.clear();
    return result;
}

Result<void> HttpParser::process_request_line(const std::string& line) {
    const auto first_space = line.find(' ');
    const auto second_space =
        first_space == std::string::npos
            ? std::string::npos
            : line.find(' ', first_space + 1U);
    if (first_space == std::string::npos ||
        second_space == std::string::npos ||
        line.find(' ', second_space + 1U) != std::string::npos) {
        set_protocol_error(HttpStatus::BadRequest);
        return Result<void>::success();
    }

    const std::string_view method{line.data(), first_space};
    const std::string_view target{
        line.data() + first_space + 1U,
        second_space - first_space - 1U};
    const std::string_view version{
        line.data() + second_space + 1U,
        line.size() - second_space - 1U};

    if (version != "HTTP/1.1") {
        set_protocol_error(
            version.rfind("HTTP/", 0U) == 0U
                ? HttpStatus::HttpVersionNotSupported
                : HttpStatus::BadRequest);
        return Result<void>::success();
    }
    if (method.empty() || method.size() > limits_.max_method_bytes() ||
        !std::all_of(method.begin(), method.end(), [](char character) {
            return is_token_character(static_cast<unsigned char>(character));
        })) {
        set_protocol_error(HttpStatus::BadRequest);
        return Result<void>::success();
    }
    if (target.size() > limits_.max_target_bytes()) {
        set_protocol_error(HttpStatus::UriTooLong);
        return Result<void>::success();
    }
    if (!valid_origin_target(target)) {
        set_protocol_error(HttpStatus::BadRequest);
        return Result<void>::success();
    }
    method_.assign(method.data(), method.size());
    target_.assign(target.data(), target.size());
    state_ = State::Headers;
    return Result<void>::success();
}

Result<void> HttpParser::process_header_line(const std::string& line) {
    if (line.empty()) {
        return finish_headers();
    }
    if (line.front() == ' ' || line.front() == '\t') {
        set_protocol_error(HttpStatus::BadRequest);
        return Result<void>::success();
    }
    if (header_count_ == limits_.max_header_count()) {
        set_protocol_error(HttpStatus::RequestHeaderFieldsTooLarge);
        return Result<void>::success();
    }

    const auto colon = line.find(':');
    if (colon == std::string::npos || colon == 0U) {
        set_protocol_error(HttpStatus::BadRequest);
        return Result<void>::success();
    }
    const std::string_view raw_name{line.data(), colon};
    if (!std::all_of(raw_name.begin(), raw_name.end(), [](char character) {
            return is_token_character(static_cast<unsigned char>(character));
        })) {
        set_protocol_error(HttpStatus::BadRequest);
        return Result<void>::success();
    }
    const auto raw_value =
        trim_ows(std::string_view{line}.substr(colon + 1U));
    if (!valid_field_value(raw_value)) {
        set_protocol_error(HttpStatus::BadRequest);
        return Result<void>::success();
    }

    const auto name = lowercase(raw_name);
    const auto inserted_name = header_names_.insert(name);
    if (!inserted_name.second) {
        set_protocol_error(HttpStatus::BadRequest);
        return Result<void>::success();
    }
    if (name == "host") {
        ++host_count_;
        if (raw_value.empty() || host_count_ != 1U) {
            set_protocol_error(HttpStatus::BadRequest);
            return Result<void>::success();
        }
    } else if (name == "content-length") {
        if (content_length_.has_value()) {
            set_protocol_error(HttpStatus::BadRequest);
            return Result<void>::success();
        }
        auto parsed = parse_content_length(raw_value);
        if (!parsed) {
            set_protocol_error(
                parsed.error().code == ErrorCode::ResourceExhausted
                    ? HttpStatus::PayloadTooLarge
                    : HttpStatus::BadRequest);
            return Result<void>::success();
        }
        content_length_ = parsed.value();
    } else if (name == "transfer-encoding") {
        transfer_encoding_seen_ = true;
    } else if (name == "expect") {
        expect_seen_ = true;
    } else if (name == "upgrade") {
        upgrade_seen_ = true;
    } else if (name == "connection") {
        std::string_view remaining = raw_value;
        if (remaining.empty()) {
            set_protocol_error(HttpStatus::BadRequest);
            return Result<void>::success();
        }
        while (!remaining.empty()) {
            const auto comma = remaining.find(',');
            const auto token = trim_ows(remaining.substr(0U, comma));
            if (token.empty() ||
                !std::all_of(token.begin(), token.end(), [](char character) {
                    return is_token_character(
                        static_cast<unsigned char>(character));
                })) {
                set_protocol_error(HttpStatus::BadRequest);
                return Result<void>::success();
            }
            const auto normalized = lowercase(token);
            if (normalized == "close") {
                connection_close_ = true;
            }
            if (normalized == "upgrade") {
                upgrade_seen_ = true;
            }
            if (comma == std::string_view::npos) {
                break;
            }
            remaining.remove_prefix(comma + 1U);
            if (remaining.empty()) {
                set_protocol_error(HttpStatus::BadRequest);
                return Result<void>::success();
            }
        }
    }

    headers_.push_back(HttpHeader{
        name,
        std::string{raw_value.data(), raw_value.size()}});
    ++header_count_;
    return Result<void>::success();
}

Result<void> HttpParser::finish_headers() {
    if (host_count_ != 1U) {
        set_protocol_error(HttpStatus::BadRequest);
        return Result<void>::success();
    }
    if (transfer_encoding_seen_ && content_length_.has_value()) {
        set_protocol_error(HttpStatus::BadRequest);
        return Result<void>::success();
    }
    if (transfer_encoding_seen_ || upgrade_seen_) {
        set_protocol_error(HttpStatus::NotImplemented);
        return Result<void>::success();
    }
    if (expect_seen_) {
        set_protocol_error(HttpStatus::ExpectationFailed);
        return Result<void>::success();
    }

    const auto length = content_length_.value_or(0U);
    if (length > limits_.max_body_bytes()) {
        set_protocol_error(HttpStatus::PayloadTooLarge);
        return Result<void>::success();
    }
    if (length == 0U) {
        state_ = State::Complete;
        return Result<void>::success();
    }
    body_.reserve(length);
    state_ = State::Body;
    return Result<void>::success();
}

Result<void> HttpParser::append_body(std::string_view bytes) {
    const auto expected = content_length_.value_or(0U);
    if (bytes.size() > expected - body_.size()) {
        set_protocol_error(HttpStatus::BadRequest);
        return Result<void>::success();
    }
    body_.append(bytes.data(), bytes.size());
    if (body_.size() == expected) {
        state_ = State::Complete;
    }
    return Result<void>::success();
}

void HttpParser::set_protocol_error(HttpStatus status) noexcept {
    error_status_ = status;
    state_ = State::Error;
}

void HttpParser::reset_for_next_request() noexcept {
    state_ = State::RequestLine;
    error_status_ = HttpStatus::BadRequest;
    line_.clear();
    saw_carriage_return_ = false;
    header_bytes_ = 0U;
    header_count_ = 0U;
    content_length_.reset();
    host_count_ = 0U;
    transfer_encoding_seen_ = false;
    expect_seen_ = false;
    upgrade_seen_ = false;
    connection_close_ = false;
    method_.clear();
    target_.clear();
    header_names_.clear();
    headers_.clear();
    body_.clear();
}

}  // namespace iaisf::http
