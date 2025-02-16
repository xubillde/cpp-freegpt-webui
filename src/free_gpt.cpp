#include <chrono>
#include <format>
#include <iostream>
#include <queue>
#include <random>
#include <ranges>
#include <regex>
#include <tuple>
#include <vector>

#include <curl/curl.h>
#include <openssl/md5.h>
#include <spdlog/spdlog.h>
#include <boost/asio/as_tuple.hpp>
#include <boost/asio/experimental/channel.hpp>
#include <plusaes/plusaes.hpp>

#include "free_gpt.h"
#include "helper.hpp"

namespace {

// clang-format off
namespace detail {

template <typename C>
struct to_helper {};

template <typename Container, std::ranges::range R>
    requires std::convertible_to<std::ranges::range_value_t<R>, typename Container::value_type>
Container operator|(R&& r, to_helper<Container>) {
    return Container{r.begin(), r.end()};
}

}  // namespace detail

template <std::ranges::range Container>
    requires(!std::ranges::view<Container>)
inline auto to() {
    return detail::to_helper<Container>{};
}
// clang-format on

std::string md5(const std::string& str, bool reverse = true) {
    unsigned char hash[MD5_DIGEST_LENGTH];

    MD5_CTX md5;
    MD5_Init(&md5);
    MD5_Update(&md5, str.c_str(), str.size());
    MD5_Final(hash, &md5);

    std::stringstream ss;
    for (int i = 0; i < MD5_DIGEST_LENGTH; i++)
        ss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(hash[i]);
    auto md5_str = ss.str();
    if (reverse)
        std::ranges::reverse(md5_str);
    return md5_str;
}

auto splitString(const std::string& input, const std::string& delimiter) {
    std::vector<std::string> fields;
    std::string::size_type start = 0;
    std::string::size_type end = input.find(delimiter);

    while (end != std::string::npos) {
        fields.emplace_back(input.substr(start, end - start));
        start = end + delimiter.size();
        end = input.find(delimiter, start);
    }

    fields.emplace_back(input.substr(start));
    return fields;
}

std::vector<std::string> findAll(const std::string& pattern, const std::string& text) {
    std::regex re(pattern);
    std::sregex_iterator it(text.begin(), text.end(), re);
    std::sregex_iterator end;
    std::vector<std::string> matches;
    while (it != end) {
        matches.push_back(it->str());
        ++it;
    }
    return matches;
}

std::string paramsToQueryStr(const std::multimap<std::string, std::string>& params) {
    auto encode_query_param = [](const std::string& value) {
        std::ostringstream escaped;
        escaped.fill('0');
        escaped << std::hex;
        for (auto c : value) {
            if (std::isalnum(static_cast<uint8_t>(c)) || c == '-' || c == '_' || c == '.' || c == '!' || c == '~' ||
                c == '*' || c == '\'' || c == '(' || c == ')') {
                escaped << c;
            } else {
                escaped << std::uppercase;
                escaped << '%' << std::setw(2) << static_cast<int>(static_cast<unsigned char>(c));
                escaped << std::nouppercase;
            }
        }
        return escaped.str();
    };
    std::string query;
    for (auto it = params.begin(); it != params.end(); ++it) {
        if (it != params.begin())
            query += "&";
        query += it->first;
        query += "=";
        query += encode_query_param(it->second);
    }
    return query;
}

enum class Status : uint8_t {
    Ok,
    Close,
    HasError,
    UnexpectedHttpCode,
};

void printHttpHeader(auto& http_packet) {
    std::stringstream ss;
    ss << http_packet.base();
    SPDLOG_INFO("\n{}", ss.str());
}

std::optional<std::smatch> parse(const std::string& url) {
    static const auto url_regex =
        std::regex(R"regex((http|https)://([^/ :]+):?([^/ ]*)((/?[^ #?]*)\x3f?([^ #]*)#?([^ ]*)))regex",
                   std::regex_constants::icase | std::regex_constants::optimize);
    auto match = std::smatch();
    if (!std::regex_match(url, match, url_regex)) {
        SPDLOG_ERROR("invalid http_proxy: {}", url);
        return std::nullopt;
    }
    return match;
}

boost::asio::awaitable<Status> sendRequestRecvChunk(
    std::string& error_info, auto& stream_, auto& req, std::size_t http_code, std::function<void(std::string)> cb,
    std::function<void(const boost::beast::http::parser<false, boost::beast::http::empty_body>&)> h_cb = nullptr) {
    boost::system::error_code err{};
    auto [ec, count] = co_await boost::beast::http::async_write(stream_, req, use_nothrow_awaitable);
    if (ec) {
        SPDLOG_ERROR("{}", ec.message());
        error_info = ec.message();
        co_return Status::HasError;
    }

    boost::beast::flat_buffer buffer;
    boost::beast::http::parser<false, boost::beast::http::empty_body> p;
    std::tie(ec, count) = co_await boost::beast::http::async_read_header(stream_, buffer, p, use_nothrow_awaitable);
    if (ec == boost::beast::http::error::end_of_stream) {
        SPDLOG_INFO("server close!!!");
        co_return Status::Close;
    }
    if (ec) {
        SPDLOG_ERROR("{}", ec.message());
        error_info = ec.message();
        co_return Status::HasError;
    }

    if (h_cb)
        h_cb(p);
    auto& headers = p.get();
    printHttpHeader(headers);

    auto result_int = headers.result_int();
    if (result_int != http_code) {
        std::string reason{headers.reason()};
        SPDLOG_ERROR("http response code: {}, reason: {}", headers.result_int(), reason);
        error_info = std::format("return unexpected http status code: {}({})", result_int, reason);
        co_return Status::UnexpectedHttpCode;
    }

    boost::beast::http::chunk_extensions ce;
    std::string chunk;

    auto header_cb = [&](std::uint64_t size, std::string_view extensions, boost::beast::error_code& ev) {
        ce.parse(extensions, ev);
        if (ev)
            return;
        if (size > (std::numeric_limits<std::size_t>::max)()) {
            ev = boost::beast::http::error::body_limit;
            return;
        }
        chunk.reserve(static_cast<std::size_t>(size));
        chunk.clear();
    };
    p.on_chunk_header(header_cb);

    auto body_cb = [&](std::uint64_t remain, std::string_view body, boost::beast::error_code& ec) {
        if (remain == body.size())
            ec = boost::beast::http::error::end_of_chunk;
        chunk.append(body.data(), body.size());

        std::string chunk_str{body};
        cb(std::move(chunk_str));
        return body.size();
    };
    p.on_chunk_body(body_cb);

    while (!p.is_done()) {
        std::tie(ec, count) = co_await boost::beast::http::async_read(stream_, buffer, p, use_nothrow_awaitable);
        if (!ec)
            continue;
        else if (ec != boost::beast::http::error::end_of_chunk) {
            co_return Status::HasError;
        } else
            ec = {};
    }
    co_return Status::Ok;
}

boost::asio::awaitable<Status> sendRequestRecvChunk(
    auto& ch, auto& stream_, auto& req, std::size_t http_code, std::function<void(std::string)> cb,
    std::function<void(const boost::beast::http::parser<false, boost::beast::http::empty_body>&)> header_cb =
        nullptr) {
    std::string error_info;
    auto ret = co_await sendRequestRecvChunk(error_info, stream_, req, http_code, std::move(cb), header_cb);
    if (!error_info.empty()) {
        boost::system::error_code err{};
        co_await ch->async_send(err, std::move(error_info), use_nothrow_awaitable);
    }
    co_return ret;
}

boost::asio::awaitable<
    std::expected<std::tuple<boost::beast::http::response<boost::beast::http::string_body>, boost::asio::ssl::context,
                             boost::beast::ssl_stream<boost::beast::tcp_stream>>,
                  std::string>>
sendRequestRecvResponse(auto& req, std::string_view host, std::string_view port, auto create_http_client) {
    int recreate_num{0};
create_client:
    boost::asio::ssl::context ctx(boost::asio::ssl::context::tls);
    ctx.set_verify_mode(boost::asio::ssl::verify_none);
    auto client = co_await create_http_client(ctx, host, port);
    if (!client.has_value()) {
        SPDLOG_ERROR("createHttpClient: {}", client.error());
        co_return std::unexpected(client.error());
    }
    auto& stream_ = client.value();

    auto [ec, count] = co_await boost::beast::http::async_write(stream_, req, use_nothrow_awaitable);
    if (ec) {
        SPDLOG_ERROR("{}", ec.message());
        co_return std::unexpected(ec.message());
    }
    boost::beast::flat_buffer b;
    boost::beast::http::response<boost::beast::http::string_body> res;
    std::tie(ec, count) = co_await boost::beast::http::async_read(stream_, b, res, use_nothrow_awaitable);
    if (ec == boost::beast::http::error::end_of_stream) {
        if (recreate_num == 0) {
            recreate_num++;
            goto create_client;
        }
    }
    if (ec) {
        SPDLOG_ERROR("{}", ec.message());
        co_return std::unexpected(ec.message());
    }
    co_return std::make_tuple(res, std::move(ctx), std::move(stream_));
}

void curlEasySetopt(CURL* curl) {
    curl_easy_setopt(curl, CURLOPT_CAINFO, nullptr);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 20L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
}

auto getConversationJson(const nlohmann::json& json) {
    auto conversation = json.at("meta").at("content").at("conversation");
    conversation.push_back(json.at("meta").at("content").at("parts").at(0));
    return conversation;
}

template <typename T = std::chrono::milliseconds>
uint64_t getTimestamp(std::chrono::time_point<std::chrono::system_clock> now = std::chrono::system_clock::now()) {
    uint64_t timestamp = std::chrono::duration_cast<T>(now.time_since_epoch()).count();
    return timestamp;
}

struct CurlHttpRequest {
    CURL* curl{nullptr};
    std::string url;
    std::string_view http_proxy;
    size_t (*cb)(void* contents, size_t size, size_t nmemb, void* userp){nullptr};
    void* input{nullptr};
    std::unordered_map<std::string, std::string>& headers;
    std::string body;
    std::multimap<std::string, std::string>* response_header_ptr{nullptr};
    int32_t expect_response_code{200};
    bool ssl_verify{false};
};

std::optional<std::string> sendHttpRequest(const CurlHttpRequest& curl_http_request) {
    auto& [curl, url, http_proxy, stream_action_cb, input, http_headers, body, response_header_ptr, response_code,
           ssl_verify] = curl_http_request;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    if (!http_proxy.empty())
        curl_easy_setopt(curl, CURLOPT_PROXY, http_proxy.data());
    if (!ssl_verify) {
        curl_easy_setopt(curl, CURLOPT_CAINFO, nullptr);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    }
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 20L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
    if (stream_action_cb != nullptr)
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, stream_action_cb);
    if (input != nullptr)
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, input);

    std::string buffer;
    auto header_callback = [](char* buffer, size_t size, size_t nitems, void* userdata) {
        std::string str{(char*)buffer, size * nitems};
        static_cast<std::string*>(userdata)->append((char*)buffer, size * nitems);
        return nitems * size;
    };
    size_t (*fn_header_callback)(char* buffer, size_t size, size_t nitems, void* userdata) = header_callback;
    if (response_header_ptr) {
        curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, fn_header_callback);
        curl_easy_setopt(curl, CURLOPT_HEADERDATA, &buffer);
    }

    struct curl_slist* headers = nullptr;
    std::vector<std::string> headers_list;
    for (auto& [k, v] : http_headers)
        headers_list.emplace_back(std::format("{}: {}", k, v));
    for (auto& header : headers_list)
        headers = curl_slist_append(headers, header.c_str());
    if (headers != nullptr)
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    ScopeExit auto_exit{[=] { curl_slist_free_all(headers); }};

    if (!body.empty())
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());

    auto res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        auto error_info = std::format("curl_easy_perform() failed:{}", curl_easy_strerror(res));
        return error_info;
    }
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
    if (response_code != 200) {
        auto error_info = std::format("response_code :{}", response_code);
        return error_info;
    }
    if (!buffer.empty() && response_header_ptr != nullptr) {
        std::regex pattern(R"(([^:\r\n]+):([^\r\n]+))");
        std::smatch matches;
        auto start = buffer.cbegin();
        auto end = buffer.cend();
        while (std::regex_search(start, end, matches, pattern)) {
            std::string field_name = matches[1].str();
            std::string field_value = matches[2].str();
            (*response_header_ptr).insert(std::pair{field_name, field_value});
            start = matches[0].second;
        }
    }
    return std::nullopt;
}

std::expected<nlohmann::json, std::string> callZeus(const std::string& host, const std::string& request_body) {
    CURLcode res;
    CURL* curl = curl_easy_init();
    if (!curl) {
        auto error_info = std::format("callZeus curl_easy_init() failed:{}", curl_easy_strerror(res));
        return std::unexpected(error_info);
    }
    ScopeExit auto_exit{[=] { curl_easy_cleanup(curl); }};
    std::string http_proxy;
    std::string recv;

    auto ret = sendHttpRequest(CurlHttpRequest{
        .curl = curl,
        .url = host,
        .http_proxy = http_proxy,
        .cb = [](void* contents, size_t size, size_t nmemb, void* userp) mutable -> size_t {
            auto recv_ptr = static_cast<std::string*>(userp);
            std::string data{(char*)contents, size * nmemb};
            recv_ptr->append(data);
            return size * nmemb;
        },
        .input = &recv,
        .headers = [&] -> auto& {
            static std::unordered_map<std::string, std::string> headers{
                {"Content-Type", "application/json"},
            };
            return headers;
        }(),
        .body = request_body,
        .response_header_ptr = nullptr,
        .expect_response_code = 200,
        .ssl_verify = false,
    });
    if (ret)
        return std::unexpected(ret.value());
    nlohmann::json rsp = nlohmann::json::parse(recv, nullptr, false);
    if (rsp.is_discarded()) {
        SPDLOG_ERROR("json parse error");
        return std::unexpected("parse callZeus error");
    }
    return rsp;
}

}  // namespace

