# AnonXMusic — C++ Port

A phased rewrite of the AnonXMusic Telegram voice-chat music bot from Python to
modern C++ (C++17). Completed so far:

- **Phase 1 — Data layer:** a lightweight, embedded persistence layer (SQLite)
  that replaces the original MongoDB code.
- **Phase 2 — Config, logging & app skeleton:** `.env`/environment configuration,
  a thread-safe rotating logger, and the boot/run/stop lifecycle that ties the
  pieces together (the C++ analogue of `config.py` + `anony/__init__.py` +
  `anony/__main__.py`).
- **Phase 3 — YouTube service:** a thin launcher/parser around the `yt-dlp`
  binary (search / playlist / download) invoked as a subprocess — the C++
  analogue of `anony/core/youtube.py`. yt-dlp itself is never reimplemented or
  linked; if it is absent, every operation degrades gracefully.
- **Phase 4 — Telegram client:** the bot account and assistant userbots on top
  of **TDLib**'s JSON interface, plus a Pyrogram-style dispatcher and composable
  message/callback filters — the C++ analogue of `anony/core/bot.py`,
  `anony/core/userbot.py`, and the `@app.on_message(...)` handlers.
- **Phase 5 — Voice & queue:** the per-chat playback queue and the call
  orchestration (join / play / pause / resume / stop, stream-ended → auto-next,
  looping) — the C++ analogue of `anony/core/calls.py` and
  `anony/helpers/_queue.py`. The voice engine sits behind an abstract
  `VoiceTransport`, so the real **NTgCalls** backend is opt-in while the queue
  and orchestration are verified offline against a scripted fake.
- **Phase 6a — Playback commands:** the user-facing commands (`/play`, `/vplay`,
  `/skip`, `/pause`, `/resume`, `/stop`, `/loop`, `/queue`, `/seek`) and the
  inline player buttons, plus the multilingual string subsystem and the
  permission guards they rely on — the C++ analogue of `anony/plugins/play/`,
  `anony/plugins/admins/`, `anony/core/lang.py`, `anony/helpers/_admins.py` and
  `anony/helpers/_inline.py`. The handlers talk to Telegram through an abstract
  `BotApi`, so all of it runs offline against fakes.
- **Phase 6b — Admin & menu commands:** everything that is not playback — per-chat
  authorized users (`/auth` `/unauth` `/authlist`), the chat/user blacklist,
  `/gcast`, `/addsudo` `/rmsudo` `/sudolist`, `/lang`, `/ping`, `/stats`, `/ac`,
  the `/start` `/help` `/settings` inline menus, `/logger on|off` and the chat
  watcher that registers and announces new chats (each command also keeps its
  aliases — `/language`, `/alive`, `/gstats`, `/whitelist`, `/broadcast`,
  `/activevc`) — the C++ analogue of
  `anony/plugins/tools/*` and `anony/plugins/sudo/*`. Host metrics (`psutil` in
  Python) come from a POSIX `SystemInfo` reader whose getters are virtual, so the
  `/ping` and `/stats` cards are asserted byte for byte against a fake.

## Why this design

| Concern | Choice | Reason |
|---|---|---|
| Storage engine | **SQLite** (single file, embedded) | No server to run; ideal for one VPS handling 100k+ users. |
| Durability / backup | **Litestream → Cloudflare R2** | The SQLite file is streamed to S3-compatible storage out-of-process. From C++ it's just a normal local DB. |
| Read speed | **Write-through in-memory cache** | Hot reads hit RAM (`unordered_map`/`unordered_set`); a miss lazily loads one row and caches it; writes update RAM **and** disk. |
| Concurrency | **WAL + single mutex** | Multiple assistant accounts run on separate threads; every cache/DB access is serialized. WAL lets Litestream read while the bot writes. |
| Ephemeral state | **`CacheManager` (RAM-only)** | `active_calls` and `loop` are high-frequency and meaningless after a restart, so they never touch disk. |

## Layout

