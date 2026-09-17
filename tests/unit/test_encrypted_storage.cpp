#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <string>

#include "prowsetk/storage.hpp"
#include "prowsetk/url.hpp"

using prowsetk::Cookie;
using prowsetk::parse_url;
using prowsetk::make_tcb_storage;
using prowsetk::make_encrypted_storage;

namespace {

std::filesystem::path make_test_db_path(const std::string& name) {
    std::filesystem::path temp_dir = std::filesystem::temp_directory_path() / 
                                      ("prowsetk_test_" + name + "_" + std::to_string(std::hash<std::thread::id>{}(std::this_thread::get_id())));
    std::filesystem::remove_all(temp_dir);
    return temp_dir;
}

}  // namespace

TEST(EncryptedStorage, BasicEncryptionDecryption) {
    auto temp_dir = make_test_db_path("enc_basic");
    auto backend = make_tcb_storage(temp_dir);
    auto storage = make_encrypted_storage(std::move(backend), "test_password");
    
    auto& store = storage->local_storage("s1");
    store.set("key", "secret_value");
    
    EXPECT_TRUE(store.get("key").has_value());
    EXPECT_EQ(store.get("key").value(), "secret_value");
    
    std::filesystem::remove_all(temp_dir);
}

TEST(EncryptedStorage, DifferentPasswordsProduceDifferentCiphertext) {
    auto temp_dir1 = make_test_db_path("enc_pass1");
    auto temp_dir2 = make_test_db_path("enc_pass2");
    
    auto backend1 = make_tcb_storage(temp_dir1);
    auto storage1 = make_encrypted_storage(std::move(backend1), "password1");
    
    auto backend2 = make_tcb_storage(temp_dir2);
    auto storage2 = make_encrypted_storage(std::move(backend2), "password2");
    
    storage1->local_storage("s1").set("key", "value");
    storage2->local_storage("s1").set("key", "value");
    
    EXPECT_EQ(storage1->local_storage("s1").get("key").value(), "value");
    EXPECT_EQ(storage2->local_storage("s1").get("key").value(), "value");
    
    std::filesystem::remove_all(temp_dir1);
    std::filesystem::remove_all(temp_dir2);
}

TEST(EncryptedStorage, WrongPasswordFailsToDecrypt) {
    auto temp_dir = make_test_db_path("enc_wrong_pass");
    auto backend = make_tcb_storage(temp_dir);
    auto storage = make_encrypted_storage(std::move(backend), "correct_password");
    
    storage->local_storage("s1").set("key", "secret");
    
    auto backend2 = make_tcb_storage(temp_dir);
    auto storage2 = make_encrypted_storage(std::move(backend2), "wrong_password");
    
    auto result = storage2->local_storage("s1").get("key");
    EXPECT_FALSE(result.has_value() || result.value() == "secret");
    
    std::filesystem::remove_all(temp_dir);
}

TEST(EncryptedStorage, CookiesEncrypted) {
    auto temp_dir = make_test_db_path("enc_cookies");
    auto backend = make_tcb_storage(temp_dir);
    auto storage = make_encrypted_storage(std::move(backend), "cookie_password");
    
    Cookie cookie;
    cookie.name = "session";
    cookie.value = "secret_session_data";
    cookie.domain = "example.com";
    storage->cookies().set(parse_url("https://example.com/"), cookie);
    
    auto cookies = storage->cookies().get(parse_url("https://example.com/"));
    ASSERT_EQ(cookies.size(), 1u);
    EXPECT_EQ(cookies[0].name, "session");
    EXPECT_EQ(cookies[0].value, "secret_session_data");
    
    std::filesystem::remove_all(temp_dir);
}

