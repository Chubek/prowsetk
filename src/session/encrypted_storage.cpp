#include "prowsetk/storage.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <ctime>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

extern "C" {
#include <tomcrypt.h>
}

namespace prowsetk {
namespace {

constexpr std::size_t SALT_SIZE = 16;
constexpr std::size_t KEY_SIZE = 32;
constexpr std::size_t NONCE_SIZE = 12;
constexpr std::size_t TAG_SIZE = 16;
constexpr int ITERATIONS = 120000;
constexpr std::string_view VALUE_MAGIC = "PTE3";
constexpr std::string_view COOKIE_MAGIC = "PTKC1";
constexpr std::string_view COOKIE_RECORD_KEY = "__prowsetk_encrypted_cookies";
constexpr std::string_view COOKIE_SESSION_ID = "__prowsetk_encrypted_cookie_jar";
constexpr std::string_view STORAGE_AAD = "prowsetk-storage-v3";

void ensure_libtomcrypt_ready() {
    static std::once_flag once;
    std::call_once(once, [] {
        if (register_cipher(&aes_desc) == -1 && find_cipher("aes") == -1) {
            throw std::runtime_error("EncryptedStorage: AES unavailable");
        }
        if (register_hash(&sha256_desc) == -1 && find_hash("sha256") == -1) {
            throw std::runtime_error("EncryptedStorage: SHA-256 unavailable");
        }
    });
}

std::string ltc_error(int code) {
    return std::string(error_to_string(code));
}

std::string random_bytes(std::size_t size) {
    std::string out(size, '\0');
    if (rng_get_bytes(reinterpret_cast<unsigned char*>(out.data()),
                      static_cast<unsigned long>(out.size()), nullptr) != out.size()) {
        throw std::runtime_error("EncryptedStorage: random generation failed");
    }
    return out;
}

std::string derive_key(const std::string& password, const std::string& salt) {
    ensure_libtomcrypt_ready();
    std::string key(KEY_SIZE, '\0');
    unsigned long out_len = KEY_SIZE;
    const int hash = find_hash("sha256");
    const int err = pkcs_5_alg2(
        reinterpret_cast<const unsigned char*>(password.data()),
        static_cast<unsigned long>(password.size()),
        reinterpret_cast<const unsigned char*>(salt.data()),
        static_cast<unsigned long>(salt.size()), ITERATIONS, hash,
        reinterpret_cast<unsigned char*>(key.data()), &out_len);
    if (err != CRYPT_OK || out_len != KEY_SIZE) {
        throw std::runtime_error("EncryptedStorage: key derivation failed: " +
                                 ltc_error(err));
    }
    return key;
}

void wipe(std::string& value) {
    if (!value.empty()) {
        zeromem(value.data(), value.size());
    }
}

std::string encrypt_value(const std::string& password, std::string_view plaintext) {
    ensure_libtomcrypt_ready();
    std::string salt = random_bytes(SALT_SIZE);
    std::string nonce = random_bytes(NONCE_SIZE);
    std::string key = derive_key(password, salt);
    std::string plaintext_copy(plaintext);
    std::string ciphertext(plaintext.size(), '\0');
    std::string tag(TAG_SIZE, '\0');
    unsigned long tag_len = TAG_SIZE;

    const int err = gcm_memory(
        find_cipher("aes"), reinterpret_cast<const unsigned char*>(key.data()),
        static_cast<unsigned long>(key.size()),
        reinterpret_cast<const unsigned char*>(nonce.data()),
        static_cast<unsigned long>(nonce.size()),
        reinterpret_cast<const unsigned char*>(STORAGE_AAD.data()),
        static_cast<unsigned long>(STORAGE_AAD.size()),
        reinterpret_cast<unsigned char*>(plaintext_copy.data()),
        static_cast<unsigned long>(plaintext_copy.size()),
        reinterpret_cast<unsigned char*>(ciphertext.data()),
        reinterpret_cast<unsigned char*>(tag.data()), &tag_len, GCM_ENCRYPT);
    wipe(plaintext_copy);
    wipe(key);
    if (err != CRYPT_OK || tag_len != TAG_SIZE) {
        throw std::runtime_error("EncryptedStorage: encryption failed: " +
                                 ltc_error(err));
    }

    std::string out;
    out.reserve(VALUE_MAGIC.size() + salt.size() + nonce.size() + tag.size() +
                ciphertext.size());
    out.append(VALUE_MAGIC);
    out.append(salt);
    out.append(nonce);
    out.append(tag);
    out.append(ciphertext);
    return out;
}

std::optional<std::string> decrypt_value(const std::string& password,
                                         std::string_view envelope) {
    ensure_libtomcrypt_ready();
    const std::size_t header_size =
        VALUE_MAGIC.size() + SALT_SIZE + NONCE_SIZE + TAG_SIZE;
    if (envelope.size() < header_size ||
        envelope.substr(0, VALUE_MAGIC.size()) != VALUE_MAGIC) {
        return std::nullopt;
    }

    std::size_t pos = VALUE_MAGIC.size();
    const std::string salt(envelope.substr(pos, SALT_SIZE));
    pos += SALT_SIZE;
    const std::string nonce(envelope.substr(pos, NONCE_SIZE));
    pos += NONCE_SIZE;
    std::string tag(envelope.substr(pos, TAG_SIZE));
    pos += TAG_SIZE;
    const std::string_view ciphertext = envelope.substr(pos);

    std::string key = derive_key(password, salt);
    std::string plaintext(ciphertext.size(), '\0');
    unsigned long tag_len = TAG_SIZE;
    std::string ciphertext_copy(ciphertext);
    const int err = gcm_memory(
        find_cipher("aes"), reinterpret_cast<const unsigned char*>(key.data()),
        static_cast<unsigned long>(key.size()),
        reinterpret_cast<const unsigned char*>(nonce.data()),
        static_cast<unsigned long>(nonce.size()),
        reinterpret_cast<const unsigned char*>(STORAGE_AAD.data()),
        static_cast<unsigned long>(STORAGE_AAD.size()),
        reinterpret_cast<unsigned char*>(plaintext.data()),
        static_cast<unsigned long>(plaintext.size()),
        reinterpret_cast<unsigned char*>(ciphertext_copy.data()),
        reinterpret_cast<unsigned char*>(tag.data()), &tag_len, GCM_DECRYPT);
    wipe(key);
    if (err != CRYPT_OK || tag_len != TAG_SIZE) {
        return std::nullopt;
    }
    return plaintext;
}

void write_u32(std::string& out, std::uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8) {
        out.push_back(static_cast<char>((value >> shift) & 0xffu));
    }
}

bool read_u32(std::string_view input, std::size_t& pos, std::uint32_t& value) {
    if (input.size() - pos < 4) {
        return false;
    }
    value = 0;
    for (int i = 0; i < 4; ++i) {
        value = (value << 8u) |
                static_cast<unsigned char>(input[pos++]);
    }
    return true;
}

void write_field(std::string& out, std::string_view value) {
    if (value.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw std::runtime_error("EncryptedStorage: field too large");
    }
    write_u32(out, static_cast<std::uint32_t>(value.size()));
    out.append(value);
}

bool read_field(std::string_view input, std::size_t& pos, std::string& value) {
    std::uint32_t length = 0;
    if (!read_u32(input, pos, length) || input.size() - pos < length) {
        return false;
    }
    value.assign(input.substr(pos, length));
    pos += length;
    return true;
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
    return request_host.size() > cookie_domain.size() &&
           request_host.compare(request_host.size() - cookie_domain.size(),
                                cookie_domain.size(), cookie_domain) == 0 &&
           request_host[request_host.size() - cookie_domain.size() - 1] == '.';
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
    return *cookie.expires_unix <= static_cast<std::int64_t>(std::time(nullptr));
}

std::string serialize_cookies(const std::vector<Cookie>& cookies) {
    std::string out;
    out.append(COOKIE_MAGIC);
    write_u32(out, static_cast<std::uint32_t>(cookies.size()));
    for (const Cookie& cookie : cookies) {
        write_field(out, cookie.name);
        write_field(out, cookie.value);
        write_field(out, cookie.domain);
        write_field(out, cookie.path);
        write_field(out, cookie.secure ? "1" : "0");
        write_field(out, cookie.http_only ? "1" : "0");
        write_field(out, cookie.host_only ? "1" : "0");
        write_field(out, cookie.expires_unix ? std::to_string(*cookie.expires_unix)
                                             : "");
        write_field(out, cookie.same_site);
    }
    return out;
}

std::vector<Cookie> deserialize_cookies(std::string_view input) {
    if (input.substr(0, COOKIE_MAGIC.size()) != COOKIE_MAGIC) {
        return {};
    }
    std::size_t pos = COOKIE_MAGIC.size();
    std::uint32_t count = 0;
    if (!read_u32(input, pos, count)) {
        return {};
    }
    std::vector<Cookie> cookies;
    cookies.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        Cookie cookie;
        std::string secure;
        std::string http_only;
        std::string host_only;
        std::string expiration;
        if (!read_field(input, pos, cookie.name) ||
            !read_field(input, pos, cookie.value) ||
            !read_field(input, pos, cookie.domain) ||
            !read_field(input, pos, cookie.path) ||
            !read_field(input, pos, secure) ||
            !read_field(input, pos, http_only) ||
            !read_field(input, pos, host_only) ||
            !read_field(input, pos, expiration) ||
            !read_field(input, pos, cookie.same_site)) {
            return {};
        }
        cookie.secure = secure == "1";
        cookie.http_only = http_only == "1";
        cookie.host_only = host_only == "1";
        if (!expiration.empty()) {
            try {
                cookie.expires_unix = std::stoll(expiration);
            } catch (...) {
                return {};
            }
        }
        if (!cookie.name.empty()) {
            cookies.push_back(std::move(cookie));
        }
    }
    return pos == input.size() ? cookies : std::vector<Cookie>{};
}

