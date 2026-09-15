#include "prowsetk/storage.hpp"

#include <algorithm>
#include <ctime>
#include <map>
#include <mutex>
#include <unordered_map>

namespace prowsetk {
namespace {

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

}  // namespace

class InMemoryCookieJar : public CookieJar {
public:
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

        const auto same_cookie = [&cookie](const Cookie& existing) {
            return existing.name == cookie.name &&
                   existing.domain == cookie.domain &&
                   existing.path == cookie.path;
        };
        cookies_.erase(std::remove_if(cookies_.begin(), cookies_.end(),
                                      same_cookie),
                       cookies_.end());
        if (!expired(cookie)) {
            cookies_.push_back(std::move(cookie));
        }
    }

    std::vector<Cookie> get(const Url& origin) const override {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<Cookie> result;
        for (const auto& cookie : cookies_) {
            if (expired(cookie)) {
                continue;
            }
            if (!domain_matches(cookie, origin.host)) {
                continue;
            }
            if (cookie.secure && origin.scheme != "https") {
                continue;
            }
            if (!path_matches(cookie, origin.path.empty() ? "/" : origin.path)) {
                continue;
            }
            result.push_back(cookie);
        }
        std::stable_sort(result.begin(), result.end(),
                         [](const Cookie& left, const Cookie& right) {
                             return left.path.size() > right.path.size();
                         });
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
        cookies_.clear();
    }

    std::vector<Cookie> all() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return cookies_;
    }

private:
    mutable std::mutex mutex_;
    std::vector<Cookie> cookies_;
};

class InMemoryKeyValueStore : public KeyValueStore {
public:
    std::optional<std::string> get(std::string_view key) const override {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = values_.find(std::string(key));
        if (it == values_.end()) {
            return std::nullopt;
        }
        return it->second;
    }

    void set(std::string key, std::string value) override {
        std::lock_guard<std::mutex> lock(mutex_);
        values_[std::move(key)] = std::move(value);
    }

    bool remove(std::string_view key) override {
        std::lock_guard<std::mutex> lock(mutex_);
        return values_.erase(std::string(key)) > 0;
    }

    void clear() override {
        std::lock_guard<std::mutex> lock(mutex_);
        values_.clear();
    }

    std::vector<std::string> keys() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<std::string> result;
        result.reserve(values_.size());
        for (const auto& [key, value] : values_) {
            (void)value;
            result.push_back(key);
        }
        return result;
    }

private:
    mutable std::mutex mutex_;
    std::map<std::string, std::string> values_;
};

struct MemoryStorage::Impl {
    InMemoryCookieJar cookies;
    std::mutex mutex;
    std::unordered_map<std::string, std::unique_ptr<InMemoryKeyValueStore>> local;
    std::unordered_map<std::string, std::unique_ptr<InMemoryKeyValueStore>>
        session;

    InMemoryKeyValueStore& get(
        std::unordered_map<std::string, std::unique_ptr<InMemoryKeyValueStore>>&
            map,
        std::string_view session_id) {
        std::lock_guard<std::mutex> lock(mutex);
        const std::string key(session_id);
        auto it = map.find(key);
        if (it == map.end()) {
            auto store = std::make_unique<InMemoryKeyValueStore>();
            auto* raw = store.get();
            map.emplace(key, std::move(store));
            return *raw;
        }
        return *it->second;
    }
};

MemoryStorage::MemoryStorage() : impl_(std::make_unique<Impl>()) {}
MemoryStorage::~MemoryStorage() = default;

CookieJar& MemoryStorage::cookies() { return impl_->cookies; }

KeyValueStore& MemoryStorage::local_storage(std::string_view session_id) {
    return impl_->get(impl_->local, session_id);
}

KeyValueStore& MemoryStorage::session_storage(std::string_view session_id) {
    return impl_->get(impl_->session, session_id);
}

void MemoryStorage::release_session(std::string_view session_id) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->session.erase(std::string(session_id));
    impl_->local.erase(std::string(session_id));
}

}  // namespace prowsetk
