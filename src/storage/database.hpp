#pragma once

#include <string>
#include <string_view>
#include <variant>
#include <cmath>
#include <cstdint>
#include <vector>
#include <optional>
#include <limits>

#include "progressive_rehash_map.hpp"
#include "sorted_set.hpp"
#include "entry.hpp"
#include "expire_heap.hpp"

#include "../util.hpp"

enum class RequestStatus{
    OK,
    Missing,
    WrongType,
    InvalidScore,
    InvalidExpire
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
    ExpireHeap expire_heap;

    Entry* find_live_entry(std::string_view key, std::int64_t now_ms){
        Entry* entry = data.find(key);
        if(entry == nullptr) return nullptr;

        const auto expires_at = expire_heap.get_expire_time(entry);
        if(expires_at && *expires_at <= now_ms){
            erase_entry(entry);
            return nullptr;
        }
        return entry;
    }

    void erase_entry(Entry* entry){
        data.extract(entry->key);
        expire_heap.remove(entry);
        if(auto* sorted_set = std::get_if<SortedSet*>(&entry->value)){
            delete *sorted_set;
        }
        delete entry;
    }

public:
    Database(): data() {}

    GetResult get(std::string_view key){
        const Entry* entry = find_live_entry(key, monotonic_ms());
        if(entry == nullptr) return {RequestStatus::Missing, nullptr};

        const auto* value = std::get_if<std::string>(&entry->value);
        if(value == nullptr) return {RequestStatus::WrongType, nullptr};
        return {RequestStatus::OK, value};
    }

    RequestStatus set(std::string_view key, std::string_view value,
                      std::optional<std::int64_t> ttl_ms = std::nullopt){
        const std::int64_t now = monotonic_ms();
        if(ttl_ms && (*ttl_ms <= 0 || *ttl_ms > std::numeric_limits<std::int64_t>::max() - now)){
            return RequestStatus::InvalidExpire;
        }

        std::string replacement(value);
        if(Entry* entry = find_live_entry(key, now)){
            if(ttl_ms) expire_heap.set(entry, now + *ttl_ms);
            else expire_heap.remove(entry);

            SortedSet* old_set = nullptr;
            if(auto* sorted_set = std::get_if<SortedSet*>(&entry->value)) old_set = *sorted_set;
            entry->value = std::move(replacement);
            delete old_set;
        }else{
            entry = new Entry(key, std::move(replacement));
            data.insert(entry);
            if(ttl_ms) expire_heap.set(entry, now + *ttl_ms);
        }
        return RequestStatus::OK;
    }

    bool del(std::string_view key){
        Entry* entry = find_live_entry(key, monotonic_ms());
        if(entry == nullptr) return false;
        erase_entry(entry);
        return true;
    }

    RequestResult<std::size_t> zadd(std::string_view key, const std::vector<Member>& members){
        for(const auto& member : members){
            if(std::isnan(member.score)) return {RequestStatus::InvalidScore, 0};
        }
        if(members.empty()) return {RequestStatus::OK, 0};

        Entry* entry = find_live_entry(key, monotonic_ms());
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
        Entry* entry = find_live_entry(key, monotonic_ms());
        if(entry == nullptr) return {RequestStatus::OK, 0};
        auto* sorted_set = std::get_if<SortedSet*>(&entry->value);
        if(sorted_set == nullptr) return {RequestStatus::WrongType, 0};

        std::size_t removed = 0;
        for(const auto member : members){
            removed += (*sorted_set)->remove(member);
        }
        if((*sorted_set)->size() == 0) erase_entry(entry);
        return {RequestStatus::OK, removed};
    }

    RequestResult<double> zscore(std::string_view key, std::string_view member){
        Entry* entry = find_live_entry(key, monotonic_ms());
        if(entry == nullptr) return {RequestStatus::Missing, 0};
        auto* sorted_set = std::get_if<SortedSet*>(&entry->value);
        if(sorted_set == nullptr) return {RequestStatus::WrongType, 0};
        const auto score = (*sorted_set)->score(member);
        if(!score) return {RequestStatus::Missing, 0};
        return {RequestStatus::OK, *score};
    }

    RequestResult<std::vector<Member>> zrange(std::string_view key, std::int64_t start, std::int64_t end){
        Entry* entry = find_live_entry(key, monotonic_ms());
        if(entry == nullptr) return {RequestStatus::OK, {}};
        auto* sorted_set = std::get_if<SortedSet*>(&entry->value);
        if(sorted_set == nullptr) return {RequestStatus::WrongType, {}};
        return {RequestStatus::OK, (*sorted_set)->range(start, end)};
    }

    RequestResult<std::size_t> zrank(std::string_view key, std::string_view member){
        Entry* entry = find_live_entry(key, monotonic_ms());
        if(entry == nullptr) return {RequestStatus::Missing, 0};
        auto* sorted_set = std::get_if<SortedSet*>(&entry->value);
        if(sorted_set == nullptr) return {RequestStatus::WrongType, 0};
        const auto rank = (*sorted_set)->rank(member);
        if(!rank) return {RequestStatus::Missing, 0};
        return {RequestStatus::OK, *rank};
    }

    RequestResult<bool> pexpire(std::string_view key, std::int64_t ttl_ms){
        const std::int64_t now = monotonic_ms();
        if(ttl_ms > std::numeric_limits<std::int64_t>::max() - now){
            return {RequestStatus::InvalidExpire, false};
        }
        Entry* entry = find_live_entry(key, now);
        if(entry == nullptr) return {RequestStatus::OK, false};

        if(ttl_ms <= 0) erase_entry(entry);
        else expire_heap.set(entry, now + ttl_ms);
        return {RequestStatus::OK, true};
    }

    std::int64_t pttl(std::string_view key){
        const std::int64_t now = monotonic_ms();
        Entry* entry = find_live_entry(key, now);
        if(entry == nullptr) return -2;
        const auto expires_at = expire_heap.get_expire_time(entry);
        return expires_at ? *expires_at - now : -1;
    }

    bool persist(std::string_view key){
        Entry* entry = find_live_entry(key, monotonic_ms());
        return entry != nullptr && expire_heap.remove(entry);
    }

    std::size_t expire_due(std::int64_t now_ms, std::size_t budget = DEFAULT_EXPIRE_BUDGET){
        std::size_t removed = 0;
        while(removed < budget){
            const HeapItem* item = expire_heap.top();
            if(item == nullptr || item->expires_time > now_ms) break;
            erase_entry(item->map_entry);
            ++removed;
        }
        return removed;
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
