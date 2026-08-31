// AnonXMusic C++ port — Phase 3
// youtube.cpp — implementation of the YouTube service (yt-dlp subprocess).

#include "anonx/youtube.hpp"

#include "anonx/logger.hpp"
#include "anonx/utils.hpp"

#include <nlohmann/json.hpp>

#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

#include <dirent.h>     // opendir / readdir
#include <signal.h>     // kill
#include <sys/wait.h>   // WIFEXITED / WEXITSTATUS / waitpid
#include <time.h>       // nanosleep
#include <unistd.h>     // access / fork / pipe / execl

namespace anonx {
namespace {

using nlohmann::json;

// ---- small JSON accessors (guarded, never throw) ----
std::string jstr(const json& j, const char* key) {
    if (j.contains(key)) {
        const json& v = j[key];
        if (v.is_string()) return v.get<std::string>();
    }
    return "";
}

double jnum(const json& j, const char* key) {
    if (j.contains(key)) {
        const json& v = j[key];
        if (v.is_number()) return v.get<double>();
    }
    return 0.0;
}

// ---- string / time helpers ----
std::string formatSeconds(int s) {
    if (s < 0) s = 0;
    const int h = s / 3600, m = (s % 3600) / 60, sec = s % 60;
    char buf[32];
    if (h > 0) std::snprintf(buf, sizeof(buf), "%d:%02d:%02d", h, m, sec);
    else       std::snprintf(buf, sizeof(buf), "%d:%02d", m, sec);
    return std::string(buf);
}

// Parse "H:M:S" / "M:S" / "S" -> seconds (mirrors utils.to_seconds).
int toSeconds(const std::string& t) {
    std::vector<int> parts;
    std::stringstream ss(t);
    std::string p;
    while (std::getline(ss, p, ':')) {
        try {
            parts.push_back(std::stoi(p));
        } catch (...) {
            return 0;
        }
    }
    int total = 0, mul = 1;
    for (auto it = parts.rbegin(); it != parts.rend(); ++it) {
        total += *it * mul;
        mul *= 60;
    }
    return total;
}

// Truncate to at most `maxCp` UTF-8 code points without splitting a character.
std::string utf8Truncate(const std::string& s, std::size_t maxCp) {
    std::size_t cp = 0, i = 0;
    while (i < s.size() && cp < maxCp) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        std::size_t len = 1;
        if (c >= 0xF0) len = 4;
        else if (c >= 0xE0) len = 3;
        else if (c >= 0xC0) len = 2;
        i += len;
        ++cp;
    }
    if (i > s.size()) i = s.size();
    return s.substr(0, i);
}

std::string stripQuery(std::string u) {
    const auto p = u.find('?');
    if (p != std::string::npos) u = u.substr(0, p);
    return u;
}



bool fileExists(const std::string& path) {
    return ::access(path.c_str(), F_OK) == 0;
}

// Run `cmd` and capture its stdout. Used by the one-shot JSON calls; the
// streaming download needs a killable child instead (see Child below).
std::string runCapture(const std::string& cmd) {
    std::string out;
    std::FILE* pipe = ::popen(cmd.c_str(), "r");
    if (!pipe)
        return out;
    char buf[4096];
    std::size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), pipe)) > 0) {
        out.append(buf, n);
    }
    ::pclose(pipe);
    return out;
}

// ---------------------------------------------------------------------------
// A child process we can both READ FROM and KILL (Phase 7.2)
//
// popen() is enough for the one-shot JSON calls, but it hides the pid, and
// without a pid a download cannot be cancelled. So the streaming path forks
// explicitly: the child gets its own process group, which is what lets a cancel
// take yt-dlp's own children (ffmpeg) down with it instead of orphaning them.
// ---------------------------------------------------------------------------
class Child {
public:
    // Start `cmd` under /bin/sh. Returns false if the fork or pipe failed.
    bool start(const std::string& cmd) {
        int fds[2];
        if (::pipe(fds) != 0)
            return false;

        pid_ = ::fork();
        if (pid_ < 0) {
            ::close(fds[0]);
            ::close(fds[1]);
            return false;
        }

        if (pid_ == 0) {
            // --- child ---
            ::setpgid(0, 0);              // own group, so one kill reaches all
            ::close(fds[0]);
            ::dup2(fds[1], STDOUT_FILENO);
            ::close(fds[1]);
            ::execl("/bin/sh", "sh", "-c", cmd.c_str(), static_cast<char*>(nullptr));
            ::_exit(127);                 // exec failed
        }

        // --- parent ---
        ::setpgid(pid_, pid_);            // race-free: both sides set the group
        ::close(fds[1]);
        out_ = ::fdopen(fds[0], "r");
        if (!out_) {
            ::close(fds[0]);
            return false;
        }
        return true;
    }

