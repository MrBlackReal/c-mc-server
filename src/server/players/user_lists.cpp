#include "server/players/user_lists.hpp"

#include <algorithm>
#include <ctime>
#include <format>
#include <fstream>
#include <sstream>

#include "util/json.hpp"
#include "util/log.hpp"

namespace mc {

namespace {
// Gson's string escaping (HTML-safe by default: <, >, &, = and ' as \u escapes).
std::string gsonString(std::string_view s) {
    std::string out = "\"";
    for (unsigned char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '<': case '>': case '&': case '=': case '\'': out += std::format("\\u{:04x}", c); break;
        default:
            if (c < 0x20) out += std::format("\\u{:04x}", c);
            else out += static_cast<char>(c);
        }
    }
    return out + "\"";
}

std::optional<std::string> readFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void writeFile(const std::filesystem::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

// SimpleDateFormat("yyyy-MM-dd HH:mm:ss Z") in the default time zone.
std::string formatDate(std::chrono::system_clock::time_point t) {
    std::time_t tt = std::chrono::system_clock::to_time_t(t);
    std::tm local{};
    localtime_r(&tt, &local);
    char buf[64];
    std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S %z", &local);
    return buf;
}

std::optional<std::chrono::system_clock::time_point> parseDate(const std::string& s) {
    std::tm tm{};
    int offset = 0;
    char sign = '+';
    int n = std::sscanf(s.c_str(), "%d-%d-%d %d:%d:%d %c%4d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday, &tm.tm_hour, &tm.tm_min, &tm.tm_sec, &sign, &offset);
    if (n != 8 || (sign != '+' && sign != '-')) return std::nullopt;
    tm.tm_year -= 1900;
    tm.tm_mon -= 1;
    int offsetSeconds = (offset / 100 * 3600 + offset % 100 * 60) * (sign == '-' ? -1 : 1);
    return std::chrono::system_clock::from_time_t(timegm(&tm) - offsetSeconds);
}

// Calendar.add(MONTH, 1) in the default time zone (the day clamps to the month's length).
std::chrono::system_clock::time_point plusOneMonth(std::chrono::system_clock::time_point t) {
    std::time_t tt = std::chrono::system_clock::to_time_t(t);
    std::tm local{};
    localtime_r(&tt, &local);
    int year = local.tm_year + 1900 + (local.tm_mon == 11 ? 1 : 0);
    int month = (local.tm_mon + 1) % 12;
    static const int kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    int days = month == 1 && leap ? 29 : kDays[month];
    local.tm_year = year - 1900;
    local.tm_mon = month;
    local.tm_mday = std::min(local.tm_mday, days);
    local.tm_isdst = -1;
    return std::chrono::system_clock::from_time_t(std::mktime(&local));
}

std::optional<NameAndId> nameAndIdFromJson(const json::Value& o) {
    if (!o["uuid"].isString() || !o["name"].isString()) return std::nullopt;
    auto id = UUID::fromJavaString(o["uuid"].string());
    if (!id) return std::nullopt;
    return NameAndId{*id, o["name"].string()};
}

// StringUtil.isValidPlayerName
bool isValidPlayerName(const std::string& name) {
    if (name.size() > 16) return false; // (UTF-8 bytes; names that pass are ASCII anyway)
    for (unsigned char c : name)
        if (c <= 32 || c >= 127) return false;
    return true;
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
} // namespace

// ---------------------------------------------------------------- JavaStringKeyOrder

int32_t JavaStringKeyOrder::stringHash(std::string_view s) {
    uint32_t h = 0;
    for (size_t i = 0; i < s.size();) { // UTF-8 -> UTF-16 units
        auto c = static_cast<unsigned char>(s[i]);
        uint32_t cp = c;
        int len = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
        if (len > 1) {
            cp = c & (0xFF >> (len + 1));
            for (int k = 1; k < len && i + k < s.size(); k++) cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
        }
        i += len;
        if (cp >= 0x10000) {
            cp -= 0x10000;
            h = h * 31 + (0xD800 + (cp >> 10));
            h = h * 31 + (0xDC00 + (cp & 0x3FF));
        } else {
            h = h * 31 + cp;
        }
    }
    return static_cast<int32_t>(h);
}

void JavaStringKeyOrder::added(size_t newSize) {
    if (capacity_ == 0) capacity_ = 16;
    while (newSize > capacity_ * 3 / 4) capacity_ *= 2;
}

std::vector<size_t> JavaStringKeyOrder::order(const std::vector<std::string>& keys) const {
    std::vector<size_t> idx(keys.size());
    for (size_t i = 0; i < idx.size(); i++) idx[i] = i;
    if (capacity_ == 0) return idx;
    auto bucket = [&](size_t i) {
        auto h = static_cast<uint32_t>(stringHash(keys[i]));
        return (h ^ (h >> 16)) & (capacity_ - 1);
    };
    std::stable_sort(idx.begin(), idx.end(), [&](size_t a, size_t b) { return bucket(a) < bucket(b); });
    return idx;
}

// ---------------------------------------------------------------- ServerOpList

void ServerOpList::load() {
    auto text = readFile(file_);
    if (!text) return;
    json::Value doc = json::parse(*text);
    entries_.clear(); // (the map is cleared, its table keeps its size)
    if (!doc.isArray()) return;
    for (const json::Value& o : doc.array()) {
        if (!o.isObject()) throw std::runtime_error("Expected entry to be a JsonObject");
        auto user = nameAndIdFromJson(o);
        if (!user) continue;
        Entry e;
        e.user = *user;
        // PermissionLevel.byId clamps
        e.level = o.has("level") ? std::clamp(static_cast<int>(o["level"].number()), 0, 4) : 0;
        e.bypassesPlayerLimit = o.has("bypassesPlayerLimit") && o["bypassesPlayerLimit"].boolean();
        auto it = std::find_if(entries_.begin(), entries_.end(), [&](const Entry& x) { return x.user.id == e.user.id; });
        if (it != entries_.end()) {
            *it = e;
        } else {
            entries_.push_back(e);
            order_.added(entries_.size());
        }
    }
}

std::vector<size_t> ServerOpList::iterationOrder() const {
    std::vector<std::string> keys;
    for (const auto& e : entries_) keys.push_back(e.user.id.toString());
    return order_.order(keys);
}

void ServerOpList::save() const {
    // Gson with pretty printing.
    std::string out = "[";
    bool first = true;
    for (size_t i : iterationOrder()) {
        const Entry& e = entries_[i];
        out += first ? "\n" : ",\n";
        first = false;
        out += "  {\n";
        out += "    \"uuid\": " + gsonString(e.user.id.toString()) + ",\n";
        out += "    \"name\": " + gsonString(e.user.name) + ",\n";
        out += "    \"level\": " + std::to_string(e.level) + ",\n";
        out += std::string("    \"bypassesPlayerLimit\": ") + (e.bypassesPlayerLimit ? "true" : "false") + "\n";
        out += "  }";
    }
    out += first ? "]" : "\n]";
    writeFile(file_, out);
}

const ServerOpList::Entry* ServerOpList::get(const UUID& id) const {
    for (const auto& e : entries_)
        if (e.user.id == id) return &e;
    return nullptr;
}

bool ServerOpList::add(Entry entry) {
    auto it = std::find_if(entries_.begin(), entries_.end(), [&](const Entry& x) { return x.user.id == entry.user.id; });
    if (it != entries_.end()) {
        // (StoredUserEntry has no equals: a new entry always replaces the old one)
        *it = std::move(entry);
    } else {
        entries_.push_back(std::move(entry));
        order_.added(entries_.size());
    }
    save();
    return true;
}

bool ServerOpList::remove(const UUID& id) {
    auto it = std::find_if(entries_.begin(), entries_.end(), [&](const Entry& x) { return x.user.id == id; });
    if (it == entries_.end()) return false;
    entries_.erase(it);
    save();
    return true;
}

std::vector<std::string> ServerOpList::names() const {
    std::vector<std::string> out;
    for (size_t i : iterationOrder()) out.push_back(entries_[i].user.name);
    return out;
}

// ---------------------------------------------------------------- UserNameToIdResolver

UserNameToIdResolver::UserNameToIdResolver(std::filesystem::path file) : file_(std::move(file)) {
    std::vector<Info> loaded;
    if (auto text = readFile(file_)) {
        try {
            json::Value doc = json::parse(*text);
            if (doc.isArray()) {
                for (const json::Value& o : doc.array()) {
                    if (!o.isObject()) continue;
                    auto profile = nameAndIdFromJson(o);
                    if (!profile || !o["expiresOn"].isString()) continue;
                    auto expires = parseDate(o["expiresOn"].string());
                    if (!expires) {
                        log::warn("Failed to parse date {}", o["expiresOn"].string());
                        continue;
                    }
                    loaded.push_back({*profile, *expires, 0});
                }
            }
        } catch (const std::exception& e) {
            log::warn("Failed to load profile cache {}: {}", file_.string(), e.what());
        }
    }
    std::reverse(loaded.begin(), loaded.end());
    for (auto& info : loaded) safeAdd(std::move(info));
}

void UserNameToIdResolver::safeAdd(Info info) {
    info.lastAccess = ++operationCount_;
    // profilesByName (lower-case) and profilesByUUID: a new entry replaces the one with the same
    // name or id.
    std::string name = lower(info.profile.name);
    std::erase_if(profiles_, [&](const Info& p) { return p.profile.id == info.profile.id || lower(p.profile.name) == name; });
    profiles_.push_back(std::move(info));
}

UserNameToIdResolver::Info& UserNameToIdResolver::addInternal(const NameAndId& profile) {
    safeAdd({profile, plusOneMonth(std::chrono::system_clock::now()), 0});
    save();
    return profiles_.back();
}

void UserNameToIdResolver::add(const NameAndId& profile) { addInternal(profile); }

std::optional<NameAndId> UserNameToIdResolver::lookupGameProfile(const std::string& name) {
    if (isValidPlayerName(name) && lookup_)
        if (auto found = lookup_(name)) return found;
    if (resolveOfflineUsers_) return NameAndId::createOffline(name);
    return std::nullopt;
}

std::optional<NameAndId> UserNameToIdResolver::get(const std::string& name) {
    std::string key = lower(name);
    auto it = std::find_if(profiles_.begin(), profiles_.end(), [&](const Info& p) { return lower(p.profile.name) == key; });
    bool needsSave = false;
    if (it != profiles_.end() && std::chrono::system_clock::now() >= it->expires) {
        profiles_.erase(it);
        it = profiles_.end();
        needsSave = true;
    }
    std::optional<NameAndId> result;
    if (it != profiles_.end()) {
        it->lastAccess = ++operationCount_;
        result = it->profile;
    } else if (auto profile = lookupGameProfile(key)) { // (the lower-cased name, like vanilla)
        result = addInternal(*profile).profile;
        needsSave = false;
    }
    if (needsSave) save();
    return result;
}

std::optional<NameAndId> UserNameToIdResolver::get(const UUID& id) {
    for (auto& p : profiles_) {
        if (p.profile.id == id) {
            p.lastAccess = ++operationCount_;
            return p.profile;
        }
    }
    return std::nullopt;
}

void UserNameToIdResolver::save() const {
    std::vector<const Info*> top;
    for (const auto& p : profiles_) top.push_back(&p);
    std::stable_sort(top.begin(), top.end(), [](const Info* a, const Info* b) { return a->lastAccess > b->lastAccess; });
    if (top.size() > 1000) top.resize(1000);
    std::string out = "[";
    for (size_t i = 0; i < top.size(); i++) {
        if (i) out += ",";
        out += "{\"uuid\":" + gsonString(top[i]->profile.id.toString()) + ",\"name\":" + gsonString(top[i]->profile.name) +
               ",\"expiresOn\":" + gsonString(formatDate(top[i]->expires)) + "}";
    }
    out += "]";
    writeFile(file_, out);
}

} // namespace mc