FreeGpt::FreeGpt(Config& cfg)
    : m_cfg(cfg), m_thread_pool_ptr(std::make_shared<boost::asio::thread_pool>(m_cfg.work_thread_num * 2)) {}

boost::asio::awaitable<std::expected<boost::beast::ssl_stream<boost::beast::tcp_stream>, std::string>>
FreeGpt::createHttpClient(boost::asio::ssl::context& ctx, std::string_view host, std::string_view port) {
    if (m_cfg.http_proxy.empty()) {
        boost::beast::ssl_stream<boost::beast::tcp_stream> stream_{co_await boost::asio::this_coro::executor, ctx};
        boost::system::error_code err{};
        if (!SSL_set_tlsext_host_name(stream_.native_handle(), host.data())) {
            SPDLOG_ERROR("SSL_set_tlsext_host_name");
            co_return std::unexpected(std::string("SSL_set_tlsext_host_name"));
        }
        auto resolver = boost::asio::ip::tcp::resolver(co_await boost::asio::this_coro::executor);
        auto [ec, results] = co_await resolver.async_resolve(host.data(), port.data(), use_nothrow_awaitable);
        if (ec) {
            SPDLOG_INFO("async_resolve: {}", ec.message());
            co_return std::unexpected(ec.message());
        }
        for (auto& endpoint : results) {
            std::stringstream ss;
            ss << endpoint.endpoint();
            SPDLOG_INFO("resolver_results: [{}]", ss.str());
        }
        boost::beast::get_lowest_layer(stream_).expires_after(std::chrono::seconds(30));
        if (auto [ec, _] =
                co_await boost::beast::get_lowest_layer(stream_).async_connect(results, use_nothrow_awaitable);
            ec) {
            co_return std::unexpected(ec.message());
        }
        boost::beast::get_lowest_layer(stream_).expires_never();
        std::tie(ec) = co_await stream_.async_handshake(boost::asio::ssl::stream_base::client, use_nothrow_awaitable);
        if (ec) {
            SPDLOG_INFO("async_handshake: {}", ec.message());
            co_return std::unexpected(ec.message());
        }
        co_return stream_;
    }

    std::string userinfo, proxy_host, proxy_port;
    auto is_auth_proxy = [](const std::string& str) -> bool {
        std::regex pattern("^http://[^:]+:[^@]+@[^:]+:[0-9]+$");
        return std::regex_match(str, pattern);
    };
    // http://username:password@proxy.example.com:8080
    if (is_auth_proxy(m_cfg.http_proxy)) {
        static std::regex pattern("(http|https)://([^:]+):([^@]+)@([^:]+):([0-9]+)");
        std::smatch matches;
        if (!std::regex_match(m_cfg.http_proxy, matches, pattern))
            co_return std::unexpected(std::format("invalid http_proxy: {}", m_cfg.http_proxy));
        // std::string protocol = matches[1];
        std::string username = matches[2];
        std::string password = matches[3];
        proxy_host = matches[4];
        proxy_port = matches[5];
        userinfo = std::format("{}:{}", username, password);
    } else {
        auto match_opt = parse(m_cfg.http_proxy);
        if (!match_opt.has_value())
            co_return std::unexpected(std::format("invalid http_proxy: {}", m_cfg.http_proxy));
        auto& match = match_opt.value();
        // auto& protocol = match[1];
        // auto& target = match[4];
        proxy_host = match[2];
        proxy_port = match[3];
    }
    SPDLOG_INFO("CONNECT TO HTTP_PROXY [{}:{}]", proxy_host, proxy_port);

    auto resolver = boost::asio::ip::tcp::resolver(co_await boost::asio::this_coro::executor);
    auto [ec, results] = co_await resolver.async_resolve(proxy_host, proxy_port, use_nothrow_awaitable);
    if (ec) {
        SPDLOG_INFO("async_resolve: {}", ec.message());
        co_return std::unexpected(ec.message());
    }
    boost::asio::ip::tcp::socket socket_{co_await boost::asio::this_coro::executor};
    if (auto [ec, count] = co_await boost::asio::async_connect(socket_, results, use_nothrow_awaitable); ec) {
        SPDLOG_INFO("async_connect: {}", ec.message());
        co_return std::unexpected(ec.message());
    }

    boost::beast::ssl_stream<boost::beast::tcp_stream> stream_{std::move(socket_), ctx};
    int http_version = 11;
    boost::beast::http::request<boost::beast::http::string_body> connect_req{
        boost::beast::http::verb::connect, std::format("{}:{}", host, port), http_version};
    connect_req.set(boost::beast::http::field::host, host);

    if (!userinfo.empty()) {
        std::string result(boost::beast::detail::base64::encoded_size(userinfo.size()), 0);
        auto len =
            boost::beast::detail::base64::encode(static_cast<char*>(result.data()), userinfo.c_str(), userinfo.size());
        result.resize(len);
        result = "Basic " + result;
        connect_req.set(boost::beast::http::field::proxy_authorization, result);
    }

    std::size_t count;
    std::tie(ec, count) = co_await boost::beast::http::async_write(boost::beast::get_lowest_layer(stream_),
                                                                   connect_req, use_nothrow_awaitable);
    if (ec) {
        SPDLOG_ERROR("{}", ec.message());
        co_return std::unexpected(ec.message());
    }
    boost::beast::http::response<boost::beast::http::empty_body> res;
    boost::beast::http::parser<false, boost::beast::http::empty_body> http_parser(res);
    http_parser.skip(true);

    boost::beast::flat_buffer buffer;
    std::tie(ec, count) = co_await boost::beast::http::async_read(boost::beast::get_lowest_layer(stream_), buffer,
                                                                  http_parser, use_nothrow_awaitable);
    if (boost::beast::http::status::ok != res.result()) {
        SPDLOG_ERROR("Proxy response failed : {}", res.result_int());
        co_return std::unexpected(ec.message());
    }
    if (!SSL_set_tlsext_host_name(stream_.native_handle(), host.data())) {
        SPDLOG_ERROR("SSL_set_tlsext_host_name");
        co_return std::unexpected(std::string("SSL_set_tlsext_host_name"));
    }
    std::tie(ec) = co_await stream_.async_handshake(boost::asio::ssl::stream_base::client, use_nothrow_awaitable);
    if (ec) {
        SPDLOG_INFO("async_handshake: {}", ec.message());
        co_return std::unexpected(ec.message());
    }
    co_return stream_;
}

boost::asio::awaitable<void> FreeGpt::deepAi(std::shared_ptr<Channel> ch, nlohmann::json json) {
    co_await boost::asio::post(boost::asio::bind_executor(*m_thread_pool_ptr, boost::asio::use_awaitable));

    boost::system::error_code err{};
    ScopeExit _exit{[=] { boost::asio::post(ch->get_executor(), [=] { ch->close(); }); }};
    auto prompt = json.at("meta").at("content").at("parts").at(0).at("content").get<std::string>();

    std::string user_agent{
        R"(Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/110.0.0.0 Safari/537.36)"};

    std::random_device rd;
    std::mt19937 mt(rd());
    std::uniform_int_distribution<uint64_t> dist(0, 100000000);
    uint64_t part1{dist(mt)};
    auto part2 = md5(user_agent + md5(user_agent + md5(std::format("{}{}x", user_agent, part1))));
    auto api_key = std::format("tryit-{}-{}", part1, part2);

    constexpr char CRLF[] = "\r\n";
    static std::string MULTI_PART_BOUNDARY = "9bc627aea4f77e150e6057f78036e73f";

    CURLcode res;
    CURL* curl = curl_easy_init();
    if (!curl) {
        auto error_info = std::format("curl_easy_init() failed:{}", curl_easy_strerror(res));
        co_await boost::asio::post(boost::asio::bind_executor(ch->get_executor(), boost::asio::use_awaitable));
        ch->try_send(err, error_info);
        co_return;
    }
    curl_easy_setopt(curl, CURLOPT_URL, "https://api.deepai.org/hacking_is_a_crime");

    if (!m_cfg.http_proxy.empty())
        curl_easy_setopt(curl, CURLOPT_PROXY, m_cfg.http_proxy.c_str());

    struct Input {
        std::shared_ptr<Channel> ch;
        std::string recv;
    };
    Input input{ch};
    auto action_cb = [](void* contents, size_t size, size_t nmemb, void* userp) -> size_t {
        boost::system::error_code err{};
        auto input_ptr = static_cast<Input*>(userp);
        std::string data{(char*)contents, size * nmemb};
        auto& [ch, recv] = *input_ptr;
        boost::asio::post(ch->get_executor(), [=] { ch->try_send(err, data); });
        return size * nmemb;
    };
    size_t (*action_fn)(void* contents, size_t size, size_t nmemb, void* userp) = action_cb;
    curlEasySetopt(curl);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, action_fn);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &input);

    nlohmann::json request_json{{{"role", "user"}, {"content", std::move(prompt)}}};

    std::ostringstream payload;
    payload << "--" << MULTI_PART_BOUNDARY << CRLF << R"(Content-Disposition: form-data; name="chat_style")" << CRLF
            << CRLF << "chat" << CRLF << "--" << MULTI_PART_BOUNDARY << CRLF
            << R"(Content-Disposition: form-data; name="chatHistory")" << CRLF << CRLF << request_json.dump() << CRLF
            << "--" << MULTI_PART_BOUNDARY << "--" << CRLF;
    SPDLOG_INFO("{}", payload.str());
    auto str = payload.str();
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, str.c_str());

    struct curl_slist* headers = nullptr;
    auto content_type_str = std::format("Content-Type: multipart/form-data; boundary={}", MULTI_PART_BOUNDARY);
    SPDLOG_INFO("content_type_str: {}", content_type_str);
    headers = curl_slist_append(headers, content_type_str.c_str());
    auto api_key_str = std::format("api-key: {}", api_key);
    headers = curl_slist_append(headers, api_key_str.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    ScopeExit auto_exit{[=] {
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
    }};

    res = curl_easy_perform(curl);

    if (res != CURLE_OK) {
        co_await boost::asio::post(boost::asio::bind_executor(ch->get_executor(), boost::asio::use_awaitable));
        auto error_info = std::format("curl_easy_perform() failed:{}", curl_easy_strerror(res));
        ch->try_send(err, error_info);
        co_return;
    }
    int32_t response_code;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
    if (response_code != 200) {
        co_await boost::asio::post(boost::asio::bind_executor(ch->get_executor(), boost::asio::use_awaitable));
        ch->try_send(err, std::format("deepai http code:{}", response_code));
        co_return;
    }
    co_return;
}

