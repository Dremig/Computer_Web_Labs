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
    return _next_seqno - _last_ack_seqno;
}

void TCPSender::fill_window() {
    if (_fin_sent) {
        return;
    }

    size_t current_window = _window_size == 0 ? 1 : _window_size;

    while (current_window > bytes_in_flight()) {
        TCPSegment seg;
        
        // 1. 处理 SYN
        if (_next_seqno == 0) {
            seg.header().syn = true;
        }

        size_t window_remain = current_window - bytes_in_flight();
        size_t payload_capacity = window_remain - (seg.header().syn ? 1 : 0);
        
        string payload = _stream.read(min(payload_capacity, TCPConfig::MAX_PAYLOAD_SIZE));
        seg.payload() = Buffer(std::move(payload));

        if (!_fin_sent && _stream.eof() && (seg.length_in_sequence_space() < window_remain)) {
            seg.header().fin = true;
            _fin_sent = true; 
        }

        if (seg.length_in_sequence_space() == 0) {
            break;
        }

        seg.header().seqno = wrap(_next_seqno, _isn);
        
        _segments_out.push(seg);
        _segments_outstanding.push_back(seg);
        
        _next_seqno += seg.length_in_sequence_space();

        if (!_timer_running) {
            _timer_running = true;
            _time_elapsed = 0;
        }

        if (seg.header().fin) {
            break;
        }
    }
}

//! \param ackno The remote receiver's ackno (acknowledgment number)
//! \param window_size The remote receiver's advertised window size
void TCPSender::ack_received(const WrappingInt32 ackno, const uint16_t window_size) {
    uint64_t abs_ack = unwrap(ackno, _isn, _next_seqno);

    // unacceptable ackno
    if (abs_ack > _next_seqno) {
        return;
    }

    _window_size = window_size;

    bool is_new_data = false;

    if (abs_ack > _last_ack_seqno) {
        _last_ack_seqno = abs_ack;
        is_new_data = true;

        _current_rto = _initial_retransmission_timeout;
        _consecutive_retransmissions = 0;

        _time_elapsed = 0;
    }

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

    fill_window();

    if (_segments_outstanding.empty()) {
        _timer_running = false;
        _time_elapsed = 0; // set to 0 when timer stops
    } else if (is_new_data) {
        _timer_running = true;
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
    TCPSegment seg;
    seg.header().seqno = wrap(_next_seqno, _isn);
    _segments_out.push(seg);
}