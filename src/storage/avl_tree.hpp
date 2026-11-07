#pragma once

#include "node.hpp"
#include <cstddef>


class AVLTree{
private:
    AVLHook* root;

    
    void replace_node(AVLHook* old_node, AVLHook* new_node){
        AVLHook* c_parent = old_node->parent;
        if(c_parent == nullptr){
            root = new_node;
        }else if(c_parent->left == old_node){
            c_parent->left = new_node;
        }else{
            c_parent->right = new_node;
        }
        if(new_node != nullptr) new_node->parent = c_parent;
    }

    const AVLHook* select_with_node(std::size_t rank, const AVLHook* node) const{
        if(node == nullptr) return nullptr;
        const std::size_t left_cnt = AVLHook::get_count(node->left);
        if(left_cnt < rank){
            return select_with_node(rank - left_cnt - 1, node->right);
        }else if(left_cnt > rank){
            return select_with_node(rank, node->left);
        }else{
            return node;
        }
    }
public:
    AVLTree(): root(nullptr) {}


    // Compare -> true: node go to left side
    template<typename Compare>
    void insert(AVLHook* node, const Compare& compare){
        if(root == nullptr){
            root = node;
            return;
        }

        AVLHook* current = root;

        while(true){
            AVLHook* next;
            bool is_left;
            if(compare(node, current)){
                next = current->left;
                is_left = true;
            }else{
                next = current->right;
                is_left = false;
            }

            if(next != nullptr){
                current = next;
            }else{
                if(is_left){
                    current->left = node;
                    node->parent = current;
                }else{
                    current->right = node;
                    node->parent = current;
                }

                root = node->rebalance();
                return;
            }
        }
    }

    AVLHook* extract(AVLHook* node){
        if(node->left == nullptr && node->right == nullptr){
            AVLHook* c_parent = node->parent;
            replace_node(node, nullptr);
            if(c_parent != nullptr) root = c_parent->rebalance();

            node->parent = nullptr;
            return node;
        }

        if((!node->left) || (!node->right)){
            if(node->left){
                replace_node(node, node->left);
                root = node->left->rebalance();
                node->left = nullptr;
            }else{
                replace_node(node, node->right);
                root = node->right->rebalance();
                node->right = nullptr;
            }
            node->parent = nullptr;
            node->update();
            return node;
        }

        AVLHook* next = node->next();
        AVLHook* rebalance_from = next;
        if(next->parent != node){
            rebalance_from = next->parent;
            replace_node(next, next->right);
            next->right = node->right;
            next->right->parent = next;
        }
        replace_node(node, next);
        next->left = node->left;
        next->left->parent = next;
        root = rebalance_from->rebalance();

        node->parent = nullptr;
        node->left = nullptr;
        node->right = nullptr;
        node->update();
        return node;
    }

    const AVLHook* select(std::size_t rank) const{
        return select_with_node(rank, root);
    }

    std::size_t rank(const AVLHook* node) const{
        std::size_t count = AVLHook::get_count(node->left);

        const AVLHook* current = node;
        while(current->parent != nullptr){
            AVLHook* c_parent = current->parent;
            if(c_parent->right == current){
                count += 1 + AVLHook::get_count(c_parent->left);
            }
            current = c_parent;
        }

        return count;
    }

    std::size_t size() const{
        return root != nullptr ? root->count : 0;
    }

};
