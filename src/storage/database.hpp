#pragma once

#include <string>

#include "progressive_rehash_map.hpp"


class Database{
private:
    ProgressiveRehashMap data;

public:
    const std::string* get(std::string& key){
        return data.get(key);
    }

    void set(const std::string& key, const std::string& value){
        data.set(key, value);
    }

    bool del(std::string& key){
        return data.del(key);
    }
};



inline Database Database;
