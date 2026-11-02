// Disc files and save copies on a local game server; see remote_store.h.

#include "remote_store.h"

#include "runtime_config.h"
#include "runtime_log.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <iterator>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <string_view>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <utility>

#if !defined(_WIN32)
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace RemoteStore {
namespace {

// Disc reads are served from 512 KiB pieces of the server's files. Up to 64 of them (32 MiB) stay
// in memory, and each read asks two background threads for the next 4 MiB so sequential readers
// (movies, streamed music) find it ready. Smaller pieces keep the wait short when a read does
// miss.
constexpr uint64_t kChunkBytes = 512u << 10;
constexpr size_t kMaxCachedChunks = 64;
constexpr uint64_t kReadAheadChunks = 8;
constexpr int kPrefetchThreads = 2;
constexpr int kConnectTimeoutMs = 3000;
constexpr int kDataTimeoutMs = 10000;
constexpr int kManifestTimeoutMs = 8000;
// The saves the server keeps, relative to the NAND root: every title's save data.
constexpr std::string_view kSaveScope = "title/00010004";
constexpr auto kSaveScanInterval = std::chrono::seconds(5);

struct ServerUrl {
    std::string host;
    std::string port = "80";
    std::string basePath; // without a trailing slash
};

bool ParseUrl(const std::string& text, ServerUrl& url) {
    constexpr std::string_view kScheme = "http://";
    if (text.compare(0, kScheme.size(), kScheme) != 0) {
        return false;
    }
    std::string rest = text.substr(kScheme.size());
    const size_t slash = rest.find('/');
    std::string authority = rest.substr(0, slash);
    if (slash != std::string::npos) {
        url.basePath = rest.substr(slash);
        while (!url.basePath.empty() && url.basePath.back() == '/') {
            url.basePath.pop_back();
        }
    }
    const size_t colon = authority.rfind(':');
    if (colon != std::string::npos && authority.find(']') == std::string::npos) {
        url.port = authority.substr(colon + 1);
        authority.resize(colon);
    }
    url.host = authority;
    return !url.host.empty() && !url.port.empty();
}

const ServerUrl* Server() {
    static const ServerUrl* server = [] () -> const ServerUrl* {
        const std::string text = RuntimeConfigFile::RemoteUrl();
        if (text.empty()) {
            return nullptr;
        }
        static ServerUrl parsed;
        if (!ParseUrl(text, parsed)) {
            RT_LOGF("remote", "ignoring [remote] url \"%s\": only http://host:port is supported\n", text.c_str());
            return nullptr;
        }
        return &parsed;
    }();
    return server;
}

std::string PercentEncodePath(std::string_view path) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(path.size());
    for (const unsigned char c : path) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
            c == '_' || c == '.' || c == '~' || c == '/') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(kHex[c >> 4]);
            out.push_back(kHex[c & 15]);
        }
    }
    return out;
}

std::string GamePath(const std::string& gameCode, std::string_view what) {
    return Server()->basePath + "/g/" + PercentEncodePath(gameCode) + "/" + std::string(what);
}

struct Response {
    int status = 0;
    std::vector<uint8_t> body;
};

#if !defined(_WIN32)

class Socket {
public:
    explicit Socket(int fd = -1) : m_fd(fd) {}
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    ~Socket() {
        if (m_fd >= 0) close(m_fd);
    }
    int Get() const { return m_fd; }

private:
    int m_fd;
};

