#pragma once

#include <string>
#include <string_view>

#include "progressive_rehash_map.hpp"


class Database{
private:
    ProgressiveRehashMap data;

public:
    const std::string* get(std::string_view key){
        return data.get(key);
    }

    void set(std::string_view key, std::string_view value){
        data.set(key, value);
    }

    bool del(std::string_view key){
        return data.del(key);
    }
};



inline Database Database;
