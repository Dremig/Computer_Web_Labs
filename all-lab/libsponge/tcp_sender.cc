#include "tcp_sender.hh"
#include "tcp_config.hh"

#include <random>
#include <iostream>

using namespace std;

//! \param[in] capacity the capacity of the outgoing byte stream
//! \param[in] retx_timeout the initial amount of time to wait before retransmitting the oldest outstanding segment
//! \param[in] fixed_isn the Initial Sequence Number to use, if set (otherwise uses a random ISN)
TCPSender::TCPSender(const size_t capacity, const uint16_t retx_timeout, const std::optional<WrappingInt32> fixed_isn)
    : _isn(fixed_isn.value_or(WrappingInt32{std::random_device{}()}))
    , _initial_retransmission_timeout{retx_timeout}
    , _current_rto{retx_timeout}
    , _stream(capacity) {}

uint64_t TCPSender::bytes_in_flight() const {
    // 正在飞行（已发送但未确认）的字节数 = next_seqno - last_ack_seqno
    return _next_seqno - _last_ack_seqno;
}

void TCPSender::fill_window() {
    // 视窗大小为0时视为1（零窗口探测）
    size_t current_window = _window_size == 0 ? 1 : _window_size;

    while (current_window > bytes_in_flight()) {
        TCPSegment seg;
        
        // 1. 如果还没发过 SYN，加上 SYN 标志
        if (_next_seqno == 0) {
            seg.header().syn = true;
        }

        // 2. 计算 payload 还有多少空间
        // 剩余窗口 = 假定窗口 - 已发送未确认
        size_t window_remain = current_window - bytes_in_flight();
        
        // 如果 seg 已经带了 SYN，它占用了 1 个序列号
        size_t payload_capacity = window_remain - (seg.header().syn ? 1 : 0);
        
        // 从流中读取数据，不能超过剩余空间，也不能超过最大载荷
        string payload = _stream.read(min(payload_capacity, TCPConfig::MAX_PAYLOAD_SIZE));
        seg.payload() = Buffer(std::move(payload));

        // 3. 判断能否发送 FIN
        // 条件：
        // (a) 流已经完全结束 (EOF: 输入结束且缓冲读空)
        // (b) 窗口里还有空间放 FIN (seg目前的长度 + 1 <= window_remain)
        // 注意：seg.length_in_sequence_space() 此时包含 SYN + Payload 长度
        if (_stream.eof() && (seg.length_in_sequence_space() < window_remain)) {
            seg.header().fin = true;
        }

        // 4. 如果段长度为0（无SYN，无数据，无FIN），说明发不了东西了，退出
        if (seg.length_in_sequence_space() == 0) {
            break;
        }

        // 5. 设置序列号并发送
        seg.header().seqno = wrap(_next_seqno, _isn);
        
        _segments_out.push(seg);
        _segments_outstanding.push_back(seg);
        
        _next_seqno += seg.length_in_sequence_space();

        // 6. 如果定时器没启动，启动它
        if (!_timer_running) {
            _timer_running = true;
            _time_elapsed = 0;
        }

        // 如果发了 FIN，连接结束，不用再填充了
        if (seg.header().fin) {
            break;
        }
    }
}

//! \param ackno The remote receiver's ackno (acknowledgment number)
//! \param window_size The remote receiver's advertised window size
void TCPSender::ack_received(const WrappingInt32 ackno, const uint16_t window_size) {
    uint64_t abs_ack = unwrap(ackno, _isn, _next_seqno);

    // 忽略不可靠的 ACK（确认了还没发的数据）
    if (abs_ack > _next_seqno) {
        return;
    }

    _window_size = window_size;

    bool is_new_data = false;
    
    // 只有当确认号 > 上一次确认号时，才算确认了新数据
    if (abs_ack > _last_ack_seqno) {
        _last_ack_seqno = abs_ack;
        is_new_data = true;
        
        // 重置 RTO 和 连续重传计数
        _current_rto = _initial_retransmission_timeout;
        _consecutive_retransmissions = 0;
        
        // 只有确认了新数据，才重置累计时间
        _time_elapsed = 0;
    }

    // 移除已确认的段
    while (!_segments_outstanding.empty()) {
        TCPSegment &seg = _segments_outstanding.front();
        uint64_t seg_len = seg.length_in_sequence_space();
        uint64_t seg_abs_seq = unwrap(seg.header().seqno, _isn, _next_seqno);
        
        if (seg_abs_seq + seg_len <= abs_ack) {
            _segments_outstanding.pop_front();
        } else {
            break;
        }
    }

    // 尝试填充窗口
    fill_window();

    // 更新定时器状态
    if (_segments_outstanding.empty()) {
        _timer_running = false;
        _time_elapsed = 0; // 关闭时清零
    } else if (is_new_data) {
        // 如果有未确认数据，且刚才收到了新 ACK，重启定时器
        _timer_running = true;
        // _time_elapsed 在上面 if (is_new_data) 里已经置 0 了
    }
}

//! \param[in] ms_since_last_tick the number of milliseconds since the last call to this method
void TCPSender::tick(const size_t ms_since_last_tick) {
    if (!_timer_running) {
        return;
    }

    _time_elapsed += ms_since_last_tick;

    if (_time_elapsed >= _current_rto && !_segments_outstanding.empty()) {
        _segments_out.push(_segments_outstanding.front());

        // 只有在窗口非0时，才进行指数退避
        if (_window_size > 0) {
            _current_rto *= 2;
        }
        
        _consecutive_retransmissions++;
        _time_elapsed = 0;
    }
}

unsigned int TCPSender::consecutive_retransmissions() const {
    return _consecutive_retransmissions;
}

void TCPSender::send_empty_segment() {
    // 发送一个空的 ACK 段（通常用于回复 ACK 或保活）
    // 只有 seqno 字段是有意义的
    TCPSegment seg;
    seg.header().seqno = wrap(_next_seqno, _isn);
    _segments_out.push(seg);
}