#pragma once

#include <string>
#include <string_view>
#include <variant>
#include <cmath>
#include <cstdint>
#include <vector>

#include "progressive_rehash_map.hpp"
#include "sorted_set.hpp"

using Entry = HashNode<std::variant<std::string, SortedSet*>>;

enum class RequestStatus{
    OK,
    Missing,
    WrongType,
    InvalidScore
};

struct GetResult{
    RequestStatus status;
    const std::string* value;
};

template<typename T>
struct RequestResult{
    RequestStatus status;
    T value;
};

class Database{
private:
    ProgressiveRehashMap<Entry> data;

public:
    Database(): data() {}

    GetResult get(std::string_view key){
        const Entry* entry = data.find(key);
        if(entry == nullptr) return {RequestStatus::Missing, nullptr};

        const auto* value = std::get_if<std::string>(&entry->value);
        if(value == nullptr) return {RequestStatus::WrongType, nullptr};
        return {RequestStatus::OK, value};
    }

    void set(std::string_view key, std::string_view value){
        if(Entry* entry = data.find(key)){
            if(auto* text = std::get_if<std::string>(&entry->value)){
                *text = value;
            }else{
                SortedSet* sorted_set = std::get<SortedSet*>(entry->value);
                entry->value = std::string(value);
                delete sorted_set;
            }
        }else{
            entry = new Entry(key, std::string(value));
            data.insert(entry);
        }
    }

    bool del(std::string_view key){
        Entry* entry = data.extract(key);
        if(entry == nullptr) return false;

        if(auto* sorted_set = std::get_if<SortedSet*>(&entry->value)){
            delete *sorted_set;
        }
        delete entry;
        return true;
    }

    RequestResult<std::size_t> zadd(std::string_view key, const std::vector<Member>& members){
        for(const auto& member : members){
            if(std::isnan(member.score)) return {RequestStatus::InvalidScore, 0};
        }
        if(members.empty()) return {RequestStatus::OK, 0};

        Entry* entry = data.find(key);
        if(entry == nullptr){
            entry = new Entry(key, new SortedSet);
            data.insert(entry);
        }
        auto* sorted_set = std::get_if<SortedSet*>(&entry->value);
        if(sorted_set == nullptr) return {RequestStatus::WrongType, 0};

        std::size_t added = 0;
        for(const auto& member : members){
            added += (*sorted_set)->add(member.member, member.score);
        }
        return {RequestStatus::OK, added};
    }

    RequestResult<std::size_t> zrem(std::string_view key, const std::vector<std::string_view>& members){
        Entry* entry = data.find(key);
        if(entry == nullptr) return {RequestStatus::OK, 0};
        auto* sorted_set = std::get_if<SortedSet*>(&entry->value);
        if(sorted_set == nullptr) return {RequestStatus::WrongType, 0};

        std::size_t removed = 0;
        for(const auto member : members){
            removed += (*sorted_set)->remove(member);
        }
        if((*sorted_set)->size() == 0) del(key);
        return {RequestStatus::OK, removed};
    }

    RequestResult<double> zscore(std::string_view key, std::string_view member){
        Entry* entry = data.find(key);
        if(entry == nullptr) return {RequestStatus::Missing, 0};
        auto* sorted_set = std::get_if<SortedSet*>(&entry->value);
        if(sorted_set == nullptr) return {RequestStatus::WrongType, 0};
        const auto score = (*sorted_set)->score(member);
        if(!score) return {RequestStatus::Missing, 0};
        return {RequestStatus::OK, *score};
    }

    RequestResult<std::vector<Member>> zrange(std::string_view key, std::int64_t start, std::int64_t end){
        Entry* entry = data.find(key);
        if(entry == nullptr) return {RequestStatus::OK, {}};
        auto* sorted_set = std::get_if<SortedSet*>(&entry->value);
        if(sorted_set == nullptr) return {RequestStatus::WrongType, {}};
        return {RequestStatus::OK, (*sorted_set)->range(start, end)};
    }

    RequestResult<std::size_t> zrank(std::string_view key, std::string_view member){
        Entry* entry = data.find(key);
        if(entry == nullptr) return {RequestStatus::Missing, 0};
        auto* sorted_set = std::get_if<SortedSet*>(&entry->value);
        if(sorted_set == nullptr) return {RequestStatus::WrongType, 0};
        const auto rank = (*sorted_set)->rank(member);
        if(!rank) return {RequestStatus::Missing, 0};
        return {RequestStatus::OK, *rank};
    }


    ~Database(){
        data.clear([](Entry* entry) noexcept {
            if(auto* sorted_set = std::get_if<SortedSet*>(&entry->value)){
                delete *sorted_set;
            }
            delete entry;
        });
    }
};



inline Database Database;
