//===- ModifiablePriorityQueue.h  ---------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
// This file implements a priority-queue data structure for storing key-value
// pairs, with compare operations for both the key and the value type given.
// It stores these pairs in a max-heap data structure which allows for fast O(lg
// n)-time retrieval of the maximum element.
// To compare two key-value pairs, the values parts are compared first. The keys
// are compared in case of a tie. No two key-value pairs could have the same
// key.
//
// The key-value pairs are also accessible using a map data structure.
//
// In contrast to C++ STL's priority queue, this data structure allows for fast
// O(lg n)-time value update, or removal of any element while keep the max-heap
// data structure sound.
//===----------------------------------------------------------------------===//
#ifndef LLD_ELF_PROPELLER_MODIFIABLE_PRIORITY_QUEUE_H
#define LLD_ELF_PROPELLER_MODIFIABLE_PRIORITY_QUEUE_H

#include "llvm/ADT/DenseMap.h"
#include <algorithm>
#include <assert.h>
#include <functional>
#include <memory>
#include <string.h>
#include <vector>

// This class defines a node in the ModifiablePriorityQueue data structure
// containing a key, a value, pointers to its parent, and its left and right
// children. This class must be instantiated with the key-type, the value type,
// and their comparators.
template <class K, class V, class CmpK, class CmpV> class HeapNode {
public:
  K key;
  V value;
  // Pointer to the parent (this is nullptr for root).
  HeapNode<K, V, CmpK, CmpV> *parent;
  // Pointers to the two children
  HeapNode<K, V, CmpK, CmpV> *children[2];

  HeapNode(K k, V v)
      : key(k), value(std::move(v)), parent(nullptr), children() {}

  // Get pointer to the left child
  HeapNode<K, V, CmpK, CmpV> *leftChild() { return children[0]; }

  // Get pointer to the right child
  HeapNode<K, V, CmpK, CmpV> *rightChild() { return children[1]; }

  // Whether this is the left child of its parent, returns false if this is the
  // root.
  bool isLeftChild() {
    if (parent)
      return parent->leftChild() == this;
    return false;
  }

  // Whether this is the right child of its parent, returns false if this is the
  // root.
  bool isRightChild() {
    if (parent)
      return parent->rightChild() == this;
    return false;
  }

  // Set the left child for this node.
  void adoptLeftChild(HeapNode<K, V, CmpK, CmpV> *c) {
    children[0] = c;
    if (c)
      c->parent = this;
  }

  // Set the right child for this node.
  void adoptRightChild(HeapNode<K, V, CmpK, CmpV> *c) {
    children[1] = c;
    if (c)
      c->parent = this;
  }

  // Set both the left and right children for this node.
  void adoptChildren(HeapNode<K, V, CmpK, CmpV> *(&_children)[2]) {
    adoptLeftChild(_children[0]);
    adoptRightChild(_children[1]);
  }

  // Return a string representation for this node given an indentation level
  // which is used for pretty-printing of the heap tree.
  // This assumes that
  // std::to_string has been specialized for the key and value types.
  std::string toString(unsigned level) {
    std::string str = std::string(level, ' ');
    str += "NODE: " + std::to_string(key) + " -> " + std::to_string(value);
    for (auto *c : children) {
      if (c) {
        str += "\n";
        str += c->toString(level + 1);
      }
    }
    return str;
  }
};

// Comparator class for nodes given comparators for the key and value types. It
// compares the values for the two pairs first and in the case of a tie, it
// compares the keys.
template <class K, class V, class CmpK, class CmpV> struct CompareHeapNode {
  CmpK KeyComparator;
  CmpV ValueComparator;
  bool operator()(const HeapNode<K, V, CmpK, CmpV> &n1,
                  const HeapNode<K, V, CmpK, CmpV> &n2) const {
    if (ValueComparator(n1.value, n2.value))
      return true;
    if (ValueComparator(n2.value, n1.value))
      return false;
    return KeyComparator(n1.key, n2.key);
  }
};

