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
    // 获取当前接收方的窗口大小。如果接收方通告窗口为0，我们视为1（为了发送探测包）
    size_t current_window = _window_size > 0 ? _window_size : 1;

    // 循环发送，直到窗口填满 或 流为空
    while (current_window > bytes_in_flight()) {
        TCPSegment seg;
        
        // 1. 设置 SYN 标志
        if (_next_seqno == 0) {
            seg.header().syn = true;
        }

        // 2. 设置 Payload
        // 计算还能发送多少字节： min(窗口剩余空间, 最大载荷限制)
        // 注意：窗口剩余空间要考虑到 SYN 占用 1 个序号
        size_t window_remain = current_window - bytes_in_flight();
        // 如果 seg 已经有 SYN，它占用了 1 个空间，所以 payload 容量要 -1
        size_t payload_capacity = window_remain - (seg.header().syn ? 1 : 0);
        
        // 从 ByteStream 读取数据
        string payload = _stream.read(min(payload_capacity, TCPConfig::MAX_PAYLOAD_SIZE));
        seg.payload() = Buffer(std::move(payload));

        // 3. 设置 FIN 标志
        // 如果流结束了，且窗口还有空间容纳 FIN (FIN 占 1 个序号)
        // 窗口剩余空间 >= payload长度 + syn(1/0) + fin(1)
        if (!_stream.eof() && _stream.input_ended() && 
            (bytes_in_flight() + seg.length_in_sequence_space() < current_window)) {
            seg.header().fin = true;
        }

        // 4. 如果段长度为0（既没SYN，没FIN，没数据），停止发送
        if (seg.length_in_sequence_space() == 0) {
            break;
        }

        // 5. 设置序列号
        seg.header().seqno = wrap(_next_seqno, _isn);

        // 6. 发送并记录
        _segments_out.push(seg);                   // 发送给网络层
        _segments_outstanding.push_back(seg);      // 加入重传队列
        _next_seqno += seg.length_in_sequence_space(); // 更新下个序列号

        // 7. 如果定时器没启动，启动它
        if (!_timer_running) {
            _timer_running = true;
            _time_elapsed = 0;
        }

        // 如果设置了 FIN，就没必要再循环了（连接结束）
        if (seg.header().fin) {
            break;
        }
    }
}

//! \param ackno The remote receiver's ackno (acknowledgment number)
//! \param window_size The remote receiver's advertised window size
void TCPSender::ack_received(const WrappingInt32 ackno, const uint16_t window_size) {
    uint64_t abs_ack = unwrap(ackno, _isn, _next_seqno);

    // 检查 ACK 是否合法：不能确认尚未发送的数据
    if (abs_ack > _next_seqno) {
        return; // Impossible ackno
    }

    // 更新窗口大小
    _window_size = window_size;

    // 检查是否有新的数据被确认了 (abs_ack > _last_ack_seqno)
    bool is_new_data_acked = false;
    
    if (abs_ack > _last_ack_seqno) {
        _last_ack_seqno = abs_ack;
        is_new_data_acked = true;
        
        // 重置重传计数器和 RTO
        _current_rto = _initial_retransmission_timeout;
        _consecutive_retransmissions = 0;
        
        // 如果有新数据被确认，重置定时器
        _time_elapsed = 0;
    }

    // 清理重传队列中已经被完全确认的段
    while (!_segments_outstanding.empty()) {
        TCPSegment &seg = _segments_outstanding.front();
        uint64_t seg_abs_seq = unwrap(seg.header().seqno, _isn, _next_seqno);
        uint64_t seg_end = seg_abs_seq + seg.length_in_sequence_space();

        // 如果这个段的结尾 <= abs_ack，说明整个段都被确认了
        if (seg_end <= abs_ack) {
            _segments_outstanding.pop_front();
        } else {
            break;
        }
    }

    // 定时器逻辑更新
    if (_segments_outstanding.empty()) {
        // 如果没有待确认的段，关闭定时器
        _timer_running = false;
        _time_elapsed = 0;
    } else if (is_new_data_acked) {
        // 如果有待确认段且收到了新 ACK，重启定时器
        _timer_running = true;
        _time_elapsed = 0;
    }

    // 收到 ACK 可能打开了窗口空间，尝试填充
    fill_window();
}

//! \param[in] ms_since_last_tick the number of milliseconds since the last call to this method
void TCPSender::tick(const size_t ms_since_last_tick) {
    if (!_timer_running) {
        return;
    }

    _time_elapsed += ms_since_last_tick;

    // 检查是否超时
    if (_time_elapsed >= _current_rto && !_segments_outstanding.empty()) {
        // 重传最早的未确认段
        _segments_out.push(_segments_outstanding.front());

        // 处理 RTO 和 连续重传次数
        // 如果窗口大小不为0，才进行指数退避（Lab要求）
        // 注意：我们内部把窗口0当作1处理了，这里判断原始窗口大小
        if (_window_size > 0) {
            _consecutive_retransmissions++;
            _current_rto *= 2;
        }
        
        // 重置定时器累计时间
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