#include "prowsetk/storage.hpp"

#include <algorithm>
#include <cstring>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

namespace prowsetk {
namespace {

constexpr size_t SALT_SIZE = 16;
constexpr size_t KEY_SIZE = 32;
constexpr size_t NONCE_SIZE = 12;

std::string derive_key(const std::string& password, const std::string& salt) {
    std::string key(KEY_SIZE, 0);
    std::string input = password + salt;
    
    std::hash<std::string> hasher;
    size_t hash = hasher(input);
    
    for (size_t i = 0; i < KEY_SIZE; ++i) {
        key[i] = static_cast<char>((hash >> (i % 8)) & 0xFF);
    }
    
    for (int round = 0; round < 10000; ++round) {
        std::string round_input = key + input;
        hash = hasher(round_input);
        for (size_t i = 0; i < KEY_SIZE; ++i) {
            key[i] ^= static_cast<char>((hash >> (i % 8)) & 0xFF);
        }
    }
    
    return key;
}

void xor_encrypt(const std::string& key, const std::string& nonce,
                 const std::string& plaintext, std::string& ciphertext) {
    ciphertext.resize(plaintext.size());
    std::string keystream;
    keystream.resize(plaintext.size());
    
    std::string state = key + nonce;
    std::hash<std::string> hasher;
    
    for (size_t i = 0; i < plaintext.size(); ++i) {
        if (i % 32 == 0) {
            size_t hash = hasher(state + std::to_string(i / 32));
            for (size_t j = 0; j < 32 && i + j < plaintext.size(); ++j) {
                keystream[i + j] = static_cast<char>((hash >> (j % 8)) & 0xFF);
            }
        }
        ciphertext[i] = plaintext[i] ^ keystream[i];
    }
}

bool xor_decrypt(const std::string& key, const std::string& nonce,
                 const std::string& ciphertext, std::string& plaintext) {
    xor_encrypt(key, nonce, ciphertext, plaintext);
    return true;
}

std::string generate_salt() {
    std::string salt(SALT_SIZE, 0);
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, 255);
    for (size_t i = 0; i < SALT_SIZE; ++i) {
        salt[i] = static_cast<char>(dis(gen));
    }
    return salt;
}

std::string generate_nonce() {
    std::string nonce(NONCE_SIZE, 0);
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, 255);
    for (size_t i = 0; i < NONCE_SIZE; ++i) {
        nonce[i] = static_cast<char>(dis(gen));
    }
    return nonce;
}

class EncryptedKeyValueStore : public KeyValueStore {
public:
    EncryptedKeyValueStore(std::unique_ptr<KeyValueStore> backend,
                           const std::string& key, std::string name)
        : backend_(std::move(backend)), key_(key), name_(std::move(name)) {}

    ~EncryptedKeyValueStore() override = default;

    void set_access_listener(StorageAccessListener listener) override {
        listener_ = std::move(listener);
        backend_->set_access_listener([this](std::string_view store,
                                              std::string_view k,
                                              std::string_view operation) {
            if (listener_) {
                try {
                    listener_(store, k, operation);
                } catch (...) {
                }
            }
        });
    }

    std::optional<std::string> get(std::string_view key) const override {
        auto encrypted = backend_->get(key);
        if (!encrypted) {
            report("get", key);
            return std::nullopt;
        }
        
        std::string plaintext;
        if (encrypted->size() < NONCE_SIZE) {
            report("get", key);
            return std::nullopt;
        }
        std::string nonce = encrypted->substr(0, NONCE_SIZE);
        std::string ciphertext = encrypted->substr(NONCE_SIZE);
        
        if (!xor_decrypt(key_, nonce, ciphertext, plaintext)) {
            report("get", key);
            return std::nullopt;
        }
        
        report("get", key);
        return plaintext;
    }

    void set(std::string key, std::string value) override {
        std::string nonce = generate_nonce();
        std::string ciphertext;
        xor_encrypt(key_, nonce, value, ciphertext);
        
        std::string encrypted = nonce + ciphertext;
        backend_->set(std::move(key), std::move(encrypted));
        
        report("set", key);
    }