    // Next line of the child's stdout, without the newline. False at EOF.
    bool readLine(std::string& line) {
        if (!out_)
            return false;
        line.clear();
        char buf[512];
        while (std::fgets(buf, sizeof(buf), out_)) {
            line += buf;
            if (!line.empty() && line.back() == '\n') {
                line.pop_back();
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                return true;
            }
        }
        return !line.empty();   // a final line with no newline still counts
    }

    // SIGTERM the group, give it a moment, then SIGKILL whatever is left.
    void kill() {
        if (pid_ <= 0)
            return;
        ::kill(-pid_, SIGTERM);
        for (int i = 0; i < 20; ++i) {    // up to ~200 ms
            int status = 0;
            if (::waitpid(pid_, &status, WNOHANG) == pid_) {
                pid_ = -1;
                return;
            }
            struct timespec ts = {0, 10 * 1000 * 1000};   // 10 ms
            ::nanosleep(&ts, nullptr);
        }
        ::kill(-pid_, SIGKILL);
    }

    // Close the pipe and reap the child. Returns its exit code, or -1 if it did
    // not exit normally (which includes "we killed it").
    int wait() {
        if (out_) {
            std::fclose(out_);
            out_ = nullptr;
        }
        if (pid_ <= 0)
            return -1;
        int status = 0;
        const pid_t r = ::waitpid(pid_, &status, 0);
        pid_ = -1;
        if (r <= 0 || !WIFEXITED(status))
            return -1;
        return WEXITSTATUS(status);
    }

    ~Child() {
        if (pid_ > 0) {
            kill();
            wait();
        } else if (out_) {
            std::fclose(out_);
        }
    }

private:
    pid_t       pid_ = -1;
    std::FILE*  out_ = nullptr;
};

}  // namespace

YouTube::YouTube() : rng_(std::random_device{}()) {
    // Ported from the Python regexes. Compilation is guarded so a bad build of
    // std::regex can never crash the process — valid()/invalid() just degrade.
    try {
        regex_ = std::regex(
            R"((https?://)?(www\.|m\.|music\.)?(youtube\.com/(watch\?v=|shorts/|playlist\?list=)|youtu\.be/)([A-Za-z0-9_-]{11}|PL[A-Za-z0-9_-]+)([&?][^\s]*)?)");
        iregex_ = std::regex(
            R"(https?://(?:www\.|m\.|music\.)?(?:youtube\.com|youtu\.be)(?!/(watch\?v=[A-Za-z0-9_-]{11}|shorts/[A-Za-z0-9_-]{11}|playlist\?list=PL[A-Za-z0-9_-]+|[A-Za-z0-9_-]{11}))\S*)");
        regexOk_ = true;
    } catch (const std::regex_error&) {
        regexOk_ = false;
    }
}

std::string YouTube::runCommand(const std::string& cmd) {
    return runCapture(cmd);
}

std::optional<Track> YouTube::parseTrackJson(const std::string& jsonText, bool video) {
    json j = json::parse(jsonText, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object()) return std::nullopt;

    const std::string id = jstr(j, "id");
    if (id.empty()) return std::nullopt;

    Track t;
    t.id = id;
    t.video = video;

    // channel name: prefer "channel", fall back to "uploader"
    t.channel_name = jstr(j, "channel");
    if (t.channel_name.empty()) t.channel_name = jstr(j, "uploader");

    // duration: yt-dlp emits seconds as a number; "duration_string" is "M:SS"
    int secs = 0;
    if (j.contains("duration") && j["duration"].is_number()) {
        secs = static_cast<int>(jnum(j, "duration"));
    }
    const std::string durStr = jstr(j, "duration_string");
    if (secs == 0 && !durStr.empty()) secs = toSeconds(durStr);
    t.duration_sec = secs;
    t.duration = !durStr.empty() ? durStr : formatSeconds(secs);

    // title — faithfully truncated to 25 code points, as in the original
    t.title = utf8Truncate(jstr(j, "title"), 25);

    // canonical single-video URL
    t.url = "https://www.youtube.com/watch?v=" + id;

    // thumbnail: scalar "thumbnail" (query stripped) if present, else derived
    const std::string thumb = jstr(j, "thumbnail");
    t.thumbnail = !thumb.empty()
                      ? stripQuery(thumb)
                      : ("https://i.ytimg.com/vi/" + id + "/hqdefault.jpg");

    // view_count: yt-dlp gives a raw number; store it as a string ("" if absent)
    if (j.contains("view_count") && j["view_count"].is_number()) {
        t.view_count = std::to_string(static_cast<long long>(jnum(j, "view_count")));
    }

    return t;
}

