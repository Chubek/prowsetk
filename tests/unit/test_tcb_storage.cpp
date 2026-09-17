#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

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

TEST(TCBStorage, BasicCookieOperations) {
    auto temp_dir = make_test_db_path("basic_cookie");
    auto storage = make_tcb_storage(temp_dir);
    
    Cookie cookie;
    cookie.name = "session";
    cookie.value = "abc123";
    cookie.domain = "example.com";
    cookie.path = "/";
    
    storage->cookies().set(parse_url("https://example.com/"), cookie);
    
    auto cookies = storage->cookies().get(parse_url("https://example.com/"));
    ASSERT_EQ(cookies.size(), 1u);
    EXPECT_EQ(cookies[0].name, "session");
    EXPECT_EQ(cookies[0].value, "abc123");
    EXPECT_EQ(cookies[0].domain, "example.com");
    
    std::filesystem::remove_all(temp_dir);
}

TEST(TCBStorage, CookiePersistence) {
    auto temp_dir = make_test_db_path("cookie_persistence");
    
    {
        auto storage = make_tcb_storage(temp_dir);
        Cookie cookie;
        cookie.name = "persistent";
        cookie.value = "persist_value";
        storage->cookies().set(parse_url("https://example.com/"), cookie);
    }
    
    {
        auto storage = make_tcb_storage(temp_dir);
        auto cookies = storage->cookies().get(parse_url("https://example.com/"));
        ASSERT_EQ(cookies.size(), 1u);
        EXPECT_EQ(cookies[0].name, "persistent");
        EXPECT_EQ(cookies[0].value, "persist_value");
    }
    
    std::filesystem::remove_all(temp_dir);
}

