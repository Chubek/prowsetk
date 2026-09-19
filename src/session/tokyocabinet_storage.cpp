#include "prowsetk/storage.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

extern "C" {
#include <tcutil.h>
#include <tcbdb.h>
#include <tchdb.h>
}

namespace prowsetk {
namespace {

std::filesystem::path local_path(const std::filesystem::path& base,
                                 std::string_view session_id) {
    constexpr char digits[] = "0123456789abcdef";
    std::string name = "local_";
    for (unsigned char byte : session_id) {
        name.push_back(digits[byte >> 4]);
        name.push_back(digits[byte & 15]);
    }
    return base / (name + ".tcb");
}

std::string to_lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](char c) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    });
    return value;
}

bool domain_matches(const Cookie& cookie, const std::string& host) {
    const std::string cookie_domain = to_lower(cookie.domain);
    const std::string request_host = to_lower(host);
    if (cookie_domain.empty()) {
        return false;
    }
    if (cookie.host_only) {
        return request_host == cookie_domain;
    }
    if (request_host == cookie_domain) {
        return true;
    }
    if (request_host.size() > cookie_domain.size() &&
        request_host.compare(request_host.size() - cookie_domain.size(),
                             cookie_domain.size(), cookie_domain) == 0 &&
        request_host[request_host.size() - cookie_domain.size() - 1] == '.') {
        return true;
    }
    return false;
}

bool path_matches(const Cookie& cookie, const std::string& request_path) {
    if (cookie.path.empty() || cookie.path == "/") {
        return true;
    }
    if (request_path.compare(0, cookie.path.size(), cookie.path) != 0) {
        return false;
    }
    return request_path.size() == cookie.path.size() ||
           cookie.path.back() == '/' || request_path[cookie.path.size()] == '/';
}

bool expired(const Cookie& cookie) {
    if (!cookie.expires_unix.has_value()) {
        return false;
    }
    const std::int64_t now =
        static_cast<std::int64_t>(std::time(nullptr));
    return cookie.expires_unix.value() <= now;
}

std::string serialize_cookie(const Cookie& cookie) {
    std::string result;
    const auto field = [&](std::string_view value) {
        const auto length = static_cast<std::uint32_t>(value.size());
        for (int shift = 24; shift >= 0; shift -= 8) {
            result.push_back(static_cast<char>(length >> shift));
        }
        result.append(value);
    };
    result = "PTK1";
    field(cookie.name);
    field(cookie.value);
    field(cookie.domain);
    field(cookie.path);
    field(cookie.secure ? "1" : "0");
    field(cookie.http_only ? "1" : "0");
    field(cookie.host_only ? "1" : "0");
    field(cookie.expires_unix ? std::to_string(*cookie.expires_unix) : "");
    field(cookie.same_site);
    return result;
}

Cookie deserialize_cookie(const char* data, int size) {
    Cookie cookie;
    if (size < 4 || std::string_view(data, 4) != "PTK1") return cookie;
    const auto bytes = reinterpret_cast<const unsigned char*>(data);
    std::size_t offset = 4;
    const auto field = [&]() -> std::optional<std::string> {
        if (size < 0 || offset > static_cast<std::size_t>(size) ||
            static_cast<std::size_t>(size) - offset < 4) return std::nullopt;
        std::uint32_t length = 0;
        for (int i = 0; i < 4; ++i) {
            length = (length << 8) | bytes[offset++];
        }
        if (length > static_cast<std::size_t>(size) - offset) return std::nullopt;
        std::string value(data + offset, length);
        offset += length;
        return value;
    };
    const auto name = field();
    const auto value = field();
    const auto domain = field();
    const auto path = field();
    const auto secure = field();
    const auto http_only = field();
    const auto host_only = field();
    const auto expiration = field();
    const auto same_site = field();
    if (!name || !value || !domain || !path || !secure || !http_only ||
        !host_only || !expiration || !same_site || offset != static_cast<std::size_t>(size)) {
        return cookie;
    }
    cookie.name = *name;
    cookie.value = *value;
    cookie.domain = *domain;
    cookie.path = *path;
    cookie.secure = *secure == "1";
    cookie.http_only = *http_only == "1";
    cookie.host_only = *host_only == "1";
    try {
        if (!expiration->empty()) cookie.expires_unix = std::stoll(*expiration);
    } catch (...) {
        return {};
    }
    cookie.same_site = *same_site;
    return cookie;
}

}  // namespace

