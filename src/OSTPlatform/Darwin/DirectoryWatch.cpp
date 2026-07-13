#include "include/DirectoryWatch.h"

#include "include/Log.h"

#include <CoreServices/CoreServices.h>
#include <dispatch/dispatch.h>
#include <unistd.h>

#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string_view>
#include <utility>

// macOS directory watcher, mirroring the ReadDirectoryChangesW-based Windows
// backend. FSEvents is callback-driven, so a process-wide condition variable
// bridges the async delivery to the synchronous WaitAny() contract: the stream
// runs continuously and each callback appends to the watch's pending queue and
// wakes any WaitAny() blocked on it. Like the Windows path (bWatchSubtree=FALSE),
// only direct children of the watched directory are reported.
namespace OSTPlatform::DirectoryWatch {
namespace {

// One shared mutex/CV pair guards every watch's pending queue and lets WaitAny()
// block across an arbitrary set of watches (the FSEvents analogue of
// WaitForMultipleObjects over per-watch events).
std::mutex& SharedMutex() {
    // Intentionally process-lifetime. Watcher shutdown is driven by global
    // destructors in other translation units, and FSEvents may still deliver
    // an in-flight callback while those destructors join the worker threads.
    // A function-local object would itself be destroyed according to cross-TU
    // registration order, making the callback lock an invalid pthread mutex.
    static auto* mutex = new std::mutex;
    return *mutex;
}
std::condition_variable& SharedCv() {
    static auto* cv = new std::condition_variable;
    return *cv;
}

ChangeAction ClassifyEvent(FSEventStreamEventFlags flags, const char* fullPath) {
    // A rename shows up on both the old and new path with the same flag; probing
    // for existence tells them apart, which matters for atomic-save editors that
    // write a temp file and rename it onto the target.
    if (flags & kFSEventStreamEventFlagItemRenamed) {
        return access(fullPath, F_OK) == 0 ? ChangeAction::RenamedNewName
                                           : ChangeAction::RenamedOldName;
    }
    if (flags & kFSEventStreamEventFlagItemRemoved) return ChangeAction::Removed;
    if (flags & kFSEventStreamEventFlagItemCreated) return ChangeAction::Added;
    return ChangeAction::Modified;
}

void NoOp(void*) {}

} // namespace

struct Watch::Impl {
    std::string directory;      // caller-supplied path, returned by Directory()
    std::string canonicalDir;   // symlink-resolved, no trailing '/', for matching
    bool open = false;
    dispatch_queue_t queue = nullptr;
    FSEventStreamRef stream = nullptr;
    std::vector<Change> pending;  // guarded by SharedMutex()

    ~Impl() { Close(); }

    // FSEvents callback. A static member so it has the plain function-pointer type
    // FSEventStreamCreate wants while still reaching Impl's private members (a free
    // function cannot, since Impl is a private nested type and the header — a fixed
    // contract — can't be given another friend).
    static void StreamCallback(ConstFSEventStreamRef /*stream*/,
                               void* info,
                               size_t numEvents,
                               void* eventPaths,
                               const FSEventStreamEventFlags flags[],
                               const FSEventStreamEventId /*ids*/[]) {
        auto* impl = static_cast<Impl*>(info);
        const char** paths = static_cast<const char**>(eventPaths);

        std::vector<Change> batch;
        batch.reserve(numEvents);
        for (size_t i = 0; i < numEvents; ++i) {
            if (!paths[i]) continue;
            if (auto rel = impl->RelativeChild(paths[i])) {
                batch.push_back({std::move(*rel), ClassifyEvent(flags[i], paths[i])});
            }
        }
        if (batch.empty()) return;

        {
            std::lock_guard<std::mutex> lock(SharedMutex());
            for (auto& change : batch) impl->pending.push_back(std::move(change));
        }
        SharedCv().notify_all();
    }

    // Maps an absolute FSEvents path to a direct child's name, or nullopt when the
    // path is the directory itself or lives in a subdirectory (non-recursive).
    std::optional<std::string> RelativeChild(const char* fullPath) const {
        const std::string_view path(fullPath);
        if (path.size() < canonicalDir.size() + 2) return std::nullopt;
        if (path.substr(0, canonicalDir.size()) != canonicalDir) return std::nullopt;
        if (path[canonicalDir.size()] != '/') return std::nullopt;

        const std::string_view rel = path.substr(canonicalDir.size() + 1);
        if (rel.empty() || rel.find('/') != std::string_view::npos) return std::nullopt;
        return std::string(rel);
    }

