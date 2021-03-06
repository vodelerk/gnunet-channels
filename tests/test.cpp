#define BOOST_TEST_MODULE GNUnet_Channels_Tests
#include <boost/test/unit_test.hpp>

#include <iostream>
#include <chrono>
#include <thread>

#include <gnunet_channels/channel.h>
#include <gnunet_channels/cadet_port.h>
#include <gnunet_channels/service.h>
#include <gnunet_channels/namespaces.h>

#include <boost/asio.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/asio/steady_timer.hpp>

#include <sys/wait.h>

using namespace std;
using namespace gnunet_channels;
using namespace chrono_literals;

static const string config1 = "../scripts/peer1.conf";
static const string config2 = "../scripts/peer2.conf";

//--------------------------------------------------------------------
// Unfortunately GNUnet won't let us run more than one node per
// one process, so we need to fork one new process per node and
// run test code from there.
class Fork {
private:
    using Func = function<void(Service& service, asio::yield_context yield)>;

public:
    Fork(string name, string config, Func func)
        : name(name)
    {
        pid = fork();
        BOOST_CHECK(pid >= 0);
        if (!is_child()) {
            return;
        }

        {
            asio::io_service ios;
            Service service(config, ios);

            asio::spawn(ios, [&] (auto yield) {
                    asio::io_service::work w(ios);
                    sys::error_code ec;
                    service.async_setup(yield[ec]);

                    if (ec) {
                        cerr << "Failed to set up gnunet service: "
                             << ec.message()
                             << " (process " << name << ")" << endl;
                        return;
                    }

                    try {
                        func(service, yield);
                    }
                    catch (const exception& e) {
                        BOOST_ERROR("Exception was thrown");
                        _exit(1);
                    }
                });

            ios.run();
        }
        _exit(0);
    }

    ~Fork() {
        if (is_child()) return;
        int status;
        pid_t w = waitpid(pid, &status, 0);

        if (w == -1) {
            // Child may have already exited, in which case we don't care
            if (errno != ECHILD) {
                perror("waitpid");
                BOOST_REQUIRE(w != -1);
            }
        }
        else {
            if (WIFEXITED(status)) {
                auto exit_code = WEXITSTATUS(status);
                BOOST_REQUIRE(exit_code == 0);
            } else if (WIFSIGNALED(status)) {
                cerr << "killed by signal " << WTERMSIG(status) << endl;
            } else if (WIFSTOPPED(status)) {
                cerr << "stopped by signal " << WSTOPSIG(status) << endl;
            } else if (WIFCONTINUED(status)) {
                cerr << "continued" << endl;
            }
        }
    }

    bool is_child() const { return pid == 0; }

private:
    string name;
    pid_t pid;
};

//--------------------------------------------------------------------
// If an instance of this class is not destroyed within a timeout
// then the test fails.
struct FailTimeout {
    using Duration = asio::steady_timer::duration;

    FailTimeout(Duration d, string task_name)
        : _task_name(move(task_name))
    {
        _keep_going.test_and_set();

        _thread = thread([this, d] {
                Duration delta = 200ms;
                Duration total = 0s;

                while (_keep_going.test_and_set()) {
                    this_thread::sleep_for(delta);
                    total += delta;

                    if (total > d) {
                        BOOST_ERROR("Task \""
                                   + _task_name
                                   + "\" takes too long");
                        _exit(1);
                    }
                }
            });
    }

    ~FailTimeout()
    {
        _keep_going.clear();
        _thread.join();
    }

    thread _thread;
    atomic_flag _keep_going;
    string _task_name;
};

//--------------------------------------------------------------------
static string get_id(string config)
{
    FailTimeout ft(3s, "get_id");

    asio::io_service ios;
    Service service(config, ios);

    string result_id;

    asio::spawn(ios, [&] (auto yield) {
            service.async_setup(yield);
            result_id = service.identity();
        });

    ios.run();

    return result_id;
}

//--------------------------------------------------------------------
static string random_port()
{
    srand(time(0));
    stringstream ss;
    ss << "port_" << rand();
    return ss.str();
}

//--------------------------------------------------------------------
// Actual tests
//--------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(test_get_id_config1)
{
    string server_id = get_id(config1);
    BOOST_REQUIRE(!server_id.empty());
}

//--------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(test_get_id_config2)
{
    string server_id = get_id(config2);
    BOOST_REQUIRE(!server_id.empty());
}

//--------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(test_connect)
{
    const string port = random_port();

    string server_id = get_id(config1);

    Fork n1("server", config1, [&](Service& service, auto yield) {
            FailTimeout ft(3s, "server");

            sys::error_code ec;
            Channel channel(service);
            CadetPort p(service);
            p.open(channel, port, yield);
        });

    Fork n2("client", config2, [&](Service& service, auto yield) {
            FailTimeout ft(4s, "client");

            sys::error_code ec;
            asio::steady_timer t(service.get_io_service());
            t.expires_from_now(1s);
            t.async_wait(yield[ec]);
            Channel channel(service);
            channel.connect(server_id, port, yield);
        });

    // TODO: Why is this needed?
    int status; wait(&status);
}

//--------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(test_connect_and_close)
{
    const string port = random_port();

    string server_id = get_id(config1);

    Fork n1("server", config1, [&](Service& service, auto yield) {
            FailTimeout ft(3s, "server");

            sys::error_code ec;
            Channel channel(service);
            CadetPort p(service);
            p.open(channel, port, yield);
        });

    Fork n2("client", config2, [&](Service& service, auto yield) {
            FailTimeout ft(4s, "client");

            sys::error_code ec;
            asio::steady_timer t(service.get_io_service());
            t.expires_from_now(1s);
            t.async_wait(yield[ec]);
            Channel channel(service);
            channel.connect(server_id, port, yield);

            uint8_t byte_buf = 0;

            asio::async_read(channel, asio::buffer(&byte_buf, 1), yield[ec]);

            BOOST_CHECK(ec == asio::error::connection_reset);
        });

    // TODO: Why is this needed?
    int status; wait(&status);
}

//--------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(test_two_connects)
{
    const string port = random_port();

    string server_id = get_id(config1);

    Fork n1("server", config1, [&](Service& service, auto yield) {
            FailTimeout ft(4s, "server");

            sys::error_code ec;
            CadetPort p(service);

            Channel channel1(service);
            Channel channel2(service);

            p.open(channel1, port, yield[ec]);
            BOOST_REQUIRE(ec == sys::error_code());

            p.open(channel2, port, yield[ec]);
            BOOST_REQUIRE(ec == sys::error_code());

            // Prevent the channels from being destroyed for a second.
            asio::steady_timer t(service.get_io_service());
            t.expires_from_now(1s);
            t.async_wait(yield[ec]);
        });

    Fork n2("client", config2, [&](Service& service, auto yield) {
            FailTimeout ft(4s, "client");

            sys::error_code ec;

            asio::steady_timer t(service.get_io_service());
            t.expires_from_now(1s);
            t.async_wait(yield[ec]);

            {
                Channel channel(service);
                channel.connect(server_id, port, yield[ec]);
                BOOST_REQUIRE(ec == sys::error_code());
            }

            {
                Channel channel(service);
                channel.connect(server_id, port, yield[ec]);
                BOOST_REQUIRE(ec == sys::error_code());
            }
        });

    // TODO: Why is this needed?
    int status; wait(&status);
}

//--------------------------------------------------------------------
