#include "red_black_tree.h"
#include <memory>
#include <cassert>

//RBTree::Node::Node(const Bytes& key, const Bytes& value, Type type)
//    : key(key),
//    seq_number(RBTree::seq_cnt),
//    value(value),
//    type(type),
//    color(Color::Red),
//    left(nullptr),
//    right(nullptr),
//    parent(nullptr)
//{
//}

RBTree::Node::Node(ArenaEntry key_entry, ArenaEntry value_entry, Type type, uint64_t seq_num)
    : key_entry(key_entry),
    value_entry(value_entry),
    seq_number(seq_num),
    type(type),
    color(Color::Red),
    left(nullptr),
    right(nullptr),
    parent(nullptr)
{
}

bool RBTree::Node::operator<(const Node& other) const
{
    if (this->key_entry == other.key_entry)
		return this->seq_number > other.seq_number; // For the same keys, the one with higher seq_number is considered "less" to ensure it comes first in the search
    return this->key_entry < other.key_entry;
}
bool RBTree::Node::operator>(const Node& other) const
{
    if (this->key_entry == other.key_entry)
		return this->seq_number < other.seq_number; // For the same keys, the one with lower seq_number is considered "greater"
    return this->key_entry > other.key_entry;
}

size_t RBTree::Node::approximate_memory_usage() const
{
    return sizeof(Node) + this->key_entry.size + this->value_entry.size;
}

void RBTree::left_rotate(Node* v)
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

void RBTree::right_rotate(Node* v)
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

void RBTree::balance(Node* v)
{
    while (v != root &&
        v->parent != nullptr &&
        v->parent->color == Node::Color::Red)
    {
        Node* parent = v->parent;
        Node* grandparent = parent->parent;

        if (grandparent == nullptr)
            break;

        if (parent == grandparent->left)
        {
            Node* uncle = grandparent->right;

            if (uncle != nullptr && uncle->color == Node::Color::Red)
            {
                parent->color = Node::Color::Black;
                uncle->color = Node::Color::Black;
                grandparent->color = Node::Color::Red;
                v = grandparent;
            }
            else
            {
                if (v == parent->right)
                {
                    v = parent;
                    left_rotate(v);

                    parent = v->parent;
                    if (parent == nullptr) break;
                    grandparent = parent->parent;
                    if (grandparent == nullptr) break;
                }

                parent->color = Node::Color::Black;
                grandparent->color = Node::Color::Red;
                right_rotate(grandparent);
            }
        }
        else
        {
            Node* uncle = grandparent->left;

            if (uncle != nullptr && uncle->color == Node::Color::Red)
            {
                parent->color = Node::Color::Black;
                uncle->color = Node::Color::Black;
                grandparent->color = Node::Color::Red;
                v = grandparent;
            }
            else
            {
                if (v == parent->left)
                {
                    v = parent;
                    right_rotate(v);

                    parent = v->parent;
                    if (parent == nullptr) break;
                    grandparent = parent->parent;
                    if (grandparent == nullptr) break;
                }

                parent->color = Node::Color::Black;
                grandparent->color = Node::Color::Red;
                left_rotate(grandparent);
            }
        }
    }

    if (root != nullptr)
        root->color = Node::Color::Black;
}

void RBTree::bst_insert(Node* v)
{
    Node* current = root;
    Node* parent = nullptr;

    while (current != nullptr)
    {
        parent = current;
        if (*v < *current)
            current = current->left;
        else
            current = current->right;
    }

    v->parent = parent;
    if (parent == nullptr)
        root = v;
    else if (*v < *parent)
        parent->left = v;
    else
        parent->right = v;
}

void RBTree::destroy(Node* node)
{
    if (node == nullptr) return;
    destroy(node->left);
    destroy(node->right);
    delete node;
}

size_t RBTree::approximate_subtree_memory_usage(Node* node) const
{
    if (node == nullptr)
        return 0;
    return approximate_subtree_memory_usage(node->left) + approximate_subtree_memory_usage(node->right) + node->approximate_memory_usage();
}

size_t RBTree::approximate_memory_usage() const
{
    return approximate_subtree_memory_usage(root);
}

RBTree::RBTree()
    : root(nullptr) {}

RBTree::~RBTree()
{
    destroy(root);
}

using Status = RBTree::Status;

Status RBTree::insert(const InternalRecord& entry)
{
    try
    {
        std::unique_ptr<RBTree::Node> new_node;
        new_node = std::make_unique<RBTree::Node>(entry.key_entry, entry.value_entry, entry.type, entry.seq_num);
        RBTree::Node* raw = new_node.get();
        bst_insert(raw);
        balance(raw);

        new_node.release();
        return Status::OK;
    }
    catch (const std::bad_alloc&)
    {
        return Status::MemoryAllocationFailed;
    }
}

std::variant<InternalRecord, RBTree::Status> RBTree::find_latest_by_key(ArenaEntry key) const
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
        return Status::KeyNotFound;

    return InternalRecord(result->key_entry, result->value_entry, result->type, result->seq_number);
}

// Validators implementation
bool RBTree::validate() const
{   
    return (
        RBTree::root_is_black() &&
        RBTree::no_red_node_has_red_child() &&
        RBTree::bst_ordering_correct() &&
        RBTree::subtree_black_height_info(root).first
    );
}

bool RBTree::subtree_has_no_red_node_with_red_child(Node* node) const
{
    if (node == nullptr) return true;
    if (node->color == RBTree::Node::Color::Red)
    {
        if (node->left != nullptr && node->left->color == RBTree::Node::Color::Red) return false;
        if (node->right != nullptr && node->right->color == RBTree::Node::Color::Red) return false;
    }
    return (
        subtree_has_no_red_node_with_red_child(node->left) &&
        subtree_has_no_red_node_with_red_child(node->right)
    );
}

bool RBTree::no_red_node_has_red_child() const
{
    return subtree_has_no_red_node_with_red_child(root);
}

bool RBTree::root_is_black() const
{
    return (root == nullptr) || (root->color == RBTree::Node::Color::Black);
}

bool RBTree::bst_ordering_correct() const
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

bool RBTree::black_height_is_consistent() const
{
    return subtree_black_height_info(root).first;
}

std::pair<bool, int> RBTree::subtree_black_height_info(Node* node) const
{
    if (node == nullptr)
        return { true, 1 };

    auto left = subtree_black_height_info(node->left);
    auto right = subtree_black_height_info(node->right);

    if (!left.first || !right.first)
        return { false, -1 };

    if (left.second != right.second)
        return { false, -1 };

    return { true, left.second + (node->color == Node::Color::Black ? 1 : 0) };
}

// RBTree Iterator implementation
void RBTree::InorderIterator::push_left(Node* node)
{
    while (node)
    {
        st.push(node);
        node = node->left;
    }
}

RBTree::InorderIterator::InorderIterator(Node* root)
{
    push_left(root);
}

bool RBTree::InorderIterator::has_next()
{
    return !st.empty();
}

RBTree::Node* RBTree::InorderIterator::next()
{
    Node* cur = st.top();
    st.pop();
    if (cur->right)
    {
        push_left(cur->right);
    }
    return cur;
}

RBTree::Node* RBTree::root_getter()
{
    return this->root;
}

void RBTree::dump_inorder(std::vector<InternalRecord>& out) const
{
    InorderIterator it(this->root);
    while (it.has_next())
    {
        auto node = it.next();
        out.emplace_back(node->key_entry, node->value_entry, node->type, node->seq_number);
    }
}