int ConnectTo(const ServerUrl& server, int timeoutMs, std::string& error) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* results = nullptr;
    if (const int rc = getaddrinfo(server.host.c_str(), server.port.c_str(), &hints, &results); rc != 0) {
        error = std::string("cannot resolve ") + server.host + ": " + gai_strerror(rc);
        return -1;
    }
    int connected = -1;
    for (addrinfo* ai = results; ai != nullptr && connected < 0; ai = ai->ai_next) {
        const int fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        const int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        int rc = connect(fd, ai->ai_addr, ai->ai_addrlen);
        if (rc != 0 && errno == EINPROGRESS) {
            pollfd pfd{fd, POLLOUT, 0};
            rc = poll(&pfd, 1, kConnectTimeoutMs);
            int soError = 0;
            socklen_t len = sizeof(soError);
            if (rc == 1 && getsockopt(fd, SOL_SOCKET, SO_ERROR, &soError, &len) == 0 && soError == 0) {
                rc = 0;
            } else {
                error = rc == 0 ? "connection timed out" : std::string("cannot connect: ") + std::strerror(soError ? soError : errno);
                rc = -1;
            }
        } else if (rc != 0) {
            error = std::string("cannot connect: ") + std::strerror(errno);
        }
        if (rc != 0) {
            close(fd);
            continue;
        }
        fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
        timeval tv{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
#ifdef SO_NOSIGPIPE
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
        connected = fd;
    }
    freeaddrinfo(results);
    return connected;
}

bool SendAll(int fd, const void* data, size_t size) {
    const char* p = static_cast<const char*>(data);
    while (size > 0) {
#ifdef MSG_NOSIGNAL
        const ssize_t sent = send(fd, p, size, MSG_NOSIGNAL);
#else
        const ssize_t sent = send(fd, p, size, 0);
#endif
        if (sent <= 0) {
            if (sent < 0 && errno == EINTR) continue;
            return false;
        }
        p += sent;
        size -= static_cast<size_t>(sent);
    }
    return true;
}

bool Request(const char* method, const std::string& target, const std::string& extraHeaders,
             const std::vector<uint8_t>* body, int timeoutMs, Response& response, std::string& error) {
    const ServerUrl* server = Server();
    if (server == nullptr) {
        error = "no game server configured";
        return false;
    }
    Socket socket(ConnectTo(*server, timeoutMs, error));
    if (socket.Get() < 0) {
        return false;
    }
    std::string head = std::string(method) + " " + target + " HTTP/1.1\r\nHost: " + server->host + ":" +
                       server->port + "\r\nConnection: close\r\nX-Wii-Token: " +
                       RuntimeConfigFile::RemoteToken() + "\r\n" + extraHeaders;
    head += "Content-Length: " + std::to_string(body ? body->size() : 0) + "\r\n\r\n";
    if (!SendAll(socket.Get(), head.data(), head.size()) ||
        (body && !body->empty() && !SendAll(socket.Get(), body->data(), body->size()))) {
        error = "the connection dropped while sending";
        return false;
    }

    std::vector<uint8_t> buffer;
    size_t headerEnd = std::string::npos;
    uint8_t chunk[64 * 1024];
    long long contentLength = -1;
    for (;;) {
        const ssize_t got = recv(socket.Get(), chunk, sizeof(chunk), 0);
        if (got < 0 && errno == EINTR) continue;
        if (got < 0) {
            error = errno == EAGAIN || errno == EWOULDBLOCK ? "the server stopped answering" : std::string("receive failed: ") + std::strerror(errno);
            return false;
        }
        if (got == 0) break;
        buffer.insert(buffer.end(), chunk, chunk + got);
        if (headerEnd == std::string::npos) {
            const std::string_view view(reinterpret_cast<const char*>(buffer.data()), buffer.size());
            headerEnd = view.find("\r\n\r\n");
            if (headerEnd == std::string::npos) {
                if (buffer.size() > 64 * 1024) {
                    error = "malformed reply";
                    return false;
                }
                continue;
            }
            std::string headers(view.substr(0, headerEnd));
            if (headers.compare(0, 5, "HTTP/") != 0 || headers.size() < 12) {
                error = "malformed reply";
                return false;
            }
            response.status = std::atoi(headers.c_str() + 9);
            std::string lower = headers;
            std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (const size_t at = lower.find("\r\ncontent-length:"); at != std::string::npos) {
                contentLength = std::atoll(lower.c_str() + at + 17);
            }
        }
        if (contentLength >= 0 && buffer.size() - (headerEnd + 4) >= static_cast<size_t>(contentLength)) {
            break;
        }
    }
    if (headerEnd == std::string::npos) {
        error = "the server closed the connection without replying";
        return false;
    }
    response.body.assign(buffer.begin() + static_cast<std::ptrdiff_t>(headerEnd + 4), buffer.end());
    if (contentLength >= 0) {
        if (response.body.size() < static_cast<size_t>(contentLength)) {
            error = "the reply was cut short";
            return false;
        }
        response.body.resize(static_cast<size_t>(contentLength));
    }
    return true;
}