```
anonx-cpp/
├── CMakeLists.txt
├── DEPLOYMENT.md                  # runbook: real TDLib + NTgCalls, first run, systemd
├── sample.env                     # copy to .env and fill in
├── schema.sql                     # reference schema (also embedded in database.cpp)
├── include/anonx/
│   ├── database.hpp               # persistent, write-through, thread-safe DB
│   ├── cache_manager.hpp          # RAM-only ephemeral state (header-only)
│   ├── config.hpp                 # runtime config (.env / environment)
│   ├── logger.hpp                 # thread-safe rotating logger
│   ├── app.hpp                    # boot / run / stop lifecycle
│   ├── youtube.hpp                # yt-dlp launcher/parser (search/playlist/download)
│   ├── td_client.hpp              # low-level TDLib JSON transport (RAII)
│   ├── telegram_client.hpp        # one logged-in account (bot or userbot)
│   ├── dispatcher.hpp             # message/callback routing + composable filters
│   ├── userbot.hpp                # assistant-account manager
│   ├── queue.hpp                  # per-chat playback queue (header-only)
│   ├── voice_transport.hpp        # abstract voice engine interface (header-only)
│   ├── ntgcalls_transport.hpp     # real NTgCalls-backed transport (opt-in)
│   ├── call_manager.hpp           # playback orchestration (TgCall port)
│   ├── bot_api.hpp                # abstract Telegram surface the plugins call
│   ├── lang.hpp                   # multilingual strings (LangView / fmt)
│   ├── buttons.hpp                # inline-keyboard builders (_inline.py port)
│   ├── guards.hpp                 # permission checks + the /play preflight
│   ├── plugins.hpp                # playback commands + "controls" callbacks
│   ├── sysinfo.hpp                # host metrics for /ping and /stats (psutil port)
│   ├── admin_plugins.hpp          # admin/menu/info commands + the chat watcher
│   └── plugins_router.hpp         # dispatcher wiring for the plugins
├── src/
│   ├── database.cpp
│   ├── config.cpp
│   ├── logger.cpp
│   ├── app.cpp
│   ├── youtube.cpp
│   ├── td_client.cpp
│   ├── telegram_client.cpp
│   ├── dispatcher.cpp
│   ├── userbot.cpp
│   ├── call_manager.cpp
│   ├── lang.cpp
│   ├── buttons.cpp
│   ├── guards.cpp
│   ├── plugins.cpp
│   ├── sysinfo.cpp
│   ├── admin_plugins.cpp
│   ├── plugins_router.cpp
│   ├── ntgcalls_transport.cpp     # only compiled with -DANONX_WITH_NTGCALLS=ON
│   └── main.cpp                   # real entrypoint (anonx binary)
├── locales/                       # one flat JSON string table per language
│   ├── en.json                    # …ar de es fr hi ja my pa pt ru tr zh
│   └── README.md
├── examples/
│   ├── db_demo.cpp                # exercises + asserts the data layer
│   ├── app_demo.cpp               # exercises + asserts config / logger / app
│   ├── youtube_demo.cpp           # exercises + asserts the YouTube service
│   ├── telegram_demo.cpp          # exercises + asserts the Telegram layer + the router
│   ├── voice_demo.cpp             # exercises + asserts the queue + call manager
│   ├── plugins_demo.cpp           # exercises + asserts the playback commands
│   └── admin_demo.cpp             # exercises + asserts the admin/menu commands
└── test/
    ├── fake_tdjson/               # scripted, offline stand-in for libtdjson
    │   ├── td/telegram/td_json_client.h   # signature-compatible fake header
    │   ├── fake_tdjson_hook.h             # test-only update injection
    │   └── fake_tdjson.cpp                # scripted auth flow + responses
    ├── fake_ntgcalls/             # scripted, offline stand-in for the voice engine
    │   └── fake_voice_transport.hpp       # records ops, fires stream-end/close
    ├── fake_bot_api/              # recording, scriptable Telegram surface
    │   └── fake_bot_api.hpp               # logs sends/edits/answers, membership
    ├── fake_sysinfo/              # deterministic host metrics for the cards
    │   └── fake_system_info.hpp           # scripted cpu / ram / disk / uptime
    └── fake_youtube/              # canned search / playlist / download results
        └── fake_youtube.hpp               # real URL regexes, faked subprocesses
```

Libraries: `anonx_data` (Phase 1 data layer) and `anonx_core` (Phase 2–3
config/logging/app/youtube, which links `anonx_data`). The Phase 4 Telegram
sources build either against a real TDLib (`anonx_tg`, opt-in) or against the
bundled fake for testing (`anonx_telegram_demo`). The Phase 5 voice/queue logic
is `anonx_voice` (`call_manager.cpp` + the header-only queue/transport); the real
NTgCalls backend is the opt-in `anonx_voice_ntgcalls`, while tests drive
`anonx_voice` through the fake transport (`anonx_voice_demo`). The Phase 6a/6b
commands are `anonx_plugins` (lang + buttons + guards + plugins + sysinfo +
admin_plugins), which links `anonx_core` and `anonx_voice` but **no** transport;
only `plugins_router.cpp` touches the dispatcher, so it is compiled into the
Telegram targets instead of the library.

## What maps to what (vs. the Python `MongoDB` class)

