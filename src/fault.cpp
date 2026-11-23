#include "db/fault.h"

#include <cstdlib>
#include <map>
#include <set>
#include <string>

namespace db::fault {
namespace {

struct State {
    bool loaded = false;
    std::set<std::string> points;
    std::map<std::string, long> counts;
    long target = 1;
};

State& state() {
    static State s;
    return s;
}

void load() {
    State& s = state();
    if (s.loaded) {
        return;
    }
    s.loaded = true;
    const char* raw = std::getenv("DBENGINE_CRASH_AT");
    if (raw != nullptr) {
        std::string text(raw);
        std::size_t start = 0;
        while (start <= text.size()) {
            const std::size_t comma = text.find(',', start);
            const std::string item = text.substr(
                start, comma == std::string::npos ? std::string::npos : comma - start);
            if (!item.empty()) {
                s.points.insert(item);
            }
            if (comma == std::string::npos) {
                break;
            }
            start = comma + 1;
        }
    }
    if (const char* count = std::getenv("DBENGINE_CRASH_COUNT")) {
        const long parsed = std::strtol(count, nullptr, 10);
        if (parsed > 0) {
            s.target = parsed;
        }
    }
}

}  // namespace

bool enabled(std::string_view point) {
    load();
    return state().points.contains(std::string(point));
}

void hit(std::string_view point) {
    load();
    State& s = state();
    if (!s.points.contains(std::string(point))) {
        return;
    }
    long& count = s.counts[std::string(point)];
    ++count;
    if (count >= s.target) {
        std::_Exit(86);
    }
}

void reset_for_test() {
    state() = State{};
}

}  // namespace db::fault