// This class implements a pointer-based heap-like data structure for storing
// key-value pairs with efficient O(lg n) retrieval, removal, update, and
// insertion of nodes.
//
// Any insertion happens at the lowest-right-most empty leaf position. This
// position is accessible via the root pointer of the tree using the binary
// representation of [size of the tree + 1]. Starting with the left-most bit, we
// take the left child if the bit is zero and the right child if the bit is one.
//
// Deletion of a node happens by replacing the last node (lowest-right-most
// occupied leaf), with the given node, removing the node from the tree, and
// finally heapifying the replaced node up and down to keep the
// soundness of the heap.
//
// Value-update is the simplest, which requires updating the node's value
// followed by heapifying up the node up and down.
template <class K, class V, class CmpK, class CmpV>
class ModifiablePriorityQueue {
private:
  // Comparator instance for comparing HeapNodes.
  CompareHeapNode<K, V, CmpK, CmpV> HeapNodeComparator;

  // Pointers to the nodes in this heap are stored in a map to allow for fast
  // lookup of nodes given their keys.
  llvm::DenseMap<K, std::unique_ptr<HeapNode<K, V, CmpK, CmpV>>> nodes;

  // The tree-root for this priority queue, which holds the max key-value.
  HeapNode<K, V, CmpK, CmpV> *root = nullptr;

  // Total number of nodes in the tree.
  unsigned Size = 0;

  // Set the root for this tree and set it's parent to null.
  void assignRoot(HeapNode<K, V, CmpK, CmpV> *node) {
    root = node;
    if (root)
      root->parent = nullptr;
  }

  // Helper for getting a pointer to a node using a handle using its bits.
  HeapNode<K, V, CmpK, CmpV> *getNodeWithHandle(unsigned handle) {
    return getNodeWithHandleHelper(root, handle);
  }

  // This function traverses the tree from the given node, using an integer
  // handle for direction. Going from left to right in the binary representation
  // of the handle, take right if the bit is one, and left if the bit is zero.
  HeapNode<K, V, CmpK, CmpV> *
  getNodeWithHandleHelper(HeapNode<K, V, CmpK, CmpV> *node, unsigned handle) {
    if (handle == 1)
      return node;
    auto *p = getNodeWithHandleHelper(node, handle >> 1);
    return (handle & 1) ? p->rightChild() : p->leftChild();
  }

  // Insert a node into the heap tree.
  void insert(HeapNode<K, V, CmpK, CmpV> *node) {
    if (!root) {
      // If the tree is empty, we only need to set the root.
      assignRoot(node);
    } else {
      // Otherwise, insert at the lowest-right-most leaf empty-position, which
      // is encoded by [Size + 1]
      unsigned handle = Size + 1;
      // We need to find the parent of the target position.
      auto *p = getNodeWithHandleHelper(root, handle >> 1);
      // Attach this to the left or right of the parent node, depending on the
      // last bit in handle.
      if (handle & 1)
        p->adoptRightChild(node);
      else
        p->adoptLeftChild(node);
      // We only need to heapify up as this node has no children.
      heapifyUp(node);
    }
    // Increment the size of the tree.
    Size++;
  }

  // This function removes a node from the heap and returns its value.
  // Instead of removing the node directly, it moves the lowest-right-most node
  // to the position of the given node and heapifies it up and down.
  //
  // After this function, the removed node's HeapNode pointer is invalid.
  V remove(HeapNode<K, V, CmpK, CmpV> *node) {
    // Find the lowest-right-most node
    auto *last = getNodeWithHandleHelper(root, Size);
    assert(last->leftChild() == nullptr && last->rightChild() == nullptr);

    // Remove the node from the children of its parents.
    if (last->parent) {
      if (last->isLeftChild())
        last->parent->adoptLeftChild(nullptr);
      else
        last->parent->adoptRightChild(nullptr);
    }

    if (node == last) {
      // If we are removing the last node, not much needs to be done.
      if (node->parent == nullptr) {
        assert(node == root);
        assignRoot(nullptr);
      }
    } else { // node != last
      if (node->parent) {
        // Move the last node in place of the to-be-removed node.
        if (node->isLeftChild())
          node->parent->adoptLeftChild(last);
        else
          node->parent->adoptRightChild(last);
      } else { // node->parent == nullptr, so node is root
        assert(node == root);
        assignRoot(last);
      }
      // Let the moved-node adopt the children of the to-be-removed node
      last->adoptChildren(node->children);
      // Finally, heapify the moved node up and down.
      if (!heapifyUp(last))
        heapifyDown(last);
    }

    // Decrement the number of nodes.
    Size--;
    // Save the node's value to be returned.
    auto nodeElement = std::move(node->value);
    // Remove the node from the key->node mapping, effectively deallocating the
    // HeapNode.
    nodes.erase(node->key);
    return nodeElement;
  }