| Original (`anony/core/mongo.py`) | Here |
|---|---|
| `chats` collection | `chats` table + `isChat/addChat/removeChat/getChats` |
| `users` collection | `users` table + `isUser/addUser/removeUser/getUsers` |
| `auth` collection (`user_ids` set) | `auth(chat_id,user_id)` + `isAuth/addAuth/removeAuth/getAuthUsers` |
| `lang` collection | `lang` table + `getLang/setLang` (falls back to default) |
| `assistant` collection | `assistant` table + `getAssistant/setAssistant` (random assign) |
| `bl_chats` / `bl_users` (in `cache`) | `blacklist_chats` / `blacklist_users` tables + `addBlacklist(id)` sign routing |
| `sudoers` (in `cache`) | `sudoers` table + `addSudo/removeSudo/isSudo/getSudoers` |
| `cmd_delete` field on chat doc | `chats.cmd_delete` + `getCmdDelete/setCmdDelete` |
| `admin_play` field on chat doc | `chats.admin_play` + `getPlayMode/setPlayMode` — **persistent** |
| `active_calls` dict (RAM) | `CacheManager` active-call methods (RAM-only) |
| `loop` dict (RAM) | `CacheManager::getLoop/setLoop` (RAM-only) |

### Phase 2 (config / logging / lifecycle)

| Original | Here |
|---|---|
| `config.py` `Config` + `check()` | `anonx::Config` (`config.hpp`) — `.env`/env loading + `check()` |
| `MONGO_URL` | dropped → `DB_PATH` (SQLite file) |
| `anony/__init__.py` logging setup | `anonx::LogSink` + `anonx::Logger` (`logger.hpp`) — same format, rotating `log.txt` + stderr |
| `anony/__init__.py` globals (`app`, `db`, …) | `anonx::App` members (`app.hpp`) |
| `anony/__main__.py` `main()` / `idle()` / `stop()` | `App::boot()` / `App::run()` / `App::stop()` |
| `anony/core/dir.py` `ensure_dirs()` | `App::ensureDirs()` + `App::checkMediaTools()` |

### Phase 3 (YouTube service)

The original `anony/core/youtube.py` uses the `py_yt` / `yt_dlp` Python
libraries. Here the same behaviour is driven by the **`yt-dlp` command-line
binary** run as a subprocess (`popen`), with `--dump-json` output parsed via
`nlohmann::json`. Nothing about yt-dlp is reimplemented.

| Original (`anony/core/youtube.py`) | Here (`anonx::YouTube`) |
|---|---|
| `YouTube.search(query)` (py_yt `VideosSearch`) | `search()` → `yt-dlp "ytsearch1:QUERY" --dump-json --no-download` |
| `YouTube.playlist(url, limit)` | `playlist()` → `yt-dlp URL --flat-playlist --dump-json --no-download` |
| `YouTube.download(id, video)` (yt_dlp `YoutubeDL`) | `download()` → `yt-dlp URL -f SELECTOR -o downloads/%(id)s.%(ext)s [--cookies …]` |
| audio format `bestaudio[ext=webm][acodec=opus]` | same selector string |
| video format `(bestvideo[height<=?720][width<=?1280][ext=mp4])+(bestaudio)` + merge mp4 | same, with `--merge-output-format mp4` |
| `if Path(filename).exists(): return filename` | cache-hit check via `access()` — **no re-download** |
| `get_cookies()` (scan `.txt`, `random.choice`, warn once) | `pickCookie()` — scans `cookies/` + `anony/cookies/` once, random pick, warns once |
| `Track` dataclass | `anonx::Track` struct |
| `re.match(regex/iregex, url)` | `valid()` / `invalid()` via `std::regex` (`match_continuous`) |

The public header (`youtube.hpp`) stays dependency-free — `nlohmann::json` is an
implementation detail confined to `youtube.cpp`. Downloads are written to
`downloads/`; a random cookie file from `cookies/` (or `anony/cookies/`) is
attached per download if any exist.

### Phase 4 (Telegram client)

The original uses kurigram/Pyrogram (`Client`) plus py-tgcalls. The closest C++
equivalent is **TDLib**, the official Telegram client library — it is the only
option that supports *userbot* accounts and the raw MTProto needed to join voice
chats (the Bot API / tgbot-cpp cannot). TDLib is driven through its **JSON
interface** (`td_create_client_id` / `td_send` / `td_receive` / `td_execute`), a
stable C ABI, so requests and responses are plain JSON strings. A single
process-wide receive pump (one thread, as TDLib requires) routes each incoming
object to the owning client by `@client_id`; each `invoke()` tags its request
with a unique `@extra` so the matching response can be handed back to the caller.

