#include "tcp_connection.hh"

#include <iostream>
#include <limits>

using namespace std;

size_t TCPConnection::remaining_outbound_capacity() const { 
    return _sender.stream_in().remaining_capacity(); 
}

size_t TCPConnection::bytes_in_flight() const { 
    return _sender.bytes_in_flight(); 
}

size_t TCPConnection::unassembled_bytes() const { 
    return _receiver.unassembled_bytes(); 
}

size_t TCPConnection::time_since_last_segment_received() const { 
    return _time_since_last_segment_received; 
}

void TCPConnection::segment_received(const TCPSegment &seg) { 
    if (!_is_active) {
        return;
    }

    _time_since_last_segment_received = 0;

    // 1. 处理 RST 标志 (Reset)
    // 如果收到 RST，直接将两个流标记为错误，并终止连接
    if (seg.header().rst) {
        _sender.stream_in().set_error();
        _receiver.stream_out().set_error();
        _is_active = false;
        return;
    }

    // 2. 将数据包交给 Receiver 处理
    _receiver.segment_received(seg);

    // 3. 处理 ACK (如果 Sender 关心的话)
    if (seg.header().ack) {
        _sender.ack_received(seg.header().ackno, seg.header().win);
    }

    // 4. 判断是否需要 Linger (TIME_WAIT 状态逻辑)
    // 如果 Receiver 收到了 FIN (入站流结束)，并且 Sender 还没有发完流 (出站流未结束)
    // 说明我们是“被动关闭”的一方 (Passive Close)，不需要等待 Linger
    if (_receiver.stream_out().input_ended() && !_sender.stream_in().eof()) {
        _linger_after_streams_finish = false;
    }
    // 注意：更严格的判断是被动关闭方收到 FIN 时，自己还没发 FIN。
    // 这里如果 _sender.stream_in().eof() 为 false，肯定没发 FIN。

    // 5. 决定是否需要回复 ACK
    // 只要包里占用了序列号 (payload > 0, 或 SYN/FIN)，我们就必须回复 ACK
    bool need_reply = seg.length_in_sequence_space() > 0;

    // 特殊情况：Keep-alive 探测
    // 对方发来一个 seqno 正确，但长度为 0 的包，我们需要回复当前状态
    if (seg.length_in_sequence_space() == 0 && 
        _receiver.ackno().has_value() && 
        seg.header().seqno == _receiver.ackno().value() - 1) {
        need_reply = true;
    }

    // 尝试让 Sender 发送数据 (如果有数据待发)
    _sender.fill_window();

    // 如果 Sender 没有数据要发，但我们需要回复 ACK，则发送一个空包
    if (_sender.segments_out().empty() && need_reply) {
        _sender.send_empty_segment();
    }

    // 将 Sender 产生的数据包搬运到 Connection 的队列中发出
    _trans_segments_to_out_queue();
    
    // 检查是否可以结束连接
    _check_clean_shutdown();
}

bool TCPConnection::active() const { return _is_active; }

size_t TCPConnection::write(const string &data) {
    if (!_is_active) return 0;

    size_t written = _sender.stream_in().write(data);
    _sender.fill_window();
    _trans_segments_to_out_queue();
    return written;
}

//! \param[in] ms_since_last_tick number of milliseconds since the last call to this method
void TCPConnection::tick(const size_t ms_since_last_tick) { 
    if (!_is_active) return;

    _time_since_last_segment_received += ms_since_last_tick;

    // 告诉 Sender 时间流逝 (用于处理重传)
    _sender.tick(ms_since_last_tick);

    // 如果重传次数过多，发送 RST 并断开连接
    if (_sender.consecutive_retransmissions() > TCPConfig::MAX_RETX_ATTEMPTS) {
        // 清空队列，只发一个 RST
        while (!_segments_out.empty()) { _segments_out.pop(); }
        
        _send_rst_segment();
        
        _sender.stream_in().set_error();
        _receiver.stream_out().set_error();
        _is_active = false;
        return;
    }

    // 如果 Sender 产生了重传包，将其发出
    _trans_segments_to_out_queue();

    // 检查连接关闭状态 (处理 TIME_WAIT 超时)
    _check_clean_shutdown();
}

