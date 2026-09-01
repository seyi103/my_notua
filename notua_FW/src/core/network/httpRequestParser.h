#pragma once

#include <stddef.h>
#include <stdint.h>

namespace notua::http {

enum class UploadRequestResult : uint8_t {
    ok,
    malformedRequestLine,
    invalidMethod,
    unsupportedVersion,
    invalidPath,
    invalidSlot,
};

struct UploadRequest {
    uint8_t slot = 0xff;
};

UploadRequestResult parseUploadRequestLine(const char* line, size_t length,
    uint8_t maxImages, UploadRequest& request);
const char* uploadRequestError(UploadRequestResult result);

} // namespace notua::http
