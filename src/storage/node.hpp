#pragma once


#include <algorithm>
#include <cstddef>
#include <ctime>
#include <string>
#include <string_view>
#include <utility>



struct HashHook{
    std::size_t hash;
    HashHook* next;

    HashHook(size_t _hash, HashHook* _next): hash(_hash), next(_next){}
};




template<typename Value>
struct HashNode : HashHook{
    std::string key;
    Value value;

    template<typename T>
    HashNode(std::string_view _key, T&& _value): HashHook(0, nullptr), key(_key), value(std::forward<T>(_value)){}
};





struct AVLHook{
    AVLHook* parent;

    AVLHook* left;
    AVLHook* right;

    std::size_t count;

    std::size_t height;


    AVLHook(AVLHook* _parent = nullptr, AVLHook* _left = nullptr, AVLHook* _right = nullptr, std::size_t _count = 1, std::size_t _height = 1):
    parent(_parent), left(_left), right(_right), count(_count), height(_height) {}


    void update(){
        count = 1 + get_count(left) + get_count(right);
        height = 1 + std::max(get_height(left), get_height(right));
    }

    // 外部控制流会确保此处right != nullptr
    AVLHook* rotate_left(){
        AVLHook* ret = right;
        right = ret->left;
        if(right != nullptr) right->parent = this;
        ret->left = this;
        ret->parent = this->parent;
        this->parent = ret;
        update();
        ret->update();
        return ret;
    }

    // 外部控制流会确保此处left != nullptr
    AVLHook* rotate_right(){
        AVLHook* ret = left;
        left = ret->right;
        if(left != nullptr) left->parent = this;
        ret->right = this;
        ret->parent = this->parent;
        this->parent = ret;
        update();
        ret->update();
        return ret;
    }

    AVLHook* rebalance(){
        AVLHook* current = this;

        while(true){
            AVLHook* old_root = current;
            AVLHook* parent = current->parent;
            current->update();

            const std::size_t left_height = get_height(current->left);
            const std::size_t right_height = get_height(current->right);

            if(left_height > right_height + 1){
                AVLHook* temp = current->left;
                if(get_height(temp->left) >= get_height(temp->right)){
                    current = current->rotate_right();
                }else{
                    current->left = current->left->rotate_left();
                    current = current->rotate_right();
                }
            }

            if(left_height + 1 < right_height){
                AVLHook* temp = current->right;
                if(get_height(temp->left) <= get_height(temp->right)){
                    current = current->rotate_left();
                }else{
                    current->right = current->right->rotate_right();
                    current = current->rotate_left();
                }
            }

            if(parent == nullptr){
                return current;
            }

            if(parent->left == old_root){
                parent->left = current;
            }else{
                parent->right = current;
            }

            current = parent;
        }
    }

    AVLHook* next(){
        if(right == nullptr){
            AVLHook* current = this;
            while(current->parent != nullptr && current == current->parent->right){
                current = current->parent;
            }
            return current->parent;
        }

        AVLHook* current = right;

        while(current->left != nullptr){
            current = current->left;
        }

        return current;
    }

    
    static std::size_t get_height(const AVLHook* avl_hook){
        return avl_hook != nullptr ? avl_hook->height : 0;
    }

    static std::size_t get_count(const AVLHook* avl_hook){
        return avl_hook != nullptr ? avl_hook->count : 0;
    }
};





struct AVLNode: public AVLHook, public HashHook{
private:
    std::string key;
    double score;

public:
    AVLNode(std::string_view _key, double _score, std::size_t _hash): 
    HashHook(_hash, nullptr), key(_key), score(_score) {}
};
