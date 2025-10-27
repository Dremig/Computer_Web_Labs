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
    : _capacity(capacity), _bytes_written(0), _bytes_read(0), _buffer(), _error(false), _end_input(false) {
    // 容量为0是合法的，不需要抛出异常
}

size_t ByteStream::write(const string &data) {
    if (data.empty() || _error || _end_input) {
        return 0;  // Nothing to write, error state, or input ended
    }
    
    size_t available_space = _capacity - _buffer.size();
    size_t bytes_to_write = min(data.size(), available_space);
    
    if (bytes_to_write == 0) {
        return 0;  // No space left in the stream
    }
    
    for (size_t i = 0; i < bytes_to_write; ++i) {
        _buffer.push_back(data[i]);
    }
    
    _bytes_written += bytes_to_write;
    return bytes_to_write;
}

//! \param[in] len bytes will be copied from the output side of the buffer
string ByteStream::peek_output(const size_t len) const {
    if( len == 0 || _buffer.empty() || _error) {
        return "";  // Nothing to peek or stream is in error state
    }
    string output;
    size_t bytes_to_peek = min(len, _buffer.size());
    output.reserve(bytes_to_peek);
    for(size_t i = 0; i < bytes_to_peek; ++i) {
        output.push_back(_buffer[i]);
    }
    return output;
}

//! \param[in] len bytes will be removed from the output side of the buffer
void ByteStream::pop_output(const size_t len) { 
    if (len == 0 || eof() || _error) {
        return;  // Nothing to pop or stream is in error state
    }
    
    size_t bytes_to_pop = min(len, _buffer.size());
    _bytes_read += bytes_to_pop;
    
    for (size_t i = 0; i < bytes_to_pop; ++i) {
        _buffer.pop_front();
    }
}

void ByteStream::end_input() {
    _end_input = true;  // Mark the input as ended
}

bool ByteStream::input_ended() const { 
    return _end_input;  // Return the end input status
}

size_t ByteStream::buffer_size() const { 
    return _buffer.size();  // Return the current size of the buffer
}

bool ByteStream::buffer_empty() const { 
    return _buffer.empty();  // Check if the buffer is empty
}

bool ByteStream::eof() const { 
    return input_ended() && buffer_empty();  // Return true if input has ended and buffer is empty
}

size_t ByteStream::bytes_written() const { return _bytes_written; }

size_t ByteStream::bytes_read() const { return _bytes_read; }

size_t ByteStream::remaining_capacity() const { 
    return _capacity - _buffer.size(); 
}