    bool remove(std::string_view key) override {
        bool result = backend_->remove(key);
        report("remove", key);
        return result;
    }

    void clear() override {
        backend_->clear();
        report("clear", {});
    }

    std::vector<std::string> keys() const override {
        return backend_->keys();
    }

    void set_name(std::string name) {
        name_ = std::move(name);
        backend_->set_name(name_);
    }

private:
    void report(std::string_view operation, std::string_view key) const {
        if (listener_) {
            try {
                listener_(name_, key, operation);
            } catch (...) {
            }
        }
    }

    std::unique_ptr<KeyValueStore> backend_;
    std::string key_;
    std::string name_;
    StorageAccessListener listener_;
};

class EncryptedCookieJar : public CookieJar {
public:
    EncryptedCookieJar(std::unique_ptr<CookieJar> backend,
                       const std::string& key, std::string name)
        : backend_(std::move(backend)), key_(key), name_(std::move(name)) {}

    ~EncryptedCookieJar() override = default;

    void set(const Url& origin, Cookie cookie) override {
        backend_->set(origin, std::move(cookie));
        report("set", cookie.name);
    }

    std::vector<Cookie> get(const Url& origin) const override {
        auto result = backend_->get(origin);
        report("get", origin.host);
        return result;
    }

    std::string cookie_header(const Url& origin) const override {
        return backend_->cookie_header(origin);
    }

    void clear() override {
        backend_->clear();
        report("clear", {});
    }

    std::vector<Cookie> all() const override {
        return backend_->all();
    }

private:
    void report(std::string_view operation, std::string_view key) const {
        if (listener_) {
            try {
                listener_(name_, key, operation);
            } catch (...) {
            }
        }
    }

    std::unique_ptr<CookieJar> backend_;
    std::string key_;
    std::string name_;
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

EncryptedStorage::EncryptedStorage(std::unique_ptr<Storage> backend, const std::string& password)
    : impl_(std::make_unique<Impl>()) {
    impl_->backend = std::move(backend);
    impl_->password = password;
    impl_->salt = generate_salt();
    impl_->key = derive_key(password, impl_->salt);
    
    impl_->cookie_jar = std::make_unique<EncryptedCookieJar>(
        std::move(impl_->backend->cookies()), impl_->key, "cookies");
}

EncryptedStorage::~EncryptedStorage() = default;

CookieJar& EncryptedStorage::cookies() { return *impl_->cookie_jar; }

KeyValueStore& EncryptedStorage::local_storage(std::string_view session_id) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    std::string key(session_id);
    auto it = impl_->local_stores.find(key);
    if (it == impl_->local_stores.end()) {
        auto store = std::make_unique<EncryptedKeyValueStore>(
            std::make_unique<EncryptedKeyValueStore>(impl_->backend->local_storage(session_id), impl_->key, "local"),
            impl_->key, "local");
        store->set_access_listener(impl_->access_listener);
        auto* raw = store.get();
        impl_->local_stores.emplace(key, std::move(store));
        return *raw;
    }
    return *it->second;
}

KeyValueStore& EncryptedStorage::session_storage(std::string_view session_id) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    std::string key(session_id);
    auto it = impl_->session_stores.find(key);
    if (it == impl_->session_stores.end()) {
        auto store = std::make_unique<EncryptedKeyValueStore>(
            std::make_unique<EncryptedKeyValueStore>(impl_->backend->session_storage(session_id), impl_->key, "session"),
            impl_->key, "session");
        store->set_access_listener(impl_->access_listener);
        auto* raw = store.get();
        impl_->session_stores.emplace(key, std::move(store));
        return *raw;
    }
    return *it->second;
}

void EncryptedStorage::release_session(std::string_view session_id) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    std::string key(session_id);
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
    impl_->backend->set_access_listener(impl_->access_listener);
}

const std::string& EncryptedStorage::get_salt() const { return impl_->salt; }
const std::string& EncryptedStorage::get_key() const { return impl_->key; }

std::unique_ptr<Storage> make_encrypted_storage(std::unique_ptr<Storage> backend,
                                                 const std::string& password) {
    return std::make_unique<EncryptedStorage>(std::move(backend), password);
}

}  // namespace prowsetk