boost::asio::awaitable<void> FreeGpt::chatGptAi(std::shared_ptr<Channel> ch, nlohmann::json json) {
    ScopeExit auto_exit{[&] { ch->close(); }};
    boost::system::error_code err{};

    constexpr std::string_view host = "chatgpt.ai";
    constexpr std::string_view port = "443";

    constexpr std::string_view user_agent{
        R"(Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/114.0.0.0 Safari/537.36)"};

    boost::beast::http::request<boost::beast::http::empty_body> req{boost::beast::http::verb::get, "/", 11};
    req.set(boost::beast::http::field::host, "chatgpt.ai");
    req.set(boost::beast::http::field::user_agent, user_agent);
    req.set("Accept", "*/*");

    int recreate_num{0};
create_client:
    boost::asio::ssl::context ctx(boost::asio::ssl::context::tls);
    ctx.set_verify_mode(boost::asio::ssl::verify_none);
    auto client = co_await createHttpClient(ctx, host, port);
    if (!client.has_value()) {
        SPDLOG_ERROR("createHttpClient: {}", client.error());
        co_await ch->async_send(err, client.error(), use_nothrow_awaitable);
        co_return;
    }
    auto& stream_ = client.value();

    std::string chunk_body;
    auto ret = co_await sendRequestRecvChunk(
        ch, stream_, req, 200, [&ch, &chunk_body](std::string recv_str) { chunk_body.append(std::move(recv_str)); });
    if (ret == Status::Close && recreate_num == 0) {
        recreate_num++;
        goto create_client;
    }
    if (ret == Status::HasError)
        co_return;

    static std::string pattern{
        R"(data-nonce=".*"\n     data-post-id=".*"\n     data-url=".*"\n     data-bot-id=".*"\n     data-width)"};

    std::vector<std::string> matches = findAll(pattern, chunk_body);
    if (matches.size() != 1) {
        SPDLOG_ERROR("parsing login failed");
        co_await ch->async_send(err, chunk_body, use_nothrow_awaitable);
        co_return;
    }

    std::regex reg("\"([^\"]*)\"");
    std::sregex_iterator iter(matches[0].begin(), matches[0].end(), reg);
    std::sregex_iterator end;
    std::vector<std::string> results;
    while (iter != end) {
        results.emplace_back(iter->str(1));
        iter++;
    }
    if (results.size() != 4) {
        SPDLOG_ERROR("Failed to extract content");
        co_await ch->async_send(err, "Failed to extract content", use_nothrow_awaitable);
        co_return;
    }

    auto& nonce = results[0];
    auto& post_id = results[1];
    auto& data_url = results[2];
    auto& bot_id = results[3];

    SPDLOG_INFO("data_nonce: {}", nonce);
    SPDLOG_INFO("data_post_id: {}", post_id);
    SPDLOG_INFO("data_url: {}", data_url);
    SPDLOG_INFO("data_bot_id: {}", bot_id);

    auto prompt = json.at("meta").at("content").at("parts").at(0).at("content").get<std::string>();

    boost::beast::http::request<boost::beast::http::string_body> request{boost::beast::http::verb::post,
                                                                         "/wp-admin/admin-ajax.php", 11};
    request.set(boost::beast::http::field::host, host);
    request.set("authority", "chatgpt.ai");
    request.set("accept", "*/*");
    request.set("accept-language", R"(en,fr-FR;q=0.9,fr;q=0.8,es-ES;q=0.7,es;q=0.6,en-US;q=0.5,am;q=0.4,de;q=0.3)");
    request.set("cache-control", "no-cache");
    request.set("origin", "https://chatgpt.ai");
    request.set("pragma", "no-cache");
    request.set(boost::beast::http::field::referer, "https://chatgpt.ai/gpt-4/");
    request.set("sec-ch-ua", R"("Not.A/Brand";v="8", "Chromium";v="114", "Google Chrome";v="114")");
    request.set("sec-ch-ua-mobile", "?0");
    request.set("sec-ch-ua-platform", R"("Windows")");
    request.set("sec-fetch-dest", "empty");
    request.set("sec-fetch-mode", "cors");
    request.set("sec-fetch-site", "same-origin");
    request.set(boost::beast::http::field::user_agent, user_agent);
    request.set("Content-Type", "application/x-www-form-urlencoded");

    std::multimap<std::string, std::string> params{
        {"message", std::format("user: {}\nassistant: ", prompt)},
        {"_wpnonce", nonce},
        {"post_id", post_id},
        {"url", "https://chatgpt.ai"},
        {"action", "wpaicg_chat_shortcode_message"},
        {"bot_id", bot_id},
    };
    auto str = paramsToQueryStr(params);
    SPDLOG_INFO("request: {}", str);
    request.body() = str;
    request.prepare_payload();

    auto [ec, count] = co_await boost::beast::http::async_write(stream_, request, use_nothrow_awaitable);
    if (ec) {
        SPDLOG_ERROR("{}", ec.message());
        co_await ch->async_send(err, ec.message(), use_nothrow_awaitable);
        co_return;
    }
    boost::beast::flat_buffer buffer;
    boost::beast::http::response<boost::beast::http::string_body> response;
    std::tie(ec, count) = co_await boost::beast::http::async_read(stream_, buffer, response, use_nothrow_awaitable);
    if (ec) {
        SPDLOG_ERROR("{}", ec.message());
        co_await ch->async_send(err, ec.message(), use_nothrow_awaitable);
        co_return;
    }
    if (boost::beast::http::status::ok != response.result()) {
        SPDLOG_ERROR("http code: {}", response.result_int());
        co_await ch->async_send(err, response.reason(), use_nothrow_awaitable);
        co_return;
    }
    std::stringstream ss;
    ss << response.base();
    SPDLOG_INFO("{}", ss.str());
    SPDLOG_INFO("response.body(): {}", response.body());
    nlohmann::json rsp = nlohmann::json::parse(response.body(), nullptr, false);
    if (rsp.is_discarded()) {
        SPDLOG_ERROR("json parse error");
        co_await ch->async_send(err, "json parse error", use_nothrow_awaitable);
        co_return;
    }
    SPDLOG_INFO("rsp: {}", rsp.dump());
    co_await ch->async_send(err, rsp.value("data", rsp.dump()), use_nothrow_awaitable);
    co_return;
}

boost::asio::awaitable<void> FreeGpt::openAi(std::shared_ptr<Channel> ch, nlohmann::json json) {
    boost::system::error_code err{};
    ScopeExit auto_exit{[&] { ch->close(); }};

    constexpr std::string_view host = "api.openai.com";
    constexpr std::string_view port = "443";

    constexpr std::string_view user_agent{
        R"(Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/114.0.0.0 Safari/537.36)"};

    boost::asio::ssl::context ctx(boost::asio::ssl::context::tls);
    ctx.set_verify_mode(boost::asio::ssl::verify_none);

    auto client = co_await createHttpClient(ctx, host, port);
    if (!client.has_value()) {
        SPDLOG_ERROR("createHttpClient: {}", client.error());
        co_await ch->async_send(err, client.error(), use_nothrow_awaitable);
        co_return;
    }
    auto& stream_ = client.value();

    boost::beast::http::request<boost::beast::http::string_body> req{boost::beast::http::verb::post,
                                                                     "/v1/chat/completions", 11};
    req.set(boost::beast::http::field::host, host);
    req.set(boost::beast::http::field::user_agent, user_agent);
    req.set(boost::beast::http::field::content_type, "application/json");
    req.set(boost::beast::http::field::authorization, std::format("Bearer {}", m_cfg.api_key));

    constexpr std::string_view json_str = R"({
        "messages": [
            {
                "role": "user",
                "content": "hello"
            }
        ],
        "stream": true,
        "model": "gpt-3.5-turbo"
    })";
    nlohmann::json request = nlohmann::json::parse(json_str, nullptr, false);

    request["messages"] = getConversationJson(json);
    SPDLOG_INFO("{}", request.dump(2));

    req.body() = request.dump();
    req.prepare_payload();

    std::string recv;
    co_await sendRequestRecvChunk(ch, stream_, req, 200, [&ch, &recv](std::string chunk_str) {
        recv.append(chunk_str);
        while (true) {
            auto position = recv.find("\n");
            if (position == std::string::npos)
                break;
            auto msg = recv.substr(0, position + 1);
            recv.erase(0, position + 1);
            msg.pop_back();
            if (msg.empty() || !msg.contains("content"))
                continue;
            auto fields = splitString(msg, "data: ");
            boost::system::error_code err{};
            nlohmann::json line_json = nlohmann::json::parse(fields.back(), nullptr, false);
            if (line_json.is_discarded()) {
                SPDLOG_ERROR("json parse error: [{}]", fields.back());
                ch->try_send(err, std::format("json parse error: [{}]", fields.back()));
                continue;
            }
            auto str = line_json["choices"][0]["delta"]["content"].get<std::string>();
            if (!str.empty())
                ch->try_send(err, str);
        }
    });
    co_return;
}

boost::asio::awaitable<void> FreeGpt::yqcloud(std::shared_ptr<Channel> ch, nlohmann::json json) {
    boost::system::error_code err{};
    ScopeExit auto_exit{[&] { ch->close(); }};

    auto prompt = json.at("meta").at("content").at("parts").at(0).at("content").get<std::string>();

    constexpr std::string_view host = "api.aichatos.cloud";
    constexpr std::string_view port = "443";

    constexpr std::string_view user_agent{
        R"(Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:109.0) Gecko/20100101 Firefox/115.0)"};

    boost::beast::http::request<boost::beast::http::string_body> req{boost::beast::http::verb::post,
                                                                     "/api/generateStream", 11};
    req.set("authority", "p5.v50.ltd");
    req.set(boost::beast::http::field::host, host);
    req.set(boost::beast::http::field::user_agent, user_agent);
    req.set("accept", "application/json, text/plain, */*");
    req.set("Content-Type", "application/json");
    req.set("origin", "https://chat9.yqcloud.top");

    constexpr std::string_view json_str = R"({
        "prompt":"hello",
        "network":true,
        "system":"",
        "withoutContext":false,
        "stream":false
    })";

    nlohmann::json request = nlohmann::json::parse(json_str, nullptr, false);
    request["prompt"] = std::move(prompt);

    req.body() = request.dump();
    req.prepare_payload();

    int recreate_num{0};
create_client:
    boost::asio::ssl::context ctx(boost::asio::ssl::context::tls);
    ctx.set_verify_mode(boost::asio::ssl::verify_none);
    auto client = co_await createHttpClient(ctx, host, port);
    if (!client.has_value()) {
        SPDLOG_ERROR("createHttpClient: {}", client.error());
        co_await ch->async_send(err, client.error(), use_nothrow_awaitable);
        co_return;
    }
    auto& stream_ = client.value();

    auto ret = co_await sendRequestRecvChunk(ch, stream_, req, 200, [&ch](std::string str) {
        boost::system::error_code err{};
        ch->try_send(err, std::move(str));
    });
    if (ret == Status::Close && recreate_num == 0) {
        recreate_num++;
        goto create_client;
    }
    co_return;
}

boost::asio::awaitable<void> FreeGpt::huggingChat(std::shared_ptr<Channel> ch, nlohmann::json json) {
    boost::system::error_code err{};
    ScopeExit auto_exit{[&] { ch->close(); }};

    auto prompt = json.at("meta").at("content").at("parts").at(0).at("content").get<std::string>();

    constexpr std::string_view host = "huggingface.co";
    constexpr std::string_view port = "443";

    constexpr std::string_view user_agent{
        R"(Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:109.0) Gecko/20100101 Firefox/115.0)"};

    boost::beast::http::request<boost::beast::http::empty_body> req_init_cookie{boost::beast::http::verb::get,
                                                                                "/chat/", 11};
    req_init_cookie.set(boost::beast::http::field::host, host);
    req_init_cookie.set(boost::beast::http::field::user_agent, user_agent);

    auto ret = co_await sendRequestRecvResponse(req_init_cookie, host, port,
                                                std::bind_front(&FreeGpt::createHttpClient, *this));
    if (!ret.has_value()) {
        co_await ch->async_send(err, ret.error(), use_nothrow_awaitable);
        co_return;
    }
    auto& [response, ctx, stream_] = ret.value();
    if (boost::beast::http::status::ok != response.result()) {
        SPDLOG_ERROR("http status code: {}", response.result_int());
        co_await ch->async_send(err, response.reason(), use_nothrow_awaitable);
        co_return;
    }
    auto fields = splitString(response["Set-Cookie"], " ");
    if (fields.empty()) {
        std::stringstream ss;
        ss << response.base();
        SPDLOG_ERROR("get cookie error: {}", ss.str());
        co_await ch->async_send(err, "can't get cookie", use_nothrow_awaitable);
        co_return;
    }
    fields[0].pop_back();
    std::string cookie{std::move(fields[0])};
    SPDLOG_INFO("cookie: {}", cookie);

    boost::beast::http::request<boost::beast::http::string_body> req_init_conversation{boost::beast::http::verb::post,
                                                                                       "/chat/conversation", 11};
    req_init_conversation.set("Cookie", cookie);
    req_init_conversation.set(boost::beast::http::field::host, host);
    req_init_conversation.set(boost::beast::http::field::user_agent, user_agent);
    req_init_conversation.set("Accept", "*/*");
    req_init_conversation.set("Content-Type", "application/json");
    req_init_conversation.body() = R"({"model": "meta-llama/Llama-2-70b-chat-hf"})";
    req_init_conversation.prepare_payload();

    auto [ec, count] = co_await boost::beast::http::async_write(stream_, req_init_conversation, use_nothrow_awaitable);
    if (ec) {
        SPDLOG_ERROR("{}", ec.message());
        co_await ch->async_send(err, ec.message(), use_nothrow_awaitable);
        co_return;
    }
    boost::beast::flat_buffer b;
    boost::beast::http::response<boost::beast::http::string_body> res;
    std::tie(ec, count) = co_await boost::beast::http::async_read(stream_, b, res, use_nothrow_awaitable);
    if (ec) {
        SPDLOG_ERROR("{}", ec.message());
        co_await ch->async_send(err, ec.message(), use_nothrow_awaitable);
        co_return;
    }
    if (res.result_int() != 200) {
        std::string reason{res.reason()};
        SPDLOG_ERROR("reason: {}", reason);
        co_await ch->async_send(err,
                                std::format("return unexpected http status code: {}({})", res.result_int(), reason),
                                use_nothrow_awaitable);
        co_return;
    }
    nlohmann::json rsp_json = nlohmann::json::parse(res.body(), nullptr, false);
    if (rsp_json.is_discarded()) {
        SPDLOG_ERROR("json parse error: [{}]", fields.back());
        ch->try_send(err, std::format("json parse error: [{}]", fields.back()));
        co_return;
    }
    if (!rsp_json.contains("conversationId")) {
        SPDLOG_ERROR("not contains conversationId: {}", res.body());
        co_await ch->async_send(err, res.body(), use_nothrow_awaitable);
        co_return;
    }
    auto conversation_id = rsp_json["conversationId"].get<std::string>();
    SPDLOG_INFO("conversation_id: [{}]", conversation_id);

    constexpr std::string_view json_str = R"({
        "inputs":"hello",
        "parameters":{
            "temperature":0.2,
            "truncate":1000,
            "max_new_tokens":1024,
            "stop":[
                "</s>"
            ],
            "top_p":0.95,
            "repetition_penalty":1.2,
            "top_k":50,
            "return_full_text":false
        },
        "stream":true,
        "options":{
            "id":"9e9b8bc4-6604-40c6-994e-8eb78fa32e37",
            "response_id":"04ce2602-3bea-45e8-8efc-cef00680376a",
            "is_retry":false,
            "use_cache":false,
            "web_search_id":""
        }
    })";
    nlohmann::json request = nlohmann::json::parse(json_str, nullptr, false);
    request["inputs"] = prompt;
    request["options"]["response_id"] = createUuidString();
    request["options"]["id"] = createUuidString();

    boost::beast::http::request<boost::beast::http::string_body> req{
        boost::beast::http::verb::post, std::format("/chat/conversation/{}", conversation_id), 11};
    req.set("Cookie", cookie);
    req.set(boost::beast::http::field::host, host);
    req.set(boost::beast::http::field::user_agent, user_agent);
    req.set("Accept", "*/*");
    req.set("Content-Type", "application/json");
    req.body() = request.dump();
    req.prepare_payload();

    std::string recv;
    co_await sendRequestRecvChunk(ch, stream_, req, 200, [&ch, &recv](std::string chunk_str) {
        recv.append(chunk_str);
        while (true) {
            auto position = recv.find("\n");
            if (position == std::string::npos)
                break;
            auto msg = recv.substr(0, position + 1);
            recv.erase(0, position + 1);
            msg.pop_back();
            if (msg.empty())
                continue;
            boost::system::error_code err{};
            nlohmann::json line_json = nlohmann::json::parse(msg, nullptr, false);
            if (line_json.is_discarded()) {
                SPDLOG_ERROR("json parse error: [{}]", msg);
                ch->try_send(err, std::format("json parse error: [{}]", msg));
                continue;
            }
            if (!line_json.contains("type")) {
                SPDLOG_ERROR("invalid json format: [{}]", line_json.dump());
                continue;
            }
            auto type = line_json["type"].get<std::string>();
            if (type == "stream") {
                if (auto str = line_json["token"].get<std::string>(); !str.empty())
                    ch->try_send(err, str);
            } else if (type == "finalAnswer") {
                ch->close();
            }
        }
        return;
    });
    co_return;
}