    bool Open(const std::string& dir) {
        Close();

        directory = dir;
        std::error_code ec;
        const std::filesystem::path resolved = std::filesystem::canonical(dir, ec);
        canonicalDir = ec ? dir : resolved.string();
        while (canonicalDir.size() > 1 && canonicalDir.back() == '/') canonicalDir.pop_back();

        CFStringRef cfPath = CFStringCreateWithCString(
            kCFAllocatorDefault, canonicalDir.c_str(), kCFStringEncodingUTF8);
        if (!cfPath) {
            OSTP_LOG_WARN("DirectoryWatch: CFString create failed for '{}'", dir);
            return false;
        }
        CFArrayRef paths = CFArrayCreate(
            kCFAllocatorDefault, reinterpret_cast<const void**>(&cfPath), 1, &kCFTypeArrayCallBacks);

        FSEventStreamContext context{};
        context.info = this;

        stream = FSEventStreamCreate(
            kCFAllocatorDefault,
            &StreamCallback,
            &context,
            paths,
            kFSEventStreamEventIdSinceNow,
            0.1,  // coalescing latency (seconds); the caller debounces at 500 ms
            kFSEventStreamCreateFlagFileEvents | kFSEventStreamCreateFlagNoDefer);

        CFRelease(paths);
        CFRelease(cfPath);

        if (!stream) {
            OSTP_LOG_WARN("DirectoryWatch: FSEventStreamCreate failed for '{}'", dir);
            return false;
        }

        queue = dispatch_queue_create("com.opensteamtool.dirwatch", DISPATCH_QUEUE_SERIAL);
        FSEventStreamSetDispatchQueue(stream, queue);

        if (!FSEventStreamStart(stream)) {
            OSTP_LOG_WARN("DirectoryWatch: FSEventStreamStart failed for '{}'", dir);
            Close();
            return false;
        }

        open = true;
        return true;
    }

    void Close() {
        if (stream) {
            FSEventStreamStop(stream);
            FSEventStreamInvalidate(stream);  // unschedules from the dispatch queue
            FSEventStreamRelease(stream);
            stream = nullptr;
        }
        if (queue) {
            // Flush any callback already in flight on the serial queue so it cannot
            // touch this Impl after Close() returns, then drop the queue.
            dispatch_sync_f(queue, nullptr, &NoOp);
            dispatch_release(queue);
            queue = nullptr;
        }
        {
            std::lock_guard<std::mutex> lock(SharedMutex());
            pending.clear();
        }
        open = false;
        canonicalDir.clear();
        directory.clear();
    }
};

Watch::Watch() : impl_(std::make_unique<Impl>()) {}
Watch::~Watch() = default;

Watch::Watch(Watch&&) noexcept = default;
Watch& Watch::operator=(Watch&&) noexcept = default;

bool Watch::Open(const std::string& directory, uint32_t /*bufferSize*/) {
    return impl_->Open(directory);
}

bool Watch::IssueRead() {
    // FSEvents streams continuously once started; there is no per-read re-arm as on
    // Windows. Changes that arrive between drains are simply buffered.
    return IsOpen();
}

std::vector<Change> Watch::Drain() {
    std::lock_guard<std::mutex> lock(SharedMutex());
    return std::move(impl_->pending);
}

void Watch::Cancel() {
    impl_->Close();
}

bool Watch::IsOpen() const {
    return impl_ && impl_->open;
}

const std::string& Watch::Directory() const {
    return impl_->directory;
}

WaitResult WaitAny(std::span<Watch*> watches, uint32_t timeoutMs) {
    const auto firstReady = [&]() -> std::optional<size_t> {
        for (size_t i = 0; i < watches.size(); ++i) {
            Watch* watch = watches[i];
            if (watch && watch->impl_ && watch->impl_->open && !watch->impl_->pending.empty()) {
                return i;
            }
        }
        return std::nullopt;
    };

    bool anyOpen = false;
    for (Watch* watch : watches) {
        if (watch && watch->IsOpen()) { anyOpen = true; break; }
    }
    if (!anyOpen) return {WaitStatus::Failed, 0};

    std::unique_lock<std::mutex> lock(SharedMutex());
    if (auto index = firstReady()) return {WaitStatus::Signaled, *index};

    const bool woke = SharedCv().wait_for(
        lock, std::chrono::milliseconds(timeoutMs), [&] { return firstReady().has_value(); });
    if (!woke) return {WaitStatus::Timeout, 0};

    if (auto index = firstReady()) return {WaitStatus::Signaled, *index};
    return {WaitStatus::Timeout, 0};
}

} // namespace OSTPlatform::DirectoryWatch