| Original | Here (`anonx::…`) |
|---|---|
| `Bot(pyrogram.Client)` (`bot.py`) | `TelegramClient` with `Options::botToken` set |
| `Userbot` / `SESSION*` (`userbot.py`) | `Userbot` manager of `TelegramClient`s with `phoneNumber` set |
| `Client.start()` / `.stop()` | `TelegramClient::boot()` / `exit()` (drives the auth state machine) |
| `client.me` (`id`, `mention`, …) | `TelegramClient::me()` → `Me{ id, firstName, username, mention }` |
| `app.send_message(chat, text, HTML)` | `sendMessage()` (uses `parseTextEntities` for HTML) |
| `get_chat_member(chat, "me").status` | `getChatMemberStatus(chatId, userId)` |
| `@app.on_message(filters.command([...]))` | `Dispatcher::onMessage(filters::command({...}), handler)` |
| `filters.private` / `filters.group` | `filters::privateChat()` / `filters::groupChat()` |
| `filters.user(...)` / `app.bl_users` / `app.sudoers` | `filters::user({...})` / `filters::userWhere(pred)` |
| `f1 & f2`, `f1 \| f2`, `~f` | `f1 && f2`, `f1 \|\| f2`, `!f` |
| `@app.on_callback_query` | `Dispatcher::onCallback(filters::callbackData…(), handler)` |
| `message.command` / `message.reply_text` | `MessageContext::command` / `MessageContext::reply()` |
| `callback_query.data` / `.answer()` | `CallbackContext::data` / `CallbackContext::answer()` |

**Sessions are not interchangeable with the Python bot.** Pyrogram/kurigram
`SESSION` strings **cannot** be imported into TDLib — the formats are completely
different. The **bot token works unchanged**, but each assistant userbot must do
a one-time TDLib-native login (phone number, login code, and 2FA password if
enabled) on first run. TDLib then persists the session in its own per-account
database directory, so later runs need no interactive login. Give every account
a **unique** `databaseDirectory` (e.g. `tdlib/bot`, `tdlib/assistant1`).

Public headers stay dependency-free: `nlohmann::json` and the TDLib C header are
confined to the `.cpp` files, and `td_client.hpp` exposes requests/responses as
plain `std::string`.

### Phase 5 (voice + queue)

The original `anony/core/calls.py` subclasses py-tgcalls' `PyTgCalls`; the queue
lives in `anony/helpers/_queue.py`. Here the same behaviour is split into three
collaborators so it can be tested without a voice engine:

- **`Queue`** (`queue.hpp`, header-only) — a per-chat `std::deque` of media
  items, a faithful port of `_queue.py`. It stores `Track` (a superset of the
  Python `Media`/`Track` union), is guarded by a mutex, and returns items by
  value so nothing dangles across threads.
- **`VoiceTransport`** (`voice_transport.hpp`) — a small abstract interface for
  the voice engine (`play` / `pause` / `resume` / `stop` / `ping` + stream-end
  and call-closed callbacks). `play()` returns a `PlayResult` enum whose values
  map one-to-one onto the exception branches in `play_media`, so no engine
  exception leaks past the boundary.
- **`CallManager`** (`call_manager.hpp`) — the orchestration, a port of
  `TgCall`. It ties the transport to the `Queue` and the Phase 1 `CacheManager`
  (which already models `active_calls` / `loop`), and delegates all
  Telegram-specific work (downloading, the now-playing card, deleting old
  messages) through injected callbacks.

| Original (`anony/core/calls.py` / `_queue.py`) | Here (`anonx::…`) |
|---|---|
| `Queue.add` / `check_item` / `force_add` | `Queue::add` / `checkItem` / `forceAdd` |
| `Queue.get_current` / `get_next(check=)` | `Queue::getCurrent` / `getNext(check=)` |
| `Queue.get_queue` / `remove_current` / `clear` | `Queue::getQueue` / `removeCurrent` / `clear` |
| `TgCall.play_media` (build MediaStream, seek, announce) | `CallManager::playMedia` (build `MediaSource`, `PlayResult` branches) |
| `types.MediaStream(...)` + `GroupCallConfig(auto_start=False)` | `MediaSource` + `VoiceTransport::play` |
| `client.play` exception ladder (`NoActiveGroupCall`, `NoAudioSourceFound`, `RTMPStreamingUnsupported`, …) | `PlayResult::{NoActiveGroupCall,NoAudioSource,RtmpUnsupported,ServerError,FileNotFound}` |
| `TgCall.replay` / `play_next` (loop, download-on-demand, skip) | `CallManager::replay` / `playNext` |
| `TgCall.pause` / `resume` (fail → stop) | `CallManager::pause` / `resume` |
| `TgCall.stop` (clear queue, remove_call, set_loop 0, leave) | `CallManager::stop` |
| `db.playing / add_call / remove_call / get/set_loop` | `CacheManager::setPaused / addCall / removeCall / getLoop / setLoop` |
| `StreamEnded(AUDIO) → play_next` / `ChatUpdate(KICKED/LEFT/CLOSED) → stop` | transport `setStreamEndHandler` / `setCallClosedHandler` wired in the `CallManager` ctor |
| py-tgcalls `PyTgCalls` engine | `NtgCallsTransport` (opt-in) / `FakeVoiceTransport` (tests) |
| plugins/play.py enqueue-vs-start decision | `CallManager::play(force=)` (`StartedNow` / `Queued`) |

