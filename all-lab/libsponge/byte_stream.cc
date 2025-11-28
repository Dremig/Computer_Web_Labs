#include "byte_stream.hh"

#include <algorithm>
#include <iterator>
#include <stdexcept>

// Dummy implementation of a flow-controlled in-memory byte stream.

// For Lab 0, please replace with a real implementation that passes the
// automated checks run by `make check_lab0`.

// You will need to add private members to the class declaration in `byte_stream.hh`

template <typename... Targs>
void DUMMY_CODE(Targs &&... /* unused */) {}

using namespace std;

ByteStream::ByteStream(const size_t capacity) 
    : _capacity(capacity), _bytes_written(0), _bytes_read(0), _buffer(), _error(false), _input_ended(false) {}

size_t ByteStream::write(const string &data) {
    if (data.empty() || _error || _input_ended) {
        // nothing to write
        return 0;  
    }
    
    size_t space = _capacity - _buffer.size();
    size_t to_write = min(space, data.size());
    _buffer += data.substr(0, to_write);
    _bytes_written += to_write;
    return to_write;
}

//! \param[in] len bytes will be copied from the output side of the buffer
string ByteStream::peek_output(const size_t len) const {
    if( len == 0 || _buffer.empty() || _error) {
        return "";  // Nothing to peek or stream is in error state
    }
    size_t to_peek = min(len, _buffer.size());
    return _buffer.substr(0, to_peek);
}

//! \param[in] len bytes will be removed from the output side of the buffer
void ByteStream::pop_output(const size_t len) { 
    if (len == 0 || eof() || _error) {
        return;  // Nothing to pop or stream is in error state
    }
    
    size_t to_pop = min(len, _buffer.size());
    _buffer.erase(0, to_pop);
    _bytes_read += to_pop;
}

void ByteStream::end_input() {
    _input_ended = true;  // Mark the input as ended
}

bool ByteStream::input_ended() const { 
    return _input_ended;
}

size_t ByteStream::buffer_size() const { 
    return _buffer.size();
}

bool ByteStream::buffer_empty() const { 
    return _buffer.empty();
}

bool ByteStream::eof() const { 
    // let buffer empty be eof as well
    return input_ended() && buffer_empty();
}

size_t ByteStream::bytes_written() const { 
    return _bytes_written; 
}

size_t ByteStream::bytes_read() const { 
    return _bytes_read; 
}

size_t ByteStream::remaining_capacity() const { 
    return _capacity - _buffer.size(); 
}

