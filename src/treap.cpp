// Records are ordered by (user key ascending, sequence descending). Insertion
// uses the ordinary BST path, then rotations and recoloring restore red-black
// invariants. Because newer versions sort first within one user key, lookup can
// stop at the first matching internal record.
#include "treap.h"
#include <memory>
#include <cassert>
#include <format>

std::mt19937_64 Treap::rng{ std::random_device{}() };

Treap::Node::Node(ArenaEntry key, ArenaEntry value, Type record_type, uint64_t sequence_number)
    : VirtualNode(key, value, record_type, sequence_number),
    priority(Treap::random_priority()),
    left(nullptr),
    right(nullptr),
    parent(nullptr)
{
}

std::size_t Treap::Node::approximate_memory_usage() const
{
    return sizeof(Node) + this->key_entry.size + this->value_entry.size;
}

void Treap::left_rotate(Node* v)
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

void Treap::right_rotate(Node* v)
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

::Status Treap::bst_insert(Node* v)
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
    {
        root = v;
        return Status::ok();
    }

    if (*v < *parent)
        parent->left = v;
    else
        parent->right = v;

    // Bubble up according to priority.
    while (v->parent != nullptr &&
        v->priority > v->parent->priority)
    {
        Node* p = v->parent;

        if (v == p->left)
            right_rotate(p);
        else
            left_rotate(p);
    }

    return Status::ok();
}

void Treap::destroy(Node* node)
{
    if (node == nullptr) return;
    destroy(node->left);
    destroy(node->right);
    delete node;
}

std::size_t Treap::approximate_subtree_memory_usage(Node* node) const
{
    if (node == nullptr)
        return 0;
    return approximate_subtree_memory_usage(node->left) + approximate_subtree_memory_usage(node->right) + node->approximate_memory_usage();
}

void Treap::inorder_traverse(std::vector<const Node*>& collect) const
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
std::size_t Treap::approximate_memory_usage() const
{
    return approximate_subtree_memory_usage(root);
}

Treap::Treap()
    : root(nullptr)
{
}

Treap::~Treap()
{
    destroy(root);
}

Status Treap::insert(const InternalRecord& entry)
{
    try
    {
        std::unique_ptr<Treap::Node> new_node;
        new_node = std::make_unique<Treap::Node>(entry.key_entry, entry.value_entry, entry.type, entry.seq_num);
        Treap::Node* raw = new_node.get();
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

Result<std::optional<InternalRecord>> Treap::find_latest_by_key(ArenaEntry key) const
{
    Node* current = root;
    Node* result = nullptr;

    while (current != nullptr)
    {
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
bool Treap::validate() const
{
    return (
        Treap::heap_ordering_correct() &&
        Treap::bst_ordering_correct() &&
        Treap::expect_parent_links_valid(root, nullptr)
    );
}

bool Treap::bst_ordering_correct() const
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

bool Treap::heap_ordering_correct() const
{
    std::function<bool(Node*)> check = [&](Node* v)
    {
        if (!v)
            return true;

        if (v->left && v->left->priority > v->priority)
            return false;
        if (v->right && v->right->priority > v->priority)
            return false;

        if ((check(v->left) == true) && (check(v->right) == true))
            return true;
        return false;
    };

    return check(root);
}

// Treap Iterator implementation
void Treap::InorderIterator::push_left(Node* node)
{
    while (node)
    {
        st.push(node);
        node = node->left;
    }
}

Treap::InorderIterator::InorderIterator(Node* root)
{
    push_left(root);
}

bool Treap::InorderIterator::has_next()
{
    return !st.empty();
}

Treap::Node* Treap::InorderIterator::next()
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

Treap::Node* Treap::root_getter() noexcept
{
    return this->root;
}

void Treap::dump_inorder(std::vector<InternalRecord>& out) const
{
    InorderIterator it(this->root);
    while (it.has_next())
    {
        auto node = it.next();
        out.emplace_back(node->key_entry, node->value_entry, node->type, node->seq_number);
    }
}

bool Treap::expect_parent_links_valid(Treap::Node* node, Treap::Node* expected_parent)
{
    if (node == nullptr)
        return true;

    if (node->parent != expected_parent)
        return false;

    return (expect_parent_links_valid(node->right, node) && expect_parent_links_valid(node->left, node));
}

bool Treap::empty() const noexcept
{
    return root == nullptr;
}


std::uint64_t Treap::random_priority()
{
    return Treap::rng();
}