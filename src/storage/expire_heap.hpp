#pragma once

#include "entry.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>



struct HeapItem{
    Entry* map_entry;
    std::int64_t expires_time;

    HeapItem(Entry* _entry, std::int64_t _expire_time): map_entry(_entry), expires_time(_expire_time) {}
};



class ExpireHeap{
private:
    std::vector<HeapItem> heap;


    void swap(std::size_t index_a, std::size_t index_b){
        std::swap(heap[index_a], heap[index_b]);

        heap[index_a].map_entry->heap_idx = index_a;
        heap[index_b].map_entry->heap_idx = index_b;
    }

    // n -> 2n+1, 2n+2
    void adjust_up(std::size_t index){
        std::size_t parent = (index - 1) >> 1;
        while(index > 0 && heap[parent].expires_time > heap[index].expires_time){
            swap(index, parent);
            index = parent;
            parent = (index - 1) >> 1;
        }
    }

    void adjust_down(std::size_t index){
        while(true){
            std::size_t small = index;

            std::size_t left = (small << 1) + 1;
            if(left >= heap.size()) break;
            small = heap[left].expires_time < heap[small].expires_time ? left : small;
            std::size_t right = left + 1;
            if(right >= heap.size()){
                if(small != index) swap(small, index);
                break;
            }
            small = heap[right].expires_time < heap[small].expires_time ? right : small;

            if(small == index){
                break;
            }else{
                swap(small, index);
                index = small;
            }
        }
    }

public:
    ExpireHeap() = default;


    void set(Entry* entry, std::int64_t _expire_time){
        if(entry->heap_idx == NO_HEAP_INDEX){
            heap.push_back({entry, _expire_time});
            std::size_t index = heap.size() - 1;
            entry->heap_idx = index;
            adjust_up(index);
        }else{
            std::size_t index = entry->heap_idx;
            std::int64_t original = heap[index].expires_time;
            heap[index].expires_time = _expire_time;
            if(original > _expire_time){
                adjust_up(index);
            }
            if(original < _expire_time){
                adjust_down(index);
            }
        }
    }

    bool remove(Entry* entry){
        std::size_t index = entry->heap_idx;

        if(index == NO_HEAP_INDEX){
            return false;
        }

        swap(index, heap.size() - 1);
        heap.pop_back();
        entry->heap_idx = NO_HEAP_INDEX;

        if(index == heap.size()){
            return true;
        }

        if(index > 0 && heap[(index - 1) >> 1].expires_time > heap[index].expires_time){
            adjust_up(index);
        }else{
            adjust_down(index);
        }

        return true;
    }

    const HeapItem* top() const{
        return !heap.empty() ? &heap[0] : nullptr;
    }

    std::optional<std::int64_t> get_expire_time(const Entry* entry) const{
        const std::size_t idx = entry->heap_idx;
        if(idx == NO_HEAP_INDEX) return std::nullopt;

        return heap[idx].expires_time;
    }
};