#else

bool Request(const char*, const std::string&, const std::string&, const std::vector<uint8_t>*, int, Response&,
             std::string& error) {
    error = "the game server is not supported on this platform";
    return false;
}

#endif

// ---------------------------------------------------------------------------------------------
// Disc files

std::mutex g_mutex;
std::condition_variable g_changed;
std::string g_gameCode;
std::unordered_map<std::string, uint64_t> g_sizes; // relative path -> size

struct Chunk {
    std::vector<uint8_t> data;
    bool ready = false;
    bool failed = false;
};
std::unordered_map<std::string, std::shared_ptr<Chunk>> g_chunks;
std::list<std::string> g_recent; // most recently used first
std::deque<std::pair<std::string, uint64_t>> g_prefetch;
bool g_prefetcherStarted = false;

std::string ChunkKey(const std::string& path, uint64_t index) {
    return path + "#" + std::to_string(index);
}

bool FetchChunk(const std::string& path, uint64_t index, uint64_t fileSize, std::vector<uint8_t>& data,
                std::string& error) {
    const uint64_t first = index * kChunkBytes;
    const uint64_t last = std::min(fileSize, first + kChunkBytes) - 1;
    Response response;
    const std::string range = "Range: bytes=" + std::to_string(first) + "-" + std::to_string(last) + "\r\n";
    if (!Request("GET", GamePath(g_gameCode, "data/") + PercentEncodePath(path), range, nullptr, kDataTimeoutMs,
                 response, error)) {
        return false;
    }
    const uint64_t expected = last - first + 1;
    if (response.status == 200 && response.body.size() == fileSize) {
        response.body.erase(response.body.begin() + static_cast<std::ptrdiff_t>(last + 1), response.body.end());
        response.body.erase(response.body.begin(), response.body.begin() + static_cast<std::ptrdiff_t>(first));
    } else if (response.status != 206) {
        error = "the server answered " + std::to_string(response.status);
        return false;
    }
    if (response.body.size() != expected) {
        error = "the server sent " + std::to_string(response.body.size()) + " bytes instead of " + std::to_string(expected);
        return false;
    }
    data = std::move(response.body);
    return true;
}

void EvictLocked() {
    size_t ready = 0;
    for (const auto& [key, chunk] : g_chunks) {
        if (chunk->ready) ++ready;
    }
    for (auto it = g_recent.end(); ready > kMaxCachedChunks && it != g_recent.begin();) {
        --it;
        const auto found = g_chunks.find(*it);
        if (found != g_chunks.end() && found->second->ready) {
            g_chunks.erase(found);
            it = g_recent.erase(it);
            --ready;
        }
    }
}

void TouchLocked(const std::string& key) {
    g_recent.remove(key);
    g_recent.push_front(key);
}

// The chunk, fetching it unless it is cached or another thread is already fetching it.
std::shared_ptr<Chunk> GetChunk(const std::string& path, uint64_t index, uint64_t fileSize, std::string& error) {
    const std::string key = ChunkKey(path, index);
    std::unique_lock lock(g_mutex);
    for (;;) {
        auto found = g_chunks.find(key);
        if (found == g_chunks.end()) break;
        std::shared_ptr<Chunk> chunk = found->second;
        if (chunk->ready) {
            TouchLocked(key);
            return chunk;
        }
        if (chunk->failed) {
            g_chunks.erase(found);
            break;
        }
        g_changed.wait(lock);
    }
    auto chunk = std::make_shared<Chunk>();
    g_chunks[key] = chunk;
    lock.unlock();

    std::vector<uint8_t> data;
    const bool ok = FetchChunk(path, index, fileSize, data, error);

    lock.lock();
    if (ok) {
        chunk->data = std::move(data);
        chunk->ready = true;
        TouchLocked(key);
        EvictLocked();
    } else {
        chunk->failed = true;
    }
    g_changed.notify_all();
    return ok ? chunk : nullptr;
}

