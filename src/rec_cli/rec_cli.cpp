#include "rec_cli/rec_cli.h"

#include "api/local_session.h"
#include "api/session.h"
#include "base/cancel.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>

namespace construe::rec_cli {

namespace {

using Json = nlohmann::json;
namespace fs = std::filesystem;

double secondsSince(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count();
}

// Sets the flag when the time limit passes; stopping it early does not wait for the limit.
class Watchdog {
public:
    Watchdog(Cancel& c, double seconds) {
        if (seconds <= 0) return;
        thread_ = std::thread([this, &c, seconds] {
            std::unique_lock lock(m_);
            if (!cv_.wait_for(lock, std::chrono::duration<double>(seconds), [this] { return done_; })) {
                expired_ = true;
                c.requested.store(true);
            }
        });
    }
    ~Watchdog() { stop(); }
    void stop() {
        if (!thread_.joinable()) return;
        {
            std::lock_guard lock(m_);
            done_ = true;
        }
        cv_.notify_all();
        thread_.join();
    }
    bool expired() {
        std::lock_guard lock(m_);
        return expired_;
    }

private:
    std::thread thread_;
    std::mutex m_;
    std::condition_variable cv_;
    bool done_ = false, expired_ = false;
};

// Features and sketch sizes of a part element of the serialized document.
Json partStats(const Json& document, const std::string& element) {
    Json out = {{"features", Json::array()}, {"sketches", Json::array()}};
    for (const auto& e : document.at("elements")) {
        if (e.value("id", "") != element) continue;
        for (const auto& f : e.value("features", Json::array())) {
            const std::string type = f.value("type", "");
            if (type != "sketch") {
                out["features"].push_back(type);
                continue;
            }
            Json s = {{"lines", 0}, {"arcs", 0}, {"circles", 0}, {"splines", 0}, {"points", 0}, {"texts", 0},
                      {"constraints", f.value("constraints", Json::array()).size()}};
            for (const auto& en : f.value("entities", Json::array())) {
                const std::string t = en.value("type", "");
                const char* key = t == "line" ? "lines" : t == "arc" ? "arcs" : t == "circle" ? "circles" : t == "bspline" ? "splines" : t == "point" ? "points" : t == "text" ? "texts" : nullptr;
                if (key && !en.value("construction", false)) s[key] = s[key].get<int>() + 1;
            }
            out["sketches"].push_back(s);
        }
    }
    return out;
}

Json reportJson(const api::ReconstructReport& r, const Json& document) {
    Json out = {{"element", r.element}, {"level", r.level}, {"instances", r.instances}, {"base", r.base}, {"notes", Json::array()}};
    for (const auto& [kind, detail] : r.notes) out["notes"].push_back({{"kind", kind}, {"detail", detail}});
    const Json st = partStats(document, r.element);
    out["features"] = st["features"];
    out["sketches"] = st["sketches"];
    return out;
}

std::string lower(std::string s) {
    for (auto& c : s)
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return s;
}

}  // namespace

std::string usage() {
    return "construe_rec_cli <file.step|file.construe> [--out result.construe] [--log log.json] [--body ID]\n"
           "                 [--round mm] [--heal mm] [--time-limit s]\n";
}

bool parseArgs(const std::vector<std::string>& args, Options& o, std::string& error) {
    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        const bool hasValue = i + 1 < args.size();
        auto number = [&](double& v) {
            char* end = nullptr;
            v = std::strtod(args[++i].c_str(), &end);
            if (end && *end == 0) return true;
            error = "not a number: " + args[i];
            return false;
        };
        if (a == "--out" && hasValue) o.out = args[++i];
        else if (a == "--log" && hasValue) o.log = args[++i];
        else if (a == "--body" && hasValue) o.body = args[++i];
        else if (a == "--round" && hasValue) {
            if (!number(o.roundTolerance)) return false;
        } else if (a == "--heal" && hasValue) {
            if (!number(o.healTolerance)) return false;
        } else if (a == "--time-limit" && hasValue) {
            if (!number(o.timeLimit)) return false;
        } else if (a.rfind("--", 0) == 0) {
            error = "unknown option or missing value: " + a;
            return false;
        } else if (o.input.empty()) {
            o.input = a;
        } else {
            error = "more than one input file";
            return false;
        }
    }
    if (o.input.empty()) error = "no input file";
    return error.empty();
}

