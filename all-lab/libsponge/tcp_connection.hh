#ifndef SPONGE_LIBSPONGE_TCP_FACTORED_HH
#define SPONGE_LIBSPONGE_TCP_FACTORED_HH

#include "tcp_config.hh"
#include "tcp_receiver.hh"
#include "tcp_sender.hh"
#include "tcp_state.hh"

class TCPConnection {
  private:
    TCPConfig _cfg;
    TCPReceiver _receiver{_cfg.recv_capacity};
    TCPSender _sender{_cfg.send_capacity, _cfg.rt_timeout, _cfg.fixed_isn};

    std::queue<TCPSegment> _segments_out{};
    bool _linger_after_streams_finish{true};
    bool _is_active{true};
    
    // [新增] 记录自上次收到数据包以来的时间
    size_t _time_since_last_segment_received{0};

    // [新增] 辅助函数：将 Sender 产生的包取出，填充 Receiver 的信息后放入发送队列
    void _send_segments();

    // [新增] 辅助函数：强制发送 RST 并关闭连接
    void _set_rst_state(bool send_rst);

  public:
    void connect();
    size_t write(const std::string &data);
    size_t remaining_outbound_capacity() const;
    void end_input_stream();
    ByteStream &inbound_stream() { return _receiver.stream_out(); }

    size_t bytes_in_flight() const;
    size_t unassembled_bytes() const;
    size_t time_since_last_segment_received() const;
    TCPState state() const { return {_sender, _receiver, active(), _linger_after_streams_finish}; };

    void segment_received(const TCPSegment &seg);
    void tick(const size_t ms_since_last_tick);
    std::queue<TCPSegment> &segments_out() { return _segments_out; }
    bool active() const;

    explicit TCPConnection(const TCPConfig &cfg) : _cfg{cfg} {}
    ~TCPConnection();

    TCPConnection() = delete;
    TCPConnection(TCPConnection &&other) = default;
    TCPConnection &operator=(TCPConnection &&other) = default;
};

#endif