boost::asio::awaitable<void> FreeGpt::you(std::shared_ptr<Channel> ch, nlohmann::json json) {
    co_await boost::asio::post(boost::asio::bind_executor(*m_thread_pool_ptr, boost::asio::use_awaitable));
    boost::system::error_code err{};
    ScopeExit _exit{[=] { boost::asio::post(ch->get_executor(), [=] { ch->close(); }); }};

    auto prompt = json.at("meta").at("content").at("parts").at(0).at("content").get<std::string>();

    static std::mutex mtx;
    static std::queue<std::tuple<std::chrono::time_point<std::chrono::system_clock>, std::string>> cookie_queue;
    std::tuple<std::chrono::time_point<std::chrono::system_clock>, std::string> cookie_cache;
    std::queue<std::tuple<std::chrono::time_point<std::chrono::system_clock>, std::string>> tmp_queue;
    std::unique_lock lk(mtx);
    while (!cookie_queue.empty()) {
        auto& [time_point, code] = cookie_queue.front();
        if (std::chrono::system_clock::now() - time_point < std::chrono::minutes(15))
            tmp_queue.push(std::move(cookie_queue.front()));
        cookie_queue.pop();
    }
    cookie_queue = std::move(tmp_queue);
    SPDLOG_INFO("cookie_queue size: {}", cookie_queue.size());
    if (cookie_queue.empty()) {
        lk.unlock();
        CURL* curl = curl_easy_init();
        if (!curl) {
            co_await boost::asio::post(boost::asio::bind_executor(ch->get_executor(), boost::asio::use_awaitable));
            ch->try_send(err, std::format("curl_easy_init() failed"));
            co_return;
        }
        ScopeExit auto_exit{[=] { curl_easy_cleanup(curl); }};
        std::multimap<std::string, std::string> response_header;
        std::unordered_map<std::string, std::string> headers;
        auto ret = sendHttpRequest(CurlHttpRequest{
            .curl = curl,
            .url = "https://you.com",
            .http_proxy = m_cfg.http_proxy,
            .cb = [](void* contents, size_t size, size_t nmemb, void* userp) mutable -> size_t {
                return size * nmemb;
            },
            .headers = headers,
            .response_header_ptr = &response_header,
        });
        if (ret) {
            co_await boost::asio::post(boost::asio::bind_executor(ch->get_executor(), boost::asio::use_awaitable));
            ch->try_send(err, ret.value());
            co_return;
        }
        std::string cookie;
        auto range = response_header.equal_range("set-cookie");
        for (auto it = range.first; it != range.second; ++it) {
            if (!(it->second.contains("__cf_bm=")))
                continue;
            auto view = it->second | std::views::drop_while(isspace) | std::views::reverse |
                        std::views::drop_while(isspace) | std::views::reverse;
            auto fields = splitString(std::string{view.begin(), view.end()}, " ");
            if (fields.size() < 1) {
                co_await boost::asio::post(boost::asio::bind_executor(ch->get_executor(), boost::asio::use_awaitable));
                ch->try_send(err, "can't get cookie");
                co_return;
            }
            cookie = std::move(fields[0]);
            break;
        }
        if (cookie.empty()) {
            co_await boost::asio::post(boost::asio::bind_executor(ch->get_executor(), boost::asio::use_awaitable));
            ch->try_send(err, "cookie is empty");
            co_return;
        }
        cookie_cache = std::make_tuple(std::chrono::system_clock::now(), std::move(cookie));
    } else {
        cookie_cache = std::move(cookie_queue.front());
        cookie_queue.pop();
        lk.unlock();
    }
    SPDLOG_INFO("cookie: {}", std::get<1>(cookie_cache));

    CURL* curl = curl_easy_init();
    if (!curl) {
        co_await boost::asio::post(boost::asio::bind_executor(ch->get_executor(), boost::asio::use_awaitable));
        ch->try_send(err, std::format("curl_easy_init() failed"));
        co_return;
    }
    ScopeExit auto_exit{[=] { curl_easy_cleanup(curl); }};
    auto cookie_str =
        std::format("uuid_guest={}; safesearch_guest=Off; {}", createUuidString(), std::get<1>(cookie_cache));
    curl_easy_setopt(curl, CURLOPT_COOKIE, std::get<1>(cookie_cache).c_str());
    auto ret = sendHttpRequest(CurlHttpRequest{
        .curl = curl,
        .url = [&] -> auto {
            std::multimap<std::string, std::string> params{
                {"q", prompt},
                {"page", "1"},
                {"count", "10"},
                {"safeSearch", "Off"},
                {"onShoppingPage", "False"},
                {"mkt", ""},
                {"responseFilter", "WebPages,Translations,TimeZone,Computation,RelatedSearches"},
                {"domain", "youchat"},
                {"queryTraceId", createUuidString()},
            };
            return std::format("https://you.com/api/streamingSearch?{}", paramsToQueryStr(params));
        }(),
        .http_proxy = m_cfg.http_proxy,
        .cb = [](void* contents, size_t size, size_t nmemb, void* userp) -> size_t {
            boost::system::error_code err{};
            auto ch_ptr = static_cast<FreeGpt::Channel*>(userp);
            std::string data{(char*)contents, size * nmemb};
            if (data.starts_with(R"(event: youChatToken)")) {
                static std::string to_erase{"event: youChatToken\ndata: "};
                size_t pos = data.find(to_erase);
                if (pos != std::string::npos)
                    data.erase(pos, to_erase.length());
                nlohmann::json line_json = nlohmann::json::parse(data, nullptr, false);
                if (line_json.is_discarded()) {
                    SPDLOG_ERROR("json parse error: [{}]", data);
                    boost::asio::post(ch_ptr->get_executor(),
                                      [=] { ch_ptr->try_send(err, std::format("json parse error: [{}]", data)); });
                    return size * nmemb;
                }
                auto str = line_json["youChatToken"].get<std::string>();
                boost::asio::post(ch_ptr->get_executor(), [=] { ch_ptr->try_send(err, str); });
            }
            return size * nmemb;
        },
        .input = ch.get(),
        .headers = []() -> auto& {
            static std::unordered_map<std::string, std::string> headers{
                {"referer", "https://you.com/search?q=gpt4&tbm=youchat"},
                {"Accept", "text/event-stream"},
            };
            return headers;
        }(),
    });
    if (ret) {
        co_await boost::asio::post(boost::asio::bind_executor(ch->get_executor(), boost::asio::use_awaitable));
        ch->try_send(err, ret.value());
        co_return;
    }
    {
        std::lock_guard lk(mtx);
        cookie_queue.push(std::move(cookie_cache));
    }
    co_return;
}

boost::asio::awaitable<void> FreeGpt::binjie(std::shared_ptr<Channel> ch, nlohmann::json json) {
    boost::system::error_code err{};
    ScopeExit auto_exit{[&] { ch->close(); }};

    auto prompt = json.at("meta").at("content").at("parts").at(0).at("content").get<std::string>();

    constexpr std::string_view host = "api.binjie.fun";
    constexpr std::string_view port = "443";

    boost::beast::http::request<boost::beast::http::string_body> req{boost::beast::http::verb::post,
                                                                     "/api/generateStream", 11};
    req.set(boost::beast::http::field::host, host);
    req.set(
        boost::beast::http::field::user_agent,
        R"(Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/114.0.0.0 Safari/537.36)");
    req.set("Accept", "application/json, text/plain, */*");
    req.set("accept-language", "id-ID,id;q=0.9,en-US;q=0.8,en;q=0.7");
    req.set(boost::beast::http::field::content_type, "application/json");
    req.set("origin", "https://chat.jinshutuan.com");

    constexpr std::string_view json_str = R"({
        "prompt":"user: hello\nassistant:",
        "system": "Always talk in English.",
        "withoutContext":true,
        "stream":true
    })";
    nlohmann::json request = nlohmann::json::parse(json_str, nullptr, false);
    request["prompt"] = prompt;

    SPDLOG_INFO("{}", request.dump(2));

    req.body() = request.dump();
    req.prepare_payload();

    boost::asio::ssl::context ctx(boost::asio::ssl::context::tls);
    ctx.set_verify_mode(boost::asio::ssl::verify_none);
    auto client = co_await createHttpClient(ctx, host, port);
    if (!client.has_value()) {
        SPDLOG_ERROR("createHttpClient: {}", client.error());
        co_await ch->async_send(err, client.error(), use_nothrow_awaitable);
        co_return;
    }

    auto ret = co_await sendRequestRecvChunk(ch, client.value(), req, 200, [&ch](std::string str) {
        boost::system::error_code err{};
        ch->try_send(err, std::move(str));
    });
    co_return;
}

boost::asio::awaitable<void> FreeGpt::chatBase(std::shared_ptr<Channel> ch, nlohmann::json json) {
    boost::system::error_code err{};
    ScopeExit auto_exit{[&] { ch->close(); }};

    auto prompt = json.at("meta").at("content").at("parts").at(0).at("content").get<std::string>();

    constexpr std::string_view host = "www.chatbase.co";
    constexpr std::string_view port = "443";

    constexpr std::string_view user_agent{
        R"(Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/116.0.0.0 Safari/537.36)"};

    boost::asio::ssl::context ctx(boost::asio::ssl::context::tls);
    ctx.set_verify_mode(boost::asio::ssl::verify_none);

    auto client = co_await createHttpClient(ctx, host, port);
    if (!client.has_value()) {
        SPDLOG_ERROR("createHttpClient: {}", client.error());
        co_await ch->async_send(err, client.error(), use_nothrow_awaitable);
        co_return;
    }
    auto& stream_ = client.value();

    boost::beast::http::request<boost::beast::http::string_body> req{boost::beast::http::verb::post, "/api/fe/chat",
                                                                     11};
    req.set(boost::beast::http::field::host, host);
    req.set(boost::beast::http::field::user_agent, user_agent);
    req.set("Accept", "*/*");
    req.set("accept-language", "en,fr-FR;q=0.9,fr;q=0.8,es-ES;q=0.7,es;q=0.6,en-US;q=0.5,am;q=0.4,de;q=0.3");
    req.set("origin", "https://www.chatbase.co");
    req.set("referer", "https://www.chatbase.co/");
    req.set(boost::beast::http::field::content_type, "application/json");
    req.set("sec-fetch-dest", "empty");
    req.set("sec-fetch-mode", "cors");
    req.set("sec-fetch-site", "same-origin");

    constexpr std::string_view json_str = R"({
        "messages": [
            {
                "role": "user",
                "content": "hello"
            }
        ],
        "captchaCode": "hadsa",
        "chatId": "quran---tafseer-saadi-pdf-wbgknt7zn",
        "conversationId": "kcXpqEnqUie3dnJlsRi_O-quran---tafseer-saadi-pdf-wbgknt7zn"
    })";
    nlohmann::json request = nlohmann::json::parse(json_str, nullptr, false);

    request["messages"][0]["content"] = prompt;
    SPDLOG_INFO("{}", request.dump(2));

    req.body() = request.dump();
    req.prepare_payload();

    auto result = co_await sendRequestRecvChunk(ch, stream_, req, 200, [&ch](std::string str) {
        boost::system::error_code err{};
        if (!str.empty())
            ch->try_send(err, str);
    });
    co_return;
}

