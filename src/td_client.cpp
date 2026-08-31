// AnonXMusic C++ port — Phase 4
// td_client.cpp — TDLib JSON transport: shared receive pump + invoke() matching.

#include "anonx/td_client.hpp"

#include <td/telegram/td_json_client.h>

#include <nlohmann/json.hpp>

#include <chrono>
#include <thread>
#include <unordered_map>
#include <fstream>

#include "anonx/logger.hpp"

namespace anonx {
namespace {

using nlohmann::json;

// One process-wide pump: TDLib requires that td_receive() be called from a
// single thread. It routes each received object to the owning client by its
// @client_id.
class TdPump {
public:
    static TdPump& instance() {
        static TdPump inst;
        return inst;
    }

    void registerClient(int clientId, TdClient* client) {
        std::lock_guard<std::mutex> lk(mapMutex_);
        clients_[clientId] = client;
    }

    void unregisterClient(int clientId) {
        std::lock_guard<std::mutex> lk(mapMutex_);
        clients_.erase(clientId);
    }

    void ensureStarted() {
        std::lock_guard<std::mutex> lk(startMutex_);
        if (running_) return;
        running_ = true;
        stop_.store(false);
        
        // Initialize TDLib global state (e.g. logging/actor system)
        // Some TDLib versions require td_execute to fully wake up the C JSON API
        std::string req = R"({"@type":"setLogVerbosityLevel","new_verbosity_level":1})";
        td_execute(req.c_str());

        th_ = std::thread([this] { run(); });
    }

    void stop() {
        stop_.store(true);
        if (th_.joinable()) th_.join();
        running_ = false;
    }

private:
    TdPump() : running_(false), stop_(false) {}
    ~TdPump() { stop(); }

    void run() {
        while (!stop_.load()) {
            const char* r = td_receive(0.1);   // seconds
            if (!r) continue;
            std::string s(r);
            // DEBUG LOG to file so it's not lost
            {
                std::ofstream ofs("tdlib_debug.log", std::ios::app);
                ofs << s << "\n";
            }
            log().info("TD_RECEIVE_RAW: " + s);

            json j = json::parse(s, nullptr, false);
            if (j.is_discarded() || !j.is_object() || !j.contains("@client_id")) continue;

            int cid = 0;
            try {
                cid = j["@client_id"].get<int>();
            } catch (...) {
                continue;
            }

            TdClient* target = nullptr;
            {
                std::lock_guard<std::mutex> lk(mapMutex_);
                auto it = clients_.find(cid);
                if (it != clients_.end()) target = it->second;
            }
            if (target) target->onIncoming(std::move(s));
        }
    }

    std::mutex startMutex_;
    bool started_ = false;
    std::atomic<bool> stop_{false};
    std::thread thread_;

    std::mutex mapMutex_;
    std::unordered_map<int, TdClient*> clients_;
};

}  // namespace

TdClient::TdClient() {
    clientId_ = td_create_client_id();
    TdPump::instance().registerClient(clientId_, this);
    TdPump::instance().ensureStarted();

    // td_create_client_id() only hands out an identifier: "the TDLib instance
    // will not send updates until the first request is sent to it". Without a
    // first request no authorizationStateWaitTdlibParameters would ever arrive
    // and boot() would sit waiting for a state machine that never started, so
    // kick the instance awake here — the same getOption("version") TDLib's own
    // tdjson examples use for this.
    //
    // It has to happen after registerClient(): the pump is shared, so an object
    // that came back for an id it does not know yet would be dropped, and the
    // dropped one would be exactly that first authorization update. Updates
    // arriving before setUpdateHandler() are buffered (see onIncoming), so
    // sending this early loses nothing.
    send(R"({"@type":"getOption","name":"version"})");
}

TdClient::~TdClient() {
    TdPump::instance().unregisterClient(clientId_);
    // Release anyone still blocked in invoke().
    std::lock_guard<std::mutex> lk(pendingMutex_);
    for (auto& kv : pending_) {
        kv.second->set_value(R"({"@type":"error","code":500,"message":"client destroyed"})");
    }
    pending_.clear();
}

void TdClient::setUpdateHandler(UpdateHandler handler) {
    std::vector<std::string> buffered;
    UpdateHandler h;
    {
        std::lock_guard<std::mutex> lk(handlerMutex_);
        handler_ = std::move(handler);
        h = handler_;
        buffered.swap(updateBuffer_);
    }
    if (h) {
        for (auto& u : buffered) h(u);
    }
}

std::string TdClient::invoke(const std::string& requestJson, int timeoutMs) {
    json j = json::parse(requestJson, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        return R"({"@type":"error","code":400,"message":"invalid request json"})";
    }

    const std::string extra =
        "req" + std::to_string(clientId_) + "_" +
        std::to_string(extraSeq_.fetch_add(1));
    j["@extra"] = extra;

    auto prom = std::make_shared<std::promise<std::string>>();
    std::future<std::string> fut = prom->get_future();
    {
        std::lock_guard<std::mutex> lk(pendingMutex_);
        pending_[extra] = prom;
    }

    const std::string payload = j.dump();
    td_send(clientId_, payload.c_str());

    if (fut.wait_for(std::chrono::milliseconds(timeoutMs)) == std::future_status::ready) {
        return fut.get();
    }
    {
        std::lock_guard<std::mutex> lk(pendingMutex_);
        pending_.erase(extra);
    }
    return R"({"@type":"error","code":408,"message":"invoke timeout"})";
}

void TdClient::send(const std::string& requestJson) {
    td_send(clientId_, requestJson.c_str());
}

std::string TdClient::execute(const std::string& requestJson) {
    const char* r = td_execute(requestJson.c_str());
    return r ? std::string(r) : std::string();
}

void TdClient::stopRuntime() {
    TdPump::instance().stop();
}

void TdClient::onIncoming(std::string s) {
    json j = json::parse(s, nullptr, false);

    // Response to an invoke()? (TDLib echoes the "@extra" we attached.)
    if (!j.is_discarded() && j.is_object() && j.contains("@extra")) {
        std::string extra;
        try {
            extra = j["@extra"].get<std::string>();
        } catch (...) {
            extra.clear();
        }
        if (!extra.empty()) {
            std::shared_ptr<std::promise<std::string>> prom;
            {
                std::lock_guard<std::mutex> lk(pendingMutex_);
                auto it = pending_.find(extra);
                if (it != pending_.end()) {
                    prom = it->second;
                    pending_.erase(it);
                }
            }
            if (prom) {
                prom->set_value(std::move(s));
                return;
            }
        }
    }

    // Otherwise it is an asynchronous update.
    UpdateHandler h;
    {
        std::lock_guard<std::mutex> lk(handlerMutex_);
        if (handler_) {
            h = handler_;
        } else {
            updateBuffer_.push_back(std::move(s));
            return;
        }
    }
    h(s);
}

}  // namespace anonx