class EncryptedKeyValueStore : public KeyValueStore {
public:
    EncryptedKeyValueStore(KeyValueStore& backend, std::string password,
                           std::string name)
        : backend_(&backend), password_(std::move(password)), name_(std::move(name)) {}

    void set_access_listener(StorageAccessListener listener) override {
        listener_ = std::move(listener);
        backend_->set_access_listener({});
    }

    std::optional<std::string> get(std::string_view key) const override {
        const auto encrypted = backend_->get(key);
        report("get", key);
        if (!encrypted) {
            return std::nullopt;
        }
        return decrypt_value(password_, *encrypted);
    }

    void set(std::string key, std::string value) override {
        backend_->set(key, encrypt_value(password_, value));
        report("set", key);
    }

    bool remove(std::string_view key) override {
        const bool result = backend_->remove(key);
        report("remove", key);
        return result;
    }

    void clear() override {
        backend_->clear();
        report("clear", {});
    }

    std::vector<std::string> keys() const override { return backend_->keys(); }

private:
    void report(std::string_view operation, std::string_view key) const {
        if (!listener_) {
            return;
        }
        try {
            listener_(name_, key, operation);
        } catch (...) {
        }
    }

    KeyValueStore* backend_;
    std::string password_;
    std::string name_;
    mutable StorageAccessListener listener_;
};