std::optional<Track> YouTube::search(const std::string& query,
                                     std::int64_t messageId, bool video) {
    const std::string cmd = "yt-dlp " + anonx::utils::shellQuote("ytsearch1:" + query) +
                            " --dump-json --no-download --no-warnings 2>/dev/null";
    const std::string out = runCommand(cmd);

    std::istringstream iss(out);
    std::string line;
    while (std::getline(iss, line)) {
        if (line.find_first_not_of(" \t\r\n") == std::string::npos) continue;
        auto t = parseTrackJson(line, video);
        if (t) {
            t->message_id = messageId;
            return t;
        }
        break;  // first non-empty line was not usable
    }
    return std::nullopt;
}

std::vector<Track> YouTube::playlist(const std::string& url, int limit,
                                     const std::string& user, bool video) {
    std::vector<Track> tracks;
    if (limit <= 0) return tracks;

    const std::string cmd = "yt-dlp " + anonx::utils::shellQuote(url) +
                            " --flat-playlist --dump-json --no-download --no-warnings 2>/dev/null";
    const std::string out = runCommand(cmd);

    std::istringstream iss(out);
    std::string line;
    while (std::getline(iss, line) && static_cast<int>(tracks.size()) < limit) {
        if (line.find_first_not_of(" \t\r\n") == std::string::npos) continue;
        auto t = parseTrackJson(line, video);
        if (t) {
            t->user = user;
            tracks.push_back(*t);
        }
    }
    return tracks;
}

// ---------------------------------------------------------------------------
// Download progress helpers (Phase 7.2) — all pure, all unit-tested
// ---------------------------------------------------------------------------

std::optional<DownloadProgress> YouTube::parseProgressLine(const std::string& line) {
    const std::size_t plen = std::strlen(kProgressPrefix);
    if (line.compare(0, plen, kProgressPrefix) != 0)
        return std::nullopt;

    // Split the rest on whitespace. yt-dlp writes "NA" for a field it does not
    // know yet, which parses as "unknown" rather than zero.
    std::istringstream iss(line.substr(plen));
    std::vector<std::string> tok;
    std::string t;
    while (iss >> t)
        tok.push_back(t);
    if (tok.empty())
        return std::nullopt;

    auto num = [](const std::string& s, double& out) {
        if (s.empty() || s == "NA" || s == "None")
            return false;
        char* end = nullptr;
        const double v = std::strtod(s.c_str(), &end);
        if (end == s.c_str() || !std::isfinite(v))
            return false;
        out = v;
        return true;
    };

    DownloadProgress p;
    double v = 0.0;
    if (num(tok[0], v) && v >= 0)
        p.downloaded = static_cast<std::int64_t>(v);
    // Exact total first, then yt-dlp's estimate for a stream that only reports one.
    if (tok.size() > 1 && num(tok[1], v) && v > 0)
        p.total = static_cast<std::int64_t>(v);
    else if (tok.size() > 2 && num(tok[2], v) && v > 0)
        p.total = static_cast<std::int64_t>(v);
    if (tok.size() > 3 && num(tok[3], v) && v >= 0)
        p.speed = v;
    if (tok.size() > 4 && num(tok[4], v) && v >= 0)
        p.eta = static_cast<int>(v);
    return p;
}

std::string YouTube::humanBytes(std::int64_t bytes) {
    if (bytes < 0)
        bytes = 0;
    const char* units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    double v = static_cast<double>(bytes);
    int u = 0;
    while (v >= 1024.0 && u < 4) {
        v /= 1024.0;
        ++u;
    }
    char buf[64];
    // Whole bytes have no fractional part worth printing; anything scaled does.
    if (u == 0)
        std::snprintf(buf, sizeof(buf), "%lld%s", static_cast<long long>(bytes), units[0]);
    else
        std::snprintf(buf, sizeof(buf), "%.1f%s", v, units[u]);
    return buf;
}

std::string YouTube::humanRate(double bytesPerSecond) {
    if (!(bytesPerSecond > 0.0))
        return "0B";
    return humanBytes(static_cast<std::int64_t>(bytesPerSecond));
}

std::string YouTube::formatEta(int seconds) {
    if (seconds < 0)
        return "--:--";
    const int h = seconds / 3600;
    const int m = (seconds % 3600) / 60;
    const int s = seconds % 60;
    char buf[32];
    if (h > 0)
        std::snprintf(buf, sizeof(buf), "%d:%02d:%02d", h, m, s);
    else
        std::snprintf(buf, sizeof(buf), "%02d:%02d", m, s);
    return buf;
}