boost::asio::awaitable<void> FreeGpt::gptGo(std::shared_ptr<Channel> ch, nlohmann::json json) {
    co_await boost::asio::post(boost::asio::bind_executor(*m_thread_pool_ptr, boost::asio::use_awaitable));

    boost::system::error_code err{};
    ScopeExit _exit{[=] { boost::asio::post(ch->get_executor(), [=] { ch->close(); }); }};
    auto prompt = json.at("meta").at("content").at("parts").at(0).at("content").get<std::string>();

    CURLcode res;
    CURL* curl = curl_easy_init();
    if (!curl) {
        auto error_info = std::format("curl_easy_init() failed:{}", curl_easy_strerror(res));
        co_await boost::asio::post(boost::asio::bind_executor(ch->get_executor(), boost::asio::use_awaitable));
        ch->try_send(err, error_info);
        co_return;
    }

    std::multimap<std::string, std::string> params{
        {"q", prompt},
        {"hlgpt", "default"},
        {"hl", "en"},
    };
    auto get_token_url = std::format("https://gptgo.ai/action_get_token.php?{}", paramsToQueryStr(params));
    curl_easy_setopt(curl, CURLOPT_URL, get_token_url.c_str());
    if (!m_cfg.http_proxy.empty())
        curl_easy_setopt(curl, CURLOPT_PROXY, m_cfg.http_proxy.c_str());
    auto cb = [](void* contents, size_t size, size_t nmemb, void* userp) -> size_t {
        boost::system::error_code err{};
        auto recv_ptr = static_cast<std::string*>(userp);
        std::string data{(char*)contents, size * nmemb};
        recv_ptr->append(data);
        return size * nmemb;
    };
    std::string recv_str;
    size_t (*fn)(void* contents, size_t size, size_t nmemb, void* userp) = cb;
    curlEasySetopt(curl);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, fn);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &recv_str);

    ScopeExit auto_exit{[=] { curl_easy_cleanup(curl); }};
    res = curl_easy_perform(curl);

    if (res != CURLE_OK) {
        auto error_info = std::format("curl_easy_perform() failed:{}", curl_easy_strerror(res));
        co_await boost::asio::post(boost::asio::bind_executor(ch->get_executor(), boost::asio::use_awaitable));
        ch->try_send(err, error_info);
        co_return;
    }
    int32_t response_code;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
    if (response_code != 200) {
        co_await boost::asio::post(boost::asio::bind_executor(ch->get_executor(), boost::asio::use_awaitable));
        ch->try_send(err, std::format("you http code:{}", response_code));
        co_return;
    }
    SPDLOG_INFO("recv_str: [{}]", recv_str);
    nlohmann::json line_json = nlohmann::json::parse(recv_str, nullptr, false);
    if (line_json.is_discarded()) {
        SPDLOG_ERROR("json parse error: [{}]", recv_str);
        co_await boost::asio::post(boost::asio::bind_executor(ch->get_executor(), boost::asio::use_awaitable));
        ch->try_send(err, std::format("json parse error:{}", recv_str));
        co_return;
    }
    auto status = line_json["status"].get<bool>();
    if (!status) {
        SPDLOG_ERROR("status is false: [{}]", recv_str);
        co_await boost::asio::post(boost::asio::bind_executor(ch->get_executor(), boost::asio::use_awaitable));
        ch->try_send(err, recv_str);
        co_return;
    }
    auto token = line_json["token"].get<std::string>();
    SPDLOG_INFO("token: [{}]", token);

    auto url = std::format("https://gptgo.ai/action_ai_gpt.php?token={}", token);
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    if (!m_cfg.http_proxy.empty())
        curl_easy_setopt(curl, CURLOPT_PROXY, m_cfg.http_proxy.c_str());

    struct Input {
        std::shared_ptr<Channel> ch;
        std::string recv;
    };
    Input input{ch};
    auto action_cb = [](void* contents, size_t size, size_t nmemb, void* userp) -> size_t {
        boost::system::error_code err{};
        auto input_ptr = static_cast<Input*>(userp);
        std::string data{(char*)contents, size * nmemb};
        auto& [ch, recv] = *input_ptr;
        recv.append(data);
        while (true) {
            auto position = recv.find("\n");
            if (position == std::string::npos)
                break;
            auto msg = recv.substr(0, position + 1);
            recv.erase(0, position + 1);
            msg.pop_back();
            if (msg.empty() || !msg.contains("content"))
                continue;
            auto fields = splitString(msg, "data: ");
            boost::system::error_code err{};
            nlohmann::json line_json = nlohmann::json::parse(fields.back(), nullptr, false);
            if (line_json.is_discarded()) {
                SPDLOG_ERROR("json parse error: [{}]", fields.back());
                boost::asio::post(ch->get_executor(),
                                  [=] { ch->try_send(err, std::format("json parse error: [{}]", fields.back())); });
                continue;
            }
            auto str = line_json["choices"][0]["delta"]["content"].get<std::string>();
            if (!str.empty() && str != "[DONE]")
                boost::asio::post(ch->get_executor(), [=] { ch->try_send(err, str); });
        }
        return size * nmemb;
    };
    size_t (*action_fn)(void* contents, size_t size, size_t nmemb, void* userp) = action_cb;
    curlEasySetopt(curl);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, action_fn);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &input);

    res = curl_easy_perform(curl);

    if (res != CURLE_OK) {
        co_await boost::asio::post(boost::asio::bind_executor(ch->get_executor(), boost::asio::use_awaitable));
        auto error_info = std::format("curl_easy_perform() failed:{}", curl_easy_strerror(res));
        ch->try_send(err, error_info);
        co_return;
    }
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
    if (response_code != 200) {
        co_await boost::asio::post(boost::asio::bind_executor(ch->get_executor(), boost::asio::use_awaitable));
        ch->try_send(err, std::format("you http code:{}", response_code));
        co_return;
    }
    co_return;
}

boost::asio::awaitable<void> FreeGpt::aibn(std::shared_ptr<Channel> ch, nlohmann::json json) {
    co_await boost::asio::post(boost::asio::bind_executor(*m_thread_pool_ptr, boost::asio::use_awaitable));

    boost::system::error_code err{};
    ScopeExit _exit{[=] { boost::asio::post(ch->get_executor(), [=] { ch->close(); }); }};
    auto prompt = json.at("meta").at("content").at("parts").at(0).at("content").get<std::string>();

    CURLcode res;
    CURL* curl = curl_easy_init();
    if (!curl) {
        auto error_info = std::format("curl_easy_init() failed:{}", curl_easy_strerror(res));
        co_await boost::asio::post(boost::asio::bind_executor(ch->get_executor(), boost::asio::use_awaitable));
        ch->try_send(err, error_info);
        co_return;
    }
    curl_easy_setopt(curl, CURLOPT_URL, "https://aibn.cc/api/generate");
    if (!m_cfg.http_proxy.empty())
        curl_easy_setopt(curl, CURLOPT_PROXY, m_cfg.http_proxy.c_str());

    struct Input {
        std::shared_ptr<Channel> ch;
        std::string recv;
    };
    Input input{ch};
    auto action_cb = [](void* contents, size_t size, size_t nmemb, void* userp) -> size_t {
        boost::system::error_code err{};
        auto input_ptr = static_cast<Input*>(userp);
        std::string data{(char*)contents, size * nmemb};
        auto& [ch, recv] = *input_ptr;
        boost::asio::post(ch->get_executor(), [=] { ch->try_send(err, data); });
        return size * nmemb;
    };
    size_t (*action_fn)(void* contents, size_t size, size_t nmemb, void* userp) = action_cb;
    curlEasySetopt(curl);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, action_fn);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &input);

    auto generate_signature = [](int timestamp, const std::string& message, const std::string& secret = "undefined") {
        std::stringstream ss;
        ss << timestamp << ":" << message << ":" << secret;
        std::string data = ss.str();

        unsigned char digest[SHA256_DIGEST_LENGTH];
        SHA256(reinterpret_cast<const unsigned char*>(data.c_str()), data.length(), digest);

        std::stringstream sha_stream;
        for (int i = 0; i < SHA256_DIGEST_LENGTH; i++) {
            sha_stream << std::setfill('0') << std::setw(2) << std::hex << static_cast<int>(digest[i]);
        }
        return sha_stream.str();
    };
    uint64_t timestamp = getTimestamp<std::chrono::seconds>();
    std::string signature = generate_signature(timestamp, prompt);

    constexpr std::string_view request_str{R"({
        "messages":[
            {
                "role":"user",
                "content":"hello"
            }
        ],
        "pass":null,
        "sign":"7c2700b5813053ff8000cb9fb1ebdadbfcf62882829da59e4474bee466de7c89",
        "time":1695716667
    })"};
    nlohmann::json request = nlohmann::json::parse(request_str, nullptr, false);

    request["sign"] = signature;
    request["time"] = timestamp;
    request["messages"] = getConversationJson(json);

    auto str = request.dump();
    SPDLOG_INFO("request : [{}]", str);

    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, str.c_str());

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    ScopeExit auto_exit{[=] {
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
    }};

    res = curl_easy_perform(curl);

    if (res != CURLE_OK) {
        co_await boost::asio::post(boost::asio::bind_executor(ch->get_executor(), boost::asio::use_awaitable));
        auto error_info = std::format("curl_easy_perform() failed:{}", curl_easy_strerror(res));
        ch->try_send(err, error_info);
        co_return;
    }
    int32_t response_code;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
    if (response_code != 200) {
        co_await boost::asio::post(boost::asio::bind_executor(ch->get_executor(), boost::asio::use_awaitable));
        ch->try_send(err, std::format("you http code:{}", response_code));
        co_return;
    }
    co_return;
}

boost::asio::awaitable<void> FreeGpt::chatForAi(std::shared_ptr<Channel> ch, nlohmann::json json) {
    co_await boost::asio::post(boost::asio::bind_executor(*m_thread_pool_ptr, boost::asio::use_awaitable));

    boost::system::error_code err{};
    ScopeExit _exit{[=] { boost::asio::post(ch->get_executor(), [=] { ch->close(); }); }};
    auto prompt = json.at("meta").at("content").at("parts").at(0).at("content").get<std::string>();

    CURLcode res;
    CURL* curl = curl_easy_init();
    if (!curl) {
        auto error_info = std::format("curl_easy_init() failed:{}", curl_easy_strerror(res));
        co_await boost::asio::post(boost::asio::bind_executor(ch->get_executor(), boost::asio::use_awaitable));
        ch->try_send(err, error_info);
        co_return;
    }
    curl_easy_setopt(curl, CURLOPT_URL, "https://chatforai.store/api/handle/provider-openai");
    if (!m_cfg.http_proxy.empty())
        curl_easy_setopt(curl, CURLOPT_PROXY, m_cfg.http_proxy.c_str());

    struct Input {
        std::shared_ptr<Channel> ch;
        std::string recv;
    };
    Input input{ch};
    auto action_cb = [](void* contents, size_t size, size_t nmemb, void* userp) -> size_t {
        boost::system::error_code err{};
        auto input_ptr = static_cast<Input*>(userp);
        std::string data{(char*)contents, size * nmemb};
        auto& [ch, recv] = *input_ptr;
        boost::asio::post(ch->get_executor(), [=] { ch->try_send(err, data); });
        return size * nmemb;
    };
    size_t (*action_fn)(void* contents, size_t size, size_t nmemb, void* userp) = action_cb;
    curlEasySetopt(curl);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, action_fn);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &input);

    auto generate_signature = [](uint64_t timestamp, const std::string& message, const std::string& id) {
        std::string s = std::to_string(timestamp) + ":" + id + ":" + message + ":7YN8z6d6";
        unsigned char hash[SHA256_DIGEST_LENGTH];
        SHA256_CTX sha256;
        if (!SHA256_Init(&sha256))
            throw std::runtime_error("SHA-256 initialization failed");
        if (!SHA256_Update(&sha256, s.c_str(), s.length()))
            throw std::runtime_error("SHA-256 update failed");
        if (!SHA256_Final(hash, &sha256))
            throw std::runtime_error("SHA-256 finalization failed");
        std::stringstream ss;
        for (int i = 0; i < SHA256_DIGEST_LENGTH; i++)
            ss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(hash[i]);
        return ss.str();
    };
    uint64_t timestamp = getTimestamp();
    constexpr std::string_view request_str{R"({
        "conversationId": "id_1696984301982",
        "conversationType": "chat_continuous",
        "botId": "chat_continuous",
        "globalSettings": {
            "baseUrl": "https://api.openai.com",
            "model": "gpt-3.5-turbo",
            "messageHistorySize": 5,
            "temperature": 0.7,
            "top_p": 1
        },
        "botSettings": {},
        "prompt": "hello",
        "messages": [{
            "role": "user",
            "content": "hello"
        }],
        "sign": "15d8e701706743ffa74f8b96c97bd1f79354c7da4a97438c81c6bb259004cd77",
        "timestamp": 1696984302017
    })"};
    nlohmann::json request = nlohmann::json::parse(request_str, nullptr, false);
    auto conversation_id = std::format("id_{}", timestamp - 35);
    request["conversationId"] = conversation_id;
    request["timestamp"] = timestamp;
    request["sign"] = generate_signature(timestamp, prompt, conversation_id);
    request["messages"] = getConversationJson(json);
    request["prompt"] = prompt;

    auto str = request.dump();
    SPDLOG_INFO("request : [{}]", str);

    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, str.c_str());

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "Origin: https://chatforai.store");
    headers = curl_slist_append(headers, "Referer: https://chatforai.store/");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    ScopeExit auto_exit{[=] {
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
    }};

    res = curl_easy_perform(curl);

    if (res != CURLE_OK) {
        co_await boost::asio::post(boost::asio::bind_executor(ch->get_executor(), boost::asio::use_awaitable));
        auto error_info = std::format("curl_easy_perform() failed:{}", curl_easy_strerror(res));
        ch->try_send(err, error_info);
        co_return;
    }
    int32_t response_code;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
    if (response_code != 200) {
        co_await boost::asio::post(boost::asio::bind_executor(ch->get_executor(), boost::asio::use_awaitable));
        ch->try_send(err, std::format("you http code:{}", response_code));
        co_return;
    }
    co_return;
}

