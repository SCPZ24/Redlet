#pragma once


#include "avl_tree.hpp"
#include "node.hpp"
#include "progressive_rehash_map.hpp"
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>


struct Member{
    std::string_view member;
    double score;
};


class SortedSet{
private:
    AVLTree avl_tree;
    ProgressiveRehashMap<AVLNode> hash_map;

public:
    bool add(std::string_view key, double score){
        AVLNode* entry = hash_map.find(key);

        auto compare = [](const AVLHook* h1, const AVLHook* h2) -> bool{
            const auto* left = static_cast<const AVLNode*>(h1);
            const auto* right = static_cast<const AVLNode*>(h2);
            if(left->score != right->score){
                return left->score < right->score;
            }
            return left->key < right->key;
        };

        if(entry == nullptr){
            entry = new AVLNode(key, score);

            hash_map.insert(entry);
            avl_tree.insert(entry, compare);

            return true;
        }else{
            AVLNode* temp = static_cast<AVLNode*>(avl_tree.extract(entry));
            temp->score = score;
            avl_tree.insert(temp, compare);

            return false;
        }
    }

    bool remove(std::string_view key){
        AVLNode* entry = hash_map.extract(key);

        if(entry == nullptr){
            return false;
        }else{
            AVLNode* temp = static_cast<AVLNode*>(avl_tree.extract(entry));
            delete temp;

            return true;
        }
    }

    std::optional<double> score(std::string_view key){
        AVLNode* entry = hash_map.find(key);

        if(entry == nullptr){
            return std::nullopt;
        }else{
            return entry->score;
        }
    }

    std::vector<Member> range(std::int64_t start, std::int64_t end){
        std::vector<Member> result;
        const std::int64_t count = static_cast<std::int64_t>(avl_tree.size());
        if(count == 0) return result;

        if(start < 0) start += count;
        if(end < 0) end += count;
        if(start < 0) start = 0;
        if(end >= count) end = count - 1;
        if(start > end) return result;

        result.reserve(static_cast<std::size_t>(end - start + 1));
        const AVLHook* current = avl_tree.select(static_cast<std::size_t>(start));
        for(std::int64_t rank = start; rank <= end; ++rank){
            const auto* entry = static_cast<const AVLNode*>(current);
            result.push_back({entry->key, entry->score});
            current = current->next();
        }
        return result;
    }

    std::optional<std::size_t> rank(std::string_view key){
        AVLNode* entry = hash_map.find(key);

        if(entry == nullptr){
            return std::nullopt;
        }else{
            return avl_tree.rank(entry);
        }
    }

    std::size_t size() const{
        return avl_tree.size();
    }

    ~SortedSet(){
        hash_map.clear([](AVLNode* node) noexcept {
            delete node;
        });
    }
};