void PrefetchLoop() {
    for (;;) {
        std::pair<std::string, uint64_t> job;
        uint64_t fileSize = 0;
        {
            std::unique_lock lock(g_mutex);
            g_changed.wait(lock, [] { return !g_prefetch.empty(); });
            job = std::move(g_prefetch.front());
            g_prefetch.pop_front();
            if (g_chunks.count(ChunkKey(job.first, job.second)) != 0) continue;
            fileSize = g_sizes[job.first];
        }
        std::string error;
        GetChunk(job.first, job.second, fileSize, error); // a failed read-ahead is retried on demand
    }
}

void QueueReadAhead(const std::string& path, uint64_t afterIndex, uint64_t fileSize) {
    std::lock_guard lock(g_mutex);
    if (!g_prefetcherStarted) {
        g_prefetcherStarted = true;
        for (int i = 0; i < kPrefetchThreads; ++i) {
            std::thread(PrefetchLoop).detach();
        }
    }
    // The newest read decides what is needed next: queue its pieces nearest first, ahead of
    // anything left over from earlier reads, and drop the oldest leftovers.
    std::deque<std::pair<std::string, uint64_t>> wanted;
    for (uint64_t i = 1; i <= kReadAheadChunks; ++i) {
        const uint64_t index = afterIndex + i;
        if (index * kChunkBytes >= fileSize) break;
        if (g_chunks.count(ChunkKey(path, index)) != 0) continue;
        wanted.emplace_back(path, index);
    }
    for (auto& job : g_prefetch) {
        if (std::find(wanted.begin(), wanted.end(), job) == wanted.end()) wanted.push_back(std::move(job));
    }
    while (wanted.size() > 2 * kReadAheadChunks) wanted.pop_back();
    g_prefetch = std::move(wanted);
    g_changed.notify_all();
}

// ---------------------------------------------------------------------------------------------
// Saves

uint64_t Fnv1a64(const std::vector<uint8_t>& data) {
    uint64_t hash = 0xcbf29ce484222325ull;
    for (const uint8_t byte : data) {
        hash ^= byte;
        hash *= 0x100000001b3ull;
    }
    return hash;
}

std::string HashText(uint64_t hash) {
    char text[17];
    std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(hash));
    return text;
}

bool SafeRelativePath(const std::string& path) {
    if (path.empty() || path.front() == '/' || path.find('\\') != std::string::npos) return false;
    size_t start = 0;
    while (start <= path.size()) {
        const size_t end = std::min(path.find('/', start), path.size());
        const std::string_view part(path.data() + start, end - start);
        if (part.empty() || part == "." || part == "..") return false;
        start = end + 1;
    }
    return true;
}

bool ReadWholeFile(const fs::path& path, std::vector<uint8_t>& data) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    data.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    return !file.bad();
}

bool WriteFileAtomically(const fs::path& path, const std::vector<uint8_t>& data) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    const fs::path temporary = path.string() + ".remote-tmp";
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        if (!file) return false;
        file.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        if (!file) return false;
    }
    fs::rename(temporary, path, ec);
    return !ec;
}

struct SaveState {
    uintmax_t size = 0;
    fs::file_time_type modified{};
    bool operator==(const SaveState& other) const { return size == other.size && modified == other.modified; }
    bool operator!=(const SaveState& other) const { return !(*this == other); }
};

