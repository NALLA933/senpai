// AnonXMusic C++ port — Phase 3
// youtube.hpp — YouTube service.
//
// A thin launcher/parser around the `yt-dlp` binary, invoked as a subprocess.
// It never links or reimplements yt-dlp. If yt-dlp is not installed on PATH,
// every operation fails gracefully (search/playlist return empty, download
// returns nullopt) — the program still compiles and boots.
//
// Ported from anony/core/youtube.py, adapted from the py_yt/yt_dlp Python
// libraries to the yt-dlp command-line interface (--dump-json).
//
// Phase 7.2 added the streaming download: the same subprocess, but launched so
// that its progress can be reported line by line, cancelled mid-flight, and
// capped at a maximum size. See DownloadProgress / downloadStream below.

#ifndef ANONX_YOUTUBE_HPP
#define ANONX_YOUTUBE_HPP

#include <cstdint>
#include <functional>
#include <optional>
#include <random>
#include <regex>
#include <string>
#include <vector>

namespace anonx {

// Track metadata, mirroring the Python dataclass `Track`.
struct Track {
    std::string  id;
    std::string  channel_name;
    std::string  duration = "00:00";   // human-readable "M:SS" / "H:MM:SS"
    int          duration_sec = 0;
    std::string  title;
    std::string  url;
    std::string  file_path;            // set once downloaded
    std::int64_t message_id = 0;
    std::int64_t time = 0;
    std::string  thumbnail;
    std::string  user;
    std::string  view_count;
    bool         video = false;
};

// ---------------------------------------------------------------------------
// Download progress (Phase 7.2)
// ---------------------------------------------------------------------------

// One progress report from yt-dlp, already parsed. Every field is optional in
// the wire format ("NA" before the size is known), so "unknown" is encoded
// rather than guessed: 0 bytes, 0 speed, negative eta.
struct DownloadProgress {
    std::int64_t downloaded = 0;   // bytes fetched so far
    std::int64_t total      = 0;   // total bytes, 0 == not known yet
    double       speed      = 0.0; // bytes per second, 0 == not known yet
    int          eta        = -1;  // seconds remaining, < 0 == not known yet

    // 0..100, or 0 while the total size is unknown.
    double percent() const {
        return total > 0 ? 100.0 * static_cast<double>(downloaded) /
                               static_cast<double>(total)
                         : 0.0;
    }
};

// Why a download stopped. `Cancelled` and `TooLarge` are deliberately distinct
// from `Failed`: the first two are things the user must be told about, the third
// is the pre-existing "no file" error path.
enum class DownloadStatus {
    Ok,         // the file is on disk
    Failed,     // yt-dlp errored, or is not installed
    Cancelled,  // the progress sink asked to stop
    TooLarge,   // the file blew past the size ceiling
};

struct DownloadResult {
    DownloadStatus status = DownloadStatus::Failed;
    std::string    path;    // set only when status == Ok
    bool ok() const { return status == DownloadStatus::Ok; }
};

// Called once per progress line while a download runs. Return false to cancel:
// the child process group is killed and the download reports `Cancelled`. It runs
// on the calling thread, so it must not block for long.
using ProgressSink = std::function<bool(const DownloadProgress&)>;

class YouTube {
public:
    YouTube();

    // The subprocess-backed operations are virtual for the same reason
    // VoiceTransport and BotApi are abstract: a test build substitutes scripted
    // results and the whole command layer runs with no yt-dlp and no network.
    // (valid()/invalid() stay non-virtual — they are pure regex work, so tests
    // always exercise the real URL classification. download() is non-virtual
    // too: it is a thin wrapper over the virtual downloadStream(), so a fake
    // that overrides that one method serves both call styles.)
    virtual ~YouTube() = default;

    // yt-dlp "ytsearch1:QUERY" --dump-json --no-download  ->  first result.
    // Returns nullopt if nothing was found or yt-dlp is unavailable.
    virtual std::optional<Track> search(const std::string& query,
                                        std::int64_t messageId = 0,
                                        bool video = false);

    // yt-dlp URL --flat-playlist --dump-json --no-download  ->  up to `limit`
    // tracks. Returns an empty vector on any failure.
    virtual std::vector<Track> playlist(const std::string& url, int limit,
                                        const std::string& user = "", bool video = false);

