# Red-Black Tree

[A Red-Black Tree](https://en.wikipedia.org/wiki/Red%E2%80%93black_tree) is a [self-balancing](https://en.wikipedia.org/wiki/Self-balancing_binary_search_tree) [binary search tree](https://en.wikipedia.org/wiki/Binary_search_tree).

It assigns every node either a red or black color and keeps the tree balanced using a set of invariants.

Although a Red-Black Tree is considered a [height-balanced binary search tree](https://www.geeksforgeeks.org/dsa/introduction-to-height-balanced-binary-tree/), like an [AVL tree](https://en.wikipedia.org/wiki/AVL_tree), it is more loosely balanced. However, it still guarantees logarithmic search, insertion, and deletion time.

---

## Invariants overview

* Every node is either red or black.
* The root is black.
* All NIL/null leaf nodes are black.
* In most implementations, missing children are treated as black sentinel leaves.
* A red node cannot have a red child.
* Every path from a node to any descendant NIL leaf has the same number of black nodes.
* This number is called the black height.
* A newly inserted node is usually inserted as red.

---

## Time complexity overview

| Operation        | Average case | Worst case |
| ---------------- | ------------ | ---------- |
| Search / lookup  | O(log *n*)   | O(log *n*) |
| Insert           | O(log *n*)   | O(log *n*) |
| Delete           | O(log *n*)   | O(log *n*) |
| Space complexity | O(*n*)       | O(*n*)     |

---

## Implementation

While a basic Red-Black Tree implementation can be found [here](https://www.geeksforgeeks.org/dsa/introduction-to-red-black-tree/), this document focuses on a Red-Black Tree used specifically as an underlying [MemTable](https://github.com/facebook/rocksdb/wiki/MemTable) data structure in an [LSM](https://en.wikipedia.org/wiki/Log-structured_merge-tree) database.

In this implementation, each node stores one internal record:

```cpp
Node {
    key
    value
    sequence_number
    record_type    // Put or Tombstone
    color          // Red or Black

    left
    right
    parent
}
```

### Ordering

Nodes are ordered by key first.

If two records have the same key, the record with the **higher sequence number** is ordered first.

```text
compare(a, b):
    if a.key == b.key:
        return a.sequence_number > b.sequence_number

    return a.key < b.key
```

This allows the newest version of a key to appear before older versions during traversal or search.

### Insertion

New nodes are inserted as regular BST nodes first.

A newly inserted node starts as **red**. This avoids changing the black height immediately.

```text
insert(record):
    node = new Node(record)
    node.color = Red

    result = bst_insert(node)

    if result failed:
        delete node
        return result

    fix_red_black_properties(node)

    root.color = Black

    return OK
```

### BST Insert

The BST insert step only places the node according to the tree ordering.

```text
bst_insert(node):
    current = root
    parent = null

    while current != null:
        parent = current

        if node == current:
            return Duplicate

        if node < current:
            current = current.left
        else:
            current = current.right

    node.parent = parent

    if parent == null:
        root = node
    else if node < parent:
        parent.left = node
    else:
        parent.right = node

    return OK
```

### Left Rotation

A left rotation moves a node's right child up.

```text
left_rotate(x):
    y = x.right

    x.right = y.left

    if y.left != null:
        y.left.parent = x

    y.parent = x.parent

    if x.parent == null:
        root = y
    else if x == x.parent.left:
        x.parent.left = y
    else:
        x.parent.right = y

    y.left = x
    x.parent = y
```

Before:

```text
    x
     \
      y
     /
    B
```

After:

```text
      y
     /
    x
     \
      B
```

### Right Rotation

A right rotation moves a node's left child up.

```text
right_rotate(x):
    y = x.left

    x.left = y.right

    if y.right != null:
        y.right.parent = x

    y.parent = x.parent

    if x.parent == null:
        root = y
    else if x == x.parent.left:
        x.parent.left = y
    else:
        x.parent.right = y

    y.right = x
    x.parent = y
```

Before:

```text
      x
     /
    y
     \
      B
```

After:

```text
    y
     \
      x
     /
    B
```

### Rebalancing After Insert

After insertion, the tree may contain a red node with a red parent.

The balancing step fixes this using recoloring and rotations.

```text
fix_red_black_properties(node):
    while node is not root and node.parent is Red:
        parent = node.parent
        grandparent = parent.parent

        if parent is grandparent.left:
            uncle = grandparent.right

            if uncle is Red:
                parent.color = Black
                uncle.color = Black
                grandparent.color = Red
                node = grandparent
            else:
                if node is parent.right:
                    node = parent
                    left_rotate(node)

                node.parent.color = Black
                grandparent.color = Red
                right_rotate(grandparent)

        else:
            uncle = grandparent.left

            if uncle is Red:
                parent.color = Black
                uncle.color = Black
                grandparent.color = Red
                node = grandparent
            else:
                if node is parent.left:
                    node = parent
                    right_rotate(node)

                node.parent.color = Black
                grandparent.color = Red
                left_rotate(grandparent)

    root.color = Black
```

There are two main cases:

### Case 1: Parent and uncle are red

Recolor the parent and uncle to black, then recolor the grandparent to red.

```text
      G(B)
     /   \
   P(R)  U(R)
   /
 N(R)
```

Becomes:

```text
      G(R)
     /   \
   P(B)  U(B)
   /
 N(R)
```

Then balancing continues from the grandparent.

### Case 2: Parent is red, uncle is black or null

Use rotations to move the red violation upward, then recolor.

Example left-left case:

```text
      G(B)
     /
   P(R)
   /
 N(R)
```

After recoloring and right rotation:

```text
    P(B)
   /   \
 N(R)  G(R)
```

## Lookup Latest Record By Key

Because newer versions of the same key are ordered before older versions, the search can keep walking left after finding a matching key.

```text
find_latest_by_key(key):
    current = root
    result = null

    while current != null:
        if current.key < key:
            current = current.right
        else:
            if current.key == key:
                result = current

            current = current.left

    if result == null:
        return NotFound

    return result.record
```

## In-Order Dump

An in-order traversal returns records according to MemTable ordering:

```text
dump_inorder():
    result = []

    inorder(root):
        inorder(node.left)
        result.push(node.record)
        inorder(node.right)

    return result
```

For the same key, records appear from newest to oldest because higher sequence numbers are ordered first.

## Validation

The tree can be validated using the main Red-Black Tree invariants:

```text
validate():
    return root_is_black()
        and no_red_node_has_red_child()
        and bst_ordering_is_correct()
        and black_height_is_consistent()
```

### Root Is Black

```text
root_is_black():
    return root == null or root.color == Black
```

### No Red Node Has Red Child

```text
no_red_node_has_red_child(node):
    if node == null:
        return true

    if node.color == Red:
        if node.left != null and node.left.color == Red:
            return false
        if node.right != null and node.right.color == Red:
            return false

    return no_red_node_has_red_child(node.left)
       and no_red_node_has_red_child(node.right)
```

### BST Ordering Is Correct

```text
bst_ordering_is_correct():
    previous = null

    for node in inorder traversal:
        if previous != null and not (previous < node):
            return false

        previous = node

    return true
```

### Black Height Is Consistent

Null children are treated as black leaves.

```text
black_height(node):
    if node == null:
        return valid = true, height = 1

    left = black_height(node.left)
    right = black_height(node.right)

    if left is invalid or right is invalid:
        return valid = false

    if left.height != right.height:
        return valid = false

    if node.color == Black:
        return valid = true, height = left.height + 1
    else:
        return valid = true, height = left.height
```

### Delete Operation

The delete operation is usually considered the most complex operation in a Red-Black Tree because of the number of edge cases involved.

Fortunately, this MemTable implementation does not need to physically delete nodes from the tree. In an LSM database, deletes are usually represented by inserting a tombstone record. The tombstone is later resolved during reads or compaction.

For a general explanation of deletion in a Red-Black Tree, see [this article](https://www.geeksforgeeks.org/dsa/deletion-in-red-black-tree/).
