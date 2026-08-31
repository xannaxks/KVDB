// Records are ordered by (user key ascending, sequence descending). Insertion
// uses the ordinary BST path, then splay rotations to keep tree balanced.
// Because newer versions sort first within one user key, lookup can
// stop at the first matching internal record.
#include "splay_tree.h"
#include <memory>
#include <cassert>
#include <format>

SplayTree::Node::Node(ArenaEntry key, ArenaEntry value, Type record_type, uint64_t sequence_number)
    : VirtualNode(key, value, record_type, sequence_number),
    left(nullptr),
    right(nullptr),
    parent(nullptr)
{
}

std::size_t SplayTree::Node::approximate_memory_usage() const
{
    return sizeof(Node) + this->key_entry.size + this->value_entry.size;
}

void SplayTree::left_rotate(Node* v)
{
    if (v == nullptr || v->right == nullptr) return;

    Node* u = v->right;

    v->right = u->left;
    if (u->left != nullptr)
        u->left->parent = v;

    u->parent = v->parent;
    if (v->parent == nullptr)
        root = u;
    else if (v == v->parent->left)
        v->parent->left = u;
    else
        v->parent->right = u;

    u->left = v;
    v->parent = u;
}

void SplayTree::right_rotate(Node* v)
{
    if (v == nullptr || v->left == nullptr) return;

    Node* u = v->left;

    v->left = u->right;
    if (u->right != nullptr)
        u->right->parent = v;

    u->parent = v->parent;
    if (v->parent == nullptr)
        root = u;
    else if (v == v->parent->left)
        v->parent->left = u;
    else
        v->parent->right = u;

    u->right = v;
    v->parent = u;
}

::Status SplayTree::bst_insert(Node* v)
{
    Node* current = root;
    Node* parent = nullptr;

    while (current != nullptr)
    {
        parent = current;

        if (!(*v < *current) && !(*current < *v))
        {
            return ::Status{
                StatusCode::Duplicate,
                std::format("Duplicate entry for key: {}", v->key_entry)
            };
        }

        if (*v < *current)
            current = current->left;
        else
            current = current->right;
    }

    v->parent = parent;

    if (parent == nullptr)
        root = v;
    else {
        if (*v < *parent)
            parent->left = v;
        else
            parent->right = v;
    }

    splay(v);

    return Status::ok();
}

void SplayTree::destroy(Node* node)
{
    if (node == nullptr) return;
    destroy(node->left);
    destroy(node->right);
    delete node;
}

std::size_t SplayTree::approximate_subtree_memory_usage(Node* node) const
{
    if (node == nullptr)
        return 0;
    return approximate_subtree_memory_usage(node->left) + approximate_subtree_memory_usage(node->right) + node->approximate_memory_usage();
}

void SplayTree::inorder_traverse(std::vector<const Node*>& collect) const
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
std::size_t SplayTree::approximate_memory_usage() const
{
    return approximate_subtree_memory_usage(root);
}

SplayTree::SplayTree()
    : root(nullptr)
{
}

SplayTree::~SplayTree()
{
    destroy(root);
}

Status SplayTree::insert(const InternalRecord& entry)
{
    try
    {
        std::unique_ptr<SplayTree::Node> new_node;
        new_node = std::make_unique<SplayTree::Node>(entry.key_entry, entry.value_entry, entry.type, entry.seq_num);
        SplayTree::Node* raw = new_node.get();
        ::Status insert_res = bst_insert(raw);
        if (!insert_res.is_ok())
            return insert_res;

        new_node.release();
        return Status::ok();
    }
    catch (const std::bad_alloc&)
    {
        return Status{ StatusCode::OutOfMemory, "Failed to allocate memory for new node" };
    }
    catch (...)
    {
        return Status{ StatusCode::InsertionFailed, "An unknown error occurred" };
    }
}