void SaveSyncLoop(fs::path nandRoot, std::string gameCode, std::map<std::string, std::string> serverHashes) {
    std::map<std::string, SaveState> previousScan;
    std::map<std::string, SaveState> uploaded;
    for (;;) {
        std::this_thread::sleep_for(kSaveScanInterval);
        std::map<std::string, SaveState> scan;
        std::error_code ec;
        const fs::path scope = nandRoot / fs::path(std::string(kSaveScope));
        for (fs::recursive_directory_iterator it(scope, ec), end; !ec && it != end; it.increment(ec)) {
            std::error_code entryEc;
            if (!it->is_regular_file(entryEc) || entryEc) continue;
            const std::string name = it->path().filename().string();
            if (name.size() > 11 && name.compare(name.size() - 11, 11, ".remote-tmp") == 0) continue;
            SaveState state{it->file_size(entryEc), it->last_write_time(entryEc)};
            if (entryEc) continue;
            const std::string relative = fs::relative(it->path(), nandRoot, entryEc).generic_string();
            if (entryEc || !SafeRelativePath(relative)) continue;
            scan[relative] = state;
        }
        for (const auto& [relative, state] : scan) {
            // Upload a file once it has stayed unchanged for a whole scan interval, so a save the
            // game is still writing is never sent half-written.
            const auto before = previousScan.find(relative);
            if (before == previousScan.end() || before->second != state) continue;
            if (const auto done = uploaded.find(relative); done != uploaded.end() && done->second == state) continue;
            std::vector<uint8_t> data;
            if (!ReadWholeFile(nandRoot / fs::path(relative), data)) continue;
            const std::string hash = HashText(Fnv1a64(data));
            if (serverHashes[relative] != hash) {
                Response response;
                std::string error;
                if (!Request("PUT", GamePath(gameCode, "save/") + PercentEncodePath(relative), "", &data, kDataTimeoutMs,
                             response, error) ||
                    response.status != 200) {
                    RT_LOGF("remote", "could not back up %s to the game server: %s\n", relative.c_str(),
                            error.empty() ? ("status " + std::to_string(response.status)).c_str() : error.c_str());
                    continue; // retried on the next scan
                }
                serverHashes[relative] = hash;
                RT_LOGF("remote", "backed up %s (%zu bytes) to the game server\n", relative.c_str(), data.size());
            }
            uploaded[relative] = state;
        }
        previousScan = std::move(scan);
    }
}

} // namespace

bool Enabled() {
    return Server() != nullptr;
}

std::string ServerText() {
    const ServerUrl* server = Server();
    return server ? "http://" + server->host + ":" + server->port : std::string();
}

bool FetchManifest(const std::string& gameCode, std::vector<ManifestEntry>& entries, std::string& error) {
    if (!Enabled()) {
        error = "no game server configured";
        return false;
    }
    Response response;
    if (!Request("GET", GamePath(gameCode, "manifest"), "", nullptr, kManifestTimeoutMs, response, error)) {
        return false;
    }
    if (response.status != 200) {
        error = "the server answered " + std::to_string(response.status);
        return false;
    }
    entries.clear();
    const std::string text(response.body.begin(), response.body.end());
    size_t start = 0;
    while (start < text.size()) {
        const size_t end = std::min(text.find('\n', start), text.size());
        const std::string line = text.substr(start, end - start);
        start = end + 1;
        const size_t tab = line.find('\t');
        if (tab == std::string::npos) continue;
        ManifestEntry entry;
        entry.size = std::strtoull(line.c_str(), nullptr, 10);
        entry.path = line.substr(tab + 1);
        if (!SafeRelativePath(entry.path) || entry.size > 0xFFFFFFFFull) continue;
        entries.push_back(std::move(entry));
    }
    std::lock_guard lock(g_mutex);
    g_gameCode = gameCode;
    for (const auto& entry : entries) {
        g_sizes[entry.path] = entry.size;
    }
    return true;
}

bool IsRemoteHostPath(const fs::path& hostPath) {
    const std::string text = hostPath.generic_string();
    return text.compare(0, std::strlen(kHostPrefix), kHostPrefix) == 0;
}

fs::path MakeRemoteHostPath(const std::string& relativePath) {
    return fs::path(std::string(kHostPrefix) + relativePath);
}