class EncryptedCookieJar : public CookieJar {
public:
    EncryptedCookieJar(KeyValueStore& backend, std::string password)
        : store_(backend, std::move(password), "cookies") {
        if (const auto encoded = store_.get(COOKIE_RECORD_KEY)) {
            cookies_ = deserialize_cookies(*encoded);
        }
    }

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

        cookies_.erase(std::remove_if(cookies_.begin(), cookies_.end(),
                                      [&cookie](const Cookie& existing) {
                                          return existing.name == cookie.name &&
                                                 existing.domain == cookie.domain &&
                                                 existing.path == cookie.path;
                                      }),
                       cookies_.end());
        if (!expired(cookie)) {
            cookies_.push_back(std::move(cookie));
        }
        save_locked();
        report("set", cookie.name);
    }

    std::vector<Cookie> get(const Url& origin) const override {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<Cookie> result;
        for (const Cookie& cookie : cookies_) {
            if (expired(cookie) || !domain_matches(cookie, origin.host) ||
                (cookie.secure && origin.scheme != "https") ||
                !path_matches(cookie, origin.path.empty() ? "/" : origin.path)) {
                continue;
            }
            result.push_back(cookie);
        }
        std::stable_sort(result.begin(), result.end(),
                         [](const Cookie& left, const Cookie& right) {
                             return left.path.size() > right.path.size();
                         });
        report("get", origin.host);
        return result;
    }

    std::string cookie_header(const Url& origin) const override {
        std::string header;
        for (const Cookie& cookie : get(origin)) {
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
        store_.remove(COOKIE_RECORD_KEY);
        report("clear", {});
    }

    std::vector<Cookie> all() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<Cookie> result;
        for (const Cookie& cookie : cookies_) {
            if (!expired(cookie)) {
                result.push_back(cookie);
            }
        }
        report("get", "all");
        return result;
    }

    void set_access_listener(StorageAccessListener listener) override {
        listener_ = std::move(listener);
    }

