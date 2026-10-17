#include <utility>
#include <boost/asio.hpp>
#include <boost/beast.hpp>

#include <array>
#include <cstdlib>
#include <deque>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

namespace net = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;

using tcp = net::ip::tcp;

class Session;

// Exactly two connected computers.
struct Relay {
    std::array<std::weak_ptr<Session>, 2> computers;

    int join(const std::shared_ptr<Session>& session)
    {
        for (int i = 0; i < 2; ++i) {
            if (computers[i].expired()) {
                computers[i] = session;
                return i;
            }
        }

        return -1;
    }

    void leave(int slot)
    {
        if (slot >= 0) {
            computers[slot].reset();
        }
    }
};

struct Message {
    bool text;
    std::string data;
};

class Session : public std::enable_shared_from_this<Session> {
public:
    Session(tcp::socket socket, Relay& relay)
        : ws_(std::move(socket)), relay_(relay)
    {
    }

    void start()
    {
        // Initial HTTP request, including WebSocket upgrade.
        beast::get_lowest_layer(ws_).expires_after(
            std::chrono::seconds(30)
        );

        http::async_read(
            ws_.next_layer(),
            buffer_,
            request_,
            [self = shared_from_this()](
                beast::error_code error, std::size_t
            ) {
                if (error) {
                    self->stop();
                    return;
                }

                self->handle_request();
            }
        );
    }

private:
    websocket::stream<beast::tcp_stream> ws_;
    Relay& relay_;

    beast::flat_buffer buffer_;
    http::request<http::string_body> request_;

    std::deque<std::shared_ptr<Message>> outgoing_;
    std::size_t queued_bytes_ = 0;

    int slot_ = -1;
    bool stopped_ = false;

    void reply(http::status status, std::string body)
    {
        auto response =
            std::make_shared<http::response<http::string_body>>(
                status, request_.version()
            );

        response->set(http::field::content_type, "text/plain");
        response->keep_alive(false);
        response->body() = std::move(body);
        response->prepare_payload();

        http::async_write(
            ws_.next_layer(),
            *response,
            [self = shared_from_this(), response](
                beast::error_code, std::size_t
            ) {
                self->stop();
            }
        );
    }

    void handle_request()
    {
        if (request_.method() == http::verb::get &&
            request_.target() == "/health") {
            reply(http::status::ok, "OK");
            return;
        }

        if (request_.target() != "/ws" ||
            !websocket::is_upgrade(request_)) {
            reply(http::status::not_found, "Connect to /ws");
            return;
        }

        slot_ = relay_.join(shared_from_this());

        if (slot_ < 0) {
            reply(
                http::status::service_unavailable,
                "Two computers are already connected"
            );
            return;
        }

        beast::get_lowest_layer(ws_).expires_never();

        ws_.set_option(
            websocket::stream_base::timeout::suggested(
                beast::role_type::server
            )
        );

        ws_.read_message_max(8 * 1024 * 1024);

        ws_.async_accept(
            request_,
            [self = shared_from_this()](beast::error_code error) {
                if (error) {
                    self->stop();
                    return;
                }

                std::cout
                    << "Computer connected in slot "
                    << self->slot_ + 1
                    << std::endl;

                self->read();
            }
        );
    }

    void read()
    {
        ws_.async_read(
            buffer_,
            [self = shared_from_this()](
                beast::error_code error, std::size_t
            ) {
                if (error) {
                    self->stop();
                    return;
                }

                auto message = std::make_shared<Message>(
                    Message{
                        self->ws_.got_text(),
                        beast::buffers_to_string(self->buffer_.data())
                    }
                );

                self->buffer_.consume(self->buffer_.size());

                // Send exclusively to the other computer.
                auto peer = self->relay_
                    .computers[1 - self->slot_].lock();

                if (peer && peer->ws_.is_open()) {
                    peer->send(std::move(message));
                }

                if (!self->stopped_) {
                    self->read();
                }
            }
        );
    }

    void send(std::shared_ptr<Message> message)
    {
        if (stopped_) {
            return;
        }

        // Disconnect a slow receiver rather than grow indefinitely.
        constexpr std::size_t max_bytes = 16 * 1024 * 1024;
        constexpr std::size_t max_messages = 128;

        if (outgoing_.size() >= max_messages ||
            queued_bytes_ + message->data.size() > max_bytes) {
            std::cerr << "Slow receiver disconnected" << std::endl;
            stop();
            return;
        }

        const bool already_writing = !outgoing_.empty();

        queued_bytes_ += message->data.size();
        outgoing_.push_back(std::move(message));

        if (!already_writing) {
            write();
        }
    }

    void write()
    {
        auto message = outgoing_.front();

        ws_.text(message->text);

        ws_.async_write(
            net::buffer(message->data),
            [self = shared_from_this(), message](
                beast::error_code error, std::size_t
            ) {
                if (error) {
                    self->stop();
                    return;
                }

                self->queued_bytes_ -= message->data.size();
                self->outgoing_.pop_front();

                if (!self->stopped_ && !self->outgoing_.empty()) {
                    self->write();
                }
            }
        );
    }

    void stop()
    {
        if (stopped_) {
            return;
        }

        stopped_ = true;

        if (slot_ >= 0) {
            std::cout
                << "Computer disconnected from slot "
                << slot_ + 1
                << std::endl;

            relay_.leave(slot_);
        }

        beast::error_code ignored;

        auto& socket = beast::get_lowest_layer(ws_).socket();

        socket.shutdown(tcp::socket::shutdown_both, ignored);
        socket.close(ignored);
    }
};

int main()
{
    try {
        unsigned long port = 10000;

        if (const char* value = std::getenv("PORT")) {
            port = std::stoul(value);
        }

        if (port == 0 || port > 65535) {
            throw std::runtime_error("Invalid PORT");
        }

        // All callbacks execute on this single event-loop thread.
        net::io_context context;
        Relay relay;

        tcp::acceptor acceptor{
            context,
            tcp::endpoint{
                net::ip::make_address("0.0.0.0"),
                static_cast<unsigned short>(port)
            }
        };

        std::function<void()> accept_next;

        accept_next = [&] {
            acceptor.async_accept(
                [&](beast::error_code error, tcp::socket socket) {
                    if (!error) {
                        std::make_shared<Session>(
                            std::move(socket), relay
                        )->start();
                    }
                    else {
                        std::cerr
                            << "Accept error: "
                            << error.message()
                            << std::endl;
                    }

                    accept_next();
                }
            );
        };

        accept_next();

        std::cout
            << "Relay listening on 0.0.0.0:"
            << port
            << std::endl;

        context.run();
    }
    catch (const std::exception& error) {
        std::cerr << error.what() << std::endl;
        return EXIT_FAILURE;
    }
}