  // Recursively rectify the max-heap upwards from a node.
  // This takes O(lg n) time.
  // Returns true if any modifications occurs.
  bool heapifyUp(HeapNode<K, V, CmpK, CmpV> *node) {
    // Heapify upwards until no more inconsistency is found.
    if (node->parent && HeapNodeComparator(*node->parent, *node)) {
      swapWithParent(node);
      heapifyUp(node);
      return true;
    }
    return false;
  }

  // Recursively rectify the max-heap downwards from a node.
  // This takes O(lg n) time.
  // Returns true if any modifications occurs.
  bool heapifyDown(HeapNode<K, V, CmpK, CmpV> *node) {
    auto maxChild = std::max_element(
        node->children, node->children + 2,
        [this](const HeapNode<K, V, CmpK, CmpV> *c1,
               const HeapNode<K, V, CmpK, CmpV> *c2) {
          // This compares two HeapNode pointers, relying on
          // HeapNodeComparator for non-null pointers.
          return c1 == nullptr
                     ? true
                     : (c2 == nullptr ? false : HeapNodeComparator(*c1, *c2));
        });
    // Move the maximum child to the parent if required and further heapify
    // down.
    if (*maxChild != nullptr && HeapNodeComparator(*node, **maxChild)) {
      swapWithParent(*maxChild);
      heapifyDown(node);
      return true;
    }
    return false;
  }

  // Helper function for swapping a node with its parent.
  // Can only be called if the given node has a parent. Also, the given node's
  // value should compare bigger than both its parent and its sibling.
  // For example swapWithParent(C1) results in the following.
  //            G                    G
  //           /                    /
  //          P        ->          C1
  //         / \                  / \
  //        C1 C2                P  C2
  //       / \                  / \
  //      A   B                A   B
  // This function merely properlly corrects the parent-child relationships.
  void swapWithParent(HeapNode<K, V, CmpK, CmpV> *node) {
    auto *par = node->parent;
    assert(par);
    auto *gpar = node->parent->parent;
    if (!gpar) {
      assert(this->root == par);
      this->assignRoot(node);
    } else {
      if (par->isLeftChild())
        gpar->adoptLeftChild(node);
      else
        gpar->adoptRightChild(node);
    }
    auto *par_old_left = par->leftChild();
    auto *par_old_right = par->rightChild();

    par->adoptChildren(node->children);
    if (par_old_left == node) {
      node->adoptLeftChild(par);
      node->adoptRightChild(par_old_right);
    } else {
      node->adoptLeftChild(par_old_left);
      node->adoptRightChild(par);
    }
  }

public:
  // Insert a key-value pair into the heap.
  // If the same key exists in the heap, it updates the value of the existing
  // HeapNode and heapifies the node up or down.
  void insert(K key, V value) {
    auto cur = nodes.find(key);
    if (cur != nodes.end()) {
      cur->second->value = std::move(value);
      if (!heapifyUp(cur->second.get()))
        heapifyDown(cur->second.get());
    } else {
      // If the key is not found, create a new HeapNode and insert it into the
      // heap.
      auto *node = new HeapNode<K, V, CmpK, CmpV>(key, std::move(value));
      insert(node);
      nodes.try_emplace(key, node);
    }
  }

  // Removes the node associated with a key from the heap if one exists.
  void erase(K k) {
    auto cur = nodes.find(k);
    if (cur != nodes.end())
      remove(cur->second.get());
  }

  // Retrieves the HeapNode corresponding to a given key (returns nullptr if no
  // such node exists).
  HeapNode<K, V, CmpK, CmpV> *get(K k) {
    auto it = nodes.find(k);
    return it == nodes.end() ? nullptr : it->second.get();
  }

  // Removes the root (max-element) from the heap.
  void pop() {
    assert(!empty());
    remove(root);
  }

  // Returns the value of the max-element node.
  V top() { return std::move(root->value); }

  bool empty() { return Size == 0; }

  unsigned size() { return Size; };

  // Returns a string representation of the heap tree.
  std::string toString() {
    std::string str;
    str += "HEAP with ";
    str += std::to_string(Size);
    str += " nodes\n";
    if (root)
      str += root->toString(0);
    return str;
  }
};
#endif
