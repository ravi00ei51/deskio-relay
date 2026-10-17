#include <utility>
#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <nlohmann/json.hpp>
#include "database.hpp"
#include <sodium.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <regex>
#include <stdexcept>
#include <string>

namespace net = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
using tcp = net::ip::tcp;
using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;
using UserId = std::int64_t;

struct InvalidInput : std::runtime_error {
    using std::runtime_error::runtime_error;
};

std::string field(const Json& object, const char* key,
                  std::size_t minimum, std::size_t maximum)
{
    if (!object.contains(key) || !object.at(key).is_string())
        throw InvalidInput(std::string("Expected string field: ") + key);
    auto value = object.at(key).get<std::string>();
    if (value.size() < minimum || value.size() > maximum ||
        value.find('\0') != std::string::npos)
        throw InvalidInput(std::string("Invalid length/value for: ") + key);
    return value;
}

void validate_dob(const std::string& dob)
{
    if (!std::regex_match(dob, std::regex(R"(\d{4}-\d{2}-\d{2})")))
        throw InvalidInput("dob must use YYYY-MM-DD");
    const std::chrono::year_month_day date{
        std::chrono::year{std::stoi(dob.substr(0, 4))},
        std::chrono::month{static_cast<unsigned>(std::stoi(dob.substr(5, 2)))},
        std::chrono::day{static_cast<unsigned>(std::stoi(dob.substr(8, 2)))}};
    if (!date.ok() || date.year() < std::chrono::year{1900} ||
        std::chrono::sys_days{date} >
            std::chrono::floor<std::chrono::days>(std::chrono::system_clock::now()))
        throw InvalidInput("dob must be a valid date from 1900 through today");
}

std::shared_ptr<Credentials> parse_credentials(const std::string& text)
{
    Json input = Json::parse(text);
    if (!input.is_object()) throw InvalidInput("Expected a JSON object");
    auto c = std::make_shared<Credentials>();
    const bool has_profile = input.contains("name") || input.contains("dob") ||
                             input.contains("address");
    std::string action = has_profile ? "register" : "login";
    if (input.contains("type")) action = field(input, "type", 1, 20);
    if (action != "register" && action != "login")
        throw InvalidInput("type must be register or login");
    c->registration = action == "register";
    c->username = field(input, "username", 3, 64);
    if (!std::regex_match(c->username, std::regex(R"([A-Za-z0-9_.-]{3,64})")))
        throw InvalidInput("username: use 3-64 ASCII letters, digits, _, . or -");
    // Canonical usernames: Ravi and ravi identify the same account.
    for (char& ch : c->username)
        if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
    c->password = field(input, "password", c->registration ? 12 : 1, 1024);
    if (c->registration) {
        c->name = field(input, "name", 1, 200);
        c->dob = field(input, "dob", 10, 10);
        c->address = field(input, "address", 1, 2000);
        validate_dob(c->dob);
    } else if (has_profile) {
        throw InvalidInput("Login accepts username and password only");
    }
    return c;
}

class AuthService {
    std::string url_;
    std::string dummy_hash_;
public:
    explicit AuthService(std::string url) : url_(std::move(url)) {
        // DATABASE_URL is a PostgreSQL URI, as supplied by Neon.
        if (url_.find("connect_timeout=") == std::string::npos)
            url_ += (url_.find('?') == std::string::npos ? "?" : "&") +
                    std::string("connect_timeout=10");
        char hash[crypto_pwhash_STRBYTES];
        if (crypto_pwhash_str(hash, "dummy-password-value", 20,
                crypto_pwhash_OPSLIMIT_INTERACTIVE,
                crypto_pwhash_MEMLIMIT_INTERACTIVE) != 0)
            throw std::runtime_error("Password hashing initialization failed");
        dummy_hash_ = hash;
    }

    AuthResult authenticate(const Credentials& c) const {
        return database_authenticate(c, url_, dummy_hash_);
    }
};

class Session;
struct Relay {
    std::map<UserId, std::array<std::weak_ptr<Session>, 2>> users;
    int join(UserId id, const std::shared_ptr<Session>& session) {
        auto& slots = users[id];
        for (int i = 0; i < 2; ++i)
            if (slots[i].expired()) { slots[i] = session; return i; }
        return -1;
    }
    void leave(UserId id, int slot) {
        auto found = users.find(id);
        if (found == users.end() || slot < 0) return;
        found->second[slot].reset();
        if (found->second[0].expired() && found->second[1].expired())
            users.erase(found);
    }
    std::shared_ptr<Session> peer(UserId id, int slot) {
        auto found = users.find(id);
        if (found == users.end() || slot < 0) return {};
        return found->second[1 - slot].lock();
    }
};

