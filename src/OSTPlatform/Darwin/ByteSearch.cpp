#include "include/ByteSearch.h"

#include "include/Log.h"
#include "include/Stopwatch.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <limits>
#include <memory>
#include <vector>

namespace OSTPlatform::ByteSearch {
namespace {

// Boyer-Moore-Horspool fixed-pattern scanner. Identical to the Windows backend:
// the algorithm is platform-agnostic, only the file I/O below differs.
class FixedPatternScanner {
public:
    explicit FixedPatternScanner(std::span<const uint8_t> pattern)
        : pattern_(pattern.begin(), pattern.end()) {
        const size_t patternSize = pattern_.size();
        skip_.fill(patternSize == 0 ? 1 : patternSize);
        if (patternSize <= 1) return;

        for (size_t i = 0; i + 1 < patternSize; ++i) {
            skip_[pattern_[i]] = patternSize - 1 - i;
        }
    }

    std::optional<size_t> Find(std::span<const uint8_t> bytes) const {
        const size_t patternSize = pattern_.size();
        if (patternSize == 0 || bytes.size() < patternSize) return std::nullopt;

        size_t offset = 0;
        while (offset <= bytes.size() - patternSize) {
            const uint8_t tail = bytes[offset + patternSize - 1];
            if (tail == pattern_[patternSize - 1]) {
                size_t i = patternSize - 1;
                while (i > 0 && bytes[offset + i - 1] == pattern_[i - 1]) {
                    --i;
                }
                if (i == 0) return offset;
            }

            offset += skip_[tail];
        }

        return std::nullopt;
    }

    size_t PatternSize() const { return pattern_.size(); }

private:
    std::vector<uint8_t> pattern_;
    std::array<size_t, 256> skip_{};
};

class UniqueFd {
public:
    UniqueFd() = default;
    explicit UniqueFd(int fd) : fd_(fd) {}
    ~UniqueFd() { if (fd_ >= 0) ::close(fd_); }
    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;

    int get() const { return fd_; }
    explicit operator bool() const { return fd_ >= 0; }

private:
    int fd_ = -1;
};

UniqueFd OpenForSequentialRead(const std::filesystem::path& path) {
    int fd = -1;
    do {
        fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    } while (fd < 0 && errno == EINTR);
    if (fd >= 0) {
        // Bias the cache manager toward readahead for a forward scan; best-effort.
        ::fcntl(fd, F_RDAHEAD, 1);
    }
    return UniqueFd(fd);
}

// Read up to `len` bytes at absolute `offset`, retrying short reads (which for a
// regular file only occur at EOF) and EINTR. Returns bytes read, or -1 on error.
ssize_t ReadFullyAt(int fd, uint8_t* buffer, size_t len, uint64_t offset) {
    size_t total = 0;
    while (total < len) {
        const ssize_t got = ::pread(
            fd, buffer + total, len - total, static_cast<off_t>(offset + total));
        if (got < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (got == 0) break;  // EOF
        total += static_cast<size_t>(got);
    }
    return static_cast<ssize_t>(total);
}

} // namespace

std::optional<size_t> Find(std::span<const uint8_t> bytes, std::span<const uint8_t> pattern) {
    const Stopwatch scanTimer;
    const auto result = FixedPatternScanner(pattern).Find(bytes);
    OSTP_LOG_DEBUG("ByteSearch::Find bytes={} pattern={} matched={} elapsed_ms={:.3f}",
                   bytes.size(), pattern.size(), result.has_value(), scanTimer.ElapsedMs());
    return result;
}

std::optional<uint64_t> FindInFile(
    const std::filesystem::path& path,
    std::span<const uint8_t> pattern,
    size_t chunkBytes) {
    // A whole-file scan is a range scan over [0, EOF). FindInFileRange clamps the
    // requested size to the real file size, so a max sentinel means "to the end".
    return FindInFileRange(
        path, 0, (std::numeric_limits<uint64_t>::max)(), pattern, chunkBytes);
}

std::optional<uint64_t> FindInFileRange(
    const std::filesystem::path& path,
    uint64_t offset,
    uint64_t size,
    std::span<const uint8_t> pattern,
    size_t chunkBytes) {
    const Stopwatch totalTimer;
    FixedPatternScanner scanner(pattern);
    const size_t patternSize = scanner.PatternSize();
    if (patternSize == 0 || size == 0) return std::nullopt;

    UniqueFd file = OpenForSequentialRead(path);
    if (!file) {
        OSTP_LOG_DEBUG("ByteSearch::FindInFileRange open failed path={} offset=0x{:X} size={} errno={}",
                       path.string(), offset, size, errno);
        return std::nullopt;
    }

    struct stat st{};
    if (::fstat(file.get(), &st) != 0 || st.st_size <= 0) {
        OSTP_LOG_DEBUG("ByteSearch::FindInFileRange empty/invalid file path={} offset=0x{:X} size={}",
                       path.string(), offset, size);
        return std::nullopt;
    }

    const uint64_t fileSize = static_cast<uint64_t>(st.st_size);
    if (offset >= fileSize) {
        OSTP_LOG_DEBUG("ByteSearch::FindInFileRange offset past EOF path={} offset=0x{:X} file_size={}",
                       path.string(), offset, fileSize);
        return std::nullopt;
    }

    const uint64_t rangeEnd = offset + (std::min)(size, fileSize - offset);

    chunkBytes = (std::max)(chunkBytes, patternSize);
    // Consecutive chunks overlap by (patternSize - 1) bytes so a match that
    // straddles a chunk boundary is fully contained at the head of the next chunk.
    const uint64_t stride = static_cast<uint64_t>(chunkBytes) - (patternSize - 1);

    std::unique_ptr<uint8_t[]> buffer = std::make_unique_for_overwrite<uint8_t[]>(chunkBytes);

    uint64_t bytesReadTotal = 0;
    size_t chunks = 0;
    uint64_t curStart = offset;

    while (true) {
        const size_t len = static_cast<size_t>(
            (std::min)(static_cast<uint64_t>(chunkBytes), rangeEnd - curStart));

        const ssize_t got = ReadFullyAt(file.get(), buffer.get(), len, curStart);
        if (got < 0) {
            OSTP_LOG_DEBUG("ByteSearch::FindInFileRange read failed path={} offset=0x{:X} errno={}",
                           path.string(), curStart, errno);
            return std::nullopt;
        }
        if (got == 0) break;
        bytesReadTotal += static_cast<uint64_t>(got);
        ++chunks;

        const auto match = scanner.Find(
            std::span<const uint8_t>(buffer.get(), static_cast<size_t>(got)));
        if (match) {
            const uint64_t absoluteMatch = curStart + *match;
            OSTP_LOG_DEBUG("ByteSearch::FindInFileRange matched path={} range=[0x{:X},0x{:X}) match=0x{:X} bytes_read={} chunks={} total_ms={:.3f}",
                           path.string(), offset, rangeEnd, absoluteMatch,
                           bytesReadTotal, chunks, totalTimer.ElapsedMs());
            return absoluteMatch;
        }

        const uint64_t curEnd = curStart + static_cast<uint64_t>(got);
        if (curEnd >= rangeEnd) break;
        if (static_cast<size_t>(got) < len) break;  // short read == EOF
        curStart += stride;
    }

    OSTP_LOG_DEBUG("ByteSearch::FindInFileRange no match path={} range=[0x{:X},0x{:X}) bytes_read={} chunks={} total_ms={:.3f}",
                   path.string(), offset, rangeEnd, bytesReadTotal, chunks, totalTimer.ElapsedMs());
    return std::nullopt;
}

} // namespace OSTPlatform::ByteSearch