TEST(TCBStorage, CookieDomainScoping) {
    auto temp_dir = make_test_db_path("cookie_domain");
    auto storage = make_tcb_storage(temp_dir);
    
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

TEST(TCBStorage, CookieSecureFlag) {
    auto temp_dir = make_test_db_path("cookie_secure");
    auto storage = make_tcb_storage(temp_dir);
    
    Cookie secure_cookie;
    secure_cookie.name = "secure";
    secure_cookie.value = "secure_value";
    secure_cookie.secure = true;
    storage->cookies().set(parse_url("https://example.com/"), secure_cookie);
    
    EXPECT_EQ(storage->cookies().get(parse_url("https://example.com/")).size(), 1u);
    EXPECT_TRUE(storage->cookies().get(parse_url("http://example.com/")).empty());
    
    std::filesystem::remove_all(temp_dir);
}

TEST(TCBStorage, CookieHttpOnlyFlag) {
    auto temp_dir = make_test_db_path("cookie_httponly");
    auto storage = make_tcb_storage(temp_dir);
    
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

TEST(TCBStorage, CookieSameSiteAttribute) {
    auto temp_dir = make_test_db_path("cookie_samesite");
    auto storage = make_tcb_storage(temp_dir);
    
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

TEST(TCBStorage, CookieExpiration) {
    auto temp_dir = make_test_db_path("cookie_expiry");
    auto storage = make_tcb_storage(temp_dir);
    
    Cookie cookie;
    cookie.name = "expired";
    cookie.value = "old";
    cookie.expires_unix = 1;
    storage->cookies().set(parse_url("https://example.com/"), cookie);
    
    EXPECT_TRUE(storage->cookies().get(parse_url("https://example.com/")).empty());
    
    std::filesystem::remove_all(temp_dir);
}

TEST(TCBStorage, CookieHeaderGeneration) {
    auto temp_dir = make_test_db_path("cookie_header");
    auto storage = make_tcb_storage(temp_dir);
    
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

TEST(TCBStorage, CookiePathMatching) {
    auto temp_dir = make_test_db_path("cookie_path");
    auto storage = make_tcb_storage(temp_dir);
    
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

TEST(TCBStorage, LocalStorageBasicOperations) {
    auto temp_dir = make_test_db_path("local_storage");
    auto storage = make_tcb_storage(temp_dir);
    
    auto& store = storage->local_storage("session1");
    store.set("key1", "value1");
    store.set("key2", "value2");
    
    EXPECT_TRUE(store.get("key1").has_value());
    EXPECT_EQ(store.get("key1").value(), "value1");
    EXPECT_EQ(store.get("key2").value(), "value2");
    EXPECT_FALSE(store.get("nonexistent").has_value());
    
    std::filesystem::remove_all(temp_dir);
}

TEST(TCBStorage, LocalStoragePersistence) {
    auto temp_dir = make_test_db_path("local_persistence");
    
    {
        auto storage = make_tcb_storage(temp_dir);
        auto& store = storage->local_storage("session1");
        store.set("persistent_key", "persistent_value");
    }
    
    {
        auto storage = make_tcb_storage(temp_dir);
        auto& store = storage->local_storage("session1");
        EXPECT_TRUE(store.get("persistent_key").has_value());
        EXPECT_EQ(store.get("persistent_key").value(), "persistent_value");
    }
    
    std::filesystem::remove_all(temp_dir);
}

TEST(TCBStorage, LocalStorageRemove) {
    auto temp_dir = make_test_db_path("local_remove");
    auto storage = make_tcb_storage(temp_dir);
    
    auto& store = storage->local_storage("session1");
    store.set("key", "value");
    EXPECT_TRUE(store.remove("key"));
    EXPECT_FALSE(store.remove("key"));
    EXPECT_FALSE(store.get("key").has_value());
    
    std::filesystem::remove_all(temp_dir);
}

TEST(TCBStorage, LocalStorageClear) {
    auto temp_dir = make_test_db_path("local_clear");
    auto storage = make_tcb_storage(temp_dir);
    
    auto& store = storage->local_storage("session1");
    store.set("a", "1");
    store.set("b", "2");
    store.set("c", "3");
    store.clear();
    
    EXPECT_FALSE(store.get("a").has_value());
    EXPECT_FALSE(store.get("b").has_value());
    EXPECT_FALSE(store.get("c").has_value());
    EXPECT_TRUE(store.keys().empty());
    
    std::filesystem::remove_all(temp_dir);
}

TEST(TCBStorage, LocalStorageKeysEnumeration) {
    auto temp_dir = make_test_db_path("local_keys");
    auto storage = make_tcb_storage(temp_dir);
    
    auto& store = storage->local_storage("session1");
    store.set("a", "1");
    store.set("b", "2");
    store.set("c", "3");
    
    auto keys = store.keys();
    EXPECT_EQ(keys.size(), 3u);
    std::sort(keys.begin(), keys.end());
    EXPECT_EQ(keys, (std::vector<std::string>{"a", "b", "c"}));
    
    std::filesystem::remove_all(temp_dir);
}

TEST(TCBStorage, SessionStorageBasicOperations) {
    auto temp_dir = make_test_db_path("session_storage");
    auto storage = make_tcb_storage(temp_dir);
    
    auto& store = storage->session_storage("session1");
    store.set("temp_key", "temp_value");
    
    EXPECT_TRUE(store.get("temp_key").has_value());
    EXPECT_EQ(store.get("temp_key").value(), "temp_value");
    
    std::filesystem::remove_all(temp_dir);
}

TEST(TCBStorage, SessionStorageIsolation) {
    auto temp_dir = make_test_db_path("session_isolation");
    auto storage = make_tcb_storage(temp_dir);
    
    storage->session_storage("session1").set("key", "value1");
    storage->session_storage("session2").set("key", "value2");
    
    EXPECT_EQ(storage->session_storage("session1").get("key").value(), "value1");
    EXPECT_EQ(storage->session_storage("session2").get("key").value(), "value2");
    
    std::filesystem::remove_all(temp_dir);
}

TEST(TCBStorage, SessionStorageRelease) {
    auto temp_dir = make_test_db_path("session_release");
    auto storage = make_tcb_storage(temp_dir);
    
    storage->session_storage("session1").set("key", "value");
    storage->release_session("session1");
    
    EXPECT_FALSE(storage->session_storage("session1").get("key").has_value());
    
    std::filesystem::remove_all(temp_dir);
}

TEST(TCBStorage, MultipleSessionsLocalStorageIsolation) {
    auto temp_dir = make_test_db_path("multi_session");
    auto storage = make_tcb_storage(temp_dir);
    
    storage->local_storage("session1").set("shared_key", "value1");
    storage->local_storage("session2").set("shared_key", "value2");
    
    EXPECT_EQ(storage->local_storage("session1").get("shared_key").value(), "value1");
    EXPECT_EQ(storage->local_storage("session2").get("shared_key").value(), "value2");
    
    std::filesystem::remove_all(temp_dir);
}

TEST(TCBStorage, CookieJarClear) {
    auto temp_dir = make_test_db_path("jar_clear");
    auto storage = make_tcb_storage(temp_dir);
    
    Cookie cookie;
    cookie.name = "test";
    cookie.value = "value";
    storage->cookies().set(parse_url("https://example.com/"), cookie);
    
    storage->cookies().clear();
    EXPECT_TRUE(storage->cookies().get(parse_url("https://example.com/")).empty());
    
    std::filesystem::remove_all(temp_dir);
}

TEST(TCBStorage, CookieAllReturnsAllCookies) {
    auto temp_dir = make_test_db_path("cookie_all");
    auto storage = make_tcb_storage(temp_dir);
    
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

TEST(TCBStorage, StorageAccessListenerCookies) {
    auto temp_dir = make_test_db_path("listener_cookies");
    auto storage = make_tcb_storage(temp_dir);
    
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

TEST(TCBStorage, StorageAccessListenerKeyValue) {
    auto temp_dir = make_test_db_path("listener_kv");
    auto storage = make_tcb_storage(temp_dir);
    
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

TEST(TCBStorage, ConcurrentAccessThreadSafety) {
    auto temp_dir = make_test_db_path("concurrent");
    auto storage = make_tcb_storage(temp_dir);
    
    const int num_threads = 4;
    const int ops_per_thread = 100;
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

TEST(TCBStorage, EmptyValues) {
    auto temp_dir = make_test_db_path("empty_values");
    auto storage = make_tcb_storage(temp_dir);
    
    auto& store = storage->local_storage("s1");
    store.set("empty_key", "");
    
    EXPECT_TRUE(store.get("empty_key").has_value());
    EXPECT_EQ(store.get("empty_key").value(), "");
    
    std::filesystem::remove_all(temp_dir);
}

TEST(TCBStorage, LargeValues) {
    auto temp_dir = make_test_db_path("large_values");
    auto storage = make_tcb_storage(temp_dir);
    
    std::string large_value(10000, 'x');
    auto& store = storage->local_storage("s1");
    store.set("large_key", large_value);
    
    EXPECT_TRUE(store.get("large_key").has_value());
    EXPECT_EQ(store.get("large_key").value(), large_value);
    
    std::filesystem::remove_all(temp_dir);
}

TEST(TCBStorage, UnicodeKeysAndValues) {
    auto temp_dir = make_test_db_path("unicode");
    auto storage = make_tcb_storage(temp_dir);
    
    auto& store = storage->local_storage("s1");
    store.set("ключ", "значение");
    store.set("🔑", "🗝️");
    store.set("clé", "valeur");
    
    EXPECT_EQ(store.get("ключ").value(), "значение");
    EXPECT_EQ(store.get("🔑").value(), "🗝️");
    EXPECT_EQ(store.get("clé").value(), "valeur");
    
    std::filesystem::remove_all(temp_dir);
}

TEST(TCBStorage, CookieWithNullBytesInValue) {
    auto temp_dir = make_test_db_path("null_bytes");
    auto storage = make_tcb_storage(temp_dir);
    
    Cookie cookie;
    cookie.name = "test";
    cookie.value = std::string("val\0ue", 6);
    storage->cookies().set(parse_url("https://example.com/"), cookie);
    
    auto cookies = storage->cookies().get(parse_url("https://example.com/"));
    ASSERT_EQ(cookies.size(), 1u);
    
    std::filesystem::remove_all(temp_dir);
}

TEST(TCBStorage, CookieWithSpecialCharacters) {
    auto temp_dir = make_test_db_path("special_chars");
    auto storage = make_tcb_storage(temp_dir);
    
    Cookie cookie;
    cookie.name = "special";
    cookie.value = "value;with,special=chars";
    cookie.domain = "example.com";
    cookie.path = "/path/with/slashes";
    storage->cookies().set(parse_url("https://example.com/path/with/slashes"), cookie);
    
    auto cookies = storage->cookies().get(parse_url("https://example.com/path/with/slashes"));
    ASSERT_EQ(cookies.size(), 1u);
    EXPECT_EQ(cookies[0].value, "value;with,special=chars");
    
    std::filesystem::remove_all(temp_dir);
}

TEST(TCBStorage, MultipleCookieJars) {
    auto temp_dir1 = make_test_db_path("jar1");
    auto temp_dir2 = make_test_db_path("jar2");
    
    auto storage1 = make_tcb_storage(temp_dir1);
    auto storage2 = make_tcb_storage(temp_dir2);
    
    Cookie cookie;
    cookie.name = "jar1";
    cookie.value = "value1";
    storage1->cookies().set(parse_url("https://example.com/"), cookie);
    
    cookie.name = "jar2";
    cookie.value = "value2";
    storage2->cookies().set(parse_url("https://example.com/"), cookie);
    
    EXPECT_EQ(storage1->cookies().get(parse_url("https://example.com/"))[0].value, "value1");
    EXPECT_EQ(storage2->cookies().get(parse_url("https://example.com/"))[0].value, "value2");
    
    std::filesystem::remove_all(temp_dir1);
    std::filesystem::remove_all(temp_dir2);
}

TEST(TCBStorage, CookieSameNameDifferentPaths) {
    auto temp_dir = make_test_db_path("same_name_diff_path");
    auto storage = make_tcb_storage(temp_dir);
    
    Cookie cookie1;
    cookie1.name = "same";
    cookie1.value = "value1";
    cookie1.path = "/app1";
    storage->cookies().set(parse_url("https://example.com/app1/"), cookie1);
    
    Cookie cookie2;
    cookie2.name = "same";
    cookie2.value = "value2";
    cookie2.path = "/app2";
    storage->cookies().set(parse_url("https://example.com/app2/"), cookie2);
    
    auto cookies1 = storage->cookies().get(parse_url("https://example.com/app1/"));
    auto cookies2 = storage->cookies().get(parse_url("https://example.com/app2/"));
    
    ASSERT_EQ(cookies1.size(), 1u);
    ASSERT_EQ(cookies2.size(), 1u);
    EXPECT_EQ(cookies1[0].value, "value1");
    EXPECT_EQ(cookies2[0].value, "value2");
    
    std::filesystem::remove_all(temp_dir);
}

TEST(TCBStorage, CookieSameNameDifferentDomains) {
    auto temp_dir = make_test_db_path("same_name_diff_domain");
    auto storage = make_tcb_storage(temp_dir);
    
    Cookie cookie1;
    cookie1.name = "same";
    cookie1.value = "value1";
    cookie1.domain = "site1.com";
    storage->cookies().set(parse_url("https://site1.com/"), cookie1);
    
    Cookie cookie2;
    cookie2.name = "same";
    cookie2.value = "value2";
    cookie2.domain = "site2.com";
    storage->cookies().set(parse_url("https://site2.com/"), cookie2);
    
    auto cookies1 = storage->cookies().get(parse_url("https://site1.com/"));
    auto cookies2 = storage->cookies().get(parse_url("https://site2.com/"));
    
    ASSERT_EQ(cookies1.size(), 1u);
    ASSERT_EQ(cookies2.size(), 1u);
    EXPECT_EQ(cookies1[0].value, "value1");
    EXPECT_EQ(cookies2[0].value, "value2");
    
    std::filesystem::remove_all(temp_dir);
}

TEST(TCBStorage, CookieReplacesExisting) {
    auto temp_dir = make_test_db_path("cookie_replace");
    auto storage = make_tcb_storage(temp_dir);
    
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

TEST(TCBStorage, CookieDomainNormalization) {
    auto temp_dir = make_test_db_path("domain_normalize");
    auto storage = make_tcb_storage(temp_dir);
    
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

TEST(TCBStorage, CookiePathNormalization) {
    auto temp_dir = make_test_db_path("path_normalize");
    auto storage = make_tcb_storage(temp_dir);
    
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

TEST(TCBStorage, CookieHostOnlyDefault) {
    auto temp_dir = make_test_db_path("host_only_default");
    auto storage = make_tcb_storage(temp_dir);
    
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

TEST(TCBStorage, CookieSortByPathSpecificity) {
    auto temp_dir = make_test_db_path("cookie_sort");
    auto storage = make_tcb_storage(temp_dir);
    
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

TEST(TCBStorage, SessionStorageClear) {
    auto temp_dir = make_test_db_path("session_clear");
    auto storage = make_tcb_storage(temp_dir);
    
    auto& store = storage->session_storage("s1");
    store.set("k", "v");
    store.clear();
    
    EXPECT_FALSE(store.get("k").has_value());
    EXPECT_TRUE(store.keys().empty());
    
    std::filesystem::remove_all(temp_dir);
}

TEST(TCBStorage, LocalStorageAndSessionStorageSeparate) {
    auto temp_dir = make_test_db_path("local_session_separate");
    auto storage = make_tcb_storage(temp_dir);
    
    storage->local_storage("s1").set("key", "local");
    storage->session_storage("s1").set("key", "session");
    
    EXPECT_EQ(storage->local_storage("s1").get("key").value(), "local");
    EXPECT_EQ(storage->session_storage("s1").get("key").value(), "session");
    
    std::filesystem::remove_all(temp_dir);
}