struct State {
    AuthService auth;
    Relay relay;
    net::thread_pool worker{1};  // One hash/DB operation at a time: bounded memory.
    unsigned pending_auth = 0;
    unsigned minute_attempts = 0;
    Clock::time_point minute_start = Clock::now();
    explicit State(std::string url) : auth(std::move(url)) {}
    bool admit_auth() {
        if (Clock::now() - minute_start >= std::chrono::minutes{1}) {
            minute_start = Clock::now(); minute_attempts = 0;
        }
        if (pending_auth >= 8 || minute_attempts >= 60) return false;
        ++pending_auth; ++minute_attempts;
        return true;
    }
};

struct Message { bool text; std::string data; };
class Session : public std::enable_shared_from_this<Session> {
    websocket::stream<beast::tcp_stream> ws_;
    State& state_;
    net::steady_timer auth_deadline_;
    beast::flat_buffer buffer_;
    http::request<http::string_body> request_;
    std::deque<std::shared_ptr<Message>> outgoing_;
    std::size_t queued_bytes_ = 0;
    UserId user_id_ = 0;
    int slot_ = -1;
    unsigned failures_ = 0;
    bool stopped_ = false;
    Clock::time_point next_attempt_{};
public:
    Session(tcp::socket socket, State& state)
        : ws_(std::move(socket)), state_(state),
          auth_deadline_(ws_.get_executor()) {}
    void start() {
        beast::get_lowest_layer(ws_).expires_after(std::chrono::seconds{15});
        http::async_read(ws_.next_layer(), buffer_, request_,
            [self = shared_from_this()](beast::error_code ec, std::size_t) {
                if (ec) return self->stop();
                self->handle_request();
            });
    }
private:
    void stop() {
        if (stopped_) return;
        stopped_ = true;
        state_.relay.leave(user_id_, slot_);
        auth_deadline_.cancel();
        beast::error_code ignored;
        auto& socket = beast::get_lowest_layer(ws_).socket();
        socket.shutdown(tcp::socket::shutdown_both, ignored);
        socket.close(ignored);
    }
    void reply(http::status status, std::string body) {
        auto response = std::make_shared<http::response<http::string_body>>(
            status, request_.version());
        response->set(http::field::content_type, "text/plain");
        response->keep_alive(false);
        response->body() = std::move(body);
        response->prepare_payload();
        http::async_write(ws_.next_layer(), *response,
            [self = shared_from_this(), response](beast::error_code, std::size_t) {
                self->stop();
            });
    }
    void handle_request() {
        if (request_.method() == http::verb::get && request_.target() == "/health")
            return reply(http::status::ok, "OK");
        if (request_.target() != "/ws" || !websocket::is_upgrade(request_))
            return reply(http::status::not_found, "Connect to /ws");
        // Existing protocol: clients wait for upgrade before sending data.
        if (buffer_.size() != 0)
            return reply(http::status::bad_request, "Unexpected upgrade payload");
        beast::get_lowest_layer(ws_).expires_never();
        ws_.set_option(websocket::stream_base::timeout::suggested(
            beast::role_type::server));
        ws_.read_message_max(8192); // Only small authentication JSON before login.
        ws_.async_accept(request_, [self = shared_from_this()](beast::error_code ec) {
            if (ec) return self->stop();
            self->auth_deadline_.expires_after(std::chrono::seconds{60});
            self->auth_deadline_.async_wait([self](beast::error_code error) {
                if (!error && self->user_id_ == 0) self->stop();
            });
            self->read();
        });
    }
    void read() {
        if (stopped_) return;
        ws_.async_read(buffer_,
            [self = shared_from_this()](beast::error_code ec, std::size_t) {
                if (ec) return self->stop();
                auto text = beast::buffers_to_string(self->buffer_.data());
                const bool is_text = self->ws_.got_text();
                self->buffer_.consume(self->buffer_.size());
                if (self->user_id_ == 0) {
                    if (!is_text) {
                        self->auth_error("invalid_request", "Send text JSON to authenticate");
                        return self->read();
                    }
                    self->authenticate(std::move(text));
                    return;
                }
                // Authenticated relay: never broadcast across accounts.
                if (auto peer = self->state_.relay.peer(self->user_id_, self->slot_))
                    peer->send(std::make_shared<Message>(Message{is_text, std::move(text)}));
                self->read();
            });
    }
    void auth_error(const std::string& code, const std::string& message) {
        send_json({{"type", "auth.error"}, {"code", code}, {"message", message}});
    }
    void authenticate(std::string text) {
        // No further reads are started until this authentication finishes.
        if (Clock::now() < next_attempt_) {
            sodium_memzero(text.data(), text.size());
            auth_error("rate_limited", "Wait before retrying");
            return read();
        }
        if (++failures_ > 5) {
            sodium_memzero(text.data(), text.size());
            return stop();
        }
        next_attempt_ = Clock::now() + std::chrono::seconds{2};
        std::shared_ptr<Credentials> credentials;
        try {
            credentials = parse_credentials(text);
        } catch (const InvalidInput& error) {
            sodium_memzero(text.data(), text.size());
            auth_error("invalid_request", error.what());
            return read();
        } catch (const std::exception&) {
            sodium_memzero(text.data(), text.size());
            auth_error("invalid_request", "Expected valid registration/login JSON");
            return read();
        }
        sodium_memzero(text.data(), text.size());
        if (!state_.admit_auth()) {
            auth_error("server_busy", "Try again later");
            return read();
        }
        net::post(state_.worker, [self = shared_from_this(), credentials] {
            auto result = self->state_.auth.authenticate(*credentials);
            net::post(self->ws_.get_executor(),
                [self, result = std::move(result)]() mutable {
                    --self->state_.pending_auth;
                    if (self->stopped_) return;
                    if (result.user_id != 0) {
                        self->user_id_ = result.user_id;
                        self->auth_deadline_.cancel();
                        self->ws_.read_message_max(8 * 1024 * 1024);
                        self->slot_ = self->state_.relay.join(result.user_id, self);
                        result.response["relay_available"] = self->slot_ >= 0;
                        if (self->slot_ < 0)
                            result.response["message"] = "Account already has two relay connections";
                    }
                    self->send_json(result.response);
                    self->read();
                });
        });
    }
    void send_json(const Json& object) {
        send(std::make_shared<Message>(Message{true, object.dump()}));
    }
    void send(std::shared_ptr<Message> message) {
        if (stopped_) return;
        if (outgoing_.size() >= 128 ||
            queued_bytes_ + message->data.size() > 16 * 1024 * 1024)
            return stop();
        const bool busy = !outgoing_.empty();
        queued_bytes_ += message->data.size();
        outgoing_.push_back(std::move(message));
        if (!busy) write();
    }
    void write() {
        auto message = outgoing_.front();
        ws_.text(message->text);
        ws_.async_write(net::buffer(message->data),
            [self = shared_from_this(), message](beast::error_code ec, std::size_t) {
                if (ec) return self->stop();
                self->queued_bytes_ -= message->data.size();
                self->outgoing_.pop_front();
                if (!self->stopped_ && !self->outgoing_.empty()) self->write();
            });
    }
};