class TCBCookieJar : public CookieJar {
public:
    explicit TCBCookieJar(const std::filesystem::path& db_path) {
        bdb_ = tcbdbnew();
        tcbdbsetmutex(bdb_);
        tcbdbtune(bdb_, 128, 256, 32749, 8, 10, 0);
        
        std::string path = db_path.string();
        int mode = BDBOWRITER | BDBOCREAT;
        if (!tcbdbopen(bdb_, path.c_str(), mode)) {
            int ecode = tcbdbecode(bdb_);
            tcbdbdel(bdb_);
            throw std::runtime_error("Failed to open cookie database: " +
                                     std::string(tcbdberrmsg(ecode)));
        }
    }

    ~TCBCookieJar() override {
        if (bdb_) {
            tcbdbclose(bdb_);
            tcbdbdel(bdb_);
        }
    }

    TCBCookieJar(const TCBCookieJar&) = delete;
    TCBCookieJar& operator=(const TCBCookieJar&) = delete;
    TCBCookieJar(TCBCookieJar&&) = default;
    TCBCookieJar& operator=(TCBCookieJar&&) = default;

    void set(const Url& origin, Cookie cookie) override {
        std::lock_guard<std::mutex> lock(mutex_);
        
        if (cookie.name.empty() || origin.host.empty()) {
            return;
        }
        
        if (cookie.domain.empty()) {
            cookie.domain = to_lower(origin.host);
            cookie.host_only = true;
        } else {
            while (!cookie.domain.empty() && cookie.domain.front() == '.') {
                cookie.domain.erase(cookie.domain.begin());
            }
            cookie.domain = to_lower(cookie.domain);
            if (!domain_matches(cookie, origin.host)) {
                return;
            }
        }
        
        if (cookie.path.empty() || cookie.path.front() != '/') {
            cookie.path = "/";
        }

        const std::string key = cookie.domain + '\0' + cookie.path + '\0' + cookie.name;
        std::string value = serialize_cookie(cookie);
        
        if (expired(cookie)) {
            tcbdbout(bdb_, key.c_str(), key.size());
            report("remove", cookie.name);
            return;
        }
        
        if (!tcbdbput(bdb_, key.c_str(), key.size(), value.c_str(), value.size())) {
            int ecode = tcbdbecode(bdb_);
            throw std::runtime_error("Failed to store cookie: " +
                                     std::string(tcbdberrmsg(ecode)));
        }
        report("set", cookie.name);
    }

    std::vector<Cookie> get(const Url& origin) const override {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<Cookie> result;
        
        BDBCUR* cur = tcbdbcurnew(bdb_);
        tcbdbcurfirst(cur);
        
        int ksiz;
        int vsiz;
        
        // tcbdbcurkey/tcbdbcurval transfer ownership of malloc'd buffers;
        // the guards free them on every exit path, including the continues.
        while (std::unique_ptr<void, void (*)(void*)>
                   kbuf{tcbdbcurkey(cur, &ksiz), tcfree}) {
            std::unique_ptr<void, void (*)(void*)>
                vbuf{tcbdbcurval(cur, &vsiz), tcfree};
            if (!vbuf) {
                tcbdbcurnext(cur);
                continue;
            }
            
            Cookie cookie = deserialize_cookie(static_cast<const char*>(vbuf.get()), vsiz);
            if (cookie.name.empty()) {
                tcbdbcurnext(cur);
                continue;
            }
            if (expired(cookie)) {
                tcbdbcurnext(cur);
                continue;
            }
            if (!domain_matches(cookie, origin.host)) {
                tcbdbcurnext(cur);
                continue;
            }
            if (cookie.secure && origin.scheme != "https") {
                tcbdbcurnext(cur);
                continue;
            }
            if (!path_matches(cookie, origin.path.empty() ? "/" : origin.path)) {
                tcbdbcurnext(cur);
                continue;
            }
            result.push_back(std::move(cookie));
            tcbdbcurnext(cur);
        }
        
        tcbdbcurdel(cur);
        
        std::stable_sort(result.begin(), result.end(),
                         [](const Cookie& left, const Cookie& right) {
                             return left.path.size() > right.path.size();
                         });
        report("get", origin.host);
        return result;
    }

