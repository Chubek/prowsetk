#include <prowsetk/plugins/spider.hpp>
#include <lmdb++.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdexcept>

namespace prowsetk::plugins::spider {
struct Cache::Impl {
    lmdb::env env = lmdb::env::create();
    lmdb::dbi db{0};
};
Cache::Cache(const std::filesystem::path& directory, std::size_t map_bytes)
    : impl_(std::make_unique<Impl>()) {
    struct stat info{};
    if (::lstat(directory.c_str(), &info) == 0) {
        if (!S_ISDIR(info.st_mode) || info.st_uid != ::geteuid() || (info.st_mode & 0077) != 0)
            throw std::runtime_error("SecurityViolation");
    } else {
        std::filesystem::create_directories(directory);
        if (::chmod(directory.c_str(), 0700) != 0) throw std::runtime_error("IoError");
    }
    impl_->env.set_mapsize(map_bytes);
    impl_->env.open(directory.c_str(), MDB_NOTLS, 0600);
    auto txn = lmdb::txn::begin(impl_->env);
    impl_->db = lmdb::dbi::open(txn, nullptr);
    txn.commit();
}
Cache::~Cache() = default;
std::optional<std::string> Cache::get(std::string_view key) const {
    auto txn = lmdb::txn::begin(impl_->env, nullptr, MDB_RDONLY);
    const lmdb::val k{key.data(), key.size()};
    lmdb::val value;
    if (!impl_->db.get(txn, k, value)) return {};
    return std::string(value.data(), value.size());
}
void Cache::write(const std::vector<std::pair<std::string, std::string>>& puts,
                  const std::vector<std::string>& deletes) {
    auto txn = lmdb::txn::begin(impl_->env);
    for (const auto& key : deletes) impl_->db.del(txn, lmdb::val{key.data(), key.size()});
    for (const auto& [key, value] : puts) {
        if (key.empty() || key.size() > 500 || value.size() > max_message_bytes)
            throw std::runtime_error("ResourceLimit");
        const lmdb::val k{key.data(), key.size()};
        lmdb::val v{value.data(), value.size()};
        impl_->db.put(txn, k, v);
    }
    txn.commit();
}
std::vector<std::pair<std::string, std::string>> Cache::query(
    std::string_view prefix, std::size_t limit, std::string_view after) const {
    if (limit > 1000) throw std::runtime_error("ResourceLimit");
    auto txn = lmdb::txn::begin(impl_->env, nullptr, MDB_RDONLY);
    auto cursor = lmdb::cursor::open(txn, impl_->db);
    const auto start = after.empty() ? prefix : after;
    lmdb::val k{start.data(), start.size()}, v;
    std::string key, value;
    std::vector<std::pair<std::string, std::string>> out;
    bool found = cursor.get(k, v, start.empty() ? MDB_FIRST : MDB_SET_RANGE);
    if (found) { key.assign(k.data(), k.size()); value.assign(v.data(), v.size()); }
    std::size_t bytes = 0;
    while (found && key.starts_with(prefix) && out.size() < limit) {
        if (after.empty() || key > after) {
            bytes += key.size() + value.size();
            if (bytes > 1024u * 1024u && !out.empty()) break;
            out.emplace_back(key, value);
        }
        found = cursor.get(key, value, MDB_NEXT);
    }
    return out;
}
}
