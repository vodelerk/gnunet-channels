#include <iostream>
#include "channel_impl.h"
#include "gnunet_channels/error.h"

using namespace std;
using namespace gnunet_channels;

// Makes sure the destructor is called in the main thread.
template<class T>
static void preserve(shared_ptr<T>&& c) {
    auto p = c.get();
    p->get_io_service().post([c = move(c)] {});
}

ChannelImpl::ChannelImpl(shared_ptr<Cadet> cadet)
    : _cadet(move(cadet))
    , _scheduler(_cadet->scheduler())
{
    assert(_cadet);
}

void ChannelImpl::send(vector<uint8_t> data, OnSend on_send)
{
    if (_on_send) {
        // We're already sending, so queue this request.
        _send_queue.push(SendEntry{data, move(on_send)});
        return;
    }

    do_send(data, move(on_send));
}

void ChannelImpl::do_send(vector<uint8_t> data, OnSend on_send)
{
    size_t size = data.size();
    _on_send = [h = move(on_send), size] (auto ec) { h(ec, size); };

    scheduler().post([ self = shared_from_this()
                     , data = move(data)
                     ] () mutable {
        if (!self->_handle) return;

        constexpr size_t max_size = GNUNET_CONSTANTS_MAX_CADET_MESSAGE_SIZE
                                  - sizeof(GNUNET_MessageHeader);

        asio::const_buffer buf(data.data(), data.size());

        while (auto size = min(max_size, asio::buffer_size(buf))) {
            GNUNET_MessageHeader *msg;
            GNUNET_MQ_Envelope *env
                = GNUNET_MQ_msg_extra( msg
                                     , size
                                     , GNUNET_MESSAGE_TYPE_CADET_CLI);

            GNUNET_memcpy(&msg[1], asio::buffer_cast<const void*>(buf), size);

            // Only get notification once the last buffer has been sent.
            if (asio::buffer_size(buf) <= max_size) {
                GNUNET_MQ_notify_sent(env, ChannelImpl::data_sent, self.get());
            }

            GNUNET_MQ_send(GNUNET_CADET_get_mq(self->_handle), env);

            buf = buf + size;
        }

        preserve(move(self));
    });
}

// Executed in GNUnet's thread
void ChannelImpl::data_sent(void *cls)
{
    auto self = static_cast<ChannelImpl*>(cls);

    self->get_io_service().post([s = self->shared_from_this()] {
            auto f = move(s->_on_send);

            if (!f) {
                // This can happen only if `close` was called.
                assert(!s->_cadet);
                return;
            }

            // NOTE: We need to do this before we execute the callback to match
            // the order of sent packets.
            if (!s->_send_queue.empty()) {
                auto e = move(s->_send_queue.front());
                s->_send_queue.pop();
                s->do_send(move(e.data), move(e.on_send));
            }

            f(sys::error_code());
        });
}

void ChannelImpl::receive(vector<asio::mutable_buffer> output, OnReceive h)
{
    if (_recv_queue.empty()) {
        _on_receive = move(h);
        _output = move(output);
    }
    else {
        auto& input = _recv_queue.front();

        size_t size = asio::buffer_copy(output, input.info);

        input.info = input.info + size;

        if (asio::buffer_size(input.info) == 0) {
            _recv_queue.pop();
        }

        get_io_service().post([ size
                              , self = shared_from_this()
                              , h    = move(h) ] {
                                  // TODO: Check whether `close` was called?
                                  h(sys::error_code(), size);
                              });
    }
}

// Executed in GNUnet's thread
void ChannelImpl::handle_data(void *cls, const GNUNET_MessageHeader *m)
{
    auto ch = static_cast<ChannelImpl*>(cls);

    size_t payload_size = ntohs(m->size) - sizeof(*m);
    uint8_t* begin = (uint8_t*) &m[1];
    std::vector<uint8_t> payload(begin, begin + payload_size);

    // TODO: Would be nice to only call this when the _recv_queue is empty.
    // But that requires some locking.
    GNUNET_CADET_receive_done(ch->_handle);

    ch->get_io_service().post([ s = ch->shared_from_this()
                              , d = move(payload) ] {
            // TODO: Check whether `close` was called?

            if (s->_on_receive) {
                size_t size = asio::buffer_copy(s->_output, asio::buffer(d));

                if (size < d.size()) {
                    s->_recv_queue.emplace(move(d), size);
                }

                auto f = move(s->_on_receive);
                f(sys::error_code(), size);
            }
            else {
                s->_recv_queue.emplace(move(d));
            }
        });
}

