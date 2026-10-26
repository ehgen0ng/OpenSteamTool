#pragma once

#include <cstdint>
#include <string>

namespace OSTPlatform::Http {

    struct Result {
        std::string body;
        uint32_t status = 0;
        // True only for a complete response within the size limit. Callers must
        // still check status for 2xx; errors and oversized bodies are not usable.
        bool ok = false;
    };

    Result Execute(const wchar_t* method,
                   const char* url,
                   const void* reqBody = nullptr,
                   uint32_t reqBodyLen = 0,
                   const wchar_t* headers = nullptr,
                   uint32_t timeoutResolve = 5000,
                   uint32_t timeoutConnect = 5000,
                   uint32_t timeoutSend = 10000,
                   uint32_t timeoutRecv = 10000,
                   uint32_t maxBodyBytes = 256u * 1024u);

} // namespace OSTPlatform::Http
