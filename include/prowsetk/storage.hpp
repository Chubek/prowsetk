#ifndef PROWSETK_STORAGE_HPP
#define PROWSETK_STORAGE_HPP

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "prowsetk/url.hpp"

namespace prowsetk {

struct Cookie {
    std::string name;
    std::string value;
    std::string domain;
    std::string path = "/";
    bool secure = false;
    bool http_only = false;
    bool host_only = true;
    std::optional<std::int64_t> expires_unix;
    std::string same_site;
};

// Cookie storage scoped to a browsing context. Implementations must be
// replaceable through this interface (README "Storage").
class CookieJar {
public:
    virtual ~CookieJar() = default;

    virtual void set(const Url& origin, Cookie cookie) = 0;
    virtual std::vector<Cookie> get(const Url& origin) const = 0;
    virtual std::string cookie_header(const Url& origin) const = 0;
    virtual void clear() = 0;
    virtual std::vector<Cookie> all() const = 0;
};

// Simple string key/value storage used for local and session storage.
class KeyValueStore {
public:
    virtual ~KeyValueStore() = default;

    virtual std::optional<std::string> get(std::string_view key) const = 0;
    virtual void set(std::string key, std::string value) = 0;
    virtual bool remove(std::string_view key) = 0;
    virtual void clear() = 0;
    virtual std::vector<std::string> keys() const = 0;
};

// Aggregates the storage backends available to a session. `session_id` selects
// per-session namespaces so that isolated sessions never share state.
class Storage {
public:
    virtual ~Storage() = default;

    virtual CookieJar& cookies() = 0;
    virtual KeyValueStore& local_storage(std::string_view session_id) = 0;
    virtual KeyValueStore& session_storage(std::string_view session_id) = 0;
    virtual void release_session(std::string_view session_id) = 0;
};

// In-memory implementation. This is the default backend and is also the
// hermetic backend used by tests.
class MemoryStorage : public Storage {
public:
    MemoryStorage();
    ~MemoryStorage() override;

    CookieJar& cookies() override;
    KeyValueStore& local_storage(std::string_view session_id) override;
    KeyValueStore& session_storage(std::string_view session_id) override;
    void release_session(std::string_view session_id) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace prowsetk

#endif  // PROWSETK_STORAGE_HPP
