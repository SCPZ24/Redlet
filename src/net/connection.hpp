#pragma once

#include "../config.hpp"
#include "../protocol/resp_parser.hpp"
#include "../protocol/reply_writer.hpp"

#include <cerrno>
#include <cstddef>
#include <sys/types.h>
#include <unistd.h>
#include <string>
#include <algorithm>
#include <system_error>


class Connection{
private:
    int fd;
    bool want_read;
    bool want_write;
    bool want_close;

    std::string incoming;
    std::size_t consumed_pos = 0;

    std::string outcoming;
    ReplyWriter reply;

    RespParser parser;

    int inline parse_input() {
        const auto status = parser.parse(ByteRange(
            incoming.data() + consumed_pos, incoming.size() - consumed_pos));
        if(status == ParseStatus::Invalid) return EPROTO;
        if(status == ParseStatus::Complete){

        }
        return 0;
    }

public:
    Connection(int client_fd): fd(client_fd), reply(outcoming){
        want_read = true;
        want_write = false;
        want_close = false;
    }

    int get_fd()const{
        return fd;
    }


    int read_buffer(){
        // The caller invokes this only while the connection wants input.
        // No request views may survive a read or an acknowledgement.
        if(consumed_pos != 0){
            incoming.erase(0, consumed_pos);
            consumed_pos = 0;
        }
        char chunk[INPUT_BUFFER_SIZE];
        const std::size_t room = MAX_REQUEST_BYTES - incoming.size();
        if(room == 0) return EMSGSIZE;

        ssize_t rd;
        do{
            rd = read(fd, chunk, std::min(sizeof(chunk), room));
        }while(rd < 0 && errno == EINTR);

        if(rd < 0){
            const int err = errno;
            if(err == EAGAIN || err == EWOULDBLOCK){
                return 0;
            }
            throw std::system_error(err, std::generic_category(), "read");
        }else if(rd == 0){
            want_read = false;
            return 0;
        }else{
            incoming.append(chunk, static_cast<size_t>(rd));
            return parse_input();
        }

        return 0;
    }

    // Call only after dispatch and reply encoding finish; invalidates borrowed views.
    // Immediately parses an already-buffered pipeline successor.
    int finish_request() {
        consumed_pos += parser.request_size();
        parser.reset();
        if(consumed_pos == incoming.size()){
            incoming.clear();
            consumed_pos = 0;
        }
        return parse_input();
    }
    // This funcition is a draft, may be deleted later. Use it unless we make clear what could it do.


    int write_buffer(){
        return -1;
    }


    bool ready_to_close() const{
        return want_close;
    }

    bool ready_to_write() const{
        return want_write;
    }

    bool ready_to_read() const{
        return want_read;
    }


    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    ~Connection(){
        close(fd);
    }



#ifdef test

    ByteRange argument(std::size_t index) const {
        const auto& range = parser.arguments()[index];
        return ByteRange(incoming.data() + consumed_pos + range.offset, range.length);
    }

#endif
};