boost::asio::awaitable<void> FreeGpt::freeGpt(std::shared_ptr<Channel> ch, nlohmann::json json) {
    co_await boost::asio::post(boost::asio::bind_executor(*m_thread_pool_ptr, boost::asio::use_awaitable));

    boost::system::error_code err{};
    ScopeExit _exit{[=] { boost::asio::post(ch->get_executor(), [=] { ch->close(); }); }};
    auto prompt = json.at("meta").at("content").at("parts").at(0).at("content").get<std::string>();

    CURLcode res;
    CURL* curl = curl_easy_init();
    if (!curl) {
        auto error_info = std::format("curl_easy_init() failed:{}", curl_easy_strerror(res));
        co_await boost::asio::post(boost::asio::bind_executor(ch->get_executor(), boost::asio::use_awaitable));
        ch->try_send(err, error_info);
        co_return;
    }
    curl_easy_setopt(curl, CURLOPT_URL, "https://k.aifree.site/api/generate");
    if (!m_cfg.http_proxy.empty())
        curl_easy_setopt(curl, CURLOPT_PROXY, m_cfg.http_proxy.c_str());

    struct Input {
        std::shared_ptr<Channel> ch;
        std::string recv;
    };
    Input input{ch};
    auto action_cb = [](void* contents, size_t size, size_t nmemb, void* userp) -> size_t {
        boost::system::error_code err{};
        auto input_ptr = static_cast<Input*>(userp);
        std::string data{(char*)contents, size * nmemb};
        auto& [ch, recv] = *input_ptr;
        boost::asio::post(ch->get_executor(), [=] { ch->try_send(err, data); });
        return size * nmemb;
    };
    size_t (*action_fn)(void* contents, size_t size, size_t nmemb, void* userp) = action_cb;
    curlEasySetopt(curl);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, action_fn);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &input);

    auto generate_signature = [](int timestamp, const std::string& message, const std::string& secret = "") {
        std::stringstream ss;
        ss << timestamp << ":" << message << ":" << secret;
        std::string data = ss.str();

        unsigned char digest[SHA256_DIGEST_LENGTH];
        SHA256(reinterpret_cast<const unsigned char*>(data.c_str()), data.length(), digest);

        std::stringstream sha_stream;
        for (int i = 0; i < SHA256_DIGEST_LENGTH; i++) {
            sha_stream << std::setfill('0') << std::setw(2) << std::hex << static_cast<int>(digest[i]);
        }
        return sha_stream.str();
    };
    uint64_t timestamp = getTimestamp<std::chrono::seconds>();
    std::string signature = generate_signature(timestamp, prompt);

    constexpr std::string_view request_str{R"({
        "messages":[
            {
                "role":"user",
                "content":"hello"
            }
        ],
        "pass":null,
        "sign":"7c2700b5813053ff8000cb9fb1ebdadbfcf62882829da59e4474bee466de7c89",
        "time":1695716667
    })"};
    nlohmann::json request = nlohmann::json::parse(request_str, nullptr, false);

    request["sign"] = signature;
    request["time"] = timestamp;
    request["messages"] = getConversationJson(json);

    auto str = request.dump();
    SPDLOG_INFO("request : [{}]", str);

    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, str.c_str());

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    ScopeExit auto_exit{[=] {
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
    }};

    res = curl_easy_perform(curl);

    if (res != CURLE_OK) {
        co_await boost::asio::post(boost::asio::bind_executor(ch->get_executor(), boost::asio::use_awaitable));
        auto error_info = std::format("curl_easy_perform() failed:{}", curl_easy_strerror(res));
        ch->try_send(err, error_info);
        co_return;
    }
    int32_t response_code;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
    if (response_code != 200) {
        co_await boost::asio::post(boost::asio::bind_executor(ch->get_executor(), boost::asio::use_awaitable));
        ch->try_send(err, std::format("you http code:{}", response_code));
        co_return;
    }
    co_return;
}

boost::asio::awaitable<void> FreeGpt::chatGpt4Online(std::shared_ptr<Channel> ch, nlohmann::json json) {
    co_await boost::asio::post(boost::asio::bind_executor(*m_thread_pool_ptr, boost::asio::use_awaitable));

    boost::system::error_code err{};
    ScopeExit _exit{[=] { boost::asio::post(ch->get_executor(), [=] { ch->close(); }); }};
    auto prompt = json.at("meta").at("content").at("parts").at(0).at("content").get<std::string>();

    CURLcode res;
    CURL* curl = curl_easy_init();
    if (!curl) {
        auto error_info = std::format("curl_easy_init() failed:{}", curl_easy_strerror(res));
        co_await boost::asio::post(boost::asio::bind_executor(ch->get_executor(), boost::asio::use_awaitable));
        ch->try_send(err, error_info);
        co_return;
    }
    curl_easy_setopt(curl, CURLOPT_URL, "https://chatgpt4online.org/wp-json/mwai-ui/v1/chats/submit");
    if (!m_cfg.http_proxy.empty())
        curl_easy_setopt(curl, CURLOPT_PROXY, m_cfg.http_proxy.c_str());

    struct Input {
        std::shared_ptr<Channel> ch;
        std::string recv;
    };
    Input input{ch};
    auto action_cb = [](void* contents, size_t size, size_t nmemb, void* userp) -> size_t {
        boost::system::error_code err{};
        auto input_ptr = static_cast<Input*>(userp);
        std::string data{(char*)contents, size * nmemb};
        auto& [ch, recv] = *input_ptr;
        recv.append(data);
        while (true) {
            auto position = recv.find("\n");
            if (position == std::string::npos)
                break;
            auto msg = recv.substr(0, position + 1);
            recv.erase(0, position + 1);
            msg.pop_back();
            if (msg.empty())
                continue;
            auto fields = splitString(msg, "data: ");
            boost::system::error_code err{};
            nlohmann::json line_json = nlohmann::json::parse(fields.back(), nullptr, false);
            if (line_json.is_discarded()) {
                SPDLOG_ERROR("json parse error: [{}]", fields.back());
                boost::asio::post(ch->get_executor(),
                                  [=] { ch->try_send(err, std::format("json parse error: [{}]", fields.back())); });
                continue;
            }
            auto type = line_json["type"].get<std::string>();
            if (type == "live") {
                auto str = line_json["data"].get<std::string>();
                boost::asio::post(ch->get_executor(), [=] { ch->try_send(err, str); });
            }
        }
        return size * nmemb;
    };
    size_t (*action_fn)(void* contents, size_t size, size_t nmemb, void* userp) = action_cb;
    curlEasySetopt(curl);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, action_fn);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &input);

    constexpr std::string_view request_str{R"({
        "botId":"default",
        "customId":null,
        "session":"N/A",
        "chatId":"",
        "contextId":58,
        "messages":[
            {
                "role":"user",
                "content":"hello"
            }
        ],
        "newMessage":"hello",
        "stream":true
    })"};
    nlohmann::json request = nlohmann::json::parse(request_str, nullptr, false);

    request["messages"] = getConversationJson(json);
    request["newMessage"] = prompt;

    auto str = request.dump();
    SPDLOG_INFO("request : [{}]", str);

    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, str.c_str());

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    ScopeExit auto_exit{[=] {
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
    }};

    res = curl_easy_perform(curl);

    if (res != CURLE_OK) {
        co_await boost::asio::post(boost::asio::bind_executor(ch->get_executor(), boost::asio::use_awaitable));
        auto error_info = std::format("curl_easy_perform() failed:{}", curl_easy_strerror(res));
        ch->try_send(err, error_info);
        co_return;
    }
    int32_t response_code;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
    if (response_code != 200) {
        co_await boost::asio::post(boost::asio::bind_executor(ch->get_executor(), boost::asio::use_awaitable));
        ch->try_send(err, std::format("you http code:{}", response_code));
        co_return;
    }
    co_return;
}

boost::asio::awaitable<void> FreeGpt::gptalk(std::shared_ptr<Channel> ch, nlohmann::json json) {
    co_await boost::asio::post(boost::asio::bind_executor(*m_thread_pool_ptr, boost::asio::use_awaitable));
    ScopeExit _exit{[=] { boost::asio::post(ch->get_executor(), [=] { ch->close(); }); }};
    boost::system::error_code err{};

    auto prompt = json.at("meta").at("content").at("parts").at(0).at("content").get<std::string>();

    auto generate_token_hex = [](int32_t length) {
        std::random_device rd;
        std::stringstream ss;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<> dis(0, 15);
        for (int i = 0; i < length; ++i)
            ss << std::hex << dis(gen);
        std::string token = ss.str();
        token = std::string(length * 2 - token.length(), '0') + token;
        return token;
    };

    CURLcode res;
    int32_t response_code;

    struct Input {
        std::shared_ptr<Channel> ch;
        std::string recv;
        std::string last_message;
    };
    Input input{ch};

    CURL* curl = curl_easy_init();
    if (!curl) {
        auto error_info = std::format("curl_easy_init() failed:{}", curl_easy_strerror(res));
        co_await boost::asio::post(boost::asio::bind_executor(ch->get_executor(), boost::asio::use_awaitable));
        ch->try_send(err, error_info);
        co_return;
    }
    curl_easy_setopt(curl, CURLOPT_URL, "https://gptalk.net/api/chatgpt/user/login");

    if (!m_cfg.http_proxy.empty())
        curl_easy_setopt(curl, CURLOPT_PROXY, m_cfg.http_proxy.c_str());
    curlEasySetopt(curl);
    nlohmann::json login_json;
    login_json["fingerprint"] = generate_token_hex(16);
    login_json["platform"] = "fingerprint";
    std::string request_str = login_json.dump();
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request_str.c_str());

    auto action_cb = [](void* contents, size_t size, size_t nmemb, void* userp) -> size_t {
        auto input_ptr = static_cast<Input*>(userp);
        std::string data{(char*)contents, size * nmemb};
        auto& [ch, recv, _] = *input_ptr;
        recv.append(data);
        return size * nmemb;
    };
    size_t (*action_fn)(void* contents, size_t size, size_t nmemb, void* userp) = action_cb;
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, action_fn);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &input);

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "authority: gptalk.net");
    headers = curl_slist_append(headers, "origin: https://gptalk.net");
    headers = curl_slist_append(headers, "Accept: */*");
    headers = curl_slist_append(headers, "x-auth-appid: 2229");
    headers = curl_slist_append(headers, "x-auth-openid: ");
    headers = curl_slist_append(headers, "x-auth-platform: ");
    uint64_t timestamp = getTimestamp<std::chrono::seconds>();
    auto auth_timestamp = std::format("x-auth-timestamp: {}", timestamp);
    headers = curl_slist_append(headers, auth_timestamp.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    ScopeExit auto_exit{[=] {
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
    }};

    res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        co_await boost::asio::post(boost::asio::bind_executor(ch->get_executor(), boost::asio::use_awaitable));
        auto error_info = std::format("curl_easy_perform() failed:{}", curl_easy_strerror(res));
        ch->try_send(err, error_info);
        co_return;
    }
    SPDLOG_INFO("login rsp: [{}]", input.recv);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
    if (response_code != 200) {
        co_await boost::asio::post(boost::asio::bind_executor(ch->get_executor(), boost::asio::use_awaitable));
        ch->try_send(err, std::format("liaobots http code:{}", response_code));
        co_return;
    }
    nlohmann::json auth_rsp = nlohmann::json::parse(input.recv, nullptr, false);
    auto auth_token = auth_rsp["data"]["token"].get<std::string>();
    SPDLOG_INFO("token: [{}]", auth_token);

    curl_easy_setopt(curl, CURLOPT_URL, "https://gptalk.net/api/chatgpt/chatapi/text");

    if (!m_cfg.http_proxy.empty())
        curl_easy_setopt(curl, CURLOPT_PROXY, m_cfg.http_proxy.c_str());
    input.recv.clear();
    auto api_action_cb = [](void* contents, size_t size, size_t nmemb, void* userp) -> size_t {
        auto input_ptr = static_cast<Input*>(userp);
        std::string data{(char*)contents, size * nmemb};
        auto& [ch, recv, _] = *input_ptr;
        recv.append(data);
        return size * nmemb;
    };
    size_t (*api_cb)(void* contents, size_t size, size_t nmemb, void* userp) = api_action_cb;
    curlEasySetopt(curl);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, api_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &input);
    constexpr std::string_view json_str = R"({
        "content":"hello",
        "accept":"stream",
        "from":1,
        "model":"gpt-3.5-turbo",
        "is_mobile":0,
        "user_agent":"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/110.0.0.0 Safari/537.36",
        "is_open_ctx":0,
        "prompt":"",
        "roid":111,
        "temperature":0,
        "ctx_msg_count":3,
        "created_at":1696655321
    })";
    nlohmann::json request = nlohmann::json::parse(json_str, nullptr, false);
    request["created_at"] = timestamp;
    request["content"] = prompt;
    request_str = request.dump();
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request_str.c_str());

    auto auth_str = std::format("authorization: Bearer {}", auth_token);
    headers = curl_slist_append(headers, auth_str.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        co_await boost::asio::post(boost::asio::bind_executor(ch->get_executor(), boost::asio::use_awaitable));
        auto error_info = std::format("curl_easy_perform() failed:{}", curl_easy_strerror(res));
        ch->try_send(err, error_info);
        co_return;
    }
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
    if (response_code != 200) {
        co_await boost::asio::post(boost::asio::bind_executor(ch->get_executor(), boost::asio::use_awaitable));
        ch->try_send(err, std::format("liaobots http code:{}", response_code));
        co_return;
    }
    SPDLOG_INFO("input.recv: [{}]", input.recv);
    nlohmann::json get_text_rsp = nlohmann::json::parse(input.recv, nullptr, false);
    auto token = get_text_rsp["data"]["token"].get<std::string>();
    SPDLOG_INFO("token: [{}]", token);
    input.recv.clear();

    auto url = std::format("https://gptalk.net/api/chatgpt/chatapi/stream?token={}", token);

    SPDLOG_INFO("url: {}", url);
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);

    if (!m_cfg.http_proxy.empty())
        curl_easy_setopt(curl, CURLOPT_PROXY, m_cfg.http_proxy.c_str());
    curlEasySetopt(curl);

    auto stream_action_cb = [](void* contents, size_t size, size_t nmemb, void* userp) mutable -> size_t {
        auto input_ptr = static_cast<Input*>(userp);
        std::string data{(char*)contents, size * nmemb};
        auto& [ch, recv, last_message] = *input_ptr;
        recv.append(data);
        while (true) {
            auto position = recv.find("\n");
            if (position == std::string::npos)
                break;
            auto msg = recv.substr(0, position + 1);
            recv.erase(0, position + 1);
            msg.pop_back();
            if (msg.empty() || !msg.contains("content") || !msg.starts_with("data: "))
                continue;
            msg.erase(0, 6);
            boost::system::error_code err{};
            nlohmann::json line_json = nlohmann::json::parse(msg, nullptr, false);
            if (line_json.is_discarded()) {
                SPDLOG_ERROR("json parse error: [{}]", msg);
                boost::asio::post(ch->get_executor(),
                                  [=] { ch->try_send(err, std::format("json parse error: [{}]", msg)); });
                continue;
            }
            auto content = line_json["content"].get<std::string>();
            if (last_message.empty())
                last_message = content;
            else {
                auto count = last_message.size();
                last_message = content;
                content.erase(0, count);
            }
            if (content.empty())
                continue;
            boost::asio::post(ch->get_executor(), [=, content = std::move(content)] { ch->try_send(err, content); });
        }
        return size * nmemb;
    };
    size_t (*stream_action_cb_fn)(void* contents, size_t size, size_t nmemb, void* userp) = stream_action_cb;
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, stream_action_cb_fn);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &input);
    struct curl_slist* new_headers = nullptr;
    new_headers = curl_slist_append(new_headers, "Content-Type: application/json");
    new_headers = curl_slist_append(new_headers, "authority: gptalk.net");
    new_headers = curl_slist_append(new_headers, "origin: https://gptalk.net");
    new_headers = curl_slist_append(new_headers, "Accept: */*");
    new_headers = curl_slist_append(new_headers, "x-auth-appid: 2229");
    new_headers = curl_slist_append(new_headers, "x-auth-openid: ");
    new_headers = curl_slist_append(new_headers, "x-auth-platform: ");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, new_headers);
    ScopeExit new_headers_auto_exit{[=] { curl_slist_free_all(new_headers); }};

    res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        co_await boost::asio::post(boost::asio::bind_executor(ch->get_executor(), boost::asio::use_awaitable));
        auto error_info = std::format("curl_easy_perform() failed:{}", curl_easy_strerror(res));
        ch->try_send(err, error_info);
        co_return;
    }
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
    if (response_code != 200) {
        co_await boost::asio::post(boost::asio::bind_executor(ch->get_executor(), boost::asio::use_awaitable));
        ch->try_send(err, std::format("gptalk http code:{}", response_code));
        co_return;
    }
    co_return;
}