    std::string cookie_header(const Url& origin) const override {
        std::string header;
        for (const auto& cookie : get(origin)) {
            if (!header.empty()) {
                header += "; ";
            }
            header += cookie.name + "=" + cookie.value;
        }
        return header;
    }

    void clear() override {
        std::lock_guard<std::mutex> lock(mutex_);
        tcbdbvanish(bdb_);
        report("clear", {});
    }

    void set_access_listener(StorageAccessListener listener) override {
        listener_ = std::move(listener);
    }

    std::vector<Cookie> all() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<Cookie> result;
        
        BDBCUR* cur = tcbdbcurnew(bdb_);
        tcbdbcurfirst(cur);
        
        int ksiz;
        int vsiz;
        
        while (std::unique_ptr<void, void (*)(void*)>
                   kbuf{tcbdbcurkey(cur, &ksiz), tcfree}) {
            static_cast<void>(kbuf);  // freed by the guard; only the value is decoded
            std::unique_ptr<void, void (*)(void*)>
                vbuf{tcbdbcurval(cur, &vsiz), tcfree};
            if (vbuf) {
                Cookie cookie = deserialize_cookie(static_cast<const char*>(vbuf.get()), vsiz);
                if (!expired(cookie)) {
                    result.push_back(std::move(cookie));
                }
            }
            tcbdbcurnext(cur);
        }
        
        tcbdbcurdel(cur);
        report("get", "all");
        return result;
    }

private:
    mutable std::mutex mutex_;
    TCBDB* bdb_ = nullptr;
    StorageAccessListener listener_;

    void report(std::string_view operation, std::string_view key) const {
        if (listener_) {
            try {
                listener_("cookies", key, operation);
            } catch (...) {
            }
        }
    }
};

class TCBKeyValueStore : public KeyValueStore {
public:
    explicit TCBKeyValueStore(const std::filesystem::path& db_path, std::string name)
        : name_(std::move(name)) {
        bdb_ = tcbdbnew();
        tcbdbsetmutex(bdb_);
        tcbdbtune(bdb_, 128, 256, 32749, 8, 10, 0);
        
        std::string path = db_path.string();
        int mode = BDBOWRITER | BDBOCREAT;
        if (!tcbdbopen(bdb_, path.c_str(), mode)) {
            int ecode = tcbdbecode(bdb_);
            tcbdbdel(bdb_);
            throw std::runtime_error("Failed to open KV database: " +
                                     std::string(tcbdberrmsg(ecode)));
        }
    }

    ~TCBKeyValueStore() override {
        if (bdb_) {
            tcbdbclose(bdb_);
            tcbdbdel(bdb_);
        }
    }

    TCBKeyValueStore(const TCBKeyValueStore&) = delete;
    TCBKeyValueStore& operator=(const TCBKeyValueStore&) = delete;
    TCBKeyValueStore(TCBKeyValueStore&&) = default;
    TCBKeyValueStore& operator=(TCBKeyValueStore&&) = default;

    void set_access_listener(StorageAccessListener listener) override {
        listener_ = std::move(listener);
    }

    std::optional<std::string> get(std::string_view key) const override {
        std::lock_guard<std::mutex> lock(mutex_);
        report("get", key);
        
        int vsiz;
        char* vbuf = static_cast<char*>(tcbdbget(bdb_, key.data(), key.size(), &vsiz));
        if (!vbuf) {
            return std::nullopt;
        }
        std::string result(vbuf, vsiz);
        tcfree(vbuf);
        return result;
    }

