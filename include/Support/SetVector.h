//===- SetVector.h - A set with insertion order iteration -------*- C++ -*-===//
// 
//                     The LLVM Compiler Infrastructure
//
// This file was developed by Reid Spencer and is distributed under
// the University of Illinois Open Source License. See LICENSE.TXT for details.
// 
//===----------------------------------------------------------------------===//
//
// This file implements a set that has insertion order iteration 
// characteristics. This is useful for keeping a set of things that need to be
// visited later but in a deterministic order (insertion order). The interface
// is purposefully minimal.
//
//===----------------------------------------------------------------------===//

#ifndef SUPPORT_SETVECTOR_H
#define SUPPORT_SETVECTOR_H

#include <set>
#include <vector>

namespace llvm {

/// This class provides a way to keep a set of things that also has the 
/// property of a deterministic iteration order. The order of iteration is the
/// order of insertion.
/// @breif A vector that has set insertion semantics.
template <typename T>
class SetVector {

public:
  typedef T value_type;
  typedef T key_type;
  typedef T& reference;
  typedef const T& const_reference;
  typedef std::set<value_type> set_type;
  typedef std::vector<value_type> vector_type;
  typedef typename vector_type::iterator iterator;
  typedef typename vector_type::const_iterator const_iterator;
  typedef typename vector_type::size_type size_type;

  /// @brief Completely clear the SetVector
  void clear() {
    set_.clear();
    vector_.clear();
  }

  /// @brief Determine if the SetVector is empty or not.
  bool empty() const {
    return vector_.empty();
  }

  /// @brief Determine the number of elements in the SetVector.
  size_type size() const {
    return vector_.size();
  }

  /// @brief Get an iterator to the beginning of the SetVector.
  iterator begin() {
    return vector_.begin();
  }

  /// @brief Get a const_iterator to the beginning of the SetVector.
  const_iterator begin() const {
    return vector_.begin();
  }

  /// @brief Get an iterator to the end of the SetVector.
  iterator end() {
    return vector_.end();
  }

  /// @brief Get a const_iterator to the end of the SetVector.
  const_iterator end() const {
    return vector_.end();
  }

  /// @brief Index into the SetVector.
  const_reference operator[](size_type n) const {
      return vector_[n];
  }

  /// @returns true iff the element was inserted into the SetVector.
  /// @brief Insert a new element into the SetVector.
  bool insert( const value_type& X ) {
    std::pair<typename set_type::iterator,bool> insertion_result = set_.insert(X);
    if ( insertion_result.second ) {
      vector_.push_back(X);
    }
    return insertion_result.second;
  }

  /// @returns 0 if the element is not in the SetVector, 1 if it is.
  /// @brief Count the number of elements of a given key in the SetVector.
  size_type count( const key_type& key ) const {
    return set_.count(key);
  }

private:
  set_type set_;         ///< The set.
  vector_type vector_;   ///< The vector.
};

} // End llvm namespace

// vim: sw=2 ai
#endif