boost::asio::awaitable<void> FreeGpt::gptForLove(std::shared_ptr<Channel> ch, nlohmann::json json) {
    co_await boost::asio::post(boost::asio::bind_executor(*m_thread_pool_ptr, boost::asio::use_awaitable));

    boost::system::error_code err{};
    ScopeExit _exit{[=] { boost::asio::post(ch->get_executor(), [=] { ch->close(); }); }};
    auto prompt = json.at("meta").at("content").at("parts").at(0).at("content").get<std::string>();

    CURLcode res;
    CURL* curl = curl_easy_init();
    if (!curl) {
        auto error_info = std::format("curl_easy_init() failed:{}", curl_easy_strerror(res));
        co_await boost::asio::post(boost::asio::bind_executor(ch->get_executor(), boost::asio::use_awaitable));
        ch->try_send(err, error_info);
        co_return;
    }
    curl_easy_setopt(curl, CURLOPT_URL, "https://api.gptplus.one/chat-process");
    if (!m_cfg.http_proxy.empty())
        curl_easy_setopt(curl, CURLOPT_PROXY, m_cfg.http_proxy.c_str());
    struct Input {
        std::shared_ptr<Channel> ch;
        std::string recv;
    };
    Input input{ch};
    auto action_cb = [](void* contents, size_t size, size_t nmemb, void* userp) -> size_t {
        boost::system::error_code err{};
        auto input_ptr = static_cast<Input*>(userp);
        std::string data{(char*)contents, size * nmemb};
        auto& [ch, recv] = *input_ptr;
        recv.append(data);
        while (true) {
            auto position = recv.find("\n");
            if (position == std::string::npos)
                break;
            auto msg = recv.substr(0, position + 1);
            recv.erase(0, position + 1);
            msg.pop_back();
            if (msg.contains("10分钟内提问超过了5次")) {
                boost::asio::post(ch->get_executor(), [=] { ch->try_send(err, msg); });
                return size * nmemb;
            }
            if (msg.empty() || !msg.contains("content"))
                continue;
            boost::system::error_code err{};
            nlohmann::json line_json = nlohmann::json::parse(msg, nullptr, false);
            if (line_json.is_discarded()) {
                SPDLOG_ERROR("json parse error: [{}]", msg);
                boost::asio::post(ch->get_executor(),
                                  [=] { ch->try_send(err, std::format("json parse error: [{}]", msg)); });
                continue;
            }
            auto str = line_json["detail"]["choices"][0]["delta"]["content"].get<std::string>();
            if (!str.empty())
                boost::asio::post(ch->get_executor(), [=] { ch->try_send(err, str); });
        }
        return size * nmemb;
    };
    size_t (*action_fn)(void* contents, size_t size, size_t nmemb, void* userp) = action_cb;
    curlEasySetopt(curl);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, action_fn);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &input);

    constexpr std::string_view request_str{R"({
        "prompt": "hello",
        "options": {},
        "systemMessage": "You are ChatGPT, the version is GPT3.5, a large language model trained by OpenAI. Follow the user's instructions carefully.",
        "temperature": 0.8,
        "top_p": 1,
        "secret": "U2FsdGVkX18vdtlMj0nP1LoUzEqJTP0is+Q2+bQJNMk=",
        "stream": false
    })"};
    nlohmann::json request = nlohmann::json::parse(request_str, nullptr, false);

    auto secret_rsp = callZeus("http://127.0.0.1:8860/gptforlove", "{}");
    if (!secret_rsp.has_value()) {
        SPDLOG_ERROR("callZeus error: {}", secret_rsp.error());
        co_await boost::asio::post(boost::asio::bind_executor(ch->get_executor(), boost::asio::use_awaitable));
        ch->try_send(err, secret_rsp.error());
        co_return;
    }
    SPDLOG_INFO("zeus: [{}]", secret_rsp.value().dump());
    request["secret"] = secret_rsp.value()["secret"];
    request["prompt"] = prompt;

    auto str = request.dump();
    SPDLOG_INFO("request : [{}]", str);

    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, str.c_str());

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "referer: https://ai18.gptforlove.com/");
    headers = curl_slist_append(headers, "origin: https://ai18.gptforlove.com");
    headers = curl_slist_append(headers, "authority: api.gptplus.one");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    ScopeExit auto_exit{[=] {
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
    }};

    res = curl_easy_perform(curl);

    if (res != CURLE_OK) {
        co_await boost::asio::post(boost::asio::bind_executor(ch->get_executor(), boost::asio::use_awaitable));
        auto error_info = std::format("curl_easy_perform() failed:{}", curl_easy_strerror(res));
        ch->try_send(err, error_info);
        co_return;
    }
    int32_t response_code;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
    if (response_code != 200) {
        co_await boost::asio::post(boost::asio::bind_executor(ch->get_executor(), boost::asio::use_awaitable));
        ch->try_send(err, std::format("you http code:{}", response_code));
        co_return;
    }
    co_return;
}

boost::asio::awaitable<void> FreeGpt::chatGptDemo(std::shared_ptr<Channel> ch, nlohmann::json json) {
    co_await boost::asio::post(boost::asio::bind_executor(*m_thread_pool_ptr, boost::asio::use_awaitable));
    ScopeExit _exit{[=] { boost::asio::post(ch->get_executor(), [=] { ch->close(); }); }};
    boost::system::error_code err{};

    auto prompt = json.at("meta").at("content").at("parts").at(0).at("content").get<std::string>();

    struct Input {
        std::shared_ptr<Channel> ch;
        std::string recv;
    };
    Input input;

    std::unordered_map<std::string, std::string> http_headers{
        {"authority", "chat.chatgptdemo.net"},
        {"origin", "https://chat.chatgptdemo.net"},
        {"referer", "https://chat.chatgptdemo.net/"},
    };
    std::multimap<std::string, std::string> response_header;

    CURLcode res;
    CURL* curl = curl_easy_init();
    if (!curl) {
        auto error_info = std::format("curl_easy_init() failed:{}", curl_easy_strerror(res));
        co_await boost::asio::post(boost::asio::bind_executor(ch->get_executor(), boost::asio::use_awaitable));
        ch->try_send(err, error_info);
        co_return;
    }
    ScopeExit auto_exit{[=] { curl_easy_cleanup(curl); }};

    auto ret = sendHttpRequest(CurlHttpRequest{
        .curl = curl,
        .url = "https://chat.chatgptdemo.net/",
        .http_proxy = m_cfg.http_proxy,
        .cb = [](void* contents, size_t size, size_t nmemb, void* userp) mutable -> size_t {
            auto input_ptr = static_cast<Input*>(userp);
            std::string data{(char*)contents, size * nmemb};
            auto& [ch, recv] = *input_ptr;
            recv.append(data);
            return size * nmemb;
        },
        .input = [&] -> void* {
            input.recv.clear();
            return &input;
        }(),
        .headers = http_headers,
        .body = std::string{},
        .response_header_ptr = &response_header,
        .expect_response_code = 200,
        .ssl_verify = false,
    });
    if (ret) {
        co_await boost::asio::post(boost::asio::bind_executor(ch->get_executor(), boost::asio::use_awaitable));
        ch->try_send(err, ret.value());
        co_return;
    }
    for (auto& [k, v] : response_header)
        SPDLOG_INFO("{}: {}", k, v);

    auto match_ret = findAll(R"(<div id="USERID" style="display: none">(.*)<\/div>)", input.recv);
    if (match_ret.empty()) {
        co_await boost::asio::post(boost::asio::bind_executor(ch->get_executor(), boost::asio::use_awaitable));
        ch->try_send(err, std::format("not found userid"));
        co_return;
    }
    auto extract_value = [](const std::string& str) {
        size_t start = str.find('>') + 1;
        size_t end = str.find('<', start);
        std::string value = str.substr(start, end - start);
        return value;
    };
    auto user_id = extract_value(match_ret[0]);
    SPDLOG_INFO("user_id: [{}]", user_id);

    ret = sendHttpRequest(CurlHttpRequest{
        .curl = curl,
        .url = "https://chat.chatgptdemo.net/new_chat",
        .http_proxy = m_cfg.http_proxy,
        .cb = [](void* contents, size_t size, size_t nmemb, void* userp) mutable -> size_t {
            auto input_ptr = static_cast<Input*>(userp);
            std::string data{(char*)contents, size * nmemb};
            auto& [ch, recv] = *input_ptr;
            recv.append(data);
            return size * nmemb;
        },
        .input = [&] -> void* {
            input.recv.clear();
            input.ch = ch;
            return &input;
        }(),
        .headers = [&] -> auto& {
            http_headers.emplace("Content-Type", "application/json");
            return http_headers;
        }(),
        .body = [&] -> std::string {
            constexpr std::string_view json_str = R"({"user_id":"user_id"})";
            nlohmann::json request = nlohmann::json::parse(json_str, nullptr, false);
            request["user_id"] = user_id;
            return request.dump();
        }(),
        .response_header_ptr = nullptr,
        .expect_response_code = 200,
        .ssl_verify = false,
    });
    if (ret) {
        co_await boost::asio::post(boost::asio::bind_executor(ch->get_executor(), boost::asio::use_awaitable));
        ch->try_send(err, ret.value());
        co_return;
    }

    SPDLOG_INFO("input.recv: [{}]", input.recv);
    nlohmann::json get_text_rsp = nlohmann::json::parse(input.recv, nullptr, false);
    auto chat_id = get_text_rsp["id_"].get<std::string>();
    SPDLOG_INFO("chat_id: [{}]", chat_id);

    ret = sendHttpRequest(CurlHttpRequest{
        .curl = curl,
        .url = "https://chat.chatgptdemo.net/chat_api_stream",
        .http_proxy = m_cfg.http_proxy,
        .cb = [](void* contents, size_t size, size_t nmemb, void* userp) mutable -> size_t {
            auto input_ptr = static_cast<Input*>(userp);
            std::string data{(char*)contents, size * nmemb};
            auto& [ch, recv] = *input_ptr;
            recv.append(data);
            while (true) {
                auto position = recv.find("\n");
                if (position == std::string::npos)
                    break;
                auto msg = recv.substr(0, position + 1);
                recv.erase(0, position + 1);
                msg.pop_back();
                if (msg.empty() || !msg.contains("content"))
                    continue;
                if (!msg.starts_with("data: "))
                    continue;
                msg.erase(0, 6);
                boost::system::error_code err{};
                nlohmann::json line_json = nlohmann::json::parse(msg, nullptr, false);
                if (line_json.is_discarded()) {
                    SPDLOG_ERROR("json parse error: [{}]", msg);
                    boost::asio::post(ch->get_executor(),
                                      [=] { ch->try_send(err, std::format("json parse error: [{}]", msg)); });
                    continue;
                }
                auto str = line_json["choices"][0]["delta"]["content"].get<std::string>();
                if (!str.empty())
                    boost::asio::post(ch->get_executor(), [=] { ch->try_send(err, str); });
            }
            return size * nmemb;
        },
        .input = [&] -> void* {
            input.recv.clear();
            input.ch = ch;
            return &input;
        }(),
        .headers = http_headers,
        .body = [&] -> std::string {
            constexpr std::string_view ask_json_str =
                R"({"question": "hello", "chat_id": "6524f3640d0d824902f598c1", "timestamp": 1696920420510})";
            nlohmann::json ask_request = nlohmann::json::parse(ask_json_str, nullptr, false);
            ask_request["question"] = prompt;
            ask_request["chat_id"] = chat_id;
            uint64_t timestamp = getTimestamp();
            ask_request["timestamp"] = timestamp;
            std::string ask_request_str = ask_request.dump();
            SPDLOG_INFO("ask_request_str: [{}]", ask_request_str);
            return ask_request_str;
        }(),
        .response_header_ptr = nullptr,
        .expect_response_code = 200,
        .ssl_verify = false,
    });
    if (ret) {
        co_await boost::asio::post(boost::asio::bind_executor(ch->get_executor(), boost::asio::use_awaitable));
        ch->try_send(err, ret.value());
        co_return;
    }
    co_return;
}