void ChannelImpl::connect( string target_id
                         , const string& port
                         , OnConnect h)
{
    GNUNET_HashCode port_hash;
    GNUNET_CRYPTO_hash(port.c_str(), port.size(), &port_hash);

    _on_connect = move(h);

    _scheduler.post([ cadet     = _cadet
                    , tid       = move(target_id)
                    , port_hash = port_hash
                    , self      = shared_from_this()
                    ] () mutable {
        GNUNET_PeerIdentity pid;
        auto& ios = cadet->scheduler().get_io_service();

        // TODO: We can do this check before 'post'.
        if (GNUNET_OK !=
            GNUNET_CRYPTO_eddsa_public_key_from_string (tid.c_str(),
                                                        tid.size(),
                                                        &pid.public_key)) {
            return ios.post([self] {
                       self->_on_connect(error::invalid_target_id);
                   });
        }

        GNUNET_MQ_MessageHandler handlers[] = {
            GNUNET_MQ_MessageHandler{ ChannelImpl::check_data
                                    , ChannelImpl::handle_data
                                    , NULL
                                    , GNUNET_MESSAGE_TYPE_CADET_CLI
                                    , sizeof(GNUNET_MessageHeader) },
            GNUNET_MQ_handler_end()
        };

        int flags = GNUNET_CADET_OPTION_DEFAULT
                  | GNUNET_CADET_OPTION_RELIABLE;

        self->_handle
            = GNUNET_CADET_channel_create( cadet->handle()
                                         , self.get()
                                         , &pid
                                         , &port_hash
                                         , (GNUNET_CADET_ChannelOption)flags
                                         , ChannelImpl::connect_window_change
                                         , ChannelImpl::connect_channel_ended
                                         , handlers);
        preserve(move(self));
    });
}

// Executed in GNUnet's thread
int ChannelImpl::check_data(void *cls, const GNUNET_MessageHeader *message)
{
    return GNUNET_OK; // all is well-formed
}

// Executed in GNUnet's thread
void ChannelImpl::connect_channel_ended( void *cls
                                       , const GNUNET_CADET_Channel *channel)
{
    auto ch = static_cast<ChannelImpl*>(cls);
    ch->_handle = nullptr;

    ch->get_io_service().post([ch = ch->shared_from_this()] {
            auto flush = [] (auto f, auto... args) {
                if (f) f(asio::error::connection_reset, args...);
            };

            flush(move(ch->_on_receive), 0);
            flush(move(ch->_on_send));
            flush(move(ch->_on_connect));
        });
}

// Executed in GNUnet's thread
void ChannelImpl::connect_window_change( void *cls
                                       , const GNUNET_CADET_Channel*
                                       , int window_size)
{
    auto ch = static_cast<ChannelImpl*>(cls);

    ch->get_io_service().post([ch = ch->shared_from_this()] {
            if (!ch->_on_connect) return;
            auto f = move(ch->_on_connect);
            f(sys::error_code());
        });
}

Scheduler& ChannelImpl::scheduler()
{
    return _scheduler;
}

asio::io_service& ChannelImpl::get_io_service()
{
    return scheduler().get_io_service();
}

void ChannelImpl::close()
{
    if (!_cadet) return; // Already closed.

    auto& ios = get_io_service();

    if (_on_send) {
        ios.post(bind(move(_on_send), asio::error::operation_aborted));
    }

    if (_on_receive) {
        ios.post(bind(move(_on_receive), asio::error::operation_aborted, 0));
    }

    while (!_send_queue.empty()) {
        auto e = _send_queue.front();
        _send_queue.pop();
        ios.post(bind(move(e.on_send), asio::error::operation_aborted, 0));
    }

    _scheduler.post([ s = shared_from_this()
                    , c = move(_cadet)
                    ] () mutable {
            if (s->_handle) {
                GNUNET_CADET_channel_destroy(s->_handle);
                s->_handle = nullptr;
            }

            preserve(move(s));
            preserve(move(c));
        });
}

ChannelImpl::~ChannelImpl()
{
    // Make sure the `close` method was called explicitly.
    // Note, that we can't call `close` here because it
    // calls the shared_from_this method.
    assert(!_cadet);
}