**The join seam.** NTgCalls only produces/consumes the WebRTC parameters — it
does not talk to Telegram. The actual `phone.joinGroupCall` /
`phone.leaveGroupCall` must go over the assistant account's MTProto connection
(the Phase 4 `TelegramClient`). `NtgCallsTransport` therefore takes those two
calls as injected `Signaling` callbacks, keeping the engine decoupled from the
Telegram client. Everything except that binding is exercised offline by
`voice_demo`.

### Phase 6a (command plugins — playback)

The Python bot spreads this over `anony/plugins/play/play.py`,
`anony/plugins/admins/*.py`, `anony/plugins/tools/queue.py`,
`anony/plugins/bot/inline.py` and the helpers they decorate. The port keeps the
same split of responsibilities, but every piece is a plain object:

- **`Language` / `LangView`** (`lang.hpp`) — the multilingual strings. Python
  resolves a chat's language in a `language()` decorator and injects the dict as
  `message.lang`; here `lang.view(db.getLang(chatId))` returns a cheap view and
  `L["key"]` / `L.fmt("key", args…)` replace `m.lang[...]` and `.format(...)`.
  `formatStr` implements Python's positional `{}`/`{0}`/`{0:.1f}` templates, so
  the JSON files in `locales/` are used **verbatim, unmodified**. A missing key
  yields `"{key}"` instead of throwing, and a missing language falls back to the
  default one.
- **`buttons::…`** (`buttons.hpp`) — the inline keyboards, with the same layout
  and the same `callback_data` strings as `_inline.py`, so the callback router
  parses them identically.
- **`guards::…`** (`guards.hpp`) — the permission decorators and the `/play`
  preflight, rewritten as **pure predicates**: they return a verdict and never
  reply, which is what makes them directly testable. `runPlayPreflight` collapses
  `checkUB`'s branches into a `PlayGate` enum plus the parsed request (force /
  video / url / query / flags), and the caller renders the matching string.
- **`Plugins`** (`plugins.hpp`) — the handlers themselves. They take a
  `CommandEvent` / `ButtonEvent` (ids + tokens) and reach Telegram only through
  **`BotApi`** (`bot_api.hpp`), a small abstract interface — the Phase 5
  `VoiceTransport` trick applied to the bot side. That keeps this translation
  unit free of transport headers.
- **`installPlugins`** (`plugins_router.hpp`) — the only file that knows about
  the dispatcher: it converts contexts to events, applies the blacklist and
  group-only filters, and registers the handlers (both halves, plus the Phase 6b
  chat watcher and the menu callbacks).

| Original (`anony/…`) | Here (`anonx::…`) |
|---|---|
| `core/lang.py` dicts + `language()` decorator | `Language` / `LangView`, `Language::loadDir("locales")` |
| `m.lang["k"]` / `m.lang["k"].format(a, b)` | `L["k"]` / `L.fmt("k", a, b)` |
| `helpers/_inline.py` `Inline.controls` / `play_queued` / `queue_markup` | `buttons::controls` / `playQueued` / `queueMarkup` |
| `helpers/_admins.py` `is_admin` / `can_manage_vc` / `admin_check` | `guards::isAdmin` / `canManageVc` / `adminCheck` |
| `helpers/_play.py` `checkUB` decorator | `guards::runPlayPreflight` → `PlayGate` |
| `utils.get_url` (strip `&si=` tracking) | `guards::resolveUrl` |
| `plugins/play/play.py` | `Plugins::onPlay` (`/play` `/vplay` `/playforce` `/vplayforce`) |
| `plugins/admins/{skip,pause,resume,stop,loop,seek}.py` | `Plugins::onSkip` / `onPause` / `onResume` / `onStop` / `onLoop` / `onSeek` |
| `plugins/tools/queue.py` | `Plugins::onQueue` |
| `plugins/bot/inline.py` `"controls …"` handler | `Plugins::onControls` |
| `@app.on_message(filters.command([...]) & filters.group)` | `installPlugins()` filters in `plugins_router.cpp` |
| `~app.bl_users` / `~app.bl_chats` | `db.isBlacklistedUser` / `isBlacklistedChat`, re-checked per message |