boost::asio::awaitable<void> FreeGpt::llama2(std::shared_ptr<Channel> ch, nlohmann::json json) {
    co_await boost::asio::post(boost::asio::bind_executor(*m_thread_pool_ptr, boost::asio::use_awaitable));
    ScopeExit _exit{[=] { boost::asio::post(ch->get_executor(), [=] { ch->close(); }); }};
    boost::system::error_code err{};

    auto prompt = json.at("meta").at("content").at("parts").at(0).at("content").get<std::string>();

    struct Input {
        std::shared_ptr<Channel> ch;
        std::string recv;
    };
    Input input;

    CURLcode res;
    CURL* curl = curl_easy_init();
    if (!curl) {
        auto error_info = std::format("curl_easy_init() failed:{}", curl_easy_strerror(res));
        co_await boost::asio::post(boost::asio::bind_executor(ch->get_executor(), boost::asio::use_awaitable));
        ch->try_send(err, error_info);
        co_return;
    }
    ScopeExit auto_exit{[=] { curl_easy_cleanup(curl); }};

    auto ret = sendHttpRequest(CurlHttpRequest{
        .curl = curl,
        .url = "https://www.llama2.ai/api",
        .http_proxy = m_cfg.http_proxy,
        .cb = [](void* contents, size_t size, size_t nmemb, void* userp) mutable -> size_t {
            auto input_ptr = static_cast<Input*>(userp);
            std::string data{(char*)contents, size * nmemb};
            auto& [ch, recv] = *input_ptr;
            boost::asio::post(ch->get_executor(), [=] {
                boost::system::error_code err{};
                ch->try_send(err, data);
            });
            return size * nmemb;
        },
        .input = [&] -> void* {
            input.recv.clear();
            input.ch = ch;
            return &input;
        }(),
        .headers = [&] -> auto& {
            static std::unordered_map<std::string, std::string> headers{
                {"Accept", "*/*"},
                {"origin", "https://www.llama2.ai"},
                {"referer", "https://www.llama2.ai/"},
                {"Content-Type", "text/plain;charset=UTF-8"},
            };
            return headers;
        }(),
        .body = [&] -> std::string {
            constexpr std::string_view ask_json_str = R"({
                "prompt": "[INST] hello [/INST]\n",
                "version": "2796ee9483c3fd7aa2e171d38f4ca12251a30609463dcfd4cd76703f22e96cdf",
                "systemPrompt": "You are a helpful assistant.",
                "temperature": 0.75,
                "topP": 0.9,
                "maxTokens": 800
            })";
            nlohmann::json ask_request = nlohmann::json::parse(ask_json_str, nullptr, false);
            ask_request["prompt"] = std::format("[INST] {} [/INST]\n", prompt);
            std::string ask_request_str = ask_request.dump();
            SPDLOG_INFO("ask_request_str: [{}]", ask_request_str);
            return ask_request_str;
        }(),
        .response_header_ptr = nullptr,
        .expect_response_code = 200,
        .ssl_verify = false,
    });
    if (ret) {
        co_await boost::asio::post(boost::asio::bind_executor(ch->get_executor(), boost::asio::use_awaitable));
        ch->try_send(err, ret.value());
        co_return;
    }
    co_return;
}

boost::asio::awaitable<void> FreeGpt::noowai(std::shared_ptr<Channel> ch, nlohmann::json json) {
    co_await boost::asio::post(boost::asio::bind_executor(*m_thread_pool_ptr, boost::asio::use_awaitable));
    ScopeExit _exit{[=] { boost::asio::post(ch->get_executor(), [=] { ch->close(); }); }};
    boost::system::error_code err{};

    auto prompt = json.at("meta").at("content").at("parts").at(0).at("content").get<std::string>();

    struct Input {
        std::shared_ptr<Channel> ch;
        std::string recv;
    };
    Input input;

    CURLcode res;
    CURL* curl = curl_easy_init();
    if (!curl) {
        auto error_info = std::format("curl_easy_init() failed:{}", curl_easy_strerror(res));
        co_await boost::asio::post(boost::asio::bind_executor(ch->get_executor(), boost::asio::use_awaitable));
        ch->try_send(err, error_info);
        co_return;
    }
    ScopeExit auto_exit{[=] { curl_easy_cleanup(curl); }};

    auto ret = sendHttpRequest(CurlHttpRequest{
        .curl = curl,
        .url = "https://noowai.com/wp-json/mwai-ui/v1/chats/submit",
        .http_proxy = m_cfg.http_proxy,
        .cb = [](void* contents, size_t size, size_t nmemb, void* userp) mutable -> size_t {
            boost::system::error_code err{};
            auto input_ptr = static_cast<Input*>(userp);
            std::string data{(char*)contents, size * nmemb};
            auto& [ch, recv] = *input_ptr;
            recv.append(data);
            while (true) {
                auto position = recv.find("\n");
                if (position == std::string::npos)
                    break;
                auto msg = recv.substr(0, position + 1);
                recv.erase(0, position + 1);
                msg.pop_back();
                if (msg.empty())
                    continue;
                auto fields = splitString(msg, "data: ");
                nlohmann::json line_json = nlohmann::json::parse(fields.back(), nullptr, false);
                if (line_json.is_discarded()) {
                    SPDLOG_ERROR("json parse error: [{}]", fields.back());
                    boost::asio::post(ch->get_executor(), [=] {
                        ch->try_send(err, std::format("json parse error: [{}]", fields.back()));
                    });
                    continue;
                }
                auto type = line_json["type"].get<std::string>();
                if (type == "live") {
                    auto str = line_json["data"].get<std::string>();
                    boost::asio::post(ch->get_executor(), [=] { ch->try_send(err, str); });
                }
            }
            return size * nmemb;
        },
        .input = [&] -> void* {
            input.recv.clear();
            input.ch = ch;
            return &input;
        }(),
        .headers = [&] -> auto& {
            static std::unordered_map<std::string, std::string> headers{
                {"Accept", "*/*"},
                {"origin", "https://noowai.com"},
                {"referer", "https://noowai.com/"},
                {"Content-Type", "application/json"},
                {"Alt-Used", "noowai.com"},
            };
            return headers;
        }(),
        .body = [&] -> std::string {
            constexpr std::string_view ask_json_str = R"({
                "botId":"default",
                "customId":"d49bc3670c3d858458576d75c8ea0f5d",
                "session":"N/A",
                "chatId":"v82az2ltn2",
                "contextId":25,
                "messages":[
                    {
                        "role":"user",
                        "content":"hello"
                    }
                ],
                "newMessage":"hello",
                "stream":true
            })";
            nlohmann::json ask_request = nlohmann::json::parse(ask_json_str, nullptr, false);
            ask_request["messages"] = getConversationJson(json);
            ask_request["newMessage"] = prompt;
            ask_request["customId"] = createUuidString();
            ask_request["chatId"] = [](int len) -> std::string {
                static std::string chars{"abcdefghijklmnopqrstuvwxyz0123456789"};
                static std::string letter{"abcdefghijklmnopqrstuvwxyz"};
                std::random_device rd;
                std::mt19937 gen(rd());
                std::uniform_int_distribution<> dis(0, 1000000);
                std::string random_string;
                random_string += chars[dis(gen) % letter.length()];
                len = len - 1;
                for (int i = 0; i < len; i++)
                    random_string += chars[dis(gen) % chars.length()];
                return random_string;
            }(10);
            std::string ask_request_str = ask_request.dump();
            SPDLOG_INFO("ask_request_str: [{}]", ask_request_str);
            return ask_request_str;
        }(),
        .response_header_ptr = nullptr,
        .expect_response_code = 200,
        .ssl_verify = false,
    });
    if (ret) {
        co_await boost::asio::post(boost::asio::bind_executor(ch->get_executor(), boost::asio::use_awaitable));
        ch->try_send(err, ret.value());
        co_return;
    }
    co_return;
}

boost::asio::awaitable<void> FreeGpt::geekGpt(std::shared_ptr<Channel> ch, nlohmann::json json) {
    co_await boost::asio::post(boost::asio::bind_executor(*m_thread_pool_ptr, boost::asio::use_awaitable));
    ScopeExit _exit{[=] { boost::asio::post(ch->get_executor(), [=] { ch->close(); }); }};
    boost::system::error_code err{};

    auto prompt = json.at("meta").at("content").at("parts").at(0).at("content").get<std::string>();

    struct Input {
        std::shared_ptr<Channel> ch;
        std::string recv;
    };
    Input input;

    CURLcode res;
    CURL* curl = curl_easy_init();
    if (!curl) {
        auto error_info = std::format("curl_easy_init() failed:{}", curl_easy_strerror(res));
        co_await boost::asio::post(boost::asio::bind_executor(ch->get_executor(), boost::asio::use_awaitable));
        ch->try_send(err, error_info);
        co_return;
    }
    ScopeExit auto_exit{[=] { curl_easy_cleanup(curl); }};

    auto ret = sendHttpRequest(CurlHttpRequest{
        .curl = curl,
        .url = "https://ai.fakeopen.com/v1/chat/completions",
        .http_proxy = m_cfg.http_proxy,
        .cb = [](void* contents, size_t size, size_t nmemb, void* userp) mutable -> size_t {
            auto input_ptr = static_cast<Input*>(userp);
            std::string data{(char*)contents, size * nmemb};
            auto& [ch, recv] = *input_ptr;
            recv.append(data);
            while (true) {
                auto position = recv.find("\n");
                if (position == std::string::npos)
                    break;
                auto msg = recv.substr(0, position + 1);
                recv.erase(0, position + 1);
                msg.pop_back();
                if (msg.empty() || !msg.contains("content"))
                    continue;
                auto fields = splitString(msg, "data: ");
                boost::system::error_code err{};
                nlohmann::json line_json = nlohmann::json::parse(fields.back(), nullptr, false);
                if (line_json.is_discarded()) {
                    SPDLOG_ERROR("json parse error: [{}]", fields.back());
                    boost::asio::post(ch->get_executor(), [=] {
                        ch->try_send(err, std::format("json parse error: [{}]", fields.back()));
                    });
                    continue;
                }
                auto str = line_json["choices"][0]["delta"]["content"].get<std::string>();
                if (!str.empty() && str != "[DONE]")
                    boost::asio::post(ch->get_executor(), [=] { ch->try_send(err, str); });
            }
            return size * nmemb;
        },
        .input = [&] -> void* {
            input.recv.clear();
            input.ch = ch;
            return &input;
        }(),
        .headers = [&] -> auto& {
            static std::unordered_map<std::string, std::string> headers{
                {"Accept", "*/*"},
                {"origin", "https://chat.geekgpt.org"},
                {"referer", "https://chat.geekgpt.org/"},
                {"Content-Type", "application/json"},
                {"authority", "ai.fakeopen.com"},
                {"authorization", "Bearer pk-this-is-a-real-free-pool-token-for-everyone"},
            };
            return headers;
        }(),
        .body = [&] -> std::string {
            constexpr std::string_view ask_json_str = R"({
                "messages": [{
                    "role": "user",
                    "content": "hello"
                }],
                "model": "gpt-3.5-turbo",
                "temperature": 0.9,
                "presence_penalty": 0,
                "top_p": 1,
                "frequency_penalty": 0,
                "stream": true
            })";
            nlohmann::json ask_request = nlohmann::json::parse(ask_json_str, nullptr, false);
            ask_request["messages"] = getConversationJson(json);
            std::string ask_request_str = ask_request.dump();
            SPDLOG_INFO("ask_request_str: [{}]", ask_request_str);
            return ask_request_str;
        }(),
        .response_header_ptr = nullptr,
        .expect_response_code = 200,
        .ssl_verify = false,
    });
    if (ret) {
        co_await boost::asio::post(boost::asio::bind_executor(ch->get_executor(), boost::asio::use_awaitable));
        ch->try_send(err, ret.value());
        co_return;
    }
    co_return;
}
