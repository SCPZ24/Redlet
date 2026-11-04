#pragma once

#include <unordered_map>
#include <string>



class Database{
private:
    std::unordered_map<std::string, std::string> data;


public:
    const std::string* get(std::string& key){
        auto it = data.find(key);
        return it == data.end() ? nullptr : &it->second;
    }

    void set(const std::string& key, const std::string& value){
        data.insert_or_assign(key, value);
    }

    bool del(std::string& key){
        return data.erase(key) != 0;
    }
};



inline Database Database;