int main()
{
    try {
        if (sodium_init() < 0) throw std::runtime_error("libsodium initialization failed");
        const char* database_url = std::getenv("DATABASE_URL");
        if (!database_url || !*database_url)
            throw std::runtime_error("DATABASE_URL must be set");
        const auto port = std::getenv("PORT") ? std::stoul(std::getenv("PORT")) : 10000UL;
        if (port == 0 || port > 65535) throw std::runtime_error("Invalid PORT");
        // State outlives io_context, including pending worker completions.
        State state{database_url};
        net::io_context context;
        tcp::acceptor acceptor{context, tcp::endpoint{
            net::ip::make_address("0.0.0.0"), static_cast<unsigned short>(port)}};
        std::function<void()> accept_next;
        accept_next = [&] {
            acceptor.async_accept([&](beast::error_code ec, tcp::socket socket) {
                if (!ec) std::make_shared<Session>(std::move(socket), state)->start();
                if (acceptor.is_open()) accept_next();
            });
        };
        accept_next();
        std::cout << "Deskio authenticated relay listening on port " << port << std::endl;
        // Keep a single IO thread. Relay state and counters are IO-thread-only.
        context.run();
        state.worker.join();
    } catch (const std::exception&) {
        // A malformed DB URL can contain credentials; do not print exception text.
        std::cerr << "Startup failed: check PORT, DATABASE_URL and dependencies\n";
        return EXIT_FAILURE;
    }
}
