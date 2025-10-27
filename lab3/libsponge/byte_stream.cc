#include "byte_stream.hh"

// Dummy implementation of a flow-controlled in-memory byte stream.

// For Lab 0, please replace with a real implementation that passes the
// automated checks run by `make check_lab0`.

// You will need to add private members to the class declaration in `byte_stream.hh`

using namespace std;

ByteStream::ByteStream(const size_t capacity)
    : capacity_(capacity), bytes_written_(0), bytes_read_(0), input_ended_(false) {}

size_t ByteStream::write(const string &data) {
    if (input_ended_) {
        return 0;
    }

    size_t space = capacity_ - buffer_.size();
    size_t to_write = min(space, data.size());
    buffer_ += data.substr(0, to_write);
    bytes_written_ += to_write;
    return to_write;
}

//! \param[in] len bytes will be copied from the output side of the buffer
string ByteStream::peek_output(const size_t len) const {
    size_t to_peek = min(len, buffer_.size());
    return buffer_.substr(0, to_peek);
}

//! \param[in] len bytes will be removed from the output side of the buffer
void ByteStream::pop_output(const size_t len) {
    size_t to_pop = min(len, buffer_.size());
    buffer_.erase(0, to_pop);
    bytes_read_ += to_pop;
}

//! Read (i.e., copy and then pop) the next "len" bytes of the stream
//! \param[in] len bytes will be popped and returned
//! \returns a string
std::string ByteStream::read(const size_t len) {
    string output = peek_output(len);
    pop_output(output.size());
    return output;
}

void ByteStream::end_input() {
    input_ended_ = true;
}

bool ByteStream::input_ended() const {
    return input_ended_;
}

size_t ByteStream::buffer_size() const {
    return buffer_.size();
}

bool ByteStream::buffer_empty() const {
    return buffer_.empty();
}

bool ByteStream::eof() const {
    return input_ended_ && buffer_empty();
}

size_t ByteStream::bytes_written() const {
    return bytes_written_;
}

size_t ByteStream::bytes_read() const {
    return bytes_read_;
}

size_t ByteStream::remaining_capacity() const {
    return capacity_ - buffer_.size();
}