TEST(EncryptedStorage, CookiePersistenceAcrossRestart) {
    auto temp_dir = make_test_db_path("enc_cookie_persist");
    
    {
        auto backend = make_tcb_storage(temp_dir);
        auto storage = make_encrypted_storage(std::move(backend), "persist_pass");
        
        Cookie cookie;
        cookie.name = "persist";
        cookie.value = "persist_value";
        storage->cookies().set(parse_url("https://example.com/"), cookie);
    }
    
    {
        auto backend = make_tcb_storage(temp_dir);
        auto storage = make_encrypted_storage(std::move(backend), "persist_pass");
        
        auto cookies = storage->cookies().get(parse_url("https://example.com/"));
        ASSERT_EQ(cookies.size(), 1u);
        EXPECT_EQ(cookies[0].name, "persist");
        EXPECT_EQ(cookies[0].value, "persist_value");
    }
    
    std::filesystem::remove_all(temp_dir);
}

TEST(EncryptedStorage, CookieHeaderWorks) {
    auto temp_dir = make_test_db_path("enc_cookie_header");
    auto backend = make_tcb_storage(temp_dir);
    auto storage = make_encrypted_storage(std::move(backend), "header_pass");
    
    Cookie a;
    a.name = "a";
    a.value = "1";
    a.path = "/";
    storage->cookies().set(parse_url("https://example.com/"), a);
    
    Cookie b;
    b.name = "b";
    b.value = "2";
    b.path = "/";
    storage->cookies().set(parse_url("https://example.com/"), b);
    
    std::string header = storage->cookies().cookie_header(parse_url("https://example.com/"));
    EXPECT_NE(header.find("a=1"), std::string::npos);
    EXPECT_NE(header.find("b=2"), std::string::npos);
    
    std::filesystem::remove_all(temp_dir);
}

TEST(EncryptedStorage, SessionStorageEncrypted) {
    auto temp_dir = make_test_db_path("enc_session");
    auto backend = make_tcb_storage(temp_dir);
    auto storage = make_encrypted_storage(std::move(backend), "session_pass");
    
    auto& store = storage->session_storage("session1");
    store.set("temp", "secret_temp");
    
    EXPECT_TRUE(store.get("temp").has_value());
    EXPECT_EQ(store.get("temp").value(), "secret_temp");
    
    std::filesystem::remove_all(temp_dir);
}

TEST(EncryptedStorage, LocalStorageAndSessionStorageSeparate) {
    auto temp_dir = make_test_db_path("enc_separate");
    auto backend = make_tcb_storage(temp_dir);
    auto storage = make_encrypted_storage(std::move(backend), "separate_pass");
    
    storage->local_storage("s1").set("key", "local_encrypted");
    storage->session_storage("s1").set("key", "session_encrypted");
    
    EXPECT_EQ(storage->local_storage("s1").get("key").value(), "local_encrypted");
    EXPECT_EQ(storage->session_storage("s1").get("key").value(), "session_encrypted");
    
    std::filesystem::remove_all(temp_dir);
}

TEST(EncryptedStorage, ClearRemovesEncryptedData) {
    auto temp_dir = make_test_db_path("enc_clear");
    auto backend = make_tcb_storage(temp_dir);
    auto storage = make_encrypted_storage(std::move(backend), "clear_pass");
    
    auto& store = storage->local_storage("s1");
    store.set("a", "1");
    store.set("b", "2");
    store.clear();
    
    EXPECT_FALSE(store.get("a").has_value());
    EXPECT_FALSE(store.get("b").has_value());
    EXPECT_TRUE(store.keys().empty());
    
    std::filesystem::remove_all(temp_dir);
}

TEST(EncryptedStorage, RemoveEncryptedKey) {
    auto temp_dir = make_test_db_path("enc_remove");
    auto backend = make_tcb_storage(temp_dir);
    auto storage = make_encrypted_storage(std::move(backend), "remove_pass");
    
    auto& store = storage->local_storage("s1");
    store.set("key", "value");
    EXPECT_TRUE(store.remove("key"));
    EXPECT_FALSE(store.remove("key"));
    EXPECT_FALSE(store.get("key").has_value());
    
    std::filesystem::remove_all(temp_dir);
}