**The status-message handoff.** The bot sends one message ("Searching…"), edits
it to "Downloading…", and finally turns *that same message* into the now-playing
card. But the card is rendered from deep inside `CallManager` (through the
`onNowPlaying` callback), which knows nothing about the command that started the
request. `Plugins` bridges them with a per-chat *pending status message* slot:
the command stashes its message id, and whichever callback fires next **takes**
the slot and edits that message instead of sending a new one. `plugins_demo`
asserts the whole `/play` happy path is exactly one `sendMessage` and two
`editMessageText` on a single id.

**Deliberate gaps in 6a**, each marked in the source: replied-media play (the
dispatcher's `MessageContext` carries no reply/entity data yet, so
`PlayRequest::hasReply` is always false, and URL extraction is token-based); the
assistant join/unban/invite block of `checkUB`, which needs assistant-account management
wired in. Live download progress with the `cancel_dl` button and the per-request
play log (`play_log`) are no longer gaps — both shipped in Phase 7. The
served-chat and served-user registries are no longer a gap either — the Phase 6b
chat watcher
(`AdminPlugins::onSeen`) fills them.

### Phase 6b (command plugins — admin, menus & info)

Same three rules as 6a, which is what lets the two halves share one router: the
handlers take transport-free events, permission decisions come from `guards.cpp`,
and every user-visible string comes from `locales/`. Two additions:

- **`SystemInfo`** (`sysinfo.hpp`) — the `psutil` + `platform` replacement:
  `/proc/stat` for CPU, `/proc/meminfo` for RAM, `statvfs` for disk, `uname` for
  the platform string. Every getter is virtual (the `VoiceTransport` trick again),
  so `FakeSystemInfo` feeds the tests scripted numbers and the `/ping` and
  `/stats` cards can be compared byte for byte against the locale templates.
  Nothing throws: an unreadable `/proc` file renders as `0` instead of taking the
  bot down.
- **`AdminPlugins`** (`admin_plugins.hpp`) — one class for the whole set, so the
  router wiring stays uniform and a single demo can drive it. It keeps the same
  status-message handoff as `Plugins` ("Fetching stats…" is *edited* into the
  finished card) and derives the stats card's "Modules:" figure from
  `moduleCount()` = the eight playback groups plus `allCommandGroups().size()`, so
  that number cannot drift away from what is actually registered.

