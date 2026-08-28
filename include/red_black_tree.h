/**
 * @file red_black_tree.h
 * @brief Ordered MemTable backend keyed by user key and sequence number.
 */
#pragma once

#include <variant>
#include <vector>
#include <string>
#include <cstdint>
#include "status.h"
#include <stack>
#include <functional>
#include "type.h"
#include "record.h"
#include "arena.h"
#include "driver.h"

class MemTable;

/**
 * @brief Red-black tree containing versioned InternalRecord values.
 *
 * Nodes are ordered first by user key and then by descending sequence number,
 * allowing the first match for a key to represent its newest version. Standard
 * red-black rotations and recoloring keep lookup and insertion logarithmic.
 *
 * @note Nodes reference ArenaEntry storage but do not own those bytes.
 */
class RBTree : public Driver
{
public:
    //inline static uint64_t seq_cnt = 1;

    struct Node : public ::VirtualNode
    {
        enum class Color
        {
            Red,
            Black
        };

        Color color;
        Node* left;
        Node* right;
        Node* parent;

        Node(ArenaEntry key_entry, ArenaEntry value_entry, Type type, uint64_t seq_num);
    
        std::size_t approximate_memory_usage() const override;
    };

private:
    Node* root = nullptr;

    void left_rotate(Node* v);
    void right_rotate(Node* v);
    void balance(Node* v);
    ::Status bst_insert(Node* v);
    void destroy(Node* node);

    void inorder_traverse(std::vector<const Node*>& collect) const;

public:
    RBTree();
    ~RBTree() override;

    /** @brief Non-owning iterator over records in internal-key order. */
	class InorderIterator : public ::VirtualInorderIterator
    {
    private:
        std::stack<Node*> st;

        void push_left(Node* node);

    public:
        InorderIterator(Node* root);

        bool has_next() override;
        Node* next() override;
    };

    /**
     * @brief Inserts one versioned record and restores red-black invariants.
     * @callgraph
     */
    ::Status insert(const InternalRecord& entry) override;
    /** @brief Returns the highest-sequence record for @p key, if present. */
    Result<std::optional<InternalRecord>> find_latest_by_key(ArenaEntry key) const override;
    //[[nodiscard]] std::optional<InternalRecord> try_find_latest_by_key(
    //    const ArenaEntry & key
    //) const;

    bool root_is_black() const;
    bool no_red_node_has_red_child() const;
    bool bst_ordering_correct() const;
    bool black_height_is_consistent() const;
    std::pair<bool,int> subtree_black_height_info(Node* v) const;
    /** @brief Checks ordering, parent links, colors, and black-height invariants. */
    bool validate() const override;
    bool subtree_has_no_red_node_with_red_child(Node* node) const;
    size_t approximate_subtree_memory_usage(Node* node) const;
    size_t approximate_memory_usage() const override;
    bool empty() const noexcept override;

    Node* root_getter();
    /** @brief Appends all records to @p out in internal-key order. */
    void dump_inorder(std::vector<InternalRecord>& out) const override; 

    static bool expect_parent_links_valid(RBTree::Node* node, RBTree::Node* expected_parent);

    friend class MemTable;
};