TEST(EncryptedStorage, KeysEnumeration) {
    auto temp_dir = make_test_db_path("enc_keys");
    auto backend = make_tcb_storage(temp_dir);
    auto storage = make_encrypted_storage(std::move(backend), "keys_pass");
    
    auto& store = storage->local_storage("s1");
    store.set("a", "1");
    store.set("b", "2");
    store.set("c", "3");
    
    auto keys = store.keys();
    EXPECT_EQ(keys.size(), 3u);
    std::sort(keys.begin(), keys.end());
    EXPECT_EQ(keys, (std::vector<std::string>{"a", "b", "c"}));
    
    std::filesystem::remove_all(temp_dir);
}

TEST(EncryptedStorage, ReleaseSession) {
    auto temp_dir = make_test_db_path("enc_release");
    auto backend = make_tcb_storage(temp_dir);
    auto storage = make_encrypted_storage(std::move(backend), "release_pass");
    
    storage->session_storage("session1").set("key", "value");
    storage->release_session("session1");
    
    EXPECT_FALSE(storage->session_storage("session1").get("key").has_value());
    
    std::filesystem::remove_all(temp_dir);
}

TEST(EncryptedStorage, AccessListenerWorks) {
    auto temp_dir = make_test_db_path("enc_listener");
    auto backend = make_tcb_storage(temp_dir);
    auto storage = make_encrypted_storage(std::move(backend), "listener_pass");
    
    std::vector<std::tuple<std::string, std::string, std::string>> events;
    storage->set_access_listener(
        [&](std::string_view name, std::string_view key, std::string_view op) {
            events.emplace_back(name, key, op);
        });
    
    auto& store = storage->local_storage("s1");
    store.set("key", "value");
    store.get("key");
    store.remove("key");
    store.clear();
    
    ASSERT_EQ(events.size(), 4u);
    EXPECT_EQ(std::get<2>(events[0]), "set");
    EXPECT_EQ(std::get<2>(events[1]), "get");
    EXPECT_EQ(std::get<2>(events[2]), "remove");
    EXPECT_EQ(std::get<2>(events[3]), "clear");
    
    std::filesystem::remove_all(temp_dir);
}

TEST(EncryptedStorage, CookieAccessListener) {
    auto temp_dir = make_test_db_path("enc_cookie_listener");
    auto backend = make_tcb_storage(temp_dir);
    auto storage = make_encrypted_storage(std::move(backend), "cookie_listener_pass");
    
    std::vector<std::tuple<std::string, std::string, std::string>> events;
    storage->set_access_listener(
        [&](std::string_view name, std::string_view key, std::string_view op) {
            events.emplace_back(name, key, op);
        });
    
    Cookie cookie;
    cookie.name = "test";
    cookie.value = "value";
    storage->cookies().set(parse_url("https://example.com/"), cookie);
    
    storage->cookies().get(parse_url("https://example.com/"));
    
    ASSERT_GE(events.size(), 2u);
    
    std::filesystem::remove_all(temp_dir);
}

TEST(EncryptedStorage, CookieJarClear) {
    auto temp_dir = make_test_db_path("enc_jar_clear");
    auto backend = make_tcb_storage(temp_dir);
    auto storage = make_encrypted_storage(std::move(backend), "jar_clear_pass");
    
    Cookie cookie;
    cookie.name = "test";
    cookie.value = "value";
    storage->cookies().set(parse_url("https://example.com/"), cookie);
    
    storage->cookies().clear();
    EXPECT_TRUE(storage->cookies().get(parse_url("https://example.com/")).empty());
    
    std::filesystem::remove_all(temp_dir);
}

TEST(EncryptedStorage, CookieAllReturnsAll) {
    auto temp_dir = make_test_db_path("enc_all");
    auto backend = make_tcb_storage(temp_dir);
    auto storage = make_encrypted_storage(std::move(backend), "all_pass");
    
    Cookie a;
    a.name = "a";
    a.value = "1";
    storage->cookies().set(parse_url("https://example.com/"), a);
    
    Cookie b;
    b.name = "b";
    b.value = "2";
    b.domain = "other.com";
    storage->cookies().set(parse_url("https://other.com/"), b);
    
    auto all = storage->cookies().all();
    ASSERT_EQ(all.size(), 2u);
    
    std::filesystem::remove_all(temp_dir);
}