bool ReadDiscRange(const fs::path& hostPath, uint64_t offset, uint32_t length, std::vector<uint8_t>& destination,
                   std::string& error) {
    const std::string path = hostPath.generic_string().substr(std::strlen(kHostPrefix));
    uint64_t fileSize = 0;
    {
        std::lock_guard lock(g_mutex);
        const auto found = g_sizes.find(path);
        if (found == g_sizes.end()) {
            error = "the game server does not list this file";
            return false;
        }
        fileSize = found->second;
    }
    if (offset > fileSize || length > fileSize - offset) {
        error = "read past the end of the file";
        return false;
    }
    std::vector<uint8_t> staged(length);
    uint64_t done = 0;
    uint64_t lastIndex = offset / kChunkBytes;
    while (done < length) {
        const uint64_t position = offset + done;
        const uint64_t index = position / kChunkBytes;
        std::shared_ptr<Chunk> chunk;
        // The game cannot go on without the data: keep trying for about half a minute.
        static constexpr int kRetryDelaysMs[] = {0, 250, 500, 1000, 2000, 4000, 8000, 15000};
        for (const int delay : kRetryDelaysMs) {
            if (delay) std::this_thread::sleep_for(std::chrono::milliseconds(delay));
            chunk = GetChunk(path, index, fileSize, error);
            if (chunk) break;
            RT_LOGF("remote", "reading %s from %s failed: %s\n", path.c_str(), ServerText().c_str(), error.c_str());
        }
        if (!chunk) {
            error = "cannot reach the game server at " + ServerText() + " (" + error + ")";
            return false;
        }
        const uint64_t inChunk = position - index * kChunkBytes;
        const uint64_t take = std::min<uint64_t>(length - done, chunk->data.size() - inChunk);
        std::memcpy(staged.data() + done, chunk->data.data() + inChunk, take);
        done += take;
        lastIndex = index;
    }
    QueueReadAhead(path, lastIndex, fileSize);
    destination = std::move(staged);
    return true;
}

void StartSaveSync(const fs::path& nandRoot, const std::string& gameCode) {
    static bool started = false;
    if (started || !Enabled()) return;
    started = true;

    // Restore what the device lost. A save that exists here is the one the player last used, so
    // it is never replaced; the server keeps the history if that ever needs undoing by hand.
    std::map<std::string, std::string> serverHashes;
    Response index;
    std::string error;
    if (!Request("GET", GamePath(gameCode, "save-index"), "", nullptr, kManifestTimeoutMs, index, error) ||
        index.status != 200) {
        RT_LOGF("remote", "save backup unavailable this session (%s): saves stay on this device only\n",
                error.empty() ? ("status " + std::to_string(index.status)).c_str() : error.c_str());
        return;
    }
    int restored = 0;
    const std::string text(index.body.begin(), index.body.end());
    size_t start = 0;
    while (start < text.size()) {
        const size_t end = std::min(text.find('\n', start), text.size());
        const std::string line = text.substr(start, end - start);
        start = end + 1;
        const size_t tab1 = line.find('\t');
        const size_t tab2 = tab1 == std::string::npos ? tab1 : line.find('\t', tab1 + 1);
        if (tab2 == std::string::npos) continue;
        const uint64_t size = std::strtoull(line.c_str(), nullptr, 10);
        const std::string hash = line.substr(tab1 + 1, tab2 - tab1 - 1);
        const std::string relative = line.substr(tab2 + 1);
        if (!SafeRelativePath(relative) || relative.compare(0, kSaveScope.size(), kSaveScope) != 0) continue;
        serverHashes[relative] = hash;
        const fs::path local = nandRoot / fs::path(relative);
        std::error_code ec;
        if (fs::exists(local, ec)) continue;
        Response file;
        if (!Request("GET", GamePath(gameCode, "save/") + PercentEncodePath(relative), "", nullptr, kDataTimeoutMs,
                     file, error) ||
            file.status != 200 || file.body.size() != size || HashText(Fnv1a64(file.body)) != hash) {
            RT_LOGF("remote", "could not restore %s from the game server\n", relative.c_str());
            continue;
        }
        if (WriteFileAtomically(local, file.body)) {
            ++restored;
            RT_LOGF("remote", "restored %s (%zu bytes) from the game server\n", relative.c_str(), file.body.size());
        }
    }
    RT_LOGF("remote", "save backup on %s: %zu file(s) there, %d restored to this device\n", ServerText().c_str(),
            serverHashes.size(), restored);
    std::thread(SaveSyncLoop, nandRoot, gameCode, std::move(serverHashes)).detach();
}

} // namespace RemoteStore