    void set(std::string key, std::string value) override {
        std::lock_guard<std::mutex> lock(mutex_);
        report("set", key);
        
        if (!tcbdbput(bdb_, key.c_str(), key.size(), value.c_str(), value.size())) {
            int ecode = tcbdbecode(bdb_);
            throw std::runtime_error("Failed to store key: " +
                                     std::string(tcbdberrmsg(ecode)));
        }
    }

    bool remove(std::string_view key) override {
        std::lock_guard<std::mutex> lock(mutex_);
        report("remove", key);
        
        bool result = tcbdbout(bdb_, key.data(), key.size());
        return result;
    }

    void clear() override {
        std::lock_guard<std::mutex> lock(mutex_);
        report("clear", {});
        tcbdbvanish(bdb_);
    }

    std::vector<std::string> keys() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<std::string> result;
        
        BDBCUR* cur = tcbdbcurnew(bdb_);
        tcbdbcurfirst(cur);
        
        const char* kbuf;
        int ksiz;
        
        while ((kbuf = static_cast<const char*>(tcbdbcurkey(cur, &ksiz))) != nullptr) {
            result.emplace_back(kbuf, ksiz);
            // tcbdbcurkey transfers ownership of the returned buffer.
            tcfree(const_cast<char*>(kbuf));
            tcbdbcurnext(cur);
        }
        
        tcbdbcurdel(cur);
        return result;
    }

    void set_name(std::string name) { name_ = std::move(name); }

private:
    void report(std::string_view operation, std::string_view key) const {
        if (listener_) {
            try {
                listener_(name_, key, operation);
            } catch (...) {
            }
        }
    }

    mutable std::mutex mutex_;
    TCBDB* bdb_ = nullptr;
    std::string name_;
    StorageAccessListener listener_;
};

struct TCBStorage::Impl {
    std::filesystem::path base_path;
    std::mutex mutex;
    StorageAccessListener access_listener;
    std::unique_ptr<TCBCookieJar> cookies;
    std::unordered_map<std::string, std::unique_ptr<TCBKeyValueStore>> local_stores;
    MemoryStorage sessions;
};

TCBStorage::TCBStorage(const std::filesystem::path& base_path)
    : impl_(std::make_unique<Impl>()) {
    impl_->base_path = base_path;
    std::filesystem::create_directories(base_path);
    
    impl_->cookies = std::make_unique<TCBCookieJar>(base_path / "cookies.tcb");
    
    impl_->cookies->set_access_listener([this](std::string_view store, 
                                                std::string_view key,
                                                std::string_view operation) {
        if (impl_->access_listener) {
            try {
                impl_->access_listener(store, key, operation);
            } catch (...) {
            }
        }
    });
}

TCBStorage::~TCBStorage() = default;

CookieJar& TCBStorage::cookies() { return *impl_->cookies; }

KeyValueStore& TCBStorage::local_storage(std::string_view session_id) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    std::string key(session_id);
    auto it = impl_->local_stores.find(key);
    if (it == impl_->local_stores.end()) {
        auto store = std::make_unique<TCBKeyValueStore>(
            local_path(impl_->base_path, key), "local");
        store->set_access_listener(impl_->access_listener);
        auto* raw = store.get();
        impl_->local_stores.emplace(key, std::move(store));
        return *raw;
    }
    return *it->second;
}

KeyValueStore& TCBStorage::session_storage(std::string_view session_id) {
    return impl_->sessions.session_storage(session_id);
}

void TCBStorage::release_session(std::string_view session_id) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    std::string key(session_id);
    impl_->sessions.release_session(key);
    impl_->local_stores.erase(key);
}

void TCBStorage::set_access_listener(StorageAccessListener listener) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->access_listener = std::move(listener);
    if (impl_->cookies) {
        impl_->cookies->set_access_listener(impl_->access_listener);
    }
    for (auto& [_, store] : impl_->local_stores) {
        store->set_access_listener(impl_->access_listener);
    }
    impl_->sessions.set_access_listener(impl_->access_listener);
}

}  // namespace prowsetk