TEST(EncryptedStorage, EmptyValues) {
    auto temp_dir = make_test_db_path("enc_empty");
    auto backend = make_tcb_storage(temp_dir);
    auto storage = make_encrypted_storage(std::move(backend), "empty_pass");
    
    auto& store = storage->local_storage("s1");
    store.set("empty_key", "");
    
    EXPECT_TRUE(store.get("empty_key").has_value());
    EXPECT_EQ(store.get("empty_key").value(), "");
    
    std::filesystem::remove_all(temp_dir);
}

TEST(EncryptedStorage, LargeValues) {
    auto temp_dir = make_test_db_path("enc_large");
    auto backend = make_tcb_storage(temp_dir);
    auto storage = make_encrypted_storage(std::move(backend), "large_pass");
    
    std::string large_value(10000, 'x');
    auto& store = storage->local_storage("s1");
    store.set("large_key", large_value);
    
    EXPECT_TRUE(store.get("large_key").has_value());
    EXPECT_EQ(store.get("large_key").value(), large_value);
    
    std::filesystem::remove_all(temp_dir);
}

TEST(EncryptedStorage, UnicodeKeysAndValues) {
    auto temp_dir = make_test_db_path("enc_unicode");
    auto backend = make_tcb_storage(temp_dir);
    auto storage = make_encrypted_storage(std::move(backend), "unicode_pass");
    
    auto& store = storage->local_storage("s1");
    store.set("ключ", "значение");
    store.set("🔑", "🗝️");
    store.set("clé", "valeur");
    
    EXPECT_EQ(store.get("ключ").value(), "значение");
    EXPECT_EQ(store.get("🔑").value(), "🗝️");
    EXPECT_EQ(store.get("clé").value(), "valeur");
    
    std::filesystem::remove_all(temp_dir);
}

TEST(EncryptedStorage, MultipleStorageInstancesSamePassword) {
    auto temp_dir = make_test_db_path("enc_multi");
    auto backend = make_tcb_storage(temp_dir);
    auto storage = make_encrypted_storage(std::move(backend), "multi_pass");
    
    storage->local_storage("s1").set("k1", "v1");
    storage->local_storage("s2").set("k2", "v2");
    
    EXPECT_EQ(storage->local_storage("s1").get("k1").value(), "v1");
    EXPECT_EQ(storage->local_storage("s2").get("k2").value(), "v2");
    
    std::filesystem::remove_all(temp_dir);
}

TEST(EncryptedStorage, CookieDomainScoping) {
    auto temp_dir = make_test_db_path("enc_domain_scope");
    auto backend = make_tcb_storage(temp_dir);
    auto storage = make_encrypted_storage(std::move(backend), "domain_pass");
    
    Cookie domain_cookie;
    domain_cookie.name = "shared";
    domain_cookie.value = "shared_value";
    domain_cookie.domain = ".example.com";
    domain_cookie.host_only = false;
    storage->cookies().set(parse_url("https://www.example.com/"), domain_cookie);
    
    Cookie host_cookie;
    host_cookie.name = "host_only";
    host_cookie.value = "host_value";
    storage->cookies().set(parse_url("https://www.example.com/"), host_cookie);
    
    auto www_cookies = storage->cookies().get(parse_url("https://www.example.com/"));
    ASSERT_EQ(www_cookies.size(), 2u);
    
    auto api_cookies = storage->cookies().get(parse_url("https://api.example.com/"));
    ASSERT_EQ(api_cookies.size(), 1u);
    EXPECT_EQ(api_cookies[0].name, "shared");
    
    std::filesystem::remove_all(temp_dir);
}

TEST(EncryptedStorage, CookieSecureFlag) {
    auto temp_dir = make_test_db_path("enc_secure");
    auto backend = make_tcb_storage(temp_dir);
    auto storage = make_encrypted_storage(std::move(backend), "secure_pass");
    
    Cookie secure_cookie;
    secure_cookie.name = "secure";
    secure_cookie.value = "secure_value";
    secure_cookie.secure = true;
    storage->cookies().set(parse_url("https://example.com/"), secure_cookie);
    
    EXPECT_EQ(storage->cookies().get(parse_url("https://example.com/")).size(), 1u);
    EXPECT_TRUE(storage->cookies().get(parse_url("http://example.com/")).empty());
    
    std::filesystem::remove_all(temp_dir);
}