void TCPConnection::end_input_stream() {
    _sender.stream_in().end_input();
    _sender.fill_window();
    _trans_segments_to_out_queue();
    _check_clean_shutdown();
}

void TCPConnection::connect() {
    _sender.fill_window(); // 这会产生 SYN 包
    _is_active = true;
    _trans_segments_to_out_queue();
}

TCPConnection::~TCPConnection() {
    try {
        if (active()) {
            cerr << "Warning: Unclean shutdown of TCPConnection\n";
            _send_rst_segment();
        }
    } catch (const exception &e) {
        std::cerr << "Exception destructing TCP FSM: " << e.what() << std::endl;
    }
}

// 辅助函数：将 Sender 的包搬运到 Connection 的队列，并填充 ACK/Window
void TCPConnection::_trans_segments_to_out_queue() {
    while (!_sender.segments_out().empty()) {
        TCPSegment seg = _sender.segments_out().front();
        _sender.segments_out().pop();

        // 如果 Receiver 已经建立连接 (有 ackno)，则设置 ACK 标志和窗口大小
        if (_receiver.ackno().has_value()) {
            seg.header().ack = true;
            seg.header().ackno = _receiver.ackno().value();
            
            // 窗口大小限制在 uint16 范围内
            size_t win_sz = _receiver.window_size();
            seg.header().win = (win_sz > numeric_limits<uint16_t>::max()) 
                               ? numeric_limits<uint16_t>::max() 
                               : win_sz;
        }

        _segments_out.push(seg);
    }
}

// 辅助函数：发送 RST 包
void TCPConnection::_send_rst_segment() {
    TCPSegment rst_seg;
    // RST 包的 seqno 应该对应当前的 next_seqno 或者最近收到的 ackno
    // 简单起见，使用 sender 的下一个序列号
    if (_sender.segments_out().empty()) {
         _sender.send_empty_segment();
    }
    
    // 取出刚才生成的空包，打上 RST 标记
    if (!_sender.segments_out().empty()) {
        rst_seg = _sender.segments_out().front();
        _sender.segments_out().pop();
    } else {
        // Fallback: 手动构建
        rst_seg.header().seqno = _sender.next_seqno();
    }
    
    rst_seg.header().rst = true;
    
    // RST 包通常不需要带 ACK，但在某些实现中如果已有 ACK 信息也会带上。
    // 此处为了稳妥，也可以带上 ACK。
    if (_receiver.ackno().has_value()) {
        rst_seg.header().ack = true;
        rst_seg.header().ackno = _receiver.ackno().value();
    }
    
    _segments_out.push(rst_seg);
}

// 辅助函数：检查是否可以干净地关闭
void TCPConnection::_check_clean_shutdown() {
    // 1. 入站流必须结束 (收到 FIN)
    // 2. 出站流必须结束 (写完数据，且 FIN 已确认)
    //    这里判断 bytes_in_flight == 0 且 stream_in.eof() 且 next_seqno 等于 stream total + 2 (SYN+FIN)
    //    或者更简单地：Sender 认为自己发完了并且都 ACK 了。
    bool outbound_ended = _sender.stream_in().eof() && 
                          _sender.next_seqno_absolute() == _sender.stream_in().bytes_written() + 2 && 
                          _sender.bytes_in_flight() == 0;
                          
    if (_receiver.stream_out().input_ended() && outbound_ended) {
        if (!_linger_after_streams_finish) {
            // 不需要 Linger (被动关闭)，直接关闭
            _is_active = false;
        } else if (_time_since_last_segment_received >= 10 * _cfg.rt_timeout) {
            // 需要 Linger (主动关闭)，且时间已到
            _is_active = false;
        }
    }
}