Result<std::optional<InternalRecord>> SplayTree::find_latest_by_key(ArenaEntry key)
{
    Node* current = root;
    Node* result = nullptr;
    Node* last = nullptr;

    while (current != nullptr)
    {
        last = current;

        if (current->key_entry < key)
        {
            current = current->right;
        }
        else
        {
            if (current->key_entry == key)
                result = current;
            current = current->left;
        }
    }

    if (result == nullptr)
    {
        if (last)
            splay(last);

        return Result<std::optional<InternalRecord>>::ok(std::nullopt);
    }

    splay(result);

    return Result<std::optional<InternalRecord>>::ok(InternalRecord(result->key_entry, result->value_entry, result->type, result->seq_number));
}

Result<std::optional<InternalRecord>> SplayTree::find_latest_by_key(ArenaEntry key) const
{
    Node* current = root;
    Node* result = nullptr;
    Node* last = nullptr;

    while (current != nullptr)
    {
        last = current;

        if (current->key_entry < key)
        {
            current = current->right;
        }
        else
        {
            if (current->key_entry == key)
                result = current;
            current = current->left;
        }
    }

    if (result == nullptr)
        return Result<std::optional<InternalRecord>>::ok(std::nullopt);

    return Result<std::optional<InternalRecord>>::ok(InternalRecord(result->key_entry, result->value_entry, result->type, result->seq_number));
}

// Validators implementation
bool SplayTree::validate() const
{
    return (
        SplayTree::bst_ordering_correct() &&
        SplayTree::expect_parent_links_valid(root, nullptr)
    );
}

bool SplayTree::bst_ordering_correct() const
{
    InorderIterator it(root);
    Node* prev = nullptr;

    while (it.has_next())
    {
        Node* cur = it.next();
        if (prev != nullptr && !(*prev < *cur))
            return false;
        prev = cur;
    }
    return true;
}

// SplayTree Iterator implementation
void SplayTree::InorderIterator::push_left(Node* node)
{
    while (node)
    {
        st.push(node);
        node = node->left;
    }
}

SplayTree::InorderIterator::InorderIterator(Node* root)
{
    push_left(root);
}

bool SplayTree::InorderIterator::has_next()
{
    return !st.empty();
}

SplayTree::Node* SplayTree::InorderIterator::next()
{
    assert(!st.empty());
    Node* cur = st.top();
    st.pop();
    if (cur->right)
    {
        push_left(cur->right);
    }
    return cur;
}

SplayTree::Node* SplayTree::root_getter() noexcept
{
    return this->root;
}

void SplayTree::dump_inorder(std::vector<InternalRecord>& out) const
{
    InorderIterator it(this->root);
    while (it.has_next())
    {
        auto node = it.next();
        out.emplace_back(node->key_entry, node->value_entry, node->type, node->seq_number);
    }
}

bool SplayTree::expect_parent_links_valid(SplayTree::Node* node, SplayTree::Node* expected_parent)
{
    if (node == nullptr)
        return true;

    if (node->parent != expected_parent)
        return false;

    return (expect_parent_links_valid(node->right, node) && expect_parent_links_valid(node->left, node));
}

bool SplayTree::empty() const noexcept
{
    return root == nullptr;
}

void SplayTree::splay(Node* v)
{
    if (!v)
        return;

    while (v->parent != nullptr)
    {
        Node* p = v->parent;
        Node* g = p->parent;

        // Zig
        if (g == nullptr)
        {
            if (p->left == v)
                right_rotate(p);
            else
                left_rotate(p);

            continue;
        }

        // Zig-zig: left-left
        if (g->left == p && p->left == v)
        {
            right_rotate(g);
            right_rotate(p);
            continue;
        }

        // Zig-zig: right-right
        if (g->right == p && p->right == v)
        {
            left_rotate(g);
            left_rotate(p);
            continue;
        }

        // Zig-zag: right-left
        if (g->right == p && p->left == v)
        {
            right_rotate(p);
            left_rotate(g);
            continue;
        }

        // Zig-zag: left-right
        if (g->left == p && p->right == v)
        {
            left_rotate(p);
            right_rotate(g);
            continue;
        }
    }
}