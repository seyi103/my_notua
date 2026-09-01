#include "core/network/httpRequestParser.h"

#if NOTUA_SOFTAP_HTTP_SPIKE
#include <string.h>

namespace notua::http {
namespace {
struct Token { const char* data; size_t length; };

bool whitespace(char value) { return value == ' ' || value == '\t'; }
bool equals(Token token, const char* value) {
    const size_t length = strlen(value);
    return token.length == length && memcmp(token.data, value, length) == 0;
}
bool tokenize(const char* line, size_t length, Token output[3]) {
    if (!line || !length) return false;
    size_t cursor = 0;
    for (size_t token = 0; token < 3; ++token) {
        while (cursor < length && whitespace(line[cursor])) ++cursor;
        const size_t start = cursor;
        while (cursor < length && !whitespace(line[cursor])) ++cursor;
        if (start == cursor) return false;
        output[token] = {line + start, cursor - start};
    }
    while (cursor < length && whitespace(line[cursor])) ++cursor;
    return cursor == length;
}

UploadRequestResult normalizedPath(Token target, Token& path) {
    constexpr char HTTP_PREFIX[] = "http://";
    if (target.length >= sizeof(HTTP_PREFIX) - 1
        && memcmp(target.data, HTTP_PREFIX, sizeof(HTTP_PREFIX) - 1) == 0) {
        size_t cursor = sizeof(HTTP_PREFIX) - 1;
        const size_t authorityStart = cursor;
        while (cursor < target.length && target.data[cursor] != '/') ++cursor;
        if (cursor == authorityStart || cursor == target.length) return UploadRequestResult::invalidPath;
        path = {target.data + cursor, target.length - cursor};
        return UploadRequestResult::ok;
    }
    if (!target.length || target.data[0] != '/') return UploadRequestResult::invalidPath;
    path = target;
    return UploadRequestResult::ok;
}
} // namespace

UploadRequestResult parseUploadRequestLine(const char* line, size_t length,
    uint8_t maxImages, UploadRequest& request) {
    request.slot = 0xff;
    Token tokens[3]{};
    if (!tokenize(line, length, tokens)) return UploadRequestResult::malformedRequestLine;
    if (!equals(tokens[0], "PUT")) return UploadRequestResult::invalidMethod;
    if (!equals(tokens[2], "HTTP/1.1") && !equals(tokens[2], "HTTP/1.0"))
        return UploadRequestResult::unsupportedVersion;

    Token path{};
    const UploadRequestResult normalized = normalizedPath(tokens[1], path);
    if (normalized != UploadRequestResult::ok) return normalized;
    constexpr char IMAGE_PREFIX[] = "/images/";
    constexpr size_t PREFIX_LENGTH = sizeof(IMAGE_PREFIX) - 1;
    if (path.length <= PREFIX_LENGTH || memcmp(path.data, IMAGE_PREFIX, PREFIX_LENGTH) != 0)
        return UploadRequestResult::invalidPath;
    const Token slot{path.data + PREFIX_LENGTH, path.length - PREFIX_LENGTH};
    unsigned value = 0;
    for (size_t index = 0; index < slot.length; ++index) {
        if (slot.data[index] < '0' || slot.data[index] > '9') return UploadRequestResult::invalidSlot;
        value = value * 10U + static_cast<unsigned>(slot.data[index] - '0');
        if (value >= maxImages) return UploadRequestResult::invalidSlot;
    }
    request.slot = static_cast<uint8_t>(value);
    return UploadRequestResult::ok;
}

const char* uploadRequestError(UploadRequestResult result) {
    switch (result) {
    case UploadRequestResult::ok: return "ok";
    case UploadRequestResult::malformedRequestLine: return "malformed-request-line";
    case UploadRequestResult::invalidMethod: return "invalid-method";
    case UploadRequestResult::unsupportedVersion: return "http-version";
    case UploadRequestResult::invalidPath: return "invalid-path";
    case UploadRequestResult::invalidSlot: return "invalid-slot";
    }
    return "malformed-request-line";
}
} // namespace notua::http
#endif