| Original (`anony/plugins/…`, `tools/` + `sudo/`) | Here (`anonx::AdminPlugins::…`) |
|---|---|
| `auth.py` (`/auth` `/unauth` `/authlist`) | `onAuth` / `onAuthList` |
| `blacklist.py` (`/blacklist` `/unblacklist` `/whitelist`) | `onBlacklist` (the id's sign routes chat vs. user) |
| `broadcast.py` (`/gcast` `/broadcast`, `-nochat` `-user` `-copy`) | `onGcast` (one at a time, via `broadcasting_`) |
| `sudoers.py` (`/addsudo` `/rmsudo` `/sudolist`) | `onSudo` / `onSudoList` (owner-gated) |
| `language.py` + the language keyboard (`/lang` `/language`) | `onLang` + `onMenu` (`"lang …"` payloads) |
| `ping.py` (`/ping` `/alive`: latency + host card) | `onPing` (round trip of its own status message) |
| `stats.py` (`/stats` `/gstats`: counters + sudo-only block) | `onStats` (its "Modules:" figure comes from `moduleCount()`) |
| `activevc.py` (`/ac` `/activevc`) | `onActiveVc` |
| `start.py` (`/start` `/help` `/settings` + menus) | `onStart` / `onHelp` / `onSettings` + `onMenu` |
| `logger.py` (`/logger on\|off`) | `onLogger` (persisted via `Database::setLoggerEnabled`) |
| the chat watcher (`@app.on_message`, no command) | `onSeen`, registered with `Dispatcher::onEveryMessage` |

**Deliberate omissions in 6b**, each documented in `admin_plugins.hpp` next to
the locale keys it leaves unused: `/eval` (arbitrary code execution, and there is
no C++ interpreter to run the snippet in), `/logs` and `/restart` (process-level
operations that belong with the launcher, not the command layer), and `/reload`
(6a resolves admin status live through `BotApi::getChatMemberStatus`, so there is
no admin cache to refresh). The three version lines of the `stats_sudo` card keep
their Python labels because the locale files stay byte-identical to the original;
the *values* are this port's equivalents, which is why `SystemInfo` names them
`toolchainVersion` / `telegramLibrary` / `voiceLibrary`.

**How the routing table itself is verified.** `installPlugins()` used to be only
compile-checked — nothing proved that `/pause` reaches `onPause` rather than
`onSkip`, or that `/authlist` stays out of private chats. Part 3 of
`telegram_demo` now tests it by *equivalence*: it dispatches a command through a
real `Dispatcher` populated by `installPlugins()`, then replays the same event on
an identical second fixture by calling that command's handler directly, and
requires the two recorded `FakeBotApi` traces to match exactly — text, keyboard
rows and callback data included. Every command alias, both blacklist filters, the
watcher and all seven callback prefixes are covered this way (60 traces). To check
the test can actually fail, 18 deliberate mis-wirings of `plugins_router.cpp`
(wrong handler, wrong scope, missing registration, defeated filter, damaged
adapter) were each caught.

## Configuration

Copy `sample.env` to `.env` and fill in the required values (`API_ID`,
`API_HASH`, `BOT_TOKEN`, `LOGGER_ID`, `OWNER_ID`, `SESSION`). Real environment
variables take precedence over `.env` (like `python-dotenv` with
`override=False`). `SESSION2`/`SESSION3` add extra assistant userbots; the count
of non-empty sessions seeds `Database::setAssistantCount()`.

## Build

**Deploying for real?** See **[DEPLOYMENT.md](DEPLOYMENT.md)** — installing
TDLib and NTgCalls, the one-time assistant phone logins, the systemd unit and
troubleshooting keyed to actual log lines. This section covers the default
offline build, the one the tests use.

Requires `g++`/`clang` with C++17, CMake ≥ 3.14, and **libsqlite3-dev**.
`nlohmann_json` (header-only) is used by the YouTube service; if it is not
already installed CMake fetches it automatically via `FetchContent`.

At **runtime**, the YouTube service shells out to **`yt-dlp`** (and `ffmpeg`
for muxing video). Install those separately (e.g. `pip install yt-dlp`,
`apt-get install ffmpeg`); the code compiles and boots without them and simply
degrades if they are missing.

```bash
# Debian/Ubuntu: build deps (nlohmann-json3-dev is optional — CMake can fetch it)
sudo apt-get install -y libsqlite3-dev nlohmann-json3-dev cmake g++
# runtime deps for actual playback:
pip install yt-dlp && sudo apt-get install -y ffmpeg

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure     # runs all seven demos as tests

cp sample.env .env && $EDITOR .env             # fill in your credentials
./build/anonx .env                              # run the bot (boots + idles)
```

Targets: `anonx` (the bot), `anonx_db_demo`, `anonx_app_demo`,
`anonx_youtube_demo`, `anonx_telegram_demo`, `anonx_voice_demo`,
`anonx_plugins_demo`, and `anonx_admin_demo` (self-checking tests). Each demo
exits non-zero on the first failed assertion, so `ctest` needs no test framework.
The three demos that render locale strings (`anonx_telegram_demo`,
`anonx_plugins_demo`, `anonx_admin_demo`) are compiled with
`-DANONX_LOCALES_DIR=…/locales`, so they load the real string tables no matter
which directory they run from.

### TDLib (Phase 4)

The Telegram layer targets **TDLib**. To keep the core project buildable without
it, the real dependency is **opt-in**: the tests instead compile the same
Telegram sources against a small scripted fake (`test/fake_tdjson/`), so
`ctest` verifies the auth flow, dispatcher and userbot manager **offline, with no
TDLib install and no network**.

For a real deployment, build TDLib (see the TDLib docs) so its CMake package and
the `Td::TdJson` target are discoverable, then enable the Telegram library:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DANONX_WITH_TDLIB=ON
cmake --build build -j        # now also builds the anonx_tg library
```

### NTgCalls (Phase 5)

The voice engine is opt-in the same way. By default only the engine-agnostic
`anonx_voice` library (queue + `CallManager`) is built, and `ctest` verifies the
orchestration through `FakeVoiceTransport` — **no NTgCalls, no WebRTC, no
network**. To build the real backend, install NTgCalls so its CMake package is
discoverable, then:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DANONX_WITH_NTGCALLS=ON
cmake --build build -j        # now also builds anonx_voice_ntgcalls
```

`ntgcalls_transport.cpp` binds to the NTgCalls C++ API (`<ntgcalls/ntgcalls.hpp>`).
NTgCalls evolves quickly; the version-sensitive spots (media-description
construction, exception class names, the stream-end enum) are isolated and
clearly marked, so adapting to a specific NTgCalls release touches only those
few lines. The join/leave signaling is injected (see "The join seam" above), so
wire it to the assistant `TelegramClient` in the final integration step.

Or a one-liner without CMake (needs the `nlohmann/json.hpp` header on the
include path — `apt-get install nlohmann-json3-dev`, or point `-I` at it):

```bash
g++ -std=c++17 -O2 -Iinclude \
    src/database.cpp src/config.cpp src/logger.cpp src/app.cpp \
    src/youtube.cpp src/main.cpp \
    -lsqlite3 -lpthread -o anonx && ./anonx .env
```

## Litestream (backup) — separate, no code changes

Litestream runs as its own process and watches the SQLite file. WAL mode (enabled
automatically by `Database`) is what makes continuous replication possible.
Example `litestream.yml`:

```yaml
dbs:
  - path: /var/lib/anonx/anonx.db
    replicas:
      - type: s3
        endpoint: https://<accountid>.r2.cloudflarestorage.com
        bucket: anonx-backups
        path: anonx.db
```

Run: `litestream replicate -config litestream.yml`. On a fresh VPS,
`litestream restore` pulls the DB back before the bot starts.

## Usage sketch

```cpp
#include "anonx/database.hpp"
#include "anonx/cache_manager.hpp"

anonx::Database db("anonx.db");   // opens, WAL, creates schema
db.setDefaultLang("en");
db.setAssistantCount(3);          // number of userbot accounts

if (db.isBlacklistedChat(chatId)) return;
if (db.getPlayMode(chatId) && !isAdmin) return;   // admin-only play
auto lang = db.getLang(chatId);
int assistant = db.getAssistant(chatId);          // 1..N

anonx::CacheManager cache;
cache.addCall(chatId);            // ephemeral: never written to disk
cache.setLoop(chatId, 3);
```

Wiring the commands (what `installPlugins` does, see `src/plugins_router.cpp`):

```cpp
#include "anonx/plugins_router.hpp"

anonx::Language lang;
lang.loadDir("locales");          // 13 languages, 153 keys each
lang.setDefault("en");

anonx::Queue        queue;
anonx::YouTube      yt;
anonx::SystemInfo   sys;
anonx::CallManager  calls(transport, queue, cache);   // transport = NTgCalls/fake
anonx::Plugins      plugins({api, db, cache, queue, yt, calls, lang, config});
anonx::AdminPlugins admin({api, db, cache, calls, sys, lang, config});

// Registers every command (playback + admin/menus), the "controls" and menu
// buttons and the chat watcher, applies the group-only and blacklist filters,
// and hands CallManager the card/notice callbacks.
anonx::installPlugins(dispatcher, plugins, admin, db);
```

`plugins`, `admin` and everything they borrow must outlive `dispatcher`.

## Roadmap

- **Phase 1 — Data layer** ✅ — SQLite + cache, thread-safe, RAII.
- **Phase 2 — Config + logging + app skeleton** ✅ — env parsing, logger, lifecycle.
- **Phase 3 — YouTube service** ✅ — `yt-dlp` launcher/parser (search / playlist / download) via subprocess.
- **Phase 4 — Telegram client** ✅ — TDLib JSON transport, bot + assistant
  userbots, auth state machine, Pyrogram-style dispatcher and filters.
- **Phase 5 — Voice/calls** ✅ — per-chat queue + call orchestration (join, play,
  pause/resume/stop, stream-ended → auto-next, looping) behind an abstract
  `VoiceTransport`; real NTgCalls backend opt-in, verified offline via a fake.
- **Phase 6a — Playback commands** ✅ — `/play` `/vplay` `/skip` `/pause`
  `/resume` `/stop` `/loop` `/queue` `/seek` + the inline player buttons, on top
  of the multilingual string subsystem and the permission guards; driven through
  an abstract `BotApi` and verified offline (243 assertions, including the Phase 7
  download UI).
- **Phase 6b — Admin & menu commands** ✅ — `/auth` `/unauth` `/authlist`,
  `/blacklist` `/unblacklist`, `/gcast`, `/addsudo` `/rmsudo` `/sudolist`,
  `/lang`, `/ping`, `/stats`, `/ac`, `/start` `/help` `/settings` with their
  inline menus, `/logger on|off` and the chat watcher; host metrics behind a
  virtual `SystemInfo`, verified offline (251 assertions) together with the
  dispatcher routing table itself (60 matched traces, 18/18 mis-wirings caught).
- **Phase 7 — Extras** ✅ — The final set of features: the per-request **play log**
  (posted to `LOGGER_ID` behind the same gate as the Phase 6b notices, one card per
  `/play`), **live download progress** (a throttled progress bar in the
  request's own message, a `cancel_dl` button that reaps yt-dlp's whole process
  group, the 200 MiB ceiling and the `dl_active` de-duplication), **thumbnail generation**
  (shelling out to ffmpeg for now-playing cards), **cookies rotation** (`COOKIES_URL` fetching),
  **`auto_leave`/`auto_end`** (idle timers and queue draining logic), **replied-media play** (downloading directly via `tg://` urls),
  and the **assistant lifecycle** (checking membership, unbanning, and joining via invite links in the `/play` preflight).
  The admin cache (`/reload`) was explicitly declined as the direct O(1) TDLib query eliminates the staleness and TTL complexities of a cache. The port is now complete.
