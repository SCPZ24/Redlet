#include "../util.hpp"

#include "connection.hpp"
#include <cmath>
#include <cstdint>



class IdleQueue{
private:
    Connection* pivot;


    bool empty() const{
        return pivot->next == pivot;
    }

    void emplace(Connection* conn){
        conn->next = pivot->next;
        conn->prev = pivot;
        pivot->next->prev = conn;
        pivot->next = conn;
    }
public:
    IdleQueue(){
        pivot = new Connection(-1);
        pivot->next = pivot;
        pivot->prev = pivot;
    }


    // 晚来的放队尾，检查时查看队首即可。

    // 返回空指针表示没什么好销毁的连接。
    inline Connection* takle_timeout(std::int64_t now){
        if(empty()) return nullptr;

        Connection* last = pivot->prev;
        if(now < last->timeout){
            return nullptr;
        }

        return last; //外部会释放这个连接。
    }

    inline void reset(Connection* conn){
        unlink(conn);

        add(conn);
    }

    inline void add(Connection* conn){
        conn->timeout = monotonic_ms() + DEFAULT_CONNECTION_TIMEOUT_MS;

        emplace(conn);
    }


    // 外部控制流应该保证此处conn非空，且conn != pivot。
    inline void unlink(Connection* conn){
        // 这里不用asset或者if。如果重复调用unlink，直接报错。
        conn->prev->next = conn->next;
        conn->next->prev = conn->prev;
        conn->next = nullptr;
        conn->prev = nullptr;
    }


    ~IdleQueue(){
        while(!empty()){
            unlink(pivot->next);
        }

        delete pivot;
    }
};