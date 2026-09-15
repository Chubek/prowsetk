#include <gtest/gtest.h>

#include "prowsetk/error.hpp"
#include "prowsetk/network_client.hpp"

using prowsetk::Error;
using prowsetk::HttpRequest;
using prowsetk::HttpResponse;
using prowsetk::MemoryNetworkClient;

namespace {

HttpRequest make_request(std::string method, std::string url) {
    HttpRequest request;
    request.method = std::move(method);
    request.url = std::move(url);
    return request;
}

}  // namespace

TEST(Network, ReturnsRegisteredResponses) {
    MemoryNetworkClient client;
    HttpResponse response;
    response.status = 200;
    response.body = "<html><title>Hi</title></html>";
    response.headers.emplace_back("Content-Type", "text/html");
    client.set_response("https://example.com/", response);

    const HttpResponse actual =
        client.send(make_request("GET", "https://example.com/"));
    EXPECT_EQ(actual.status, 200);
    EXPECT_EQ(actual.header("content-type"), "text/html");
    EXPECT_EQ(actual.final_url, "https://example.com/");
    EXPECT_TRUE(actual.ok());
}

TEST(Network, RecordsRequestsInOrder) {
    MemoryNetworkClient client;
    HttpResponse response;
    response.status = 204;
    client.set_handler([&](const HttpRequest&) { return response; });

    client.send(make_request("GET", "https://example.com/a"));
    client.send(make_request("POST", "https://example.com/b"));

    ASSERT_EQ(client.requests().size(), 2u);
    EXPECT_EQ(client.requests()[0].method, "GET");
    EXPECT_EQ(client.requests()[1].url, "https://example.com/b");
}

TEST(Network, MissingResponseThrows) {
    MemoryNetworkClient client;
    EXPECT_THROW(client.send(make_request("GET", "https://missing.test/")),
                 Error);
}

TEST(Network, HandlerOverridesRegisteredResponse) {
    MemoryNetworkClient client;
    HttpResponse registered;
    registered.status = 200;
    client.set_response("https://example.com/", registered);
    client.set_handler([](const HttpRequest&) {
        HttpResponse response;
        response.status = 503;
        return response;
    });
    EXPECT_EQ(client.send(make_request("GET", "https://example.com/")).status,
              503);
}