    // Download by video id, reporting progress and honouring cancellation.
    // CRITICAL: if downloads/<id>.<ext> already exists it is returned
    // immediately, with no subprocess launched and no progress reported.
    // `onProgress` may be empty, in which case nothing is reported and the
    // download simply runs to completion.
    //
    // This is the ONLY virtual download entry point, so a fake overrides one
    // method and both call styles below go through it.
    virtual DownloadResult downloadStream(const std::string& videoId, bool video,
                                          const ProgressSink& onProgress,
                                          std::int64_t maxBytes = kMaxDownloadBytes);

    // The Phase 3 signature, kept for every caller that does not care about
    // progress. Non-virtual on purpose: it forwards to downloadStream().
    std::optional<std::string> download(const std::string& videoId,
                                        bool video = false);

    // URL classification, ported from the Python regexes.
    bool valid(const std::string& url) const;    // a watch/shorts/playlist link
    bool invalid(const std::string& url) const;  // a YouTube link we can't handle

    // Parse ONE yt-dlp --dump-json object (a single JSON line) into a Track.
    // Pure and side-effect-free, so it can be unit-tested without the binary.
    // Returns nullopt if the text is not a usable object (e.g. missing id).
    static std::optional<Track> parseTrackJson(const std::string& jsonText, bool video);

    // Parse ONE line of the progress stream produced by kProgressTemplate.
    // Pure; returns nullopt for any other line (yt-dlp prints plenty of them).
    static std::optional<DownloadProgress> parseProgressLine(const std::string& line);

    // Human-readable byte counts and rates, the way yt-dlp itself renders them:
    // "3.9MiB", "1.2GiB". `humanRate` is the same for a per-second figure, which
    // is why the locale template appends the "/s".
    static std::string humanBytes(std::int64_t bytes);
    static std::string humanRate(double bytesPerSecond);

    // "MM:SS", or "H:MM:SS" past an hour; "--:--" for a negative (unknown) eta.
    static std::string formatEta(int seconds);

    // Run a shell command and return its stdout (POSIX popen). Never throws;
    // returns "" if the command cannot be launched. Named per the project spec.
    static std::string runCommand(const std::string& cmd);

    // Launch `cmd` in its OWN PROCESS GROUP, feed every progress line it prints
    // to `onProgress`, and stop early when the sink returns false (Cancelled) or
    // the announced size exceeds `maxBytes` (TooLarge). Stopping early kills the
    // whole group, so yt-dlp's own children (ffmpeg) die with it. `expectFile`
    // must exist for a clean exit to count as Ok; pass "" to accept exit code 0
    // on its own.
    //
    // Public and static so tests can drive the real machinery — fork, the line
    // reader, the size ceiling and the kill path — with a shell script in place
    // of yt-dlp, which no hermetic test can assume is installed.
    static DownloadResult streamCommand(const std::string& cmd,
                                        const std::string& expectFile,
                                        const ProgressSink& onProgress,
                                        std::int64_t maxBytes = kMaxDownloadBytes);

    static constexpr const char* kDownloadsDir = "downloads";

    // The 200 MB ceiling the `dl_limit` string promises.
    static constexpr std::int64_t kMaxDownloadBytes = 200LL * 1024 * 1024;

    // The --progress-template yt-dlp is asked for. A machine-readable line of
    // our own is parsed instead of yt-dlp's human "[download]  12.3% of …",
    // whose wording has changed between releases.
    static constexpr const char* kProgressPrefix = "ANONXDL";
    static constexpr const char* kProgressTemplate =
        "download:ANONXDL %(progress.downloaded_bytes)s %(progress.total_bytes)s "
        "%(progress.total_bytes_estimate)s %(progress.speed)s %(progress.eta)s";

private:
    // Choose a random cookie file, or "" if none exist. The cookie directories
    // are scanned exactly once (result cached); a missing-cookies warning is
    // emitted at most once.
    std::string pickCookie();

    std::string  base_ = "https://www.youtube.com/watch?v=";
    std::vector<std::string> cookies_;
    bool         cookiesScanned_ = false;
    bool         warnedNoCookies_ = false;
    std::mt19937 rng_;

    std::regex   regex_;      // "valid" pattern
    std::regex   iregex_;     // "invalid" pattern
    bool         regexOk_ = false;
};

}  // namespace anonx

#endif  // ANONX_YOUTUBE_HPP
