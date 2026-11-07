#pragma once

#include <string>
#include <string_view>

#include "progressive_rehash_map.hpp"


using Entry = HashNode<std::string>;


class Database{
private:
    ProgressiveRehashMap<Entry> data;

public:
    Database(): data() {}

    const std::string* get(std::string_view key){
        const Entry* entry = data.find(key);
        return entry ? &entry->value : nullptr;
    }

    void set(std::string_view key, std::string_view value){
        if(Entry* entry = data.find(key)){
            entry->value = value;
        }else{
            data.insert(key, value);
        }
    }

    bool del(std::string_view key){
        Entry* entry = data.extract(key);
        if(entry == nullptr) return false;
        delete entry;
        return true;
    }


    ~Database(){
        data.clear([](Entry* entry) noexcept { delete entry; });
    }
};



inline Database Database;