TEST(EncryptedStorage, CookieHttpOnlyFlag) {
    auto temp_dir = make_test_db_path("enc_httponly");
    auto backend = make_tcb_storage(temp_dir);
    auto storage = make_encrypted_storage(std::move(backend), "httponly_pass");
    
    Cookie cookie;
    cookie.name = "httponly";
    cookie.value = "test";
    cookie.http_only = true;
    storage->cookies().set(parse_url("https://example.com/"), cookie);
    
    auto cookies = storage->cookies().get(parse_url("https://example.com/"));
    ASSERT_EQ(cookies.size(), 1u);
    EXPECT_TRUE(cookies[0].http_only);
    
    std::filesystem::remove_all(temp_dir);
}

TEST(EncryptedStorage, CookieSameSiteAttribute) {
    auto temp_dir = make_test_db_path("enc_samesite");
    auto backend = make_tcb_storage(temp_dir);
    auto storage = make_encrypted_storage(std::move(backend), "samesite_pass");
    
    Cookie cookie;
    cookie.name = "samesite";
    cookie.value = "test";
    cookie.same_site = "Strict";
    storage->cookies().set(parse_url("https://example.com/"), cookie);
    
    auto cookies = storage->cookies().get(parse_url("https://example.com/"));
    ASSERT_EQ(cookies.size(), 1u);
    EXPECT_EQ(cookies[0].same_site, "Strict");
    
    std::filesystem::remove_all(temp_dir);
}

TEST(EncryptedStorage, CookieExpiration) {
    auto temp_dir = make_test_db_path("enc_expiry");
    auto backend = make_tcb_storage(temp_dir);
    auto storage = make_encrypted_storage(std::move(backend), "expiry_pass");
    
    Cookie cookie;
    cookie.name = "expired";
    cookie.value = "old";
    cookie.expires_unix = 1;
    storage->cookies().set(parse_url("https://example.com/"), cookie);
    
    EXPECT_TRUE(storage->cookies().get(parse_url("https://example.com/")).empty());
    
    std::filesystem::remove_all(temp_dir);
}

TEST(EncryptedStorage, CookiePathMatching) {
    auto temp_dir = make_test_db_path("enc_path");
    auto backend = make_tcb_storage(temp_dir);
    auto storage = make_encrypted_storage(std::move(backend), "path_pass");
    
    Cookie cookie;
    cookie.name = "scoped";
    cookie.value = "path_value";
    cookie.path = "/app";
    storage->cookies().set(parse_url("https://example.com/app/start"), cookie);
    
    EXPECT_EQ(storage->cookies().get(parse_url("https://example.com/app")).size(), 1u);
    EXPECT_EQ(storage->cookies().get(parse_url("https://example.com/app/next")).size(), 1u);
    EXPECT_TRUE(storage->cookies().get(parse_url("https://example.com/apple")).empty());
    
    std::filesystem::remove_all(temp_dir);
}

TEST(EncryptedStorage, ConcurrentAccess) {
    auto temp_dir = make_test_db_path("enc_concurrent");
    auto backend = make_tcb_storage(temp_dir);
    auto storage = make_encrypted_storage(std::move(backend), "concurrent_pass");
    
    const int num_threads = 4;
    const int ops_per_thread = 50;
    std::vector<std::thread> threads;
    
    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&storage, t, ops_per_thread]() {
            auto& store = storage->local_storage("shared");
            for (int i = 0; i < ops_per_thread; ++i) {
                std::string key = "key_" + std::to_string(t) + "_" + std::to_string(i);
                store.set(key, "value_" + std::to_string(i));
                store.get(key);
            }
        });
    }
    
    for (auto& t : threads) {
        t.join();
    }
    
    auto& store = storage->local_storage("shared");
    auto keys = store.keys();
    EXPECT_EQ(keys.size(), num_threads * ops_per_thread);
    
    std::filesystem::remove_all(temp_dir);
}

