#pragma once

#include "../config.hpp"
#include "../protocol/resp_parser.hpp"
#include "../protocol/reply_writer.hpp"
#include "../command/dispatcher.hpp"

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <string>
#include <algorithm>
#include <system_error>


class IdleQueue;

class Connection{
    friend class IdleQueue;

private:
    int fd;
    bool want_read;
    bool want_write;
    bool want_close;

    std::string incoming;
    std::size_t consumed_pos = 0;

    OutputBuffer outcoming;

    ReplyWriter reply;

    RespParser parser;

    Connection* next = nullptr;
    Connection* prev = nullptr;

    // 单调时钟下的绝对过期时间，单位为毫秒，由 IdleQueue 在入队或刷新时设置。
    std::int64_t timeout = 0;

    int inline parse_input() {
        while(true){
            const ParseStatus status = parser.parse({incoming.data() + consumed_pos, incoming.size() - consumed_pos});

            if(status == ParseStatus::NeedMore){
                return 0;
            }

            if(status == ParseStatus::Complete){
                if(!dispatch(
                    {incoming.data() + consumed_pos, parser.request_size()},
                    parser.arguments(),
                    reply)){
                        return ENOBUFS;
                    }
                consumed_pos += parser.request_size();
                parser.reset();

                want_write = true;

                if(consumed_pos == incoming.size()){
                    incoming.clear();
                    consumed_pos = 0;

                    return 0;
                }
                continue;
            }

            if(status == ParseStatus::Invalid){
                return EPROTO;
            }
        }
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
            return err;
        }else if(rd == 0){
            want_read = false;
            if(!want_write){
                want_close = true;
            }
            return 0;
        }else{
            incoming.append(chunk, static_cast<size_t>(rd));
            return parse_input();
        }

        return 0;
    }

    int write_buffer(){
        if(outcoming.empty()) return 0;
        static const long system_iov_max = ::sysconf(_SC_IOV_MAX);
        if(system_iov_max <= 0) return EINVAL;
        iovec vectors[32]{};
        const auto capacity = std::min(std::size_t{32}, static_cast<std::size_t>(system_iov_max));
        const auto batch = outcoming.build_iovecs(vectors, capacity, OUTPUT_BUFFER_SIZE);
        msghdr message{};
        message.msg_iov = vectors;
        message.msg_iovlen = batch.count;
        ssize_t wr;
        do{
            wr = sendmsg(fd, &message, MSG_NOSIGNAL);
        }while(wr < 0 && errno == EINTR);

        if(wr < 0){
            const int err = errno;
            if(err == EAGAIN || err == EWOULDBLOCK){
                return 0;
            }
            return err;
        }else if(wr == 0){
            return 0;
        }else{
            outcoming.consume(static_cast<std::size_t>(wr), monotonic_ms());

            if(outcoming.empty()){
                want_write = false;

                if(want_read == false){
                    want_close = true;
                }
            }

            return 0;
        }
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

    std::string pending_output() const {
        return outcoming.flatten_for_test();
    }

    ByteRange argument(std::size_t index) const {
        const auto& range = parser.arguments()[index];
        return ByteRange(incoming.data() + consumed_pos + range.offset, range.length);
    }

#endif
};
