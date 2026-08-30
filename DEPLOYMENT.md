# Deploying AnonXMusic (C++ port)

This is the runbook for putting the bot on a real machine: build it against real
**TDLib** and real **NTgCalls**, get the assistant accounts logged in once, and
keep it running under systemd.

`README.md` describes the code and the offline test build — the build that needs
no TDLib, no NTgCalls and no network, and that `ctest` exercises. This document
covers the other build, the one that actually connects to Telegram. Where the
README already gives a command it is not repeated here; what is repeated is only
what changes for a deployment.

Everything below assumes Debian 12 or Ubuntu 22.04+ on x86-64 with at least
2 GB RAM and 2 GB free disk (TDLib's own build is the memory-hungry part; the
bot itself is small). Nothing in the port is Linux-specific beyond POSIX
(`mkdir`, `sigaction`, `statvfs`, `/proc/stat`, `/proc/meminfo`, `popen`), so a
BSD or a container works too, but `SystemInfo` reads `/proc` and will report
zeroes where it is absent.

## Before you start: what you must already have

Five things come from Telegram, not from this repo, and no amount of building
will substitute for them.

**An API id and hash** from <https://my.telegram.org> → *API development tools*.
One pair is enough; it is used by the bot account and by every assistant.

**A bot token** from [@BotFather](https://t.me/BotFather). In BotFather also
turn **Group Privacy off** (`/setprivacy` → *Disable*) or the bot will not see
plain `/play` messages in groups, and enable *Inline mode* only if you want it —
the port does not need it.

**A log group.** Create a group or channel, add the bot, and make it an
administrator. Its id is `LOGGER_ID` and it is where the startup card, the stop
notice and the served-chat log land. The id of a supergroup starts with `-100`.

**One to three assistant accounts** — ordinary Telegram user accounts, each with
its own phone number, which are the accounts that actually sit in the voice
chat. You need the phone in international form (`+15551234567`) and you must be
able to receive its login code **at the moment of the first run**, because
Telegram sends the code to that account's other logged-in devices or by SMS.

**The phone numbers, not the session strings.** If you are migrating from the
Python original you have `SESSION` strings produced by Pyrogram/kurigram. Those
**cannot be imported into TDLib** — the two libraries store authorization in
incompatible formats, and there is no supported conversion. The bot token works
unchanged; each assistant needs one interactive, TDLib-native phone login, once,
and after that its session lives in its own directory under `tdlib/` and is
reused forever.

`SESSION` is still read from the config: `Config::check()` requires it to be
non-empty, and the number of non-empty `SESSION*` slots is what
`Database::setAssistantCount()` and the startup card's denominator use. So keep
the old strings in `.env` (or any placeholder) **and** fill in `PHONE_NUMBER*`.
`sample.env` explains this next to the variables themselves.

## Step 1 — dependencies

Build and runtime dependencies of the bot itself:

```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake git pkg-config \
                        libsqlite3-dev nlohmann-json3-dev \
                        ffmpeg python3-pip curl
pip install --upgrade yt-dlp        # or: pipx install yt-dlp
```

`nlohmann-json3-dev` is optional (CMake fetches v3.11.3 if it is missing), but
installing it makes the configure step offline and much faster. `ffmpeg` and
`yt-dlp` are **runtime** dependencies, invoked as processes — `yt-dlp` for search and download, `ffmpeg` for muxing video and for feeding the
voice engine, and `curl` for fetching cookies from `COOKIES_URL`. Check they are
on the `PATH` of the user that will run the bot, not just yours:

```bash
sudo -u anonx bash -lc 'yt-dlp --version; ffmpeg -version | head -1'
```

Note that the TDLib build performs **no** media-tool check at boot: the
`Missing media tools on PATH: …` warning belongs to the skeleton build only. In
a real deployment a missing `yt-dlp` first shows up as every `/play` failing to
download, so verify it by hand here.

### TDLib

TDLib is a large C++ project and building it is the longest part of this
procedure (30–60 minutes on 2 cores; it needs roughly 2 GB of RAM per compile
job, so do not raise `-j` beyond what your RAM allows). Follow the official
build instructions for your distribution at
<https://tdlib.github.io/td/build.html> — they are version-specific and worth
using verbatim. The shape of it is:

```bash
sudo apt-get install -y gperf libssl-dev zlib1g-dev
git clone https://github.com/tdlib/td.git && cd td
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr/local ..
cmake --build . --target install -j1
```

What this project needs from the install is exactly two things: the header
`td/telegram/td_json_client.h` and the CMake package that provides the
**`Td::TdJson`** imported target, because `CMakeLists.txt` does
`find_package(Td REQUIRED)` and links `Td::TdJson`. If you install to a prefix
other than `/usr/local`, pass it on later with `-DTd_DIR=<prefix>/lib/cmake/Td`.
Verify before moving on:

```bash
ls /usr/local/include/td/telegram/td_json_client.h
ls /usr/local/lib/cmake/Td/TdConfig.cmake
```

A shared TDLib also has to be findable at run time. If `ldd` on the finished
binary shows `libtdjson.so` as *not found*, add the directory to the loader
path once:

```bash
echo /usr/local/lib | sudo tee /etc/ld.so.conf.d/tdlib.conf && sudo ldconfig
```

### NTgCalls

The voice engine. Install it (release archive or a source build) so that
`find_package(ntgcalls REQUIRED)` succeeds — `CMakeLists.txt` asks for the
package and `src/ntgcalls_transport.cpp` includes `<ntgcalls/ntgcalls.hpp>`.
Upstream is <https://github.com/pytgcalls/ntgcalls>; prefer a prebuilt release
matching your platform, since it carries WebRTC and building it from source is
heavier than TDLib.

One thing to expect at the configure step: `find_package(ntgcalls REQUIRED)`
assumes a package called `ntgcalls`, and `CMakeLists.txt` says out loud that the
name varies by install method. If CMake fails with *Could not find a package
configuration file provided by "ntgcalls"*, edit the `find_package` and
`target_link_libraries` pair inside the `ANONX_WITH_NTGCALLS` block to match what
your install actually exports. That is the expected adaptation, not a defect.

**NTgCalls evolves quickly and its C++ API is not stable across releases.**
Three places in `src/ntgcalls_transport.cpp` are version-sensitive and are
marked as such in the source: the media-description construction in
`buildMedia()`, the exception class names in the `catch` ladder, and the
stream-end enum. If the file fails to compile against your NTgCalls version,
those comments are where to adapt it — nothing else in the tree touches the
engine, because everything upstream of it talks to the abstract
`VoiceTransport`.

Because of that, build in this order: get the bot working with TDLib **only**
(step 2 below, one flag), confirm commands answer, and add NTgCalls afterwards.
Without it every command still works — `/play` reports a server error instead of
streaming, and the boot log says so:

```
built without NTgCalls (-DANONX_WITH_NTGCALLS=ON) — every command works, but
streaming will report a server error
```

## Step 2 — build the real binary

```bash
cd ~/anonx-cpp
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
      -DANONX_WITH_TDLIB=ON \
      -DANONX_WITH_NTGCALLS=ON
cmake --build build -j2
ctest --test-dir build --output-on-failure
```

`ANONX_WITH_TDLIB=ON` compiles the `anonx_tg` library (the Telegram sources plus
`src/plugins_router.cpp`), links it into `anonx` and defines `ANONX_WITH_TDLIB`
for `src/main.cpp`, which is what selects `runBot()` over the skeleton
`runSkeleton()`. `ANONX_WITH_NTGCALLS=ON` adds `anonx_voice_ntgcalls` and makes
`main.cpp` construct an `NtgCallsTransport` instead of a `NullVoiceTransport`.
Both default to `OFF`; a build with neither still produces a working `anonx`
binary that boots the data layer and idles, which is a useful way to validate a
`.env`, a database file and the locale tables before TDLib is in the picture. It
is not a way to skip the credentials, though: `Config::check()` runs in that
build too, so all six required variables must be **present** — they simply never
have to be valid, because nothing connects.

The seven demos keep passing in this configuration — they are compiled against
the committed fake `libtdjson` and `FakeVoiceTransport`, so `ctest` never talks
to Telegram no matter which flags you set. If `ctest` fails here, the problem is
the toolchain, not the deployment; do not go on.

## Step 3 — the environment file

```bash
cp sample.env .env
chmod 600 .env
$EDITOR .env
```

`sample.env` documents every variable; read it there rather than here. The six
that `Config::check()` refuses to start without are `API_ID`, `API_HASH`,
`BOT_TOKEN`, `LOGGER_ID`, `OWNER_ID` and `SESSION`. Miss one and the binary
exits 1 after **two** lines on stderr — the banner is logged before the config is
checked, so a failed start still creates `log.txt`:

```
[30-Aug-26 09:12:01 - INFO] - anonx: AnonXMusic C++ 3.0.3-cpp — initialising
fatal: Missing required environment variables: BOT_TOKEN
```

The deployment-specific points about the file, none of which `sample.env` can
tell you:

Real environment variables win over `.env` (the same precedence
`python-dotenv` gives with `override=False`), so a stray `BOT_TOKEN` exported in
a shell profile or a systemd `Environment=` line silently beats the file. If a
value seems to be ignored, check the environment first.

`DB_PATH` (default `anonx.db`), the `cache/`, `downloads/`, `tdlib/` and
`locales/` directories and `log.txt` are **all relative to the working
directory**, not to the binary. Run the bot from the project directory, or set
systemd's `WorkingDirectory` (step 6). The locale path in particular is not
configurable from `.env`: `RuntimeOptions::localesDir` is `"locales"`, and a
missing table is fatal —

```
no locale files found in 'locales' — every command would answer with placeholder keys
```

Keep `SESSION*` and `PHONE_NUMBER*` in step. The count of non-empty `SESSION*`
slots is the assistant count; the non-empty `PHONE_NUMBER*` slots are what is
actually logged in, as `AnonyUB1`, `AnonyUB2`, `AnonyUB3`, with sessions in
`tdlib/assistant1`, `tdlib/assistant2`, `tdlib/assistant3`. Empty slots are
compacted rather than held open: set only `PHONE_NUMBER2` and that account
becomes `AnonyUB1` in `tdlib/assistant1`, because the numbering follows the order
the phones appear, not the slot they came from. Three sessions and one phone is a
legal configuration; the startup card will just read `Assistants: 1/3` forever.

`SUPPORT_CHAT` is only joinable if it is a **public** `@username` or
`https://t.me/username` link. A private invite link (`t.me/+…`,
`t.me/joinchat/…`) cannot be resolved by username, so the assistants skip
joining and nothing is logged; that is intended, not a failure.

Never paste a token, session string or phone number into a shell command you
would rather not keep — `.env` is read directly and the logger is
secret-free by construction (`Config::redactedSummary()` prints the phone
**count**, never a number, and no token or session ever reaches `log.txt`).

A missing `.env` is not itself an error: the parser returns an empty map and
every value then comes from the real environment. What you see in that case is
the missing-variables line, not a "file not found".

## Step 4 — the first run, in a terminal

**The first run must be interactive.** Each assistant's TDLib login reads its
code (and 2FA password, if the account has one) from **stdin**, so start the bot
by hand in a terminal — or inside `tmux`/`screen`, which is what you want if you
are on SSH and would rather not lose the session halfway through. Do not start
it under systemd yet: there is no stdin there, the prompt is never answered, and
each assistant fails with `authorization timed out` after two minutes.

```bash
cd ~/anonx-cpp
./build/anonx .env
```

The prompts are written to **stderr**, deliberately, so that piping stdout to a
file still leaves them visible. Do not redirect stderr on this run.

A successful first boot reads like this (the format is
`[date time - LEVEL] - logger: message`, and it goes to both `log.txt` and the
terminal):

```
[30-Aug-26 09:12:01 - INFO] - anonx: AnonXMusic C++ 3.0.3-cpp — initialising
[30-Aug-26 09:12:01 - INFO] - anonx.runtime: config: owner_id=… logger_id=… lang=en
    assistants=1 db=anonx.db … | secrets set: api_hash=yes bot_token=yes session=yes phones=1
[30-Aug-26 09:12:01 - INFO] - anonx.runtime: Loaded 13 language(s); default 'en'
[30-Aug-26 09:12:03 - INFO] - anonx.telegram: anonx started as @your_bot (id 7…)
[30-Aug-26 09:12:03 - INFO] - anonx.runtime: Bot authorized: YourBot (@your_bot)
Enter login code for AnonyUB1:
```

That prompt is the whole reason this step is manual. Telegram has just sent a
login code to the assistant account — check its other logged-in devices, or SMS
— and you have **two minutes** to type it (`Userbot::bootAll()`'s default
timeout is 120 s per account). If the account has two-step verification you are
then asked for the password; press Enter for a blank one if it has none.

While you are typing, the bot is deaf — and not only for the length of the
pause. The authorization state machine runs on TDLib's receive pump, so your
prompt blocks it, and the command router is not attached until every assistant
has finished booting. Messages that arrive in that window are **dropped, not
queued**: `TelegramClient::onUpdate()` has no observer yet and lets them go. So
do not test commands until the log says `Running.` The same window exists on
later, non-interactive boots, where it lasts a second or two.

```
Enter login code for AnonyUB1: 12345
[30-Aug-26 09:12:44 - INFO] - anonx.telegram: AnonyUB1 started as @assistant_one (id 6…)
[30-Aug-26 09:12:45 - INFO] - anonx.userbot: Assistant 1 started
[30-Aug-26 09:12:45 - INFO] - anonx.runtime: 1 assistant(s) up
[30-Aug-26 09:12:45 - INFO] - anonx.runtime: Running.
[30-Aug-26 09:12:45 - INFO] - anonx: Press Ctrl+C to stop.
```

At that point two messages have arrived in the log group: `Assistant Started`
from each assistant, and the bot's own startup card —

```
Bot Started

Bot: YourBot | @your_bot
Assistants: 1/1
Languages: 13
Modules: 22
Chats served: 0
```

If the card does not appear, the boot is still fine and the log will say nothing
about it: `TelegramClient::sendMessage()` returns 0 on failure without logging,
so a wrong `LOGGER_ID`, or a bot that is not a member/admin of that chat, fails
silently. Fix the id and restart.

Stop with Ctrl+C. The shutdown posts `Bot Stopped` to the log group, drains the
handler pool, closes every account (waiting up to 5 s each for TDLib to confirm)
and joins the receive pump, then logs `Stopped.`

Run it a second time to confirm the session was persisted: the second boot must
reach `1 assistant(s) up` **without** prompting for anything. Once that is true,
the interactive part of the deployment is over for good — until you add an
assistant or delete `tdlib/`.

## What the first run leaves on disk

```
anonx-cpp/
├── anonx.db            state: chats, users, auth lists, sudoers, langs, admin_play
│   anonx.db-wal        WAL + shared-memory sidecars; back these up with the db
│   anonx.db-shm
├── log.txt             rotating, 10 MB × 5 backups (log.txt.1 … log.txt.5)
├── tdlib/
│   ├── bot/            the bot account's TDLib session (recreatable from the token)
│   └── assistant1/     AnonyUB1's session — the thing you do NOT want to lose
├── cache/              created for layout parity with the Python original;
│                       nothing writes into it yet
├── downloads/          yt-dlp output, reused as a cache; safe to delete
└── cookies/            NOT created for you — make it yourself if you need
                        *.txt Netscape cookie files (scanned once per run)
```

Two directories deserve backups. `anonx.db` is all the state the bot has, and
Litestream (below) replicates it continuously. `tdlib/assistant*/` are the
sessions: lose them and you repeat the interactive phone login, so copy them
when the bot is **stopped**. Do not run two copies of the same assistant session
on two machines — Telegram may invalidate it and you will be back at the prompt.

`tdlib/bot/` needs no protection; a bot token re-authorizes in one step.

## Step 5 — smoke-test the deployment

With the bot running, in a private chat with it: `/start` should answer with the
start card, `/ping` with a latency card, and `/stats` with the usage card — as
`OWNER_ID` (who counts as sudo) that card carries the extra system block, which
is what proves `SystemInfo` is reading `/proc` correctly. Those three cover the
config, the round trip and the host readings.

Then in a group: add the bot **and make it admin**, add the assistant account as
a member, start a voice chat, and send `/play <song name>`. You should see one
message that becomes *Searching* → *Downloading* → the now-playing card with its
button row; pressing pause/resume/skip must change the stream.

Two things bite here and neither is a bug. If the bot answers in private but
ignores `/play` in the group, Group Privacy is still on in BotFather. And the
assistant does **not** invite itself: the Python original's `checkUB` block
(join / unban / invite the assistant) is a documented gap in this port, so add
the assistant to the group by hand.

## Step 6 — running it as a service

Only after the sessions exist. Create a dedicated user and put the tree where it
will live:

```bash
sudo useradd --system --home /var/lib/anonx --shell /usr/sbin/nologin anonx
sudo mkdir -p /var/lib/anonx
sudo cp -a ~/anonx-cpp/. /var/lib/anonx/
sudo chown -R anonx:anonx /var/lib/anonx
sudo chmod 600 /var/lib/anonx/.env
```

`/etc/systemd/system/anonx.service`:

```ini
[Unit]
Description=AnonXMusic (C++)
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
User=anonx
Group=anonx
# Required: anonx.db, log.txt, locales/, tdlib/, cache/ and downloads/ are all
# resolved relative to the working directory, not to the binary.
WorkingDirectory=/var/lib/anonx
ExecStart=/var/lib/anonx/build/anonx /var/lib/anonx/.env
Restart=on-failure
RestartSec=5
# Shutdown posts a notice and waits up to 5 s per account for TDLib to confirm
# the close; give it room so systemd never SIGKILLs mid-flush.
TimeoutStopSec=30
NoNewPrivileges=true
PrivateTmp=true
ProtectSystem=full
ProtectHome=true

[Install]
WantedBy=multi-user.target
```

```bash
sudo systemctl daemon-reload
sudo systemctl enable --now anonx
journalctl -u anonx -f
```

`SIGTERM` — what `systemctl stop` and `restart` send — is handled: `main.cpp`
installs a `sigaction` handler for `SIGINT` and `SIGTERM`, the idle loop notices
within 200 ms and `Runtime::stop()` runs the clean shutdown. Nothing needs
`KillMode` tweaking.

Keep secrets in `.env` (mode 600), not in `Environment=` lines: those are
readable by anyone who can run `systemctl show anonx`. If you prefer
`EnvironmentFile=`, remember that real environment variables **override** the
file, which is occasionally useful and occasionally baffling.

Every log record goes to **stderr** as well as to `log.txt` — the port copies
Python's stream handler — and nothing is written to stdout at all. So the journal
copy you see in `journalctl` comes from stderr, and `StandardError=null` is what
drops it, leaving `log.txt` and its rotation intact. Do not carry that setting
over to a run by hand: the assistant login prompts are on stderr too.

## Backups with Litestream

`README.md` has the `litestream.yml` for replicating `anonx.db` to S3/R2; WAL
mode is on by default, which is what makes continuous replication work. Two
deployment notes: point Litestream at the same absolute path the service uses
(`/var/lib/anonx/anonx.db`), and on a fresh machine run `litestream restore`
**before** the first `systemctl start`, otherwise the bot creates an empty
database and the restore then refuses to overwrite it. Litestream is a separate
process — a second unit ordered `Before=anonx.service` is the usual arrangement,
and it needs no changes in this codebase.

The sessions under `tdlib/` are not database files and Litestream does not cover
them; back those up with whatever handles the rest of the filesystem.

## Upgrading

```bash
cd /var/lib/anonx && sudo -u anonx git pull       # or copy in a new tree
sudo -u anonx cmake --build build -j2
sudo systemctl restart anonx
```

`anonx.db` needs no migration step: the schema is compiled into the binary and
applied with `CREATE TABLE IF NOT EXISTS` on every open (`schema.sql` in the
repo is the readable copy of it, not a file the bot loads), so a new build opens
an old database. The TDLib sessions are untouched, which makes an upgrade a
rebuild and a restart. Upgrading **TDLib** itself needs a reconfigure
(`cmake -S . -B build …` again) so the new package is picked up, and the session
directories survive that too — TDLib migrates its own database format.

## Troubleshooting, keyed to what you will actually see

`fatal: Missing required environment variables: …` — `Config::check()` refused
to start. The named variables are empty in both `.env` and the environment. Note
that a missing `.env` file produces exactly this rather than a "file not found".

`no locale files found in 'locales' — every command would answer with
placeholder keys` — the working directory is wrong. The bot resolves `locales/`
from the CWD; under systemd this means a missing or incorrect
`WorkingDirectory`.

`bot account failed to authorize — check BOT_TOKEN`, usually preceded by
`anonx: authorization timed out` or `anonx: authorization failed` — a wrong or
revoked token, or TDLib cannot reach Telegram at all. If several accounts time
out identically, suspect the network before the credentials: TDLib retries
silently and the only symptom of a blocked connection is the timeout.

`AnonyUB1: account is unregistered; refusing to auto-register` — that phone
number has no Telegram account. The port will not create one; sign the account
up on a phone first.

`AnonyUB1: authorization timed out` then `AnonyUB1 failed to start` and
`not every assistant authorized (0/1 up)` — the login code was not entered
within two minutes, or the process has no stdin (started under systemd). Stop
the service, run the binary by hand once to complete the login, then start the
service again. The bot deliberately keeps running in this state: every non-voice
command works, only streaming is impossible.

`no PHONE_NUMBER* configured — assistants not booted, so voice chats cannot be
joined` — `SESSION*` is set but `PHONE_NUMBER*` is not. This is the migration
mistake: the Pyrogram session strings are not usable by TDLib, so the phone
numbers are mandatory.

`LANG_CODE 'xx' is not among the loaded locales; falling back to 'en'` — the
code is not one of the 13 shipped locales; check `locales/` for the exact names.

`AnonyUB1: could not resolve support chat @…` — the value looks like a username
but does not resolve: a typo, or a chat that no longer exists. Cosmetic, the
assistant simply does not join. A `SUPPORT_CHAT` that is not username-shaped at
all — an invite link — produces **no** line here, because joining is skipped
before it is ever attempted.

`built without NTgCalls (-DANONX_WITH_NTGCALLS=ON) — every command works, but
streaming will report a server error` / `built without TDLib
(-DANONX_WITH_TDLIB=ON) — running the data-layer skeleton only; no Telegram
connection` — you are running a build that is missing a flag from step 2. The
second one never connects to Telegram at all.

`Cookies are missing; downloads might fail.` — no `*.txt` in `cookies/`. Fine
until YouTube starts asking the machine to prove it is not a bot, at which point
export cookies from a browser session into `cookies/` and **restart**: the
directory is scanned once per process, after which a random file from it is used
per download. The warning is also printed only once per process, so its absence
from a long log does not prove cookies were found.

Every `/play` fails to download but the log is otherwise clean — check `yt-dlp`
by hand as the service user (`sudo -u anonx yt-dlp -F "ytsearch1:test"`).
Remember that the TDLib build makes no PATH check at boot, and that `yt-dlp`
needs updating far more often than the bot does; a stale copy fails on
YouTube's current player format.

`error while loading shared libraries: libtdjson.so` — the loader cannot find
TDLib. `sudo ldconfig` after adding its directory to `/etc/ld.so.conf.d/`, as in
step 1.

The bot answers in private but ignores commands in groups — Group Privacy is on
in BotFather, so the bot never receives the message.

## Security checklist

`.env` mode 600, owned by the service user, and never committed. Secrets never
reach the log: the config summary reports whether each secret is *set*, never
its value, and phone numbers are logged as a **count** only, because a phone
number is personal data. `tdlib/assistant*/` is as sensitive as a password — it
is a live Telegram session for a real account — so treat backups of it
accordingly and keep the directory unreadable to other users
(`chmod 700 tdlib`). Rotate the bot token in BotFather if it ever leaks;
re-authorizing needs nothing but the new token in `.env` and a restart.

