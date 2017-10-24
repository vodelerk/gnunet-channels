#include <iostream>
#include <boost/asio/signal_set.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/asio/streambuf.hpp>
#include <boost/asio/posix/stream_descriptor.hpp>
#include <boost/asio/read_until.hpp>

#include <gnunet_channels/service.h>
#include <gnunet_channels/channel.h>
#include <gnunet_channels/cadet_port.h>

using namespace std;
using namespace gnunet_channels;

struct Chat {
    unique_ptr<Channel> channel;
    unique_ptr<CadetPort> port;
};

static void run_chat(Chat& c, asio::yield_context yield) {
    auto& ios = c.channel->get_io_service();

    // Start printing received messages
    asio::spawn(ios, [&c] (asio::yield_context yield) {
            sys::error_code ec;

            while (true) {
                string data = c.channel->receive(yield[ec]);
                if (ec) return;
                cout << "Received: " << data << endl;
            }
        });

    // Read from input and send it to peer
    asio::posix::stream_descriptor input(ios, ::dup(STDIN_FILENO));

    string out;
    asio::streambuf buffer(512);

    while (true) {
        sys::error_code ec;
        size_t n = asio::async_read_until(input, buffer, '\n', yield[ec]);
        if (ec || !c.channel) break;
        out.resize(n - 1);
        buffer.sgetn((char*) out.c_str(), n - 1);
        buffer.consume(1); // new line
        c.channel->send(out);
    }
}

static void connect_and_run_chat( Chat& chat
                                , Service& service
                                , string target_id
                                , string port
                                , asio::yield_context yield)
{
    sys::error_code ec;
    chat.channel = make_unique<Channel>(service);

    cout << "Connecting to " << target_id << endl;
    chat.channel->connect(target_id, port, yield[ec]);

    if (ec) {
        cerr << "Failed to connect: " << ec.message() << endl;
        return;
    }

    cout << "Connected" << endl;

    run_chat(chat, yield);
}

static void accept_and_run_chat( Chat& chat
                               , Service& service
                               , string port
                               , asio::yield_context yield)
{
    sys::error_code ec;

    cout << "Accepting on port \"" << port << "\"" << endl;

    chat.channel = make_unique<Channel>(service);
    chat.port    = make_unique<CadetPort>(service);

    chat.port->open(*chat.channel, port, yield[ec]);

    if (ec) {
        cerr << "Failed to accept: " << ec.message() << endl;
        return;
    }

    cout << "Accepted" << endl;

    run_chat(chat, yield);
}

static void print_usage(const char* app_name)
{
    cerr << "Usage:\n";
    cerr << "    " << app_name << " <config-file> <secret-phrase> [peer-id]\n";
    cerr << "If [peer-id] is used the app acts as a client, "
            "otherwise it acts as a server\n";
}

int main(int argc, char* const* argv)
{
    if (argc != 3 && argc != 4) {
        print_usage(argv[0]);
        return 1;
    }

    asio::io_service ios;

    Service service(argv[1], ios);

    string target_id;
    string port = argv[2];

    if (argc >= 4) {
        target_id = argv[3];
    }

    // Capture these signals so that we can disconnect gracefully.
    asio::signal_set signals(ios, SIGINT, SIGTERM);

    Chat chat;

    signals.async_wait([&](sys::error_code, int /* signal_number */) {
            chat.channel.reset();
            chat.port.reset();
        });

    asio::spawn(ios, [&] (auto yield) {
            sys::error_code ec;

            service.async_setup(yield[ec]);

            if (ec) {
                cerr << "Failed to set up gnunet service: " << ec.message() << endl;
                return;
            }

            if (!target_id.empty()) {
                connect_and_run_chat(chat, service, target_id, port, yield);
            }
            else {
                accept_and_run_chat(chat, service, port, yield);
            }
        });

    ios.run();
}
