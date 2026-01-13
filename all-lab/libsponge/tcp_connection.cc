#include "tcp_connection.hh"
#include <iostream>

using namespace std;

size_t TCPConnection::remaining_outbound_capacity() const { return _sender.stream_in().remaining_capacity(); }
size_t TCPConnection::bytes_in_flight() const { return _sender.bytes_in_flight(); }
size_t TCPConnection::unassembled_bytes() const { return _receiver.unassembled_bytes(); }
size_t TCPConnection::time_since_last_segment_received() const { return _time_since_last_segment_received; }

void TCPConnection::segment_received(const TCPSegment &seg) {
    if (!_is_active) return;
    _time_since_last_segment_received = 0;

    // 1. 处理 RST：如果收到 RST，立即销毁连接
    if (seg.header().rst) {
        _set_rst_state(false);
        return;
    }

    // 2. 将包交给 Receiver
    _receiver.segment_received(seg);

    // 3. 处理 ACK：只有在 Sender 已发送过 SYN 的情况下才处理 ACK
    if (seg.header().ack) {
        _sender.ack_received(seg.header().ackno, seg.header().win);
    }

    // 4. 状态切换逻辑：Listen 状态收到 SYN
    if (TCPState::state_summary(_receiver) == TCPReceiverStateSummary::SYN_RECV &&
        _sender.next_seqno_absolute() == 0) {
        connect();
        return;
    }

    // 5. 判断是否需要停止 Linger (被动关闭)
    // 如果对方先发送 FIN，且我方尚未达到 EOF，则不需要 Linger
    if (TCPState::state_summary(_receiver) == TCPReceiverStateSummary::FIN_RECV &&
        TCPState::state_summary(_sender) == TCPSenderStateSummary::SYN_ACKED) {
        _linger_after_streams_finish = false;
    }

    // 6. 发送回复
    // 如果收到的是占序列号的包（SYN/FIN/Payload），必须回一个 ACK
    if (seg.length_in_sequence_space() > 0) {
        _sender.fill_window();
        if (_sender.segments_out().empty()) {
            _sender.send_empty_segment();
        }
    }
    
    // 特殊处理 Keep-alive：如果收到 seqno 为空但合法的包
    if (_receiver.ackno().has_value() && (seg.length_in_sequence_space() == 0) &&
        seg.header().seqno == _receiver.ackno().value() - 1) {
        _sender.send_empty_segment();
    }

    _send_segments();
}

bool TCPConnection::active() const {
    if (!_is_active) return false;
    
    // 判断是否满足“干净关闭”的条件
    bool streams_closed = (_receiver.stream_out().input_ended() && 
                          _sender.stream_in().eof() && 
                          _sender.bytes_in_flight() == 0 &&
                          _sender.next_seqno_absolute() == _sender.stream_in().bytes_written() + 2);
    
    if (streams_closed) {
        if (!_linger_after_streams_finish) return false;
        if (_time_since_last_segment_received >= 10 * _cfg.rt_timeout) return false;
    }
    return true;
}

void TCPConnection::tick(const size_t ms_since_last_tick) {
    if (!_is_active) return;
    _time_since_last_segment_received += ms_since_last_tick;

    _sender.tick(ms_since_last_tick);

    // 检查重传上限
    if (_sender.consecutive_retransmissions() > TCPConfig::MAX_RETX_ATTEMPTS) {
        _set_rst_state(true);
        return;
    }

    _send_segments();
    if (!active()) _is_active = false;
}

void TCPConnection::connect() {
    _sender.fill_window();
    _is_active = true;
    _send_segments();
}

size_t TCPConnection::write(const string &data) {
    size_t written = _sender.stream_in().write(data);
    _sender.fill_window();
    _send_segments();
    return written;
}

void TCPConnection::end_input_stream() {
    _sender.stream_in().end_input();
    _sender.fill_window();
    _send_segments();
}

TCPConnection::~TCPConnection() {
    try {
        if (active()) {
            _set_rst_state(true);
        }
    } catch (const exception &e) {
        cerr << "Exception in destructor: " << e.what() << endl;
    }
}

// 辅助函数：取出 Sender 的包并打上 Receiver 的标记
void TCPConnection::_send_segments() {
    while (!_sender.segments_out().empty()) {
        TCPSegment seg = _sender.segments_out().front();
        _sender.segments_out().pop();

        if (_receiver.ackno().has_value()) {
            seg.header().ack = true;
            seg.header().ackno = _receiver.ackno().value();
            seg.header().win = _receiver.window_size() > 0xffff ? 0xffff : _receiver.window_size();
        }
        _segments_out.push(seg);
    }
}

void TCPConnection::_set_rst_state(bool send_rst) {
    if (send_rst) {
        TCPSegment rst_seg;
        rst_seg.header().rst = true;
        rst_seg.header().seqno = _sender.next_seqno();
        _segments_out.push(rst_seg);
    }
    _receiver.stream_out().set_error();
    _sender.stream_in().set_error();
    _is_active = false;
}