DownloadResult YouTube::streamCommand(const std::string& cmd,
                                      const std::string& expectFile,
                                      const ProgressSink& onProgress,
                                      std::int64_t maxBytes) {
    DownloadResult res;

    Child child;
    if (!child.start(cmd)) {
        res.status = DownloadStatus::Failed;
        return res;
    }

    // A stop decided inside the read loop: the child is killed, but its exit
    // code is then meaningless, so the reason is remembered here.
    std::optional<DownloadStatus> stopped;

    std::string line;
    while (child.readLine(line)) {
        auto p = parseProgressLine(line);
        if (!p)
            continue;   // yt-dlp prints plenty of other lines

        // The ceiling is enforced here rather than with yt-dlp's own
        // --max-filesize, because that exits cleanly with no file and would be
        // indistinguishable from an ordinary failure.
        if (maxBytes > 0 && (p->total > maxBytes || p->downloaded > maxBytes)) {
            stopped = DownloadStatus::TooLarge;
            break;
        }
        if (onProgress && !onProgress(*p)) {
            stopped = DownloadStatus::Cancelled;
            break;
        }
    }

    if (stopped) {
        child.kill();
        child.wait();
        res.status = *stopped;
        return res;
    }

    const int code = child.wait();
    if (code == 0 && (expectFile.empty() || fileExists(expectFile))) {
        res.status = DownloadStatus::Ok;
        res.path   = expectFile;
        return res;
    }
    res.status = DownloadStatus::Failed;
    return res;
}

DownloadResult YouTube::downloadStream(const std::string& videoId, bool video,
                                       const ProgressSink& onProgress,
                                       std::int64_t maxBytes) {
    const std::string ext = video ? "mp4" : "webm";
    const std::string filename = std::string(kDownloadsDir) + "/" + videoId + "." + ext;

    // CRITICAL: never re-download something we already have. No subprocess, and
    // no progress either — there is nothing to report.
    if (fileExists(filename))
        return {DownloadStatus::Ok, filename};

    const std::string url = base_ + videoId;
    const std::string selector =
        video ? "(bestvideo[height<=?720][width<=?1280][ext=mp4])+(bestaudio)"
              : "bestaudio[ext=webm][acodec=opus]";

    std::string cmd = "yt-dlp " + anonx::utils::shellQuote(url) +
                      " --no-playlist --geo-bypass --no-warnings --no-check-certificate";
    cmd += " -f " + anonx::utils::shellQuote(selector);
    if (video) cmd += " --merge-output-format mp4";
    cmd += " -o " + anonx::utils::shellQuote(std::string(kDownloadsDir) + "/%(id)s.%(ext)s");

    const std::string cookie = pickCookie();
    if (!cookie.empty()) cmd += " --cookies " + anonx::utils::shellQuote(cookie);

    // One progress line per update instead of a redrawn status bar, and no colour
    // escapes in the middle of it.
    cmd += " --newline --no-colors --progress --progress-template " +
           anonx::utils::shellQuote(kProgressTemplate);
    cmd += " 2>/dev/null";

    return streamCommand(cmd, filename, onProgress, maxBytes);
}

std::optional<std::string> YouTube::download(const std::string& videoId, bool video) {
    const DownloadResult r = downloadStream(videoId, video, nullptr);
    if (r.ok())
        return r.path;
    return std::nullopt;
}

std::string YouTube::pickCookie() {
    if (!cookiesScanned_) {
        const char* dirs[] = {"cookies", "anony/cookies"};
        for (const char* dir : dirs) {
            DIR* d = ::opendir(dir);
            if (!d) continue;
            struct dirent* e;
            while ((e = ::readdir(d)) != nullptr) {
                const std::string name = e->d_name;
                if (name.size() > 4 && name.compare(name.size() - 4, 4, ".txt") == 0) {
                    cookies_.push_back(std::string(dir) + "/" + name);
                }
            }
            ::closedir(d);
        }
        cookiesScanned_ = true;
    }

    if (cookies_.empty()) {
        if (!warnedNoCookies_) {
            warnedNoCookies_ = true;
            Logger("anonx.youtube").warning("Cookies are missing; downloads might fail.");
        }
        return "";
    }

    std::uniform_int_distribution<std::size_t> dist(0, cookies_.size() - 1);
    return cookies_[dist(rng_)];
}

bool YouTube::valid(const std::string& url) const {
    if (!regexOk_) return false;
    return std::regex_search(url, regex_, std::regex_constants::match_continuous);
}

bool YouTube::invalid(const std::string& url) const {
    if (!regexOk_) return false;
    return std::regex_search(url, iregex_, std::regex_constants::match_continuous);
}

}  // namespace anonx
