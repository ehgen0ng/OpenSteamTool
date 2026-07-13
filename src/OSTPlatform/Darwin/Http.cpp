#include "include/Http.h"

#include "include/Encoding.h"
#include "include/Log.h"

#include <curl/curl.h>

#include <algorithm>
#include <mutex>
#include <string>
#include <string_view>

// macOS HTTP backend, mirroring the WinHTTP-based Windows/Http.cpp. libcurl ships
// with macOS (linked from the SDK stub) and speaks TLS via Secure Transport, so
// https URLs work with no extra configuration.
namespace OSTPlatform::Http {
namespace {

// WinHTTP's read loop caps the body at 256 KiB; keep parity so callers see the
// same truncation behaviour on both platforms.
constexpr size_t kMaxBodyBytes = 256 * 1024;

void EnsureGlobalInit() {
    static std::once_flag once;
    std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

size_t WriteToString(char* ptr, size_t size, size_t nmemb, void* userdata) {
    const size_t total = size * nmemb;
    auto* body = static_cast<std::string*>(userdata);
    if (body->size() < kMaxBodyBytes) {
        const size_t room = kMaxBodyBytes - body->size();
        body->append(ptr, (std::min)(total, room));
    }
    // Report the full count as consumed even past the cap, so libcurl finishes the
    // transfer normally instead of failing with CURLE_WRITE_ERROR.
    return total;
}

std::string NarrowAscii(const wchar_t* text) {
    std::string out;
    if (!text) return out;
    for (const wchar_t* p = text; *p; ++p) {
        out.push_back(static_cast<char>(*p & 0x7F));
    }
    return out;
}

// Headers arrive as a single WinHTTP-style blob: "Name: Value\r\nName2: Value2\r\n".
// Split on CRLF and hand each non-empty line to libcurl.
curl_slist* BuildHeaderList(const wchar_t* headers) {
    if (!headers || !headers[0]) return nullptr;

    const std::string blob = Encoding::WideToUtf8(headers);
    curl_slist* list = nullptr;

    std::string_view rest(blob);
    while (!rest.empty()) {
        const size_t eol = rest.find("\r\n");
        std::string_view line = (eol == std::string_view::npos) ? rest : rest.substr(0, eol);
        if (!line.empty()) {
            list = curl_slist_append(list, std::string(line).c_str());
        }
        if (eol == std::string_view::npos) break;
        rest.remove_prefix(eol + 2);
    }
    return list;
}

} // namespace

Result Execute(const wchar_t* method,
               const char* url,
               const void* reqBody,
               uint32_t reqBodyLen,
               const wchar_t* headers,
               uint32_t timeoutResolve,
               uint32_t timeoutConnect,
               uint32_t timeoutSend,
               uint32_t timeoutRecv) {
    Result r;

    if (!url || !url[0]) {
        OSTP_LOG_WARN("Http::Execute: empty URL");
        return r;
    }

    EnsureGlobalInit();

    CURL* curl = curl_easy_init();
    if (!curl) {
        OSTP_LOG_WARN("Http::Execute({}): curl_easy_init failed", url);
        return r;
    }

    const std::string narrowMethod = NarrowAscii(method);
    curl_slist* headerList = BuildHeaderList(headers);

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "OpenSteamTool/1.0");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    // No SIGALRM-based timeouts: Execute runs on worker threads and libcurl's
    // signal path is not thread-safe.
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteToString);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &r.body);

    if (!narrowMethod.empty()) {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, narrowMethod.c_str());
    }
    if (reqBody && reqBodyLen > 0) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE,
                         static_cast<curl_off_t>(reqBodyLen));
        curl_easy_setopt(curl, CURLOPT_COPYPOSTFIELDS, reqBody);
    }
    if (headerList) {
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headerList);
    }

    // WinHTTP splits the budget into resolve/connect/send/recv phases; libcurl
    // exposes a connect-phase cap and a whole-operation cap, so fold the phases
    // into those two.
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS,
                     static_cast<long>(timeoutResolve + timeoutConnect));
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS,
                     static_cast<long>(timeoutResolve + timeoutConnect + timeoutSend + timeoutRecv));

    const CURLcode res = curl_easy_perform(curl);

    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    r.status = static_cast<uint32_t>(status);

    if (res == CURLE_OK) {
        // A response was produced (any status, including 4xx/5xx); callers still
        // check r.status for 2xx, matching the Windows contract.
        r.ok = true;
        if (r.status < 200 || r.status >= 300) {
            OSTP_LOG_WARN("{} - unexpected HTTP {} body_bytes={}", url, r.status, r.body.size());
        }
    } else {
        OSTP_LOG_WARN("{} - curl_easy_perform failed ({}: {}) status={}",
                      url, static_cast<int>(res), curl_easy_strerror(res), r.status);
    }

    OSTP_LOG_DEBUG("{} - status={} body_bytes={} ok={}", url, r.status, r.body.size(), r.ok);

    if (headerList) curl_slist_free_all(headerList);
    curl_easy_cleanup(curl);
    return r;
}

} // namespace OSTPlatform::Http
