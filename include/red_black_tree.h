#pragma once

#include <variant>
#include <vector>
#include <string>
#include <cstdint>
#include "bytes.h"
#include <stack>
#include <functional>
#include "type.h"
#include "record.h"
#include "arena.h"

class MemTable;

class RBTree
{
public:
    //inline static uint64_t seq_cnt = 1;

    enum class Status
    {
        OK,
        KeyNotFound,
        MemoryAllocationFailed,
        KeyWasDeleted,
    };

    struct Node
    {
        enum class Color
        {
            Red,
            Black
        };

        ArenaEntry key_entry;
        ArenaEntry value_entry;
        const uint64_t seq_number;
        Type type;
        Color color;
        Node* left;
        Node* right;
        Node* parent;

        Node(ArenaEntry key_entry, ArenaEntry value_entry, Type type, uint64_t seq_num);

        bool operator<(const Node& other) const;
        bool operator>(const Node& other) const;

        size_t approximate_memory_usage() const;
    };

private:
    Node* root;

    void left_rotate(Node* v);
    void right_rotate(Node* v);
    void balance(Node* v);
    void bst_insert(Node* v);
    void destroy(Node* node);

    template<typename Collection>
    void inorder_traverse(Collection& collect) const
    {
        std::function<void(const Node*)> traverse = [&](const Node* current)
        {
            if (current == nullptr)
                return;

            traverse(current->left);
            collect.emplace_back(current); 
            traverse(current->right);
        };

        traverse(root);
    }

public:
    RBTree();
    ~RBTree();

    RBTree(const RBTree&) = delete;
    RBTree& operator=(const RBTree&) = delete;

    class InorderIterator
    {
    private:
        std::stack<Node*> st;

        void push_left(Node* node);

    public:
        InorderIterator(Node* root);

        bool has_next();
        Node* next();
    };

    Status insert(const InternalRecord& entry);
    std::variant<InternalRecord, RBTree::Status> find_latest_by_key(ArenaEntry key) const;

    bool root_is_black() const;
    bool no_red_node_has_red_child() const;
    bool bst_ordering_correct() const;
    bool black_height_is_consistent() const;
    std::pair<bool,int> subtree_black_height_info(Node* v) const;
    bool validate() const;
    bool subtree_has_no_red_node_with_red_child(Node* node) const;
    size_t approximate_subtree_memory_usage(Node* node) const;
    size_t approximate_memory_usage() const;

    Node* root_getter();
    void dump_inorder(std::vector<InternalRecord>& out) const; 

    friend class MemTable;
};