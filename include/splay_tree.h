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
#include <random>
#include "type.h"
#include "record.h"
#include "arena.h"
#include "driver.h"

class MemTable;

/**
 * @brief Splay Tree containing versioned InternalRecord values.
 *
 * Nodes are ordered first by user key and then by descending sequence number,
 * allowing the first match for a key to represent its newest version. Standard
 * splay (zig and zag) rotations keep lookup and insertion logarithmic.
 *
 * @note Nodes reference ArenaEntry storage but do not own those bytes.
 */
class SplayTree : public Driver
{
public:

    struct Node : public ::VirtualNode
    {
        Node* left;
        Node* right;
        Node* parent;

        Node(ArenaEntry key_entry, ArenaEntry value_entry, Type type, uint64_t seq_num);

        std::size_t approximate_memory_usage() const override;
    };

private:
    Node* root = nullptr;

    void splay(Node* v);
    ::Status bst_insert(Node* v);
    void left_rotate(Node* v);
    void right_rotate(Node* v);
    void destroy(Node* node);

    // @brief Dumps nodes
    void inorder_traverse(std::vector<const Node*>& collect) const;

public:
    SplayTree();
    ~SplayTree() override;

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
     * @brief Inserts one versioned record, and splay.
     * @callgraph
     */
    ::Status insert(const InternalRecord& entry) override;
    /** @brief Returns the highest-sequence record for @p key, if present. Splays the last visited node. */
    Result<std::optional<InternalRecord>> find_latest_by_key(ArenaEntry key) override;
    Result<std::optional<InternalRecord>> find_latest_by_key(ArenaEntry key) const override;

    bool bst_ordering_correct() const;
    /** @brief Checks bst and parent links invariants. */
    bool validate() const override;

    size_t approximate_subtree_memory_usage(Node* node) const;
    size_t approximate_memory_usage() const override;

    bool empty() const noexcept override;
    Node* root_getter() noexcept;

    /** @brief Appends all records to @p out in internal-key order. */
    void dump_inorder(std::vector<InternalRecord>& out) const override;

    static bool expect_parent_links_valid(SplayTree::Node* node, SplayTree::Node* expected_parent);

    friend class MemTable;
};
