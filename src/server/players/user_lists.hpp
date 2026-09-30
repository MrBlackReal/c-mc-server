#pragma once

#include <chrono>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "util/uuid.hpp"

namespace mc {

// net.minecraft.server.players.NameAndId
struct NameAndId {
    UUID id;
    std::string name;
    static NameAndId createOffline(const std::string& name) { return {UUID::offlinePlayer(name), name}; }
};

// Java's HashMap iteration order for String keys: by bucket (String.hashCode spread over the
// table), then insertion order within a bucket. The table only ever grows. (Trees in overfull
// buckets aren't modelled; they need 8 keys in one bucket of a 64+ table.)
class JavaStringKeyOrder {
public:
    // A key that wasn't in the map was added.
    void added(size_t newSize);
    // Indices of `keys` (in insertion order) in iteration order.
    std::vector<size_t> order(const std::vector<std::string>& keys) const;
    static int32_t stringHash(std::string_view s); // String.hashCode (over UTF-16 units)

private:
    size_t capacity_ = 0;
};

// ServerOpList (ops.json): the operators, keyed by UUID, with their permission level.
class ServerOpList {
public:
    struct Entry {
        NameAndId user;
        int level = 4; // PermissionLevel id
        bool bypassesPlayerLimit = false;
    };
    explicit ServerOpList(std::filesystem::path file) : file_(std::move(file)) {}

    // StoredUserList.load: throws on unreadable JSON (the caller logs it).
    void load();
    void save() const;
    const Entry* get(const UUID& id) const;
    bool contains(const UUID& id) const { return get(id) != nullptr; }
    // add/remove save the file, like StoredUserList.
    bool add(Entry entry);
    bool remove(const UUID& id);
    // ServerOpList.getUserList: the names, in map order.
    std::vector<std::string> names() const;

private:
    std::vector<size_t> iterationOrder() const;

    std::filesystem::path file_;
    std::vector<Entry> entries_; // insertion order
    JavaStringKeyOrder order_;
};

// CachedUserNameToIdResolver (usercache.json): names of players seen, and the lookups the
// commands make by name.
class UserNameToIdResolver {
public:
    explicit UserNameToIdResolver(std::filesystem::path file);

    // resolveOfflineUsers: unknown names get offline-mode ids (set when not in online mode).
    void resolveOfflineUsers(bool value) { resolveOfflineUsers_ = value; }
    // The profile lookup for names that aren't cached (the services' profile repository).
    void setProfileLookup(std::function<std::optional<NameAndId>(const std::string&)> lookup) { lookup_ = std::move(lookup); }

    void add(const NameAndId& profile);
    std::optional<NameAndId> get(const std::string& name);
    std::optional<NameAndId> get(const UUID& id);
    void save() const;

private:
    struct Info {
        NameAndId profile;
        std::chrono::system_clock::time_point expires;
        int64_t lastAccess = 0;
    };
    Info& addInternal(const NameAndId& profile);
    void safeAdd(Info info);
    std::optional<NameAndId> lookupGameProfile(const std::string& name);

    std::filesystem::path file_;
    std::vector<Info> profiles_; // one per UUID
    bool resolveOfflineUsers_ = true;
    int64_t operationCount_ = 0;
    std::function<std::optional<NameAndId>(const std::string&)> lookup_;
};

} // namespace mc
