#include "mcp/McpHttp.h"

#include <Windows.h>

#include <bcrypt.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <vector>

namespace ire::mcp {

namespace {

std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

std::string trim(const std::string& text) {
    const auto first = text.find_first_not_of(" \t");
    if (first == std::string::npos) {
        return {};
    }
    const auto last = text.find_last_not_of(" \t");
    return text.substr(first, last - first + 1);
}

const char* statusText(int status) {
    switch (status) {
    case 200: return "OK";
    case 202: return "Accepted";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 413: return "Payload Too Large";
    case 500: return "Internal Server Error";
    default: return "OK";
    }
}

} // namespace

std::string HttpRequest::header(const std::string& name) const {
    const auto found = headers.find(lower(name));
    return found == headers.end() ? std::string() : found->second;
}

HttpParse parseHttpRequest(const std::string& buffer) {
    HttpParse parse;

    const auto headerEnd = buffer.find("\r\n\r\n");
    if (headerEnd == std::string::npos) {
        // A header block that has grown past anything reasonable is not a slow
        // client, it is a wedged or hostile one. Bounded here rather than in the
        // socket loop so the limit is visible next to the parsing it protects.
        if (buffer.size() > 64 * 1024) {
            parse.state = HttpParse::State::Malformed;
            parse.error = "The request headers are too large.";
        }
        return parse;
    }

    const std::string head = buffer.substr(0, headerEnd);
    std::size_t lineStart = 0;
    bool first = true;

    while (lineStart <= head.size()) {
        auto lineEnd = head.find("\r\n", lineStart);
        if (lineEnd == std::string::npos) {
            lineEnd = head.size();
        }
        const std::string line = head.substr(lineStart, lineEnd - lineStart);
        lineStart = lineEnd + 2;

        if (first) {
            first = false;
            const auto methodEnd = line.find(' ');
            if (methodEnd == std::string::npos) {
                parse.state = HttpParse::State::Malformed;
                parse.error = "The request line is malformed.";
                return parse;
            }
            const auto targetEnd = line.find(' ', methodEnd + 1);
            parse.request.method = line.substr(0, methodEnd);
            parse.request.target = line.substr(methodEnd + 1, targetEnd == std::string::npos
                                                                  ? std::string::npos
                                                                  : targetEnd - methodEnd - 1);
            continue;
        }

        if (line.empty()) {
            continue;
        }
        const auto colon = line.find(':');
        if (colon == std::string::npos) {
            parse.state = HttpParse::State::Malformed;
            parse.error = "A header line is malformed.";
            return parse;
        }
        parse.request.headers[lower(trim(line.substr(0, colon)))] = trim(line.substr(colon + 1));
    }

    std::size_t length = 0;
    if (const auto value = parse.request.header("content-length"); !value.empty()) {
        try {
            const auto parsed = std::stoll(value);
            if (parsed < 0) {
                parse.state = HttpParse::State::Malformed;
                parse.error = "Content-Length must not be negative.";
                return parse;
            }
            length = static_cast<std::size_t>(parsed);
        } catch (const std::exception&) {
            parse.state = HttpParse::State::Malformed;
            parse.error = "Content-Length is not a number.";
            return parse;
        }
    }

    // Every tool argument this accepts is a handful of scalars; a body larger
    // than this is not one of ours.
    if (length > 4 * 1024 * 1024) {
        parse.state = HttpParse::State::Malformed;
        parse.error = "The request body is too large.";
        return parse;
    }

    const std::size_t bodyStart = headerEnd + 4;
    if (buffer.size() < bodyStart + length) {
        return parse;
    }

    parse.request.body = buffer.substr(bodyStart, length);
    parse.consumed = bodyStart + length;
    parse.state = HttpParse::State::Complete;
    return parse;
}

std::string httpResponse(int status, const std::string& body, const std::string& contentType) {
    std::string response = "HTTP/1.1 " + std::to_string(status) + " " + statusText(status) + "\r\n";
    response += "Content-Type: " + contentType + "\r\n";
    response += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    // The server is bound to loopback and is not a web origin, but a page in a
    // browser can still reach 127.0.0.1. Refusing every origin outright is the
    // honest answer: nothing here is meant to be called from a page.
    response += "Access-Control-Allow-Origin: null\r\n";
    response += "Connection: close\r\n\r\n";
    response += body;
    return response;
}

bool bearerTokenMatches(const std::string& authorization, const std::string& token) {
    if (token.empty()) {
        return false;
    }
    static constexpr const char* prefix = "Bearer ";
    static constexpr std::size_t prefixLength = 7;
    if (authorization.size() <= prefixLength || lower(authorization.substr(0, prefixLength)) != lower(prefix)) {
        return false;
    }
    const std::string presented = trim(authorization.substr(prefixLength));
    // Length is not secret -- it is the same for every token this issues -- so
    // comparing it up front leaks nothing, and it lets the loop below run over a
    // fixed width.
    if (presented.size() != token.size()) {
        return false;
    }
    unsigned char difference = 0;
    for (std::size_t i = 0; i < token.size(); ++i) {
        difference |= static_cast<unsigned char>(presented[i] ^ token[i]);
    }
    return difference == 0;
}

std::string generateToken() {
    std::vector<std::uint8_t> bytes(16);
    // BCryptGenRandom rather than std::random_device or rand(): this is the one
    // thing standing between a local process and a memory-write API, and the
    // standard generators make no promise about being unpredictable.
    const NTSTATUS status = BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()),
                                            BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (status != 0) {
        // Refuse rather than fall back to something weaker. A predictable token
        // presented as a real one is worse than no server.
        return {};
    }
    static constexpr char digits[] = "0123456789abcdef";
    std::string token;
    token.reserve(bytes.size() * 2);
    for (const auto byte : bytes) {
        token.push_back(digits[byte >> 4]);
        token.push_back(digits[byte & 0x0F]);
    }
    return token;
}

} // namespace ire::mcp