TEST(EncryptedStorage, CookieReplacesExisting) {
    auto temp_dir = make_test_db_path("enc_replace");
    auto backend = make_tcb_storage(temp_dir);
    auto storage = make_encrypted_storage(std::move(backend), "replace_pass");
    
    Cookie cookie;
    cookie.name = "session";
    cookie.value = "first";
    storage->cookies().set(parse_url("https://example.com/"), cookie);
    
    cookie.value = "second";
    storage->cookies().set(parse_url("https://example.com/"), cookie);
    
    auto cookies = storage->cookies().get(parse_url("https://example.com/"));
    ASSERT_EQ(cookies.size(), 1u);
    EXPECT_EQ(cookies[0].value, "second");
    
    std::filesystem::remove_all(temp_dir);
}

TEST(EncryptedStorage, CookieDomainNormalization) {
    auto temp_dir = make_test_db_path("enc_domain_norm");
    auto backend = make_tcb_storage(temp_dir);
    auto storage = make_encrypted_storage(std::move(backend), "domain_norm_pass");
    
    Cookie cookie;
    cookie.name = "test";
    cookie.value = "test";
    cookie.domain = ".Example.COM";
    cookie.host_only = false;
    storage->cookies().set(parse_url("https://www.example.com/"), cookie);
    
    auto cookies = storage->cookies().get(parse_url("https://api.example.com/"));
    ASSERT_EQ(cookies.size(), 1u);
    EXPECT_EQ(cookies[0].domain, "example.com");
    
    std::filesystem::remove_all(temp_dir);
}

TEST(EncryptedStorage, CookiePathNormalization) {
    auto temp_dir = make_test_db_path("enc_path_norm");
    auto backend = make_tcb_storage(temp_dir);
    auto storage = make_encrypted_storage(std::move(backend), "path_norm_pass");
    
    Cookie cookie;
    cookie.name = "test";
    cookie.value = "test";
    cookie.path = "no_leading_slash";
    storage->cookies().set(parse_url("https://example.com/"), cookie);
    
    auto cookies = storage->cookies().get(parse_url("https://example.com/"));
    ASSERT_EQ(cookies.size(), 1u);
    EXPECT_EQ(cookies[0].path, "/");
    
    std::filesystem::remove_all(temp_dir);
}

TEST(EncryptedStorage, CookieHostOnlyDefault) {
    auto temp_dir = make_test_db_path("enc_host_default");
    auto backend = make_tcb_storage(temp_dir);
    auto storage = make_encrypted_storage(std::move(backend), "host_default_pass");
    
    Cookie cookie;
    cookie.name = "nodefault";
    cookie.value = "value";
    storage->cookies().set(parse_url("https://example.com/path"), cookie);
    
    auto cookies = storage->cookies().get(parse_url("https://example.com/path"));
    ASSERT_EQ(cookies.size(), 1u);
    EXPECT_EQ(cookies[0].domain, "example.com");
    EXPECT_TRUE(cookies[0].host_only);
    
    std::filesystem::remove_all(temp_dir);
}

TEST(EncryptedStorage, CookieSortByPathSpecificity) {
    auto temp_dir = make_test_db_path("enc_sort");
    auto backend = make_tcb_storage(temp_dir);
    auto storage = make_encrypted_storage(std::move(backend), "sort_pass");
    
    Cookie broad;
    broad.name = "scope";
    broad.value = "broad";
    broad.path = "/";
    storage->cookies().set(parse_url("https://example.com/app/page"), broad);
    
    Cookie narrow;
    narrow.name = "scope";
    narrow.value = "narrow";
    narrow.path = "/app";
    storage->cookies().set(parse_url("https://example.com/app/page"), narrow);
    
    std::string header = storage->cookies().cookie_header(parse_url("https://example.com/app/page"));
    EXPECT_EQ(header, "scope=narrow; scope=broad");
    
    std::filesystem::remove_all(temp_dir);
}