private:
    void save_locked() { store_.set(std::string(COOKIE_RECORD_KEY), serialize_cookies(cookies_)); }

    void report(std::string_view operation, std::string_view key) const {
        if (!listener_) {
            return;
        }
        try {
            listener_("cookies", key, operation);
        } catch (...) {
        }
    }

    mutable std::mutex mutex_;
    EncryptedKeyValueStore store_;
    std::vector<Cookie> cookies_;
    mutable StorageAccessListener listener_;
};

}  // namespace

struct EncryptedStorage::Impl {
    std::unique_ptr<Storage> backend;
    std::string password;
    std::string salt;
    std::string key;
    std::unique_ptr<EncryptedCookieJar> cookie_jar;
    std::mutex mutex;
    std::unordered_map<std::string, std::unique_ptr<EncryptedKeyValueStore>> local_stores;
    std::unordered_map<std::string, std::unique_ptr<EncryptedKeyValueStore>> session_stores;
    StorageAccessListener access_listener;
};

EncryptedStorage::EncryptedStorage(std::unique_ptr<Storage> backend,
                                   const std::string& password)
    : impl_(std::make_unique<Impl>()) {
    ensure_libtomcrypt_ready();
    impl_->backend = std::move(backend);
    impl_->password = password;
    impl_->salt = random_bytes(SALT_SIZE);
    impl_->key = derive_key(password, impl_->salt);
    impl_->cookie_jar = std::make_unique<EncryptedCookieJar>(
        impl_->backend->local_storage(COOKIE_SESSION_ID), impl_->password);
}

EncryptedStorage::~EncryptedStorage() = default;

CookieJar& EncryptedStorage::cookies() { return *impl_->cookie_jar; }

KeyValueStore& EncryptedStorage::local_storage(std::string_view session_id) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const std::string key(session_id);
    auto it = impl_->local_stores.find(key);
    if (it == impl_->local_stores.end()) {
        auto store = std::make_unique<EncryptedKeyValueStore>(
            impl_->backend->local_storage(session_id), impl_->password, "local");
        store->set_access_listener(impl_->access_listener);
        auto* raw = store.get();
        impl_->local_stores.emplace(key, std::move(store));
        return *raw;
    }
    return *it->second;
}

KeyValueStore& EncryptedStorage::session_storage(std::string_view session_id) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const std::string key(session_id);
    auto it = impl_->session_stores.find(key);
    if (it == impl_->session_stores.end()) {
        auto store = std::make_unique<EncryptedKeyValueStore>(
            impl_->backend->session_storage(session_id), impl_->password, "session");
        store->set_access_listener(impl_->access_listener);
        auto* raw = store.get();
        impl_->session_stores.emplace(key, std::move(store));
        return *raw;
    }
    return *it->second;
}

void EncryptedStorage::release_session(std::string_view session_id) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const std::string key(session_id);
    impl_->session_stores.erase(key);
    impl_->local_stores.erase(key);
    impl_->backend->release_session(session_id);
}

void EncryptedStorage::set_access_listener(StorageAccessListener listener) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->access_listener = std::move(listener);
    if (impl_->cookie_jar) {
        impl_->cookie_jar->set_access_listener(impl_->access_listener);
    }
    for (auto& [_, store] : impl_->local_stores) {
        store->set_access_listener(impl_->access_listener);
    }
    for (auto& [_, store] : impl_->session_stores) {
        store->set_access_listener(impl_->access_listener);
    }
    impl_->backend->set_access_listener({});
}

const std::string& EncryptedStorage::get_salt() const { return impl_->salt; }
const std::string& EncryptedStorage::get_key() const { return impl_->key; }

std::unique_ptr<Storage> make_encrypted_storage(std::unique_ptr<Storage> backend,
                                                const std::string& password) {
    return std::make_unique<EncryptedStorage>(std::move(backend), password);
}

}  // namespace prowsetk
