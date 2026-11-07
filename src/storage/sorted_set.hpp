#pragma once

#include "avl_tree.hpp"
#include "node.hpp"
#include "progressive_rehash_map.hpp"


using HashEntry = HashNode<double>;


class SortedSet{
private:
    AVLTree avl_tree;
    ProgressiveRehashMap<HashEntry> hash_map;
};