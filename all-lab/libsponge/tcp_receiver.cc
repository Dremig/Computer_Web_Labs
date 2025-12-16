#include "tcp_receiver.hh"

using namespace std;

void TCPReceiver::segment_received(const TCPSegment &seg) {
    const TCPHeader &header = seg.header();

    if (header.syn) {
        if (_syn_received) {
        }
        _syn_received = true;
        _isn = header.seqno;
    }

    if (!_syn_received) {
        return;
    }

    uint64_t checkpoint = _reassembler.stream_out().bytes_written() + 1;

    uint64_t abs_seqno = unwrap(header.seqno, _isn, checkpoint);
    
    uint64_t stream_index = abs_seqno - 1 + (header.syn ? 1 : 0);

    
    if (header.syn && seg.payload().size() == 0) {
        if (header.fin) {
             _reassembler.push_substring("", 0, true);
        }
        return; 
    }

    _reassembler.push_substring(seg.payload().copy(), stream_index, header.fin);
}

optional<WrappingInt32> TCPReceiver::ackno() const {
    if (!_syn_received) {
        return nullopt;
    }

    uint64_t abs_ack = _reassembler.stream_out().bytes_written() + 1;

    if (_reassembler.stream_out().input_ended()) {
        abs_ack += 1;
    }

    return wrap(abs_ack, _isn);
}

size_t TCPReceiver::window_size() const {
    return _capacity - _reassembler.stream_out().buffer_size();
}