Json run(const Options& o) {
    const auto start = std::chrono::steady_clock::now();
    Json log = {{"file", o.input}, {"status", "ok"}, {"bodies", Json::array()}};
    auto finish = [&](const std::string& status, const std::string& error) {
        log["status"] = status;
        if (!error.empty()) log["error"] = error;
        log["seconds"] = secondsSince(start);
        if (!o.log.empty()) std::ofstream(fs::u8path(o.log), std::ios::binary) << log.dump(1);
        return log;
    };
    if (!fs::exists(fs::u8path(o.input))) return finish("error", "file not found");
    auto session = api::createLocalSession();
    api::Session& s = *session;
    const std::string ext = lower(fs::u8path(o.input).extension().string());
    if (ext == ".construe") {
        if (auto st = s.openDocument(o.input); !st) return finish("error", st.error().code);
    } else {
        if (auto r = s.importStep(o.input, {.healTolerance = o.healTolerance}); !r) return finish("error", r.error().code);
    }
    std::vector<std::string> bodies;
    const Json opened = Json::parse(s.documentText());
    for (const auto& e : opened.at("elements")) {
        const std::string id = e.value("id", "");
        if (e.value("kind", "") != "body" || (!o.body.empty() && id != o.body)) continue;
        bodies.push_back(id);
    }
    if (bodies.empty()) return finish("error", "no bodies");
    Cancel cancel;
    Watchdog dog(cancel, o.timeLimit);
    for (const auto& id : bodies) {
        const auto t = std::chrono::steady_clock::now();
        Json entry = {{"element", id}};
        auto prepared = s.prepareReconstruct(id, {.roundTolerance = o.roundTolerance, .healTolerance = o.healTolerance}, &cancel);
        if (!prepared) {
            entry["level"] = "none";
            entry["error"] = prepared.error().code;
            entry["seconds"] = secondsSince(t);
            log["bodies"].push_back(entry);
            if (dog.expired()) return finish("timeout", "time limit");
            continue;
        }
        auto rep = s.applyReconstruct(std::move(*prepared));
        if (!rep) return finish("error", rep.error().code);
        const Json document = Json::parse(s.documentText());
        entry["level"] = rep->level;
        entry["seconds"] = secondsSince(t);
        entry["instances"] = rep->instances;
        entry["parts"] = Json::array();
        if (rep->parts.empty()) entry["parts"].push_back(reportJson(*rep, document));
        for (const auto& p : rep->parts) entry["parts"].push_back(reportJson(p, document));
        log["bodies"].push_back(entry);
    }
    dog.stop();
    if (!o.out.empty())
        if (auto st = s.saveDocument(o.out); !st) return finish("error", st.error().code);
    return finish("ok", "");
}

std::string summary(const Json& log) {
    std::ostringstream os;
    os << log.value("file", "") << ": " << log.value("status", "") << " " << log.value("error", "") << "\n";
    for (const auto& b : log.value("bodies", Json::array())) {
        os << "  " << b.value("element", "") << " " << b.value("level", "");
        for (const auto& p : b.value("parts", Json::array())) {
            os << " [";
            for (const auto& f : p.value("features", Json::array())) os << f.get<std::string>() << " ";
            os << "]";
        }
        os << "\n";
        for (const auto& p : b.value("parts", Json::array()))
            for (const auto& n : p.value("notes", Json::array()))
                os << "    " << n.value("kind", "") << ": " << n.value("detail", "") << "\n";
    }
    return os.str();
}

}  // namespace construe::rec_cli
