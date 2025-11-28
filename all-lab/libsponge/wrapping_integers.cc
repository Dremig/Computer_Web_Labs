#include "wrapping_integers.hh"

// Dummy implementation of a 32-bit wrapping integer

// For Lab 2, please replace with a real implementation that passes the
// automated checks run by `make check_lab2`.

template <typename... Targs>
void DUMMY_CODE(Targs &&... /* unused */) {}

using namespace std;

//! Transform an "absolute" 64-bit sequence number (zero-indexed) into a WrappingInt32
//! \param n The input absolute 64-bit sequence number
//! \param isn The initial sequence number
WrappingInt32 wrap(uint64_t n, WrappingInt32 isn) {
    uint64_t isn64 = static_cast<uint64_t>(isn.raw_value());
    uint64_t sum = isn64 + n;
    uint32_t wrapped = static_cast<uint32_t>(sum % (1ULL << 32));
    return WrappingInt32(wrapped);
}

//! Transform a WrappingInt32 into an "absolute" 64-bit sequence number (zero-indexed)
//! \param n The relative sequence number
//! \param isn The initial sequence number
//! \param checkpoint A recent absolute 64-bit sequence number
//! \returns the 64-bit sequence number that wraps to `n` and is closest to `checkpoint`
//!
//! \note Each of the two streams of the TCP connection has its own ISN. One stream
//! runs from the local TCPSender to the remote TCPReceiver and has one ISN,
//! and the other stream runs from the remote TCPSender to the local TCPReceiver and
//! has a different ISN.
uint64_t unwrap(WrappingInt32 n, WrappingInt32 isn, uint64_t checkpoint) {
    const uint64_t MOD = 1ULL << 32;
    uint64_t n64 = static_cast<uint64_t>(n.raw_value());
    uint64_t isn64 = static_cast<uint64_t>(isn.raw_value());
    uint64_t diff = (n64 - isn64 + MOD) % MOD;
    uint64_t era = checkpoint / MOD;
    uint64_t abs_seq = diff + era * MOD;
    if (abs_seq > checkpoint && (abs_seq - checkpoint > (1ULL << 31)) && abs_seq >= MOD) {
        abs_seq -= MOD;
    } else if (abs_seq < checkpoint && (checkpoint - abs_seq > (1ULL << 31))) {
        abs_seq += MOD;
    }
    return abs_seq;
}
