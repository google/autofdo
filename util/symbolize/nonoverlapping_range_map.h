// Copyright 2014 Google Inc. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// This file contains a simple map wrapper (NonOverlappingRangeMap)
// that maps address ranges to data.  The map is used to support
// efficient lookup of debug information given a query address.

#ifndef AUTOFDO_SYMBOLIZE_NONOVERLAPPING_RANGE_MAP_H_
#define AUTOFDO_SYMBOLIZE_NONOVERLAPPING_RANGE_MAP_H_

#include <algorithm>
#include <map>
#include <utility>
#include <vector>

#include "base/common.h"
#include "symbolize/dwarf3ranges.h"

namespace autofdo {

struct RangeStartLt {
  bool operator()(const AddressRangeList::Range& r1,
                  const AddressRangeList::Range& r2) const {
    return r1.first < r2.first;
  }
};

// NonOverlappingRangeMap maps address ranges
// (AddressRangeList::Range) to data.  The ranges in the map are
// guaranteed to be non-overlapping.  The map supports looking up an
// address (rather than a range) and the data for the containing
// range, if it exists, is returned.
//
// To guarantee that the ranges stored are non-overlapping, some
// checks are done upon insert.  First, if a range is inserted that is
// completely contained within another range, it will break the other
// range into two pieces and replace the mapping for the overlapping
// region.  If the inserted range intersects the middle of another
// range in any other way, a CHECK will fail.
//
// Second, if a range is inserted that completely contains one or more
// ranges, the new only fills in the gaps. For example, if the range
// [5,7) and [10,12) are already in the map, an insert of [0,15) is
// identical to the following three inserts: [0,5), [7,10), [12,15).
// This convenience behavior is useful when inserting data for
// hierarchical structures in bottom-up order.
template<typename T>
class NonOverlappingRangeMap {
 public:
  typedef map<AddressRangeList::Range, T, RangeStartLt> RangeMap;
  typedef typename RangeMap::iterator Iterator;
  typedef typename RangeMap::const_iterator ConstIterator;

  NonOverlappingRangeMap();

  void InsertRangeList(const AddressRangeList::RangeList& range_list,
                           const T& value);
  void InsertRange(uint64 low, uint64 high, const T& value);
  Iterator Find(uint64 address);
  ConstIterator Find(uint64 address) const;

  Iterator Begin();
  ConstIterator Begin() const;
  Iterator End();
  ConstIterator End() const;

  bool Empty() const { return ranges_.empty(); }

 private:
  RangeMap ranges_;
  template<class IteratorType>
  IteratorType FindHelper(uint64 address, IteratorType iter,
                          IteratorType end) const;
  bool RangeStrictlyContains(const AddressRangeList::Range& outer,
                             const AddressRangeList::Range& inner);
  void SplitRange(Iterator split, uint64 low, uint64 high, const T& value);
  DISALLOW_COPY_AND_ASSIGN(NonOverlappingRangeMap);
};

template<class T>
NonOverlappingRangeMap<T>::NonOverlappingRangeMap() { }

template<class T>
void NonOverlappingRangeMap<T>::InsertRangeList(
    const AddressRangeList::RangeList& range_list, const T& value) {

  for (AddressRangeList::RangeList::const_iterator iter = range_list.begin();
       iter != range_list.end(); ++iter) {
    InsertRange(iter->first, iter->second, value);
  }
}

template<class T>
void NonOverlappingRangeMap<T>::InsertRange(uint64 low, uint64 high,
                                            const T& value) {
  if (low == high)
    return;

  Iterator insert_point = ranges_.lower_bound(make_pair(low, high));

  if (insert_point != ranges_.begin()) {
    Iterator predecessor = insert_point;
    --predecessor;

    if (RangeStrictlyContains(predecessor->first, make_pair(low, high))) {
      SplitRange(predecessor, low, high, value);
      return;
    }

    // No containment. Check that the predecessor does not overlap with range
    CHECK(predecessor->first.second <= low);
  }

  if (insert_point != ranges_.end() &&
      RangeStrictlyContains(insert_point->first, make_pair(low, high))) {
    SplitRange(insert_point, low, high, value);
    return;
  }

  while (low < high) {
    if (insert_point == ranges_.end()) {
      ranges_.insert(make_pair(make_pair(low, high), value));
      break;
    } else {
      if (low != insert_point->first.first) {
        // low < insert_point->first.first by the invariants of lower_bound
        uint64 from = low;
        uint64 to = min(high, insert_point->first.first);

        // Ensure that insert does not end in the middle of another range
        CHECK(to == high || high >= insert_point->first.second);
        CHECK(from < to);
        pair<Iterator, bool> insert_status =
            ranges_.insert(make_pair(make_pair(from, to), value));
        CHECK(insert_status.second);
      }
      low = min(high, insert_point->first.second);
    }
    ++insert_point;
  }
}

template<class T>
typename NonOverlappingRangeMap<T>::Iterator
NonOverlappingRangeMap<T>::Find(uint64 address) {
  AddressRangeList::Range singleton_range = make_pair(address, address + 1);
  Iterator iter = ranges_.lower_bound(singleton_range);
  return FindHelper(address, iter, ranges_.end());
}

template<class T>
typename NonOverlappingRangeMap<T>::ConstIterator
NonOverlappingRangeMap<T>::Find(uint64 address) const {
  AddressRangeList::Range singleton_range = make_pair(address, address + 1);
  ConstIterator iter = ranges_.lower_bound(singleton_range);
  return FindHelper(address, iter, ranges_.end());
}

template<class T>
template<class IteratorType>
IteratorType NonOverlappingRangeMap<T>::FindHelper(uint64 address,
                                                   IteratorType iter,
                                                   IteratorType end) const {
  if (iter == end || iter->first.first != address) {
    if (iter == ranges_.begin())
      return end;
    --iter;
  }

  if (iter->first.second > address)
    return iter;
  return end;
}

template<class T>
typename NonOverlappingRangeMap<T>::Iterator
NonOverlappingRangeMap<T>::Begin() {
  return ranges_.begin();
}

template<class T>
typename NonOverlappingRangeMap<T>::ConstIterator
NonOverlappingRangeMap<T>::Begin() const {
  return ranges_.begin();
}

template<class T>
typename NonOverlappingRangeMap<T>::Iterator
NonOverlappingRangeMap<T>::End() {
  return ranges_.end();
}

template<class T>
typename NonOverlappingRangeMap<T>::ConstIterator
NonOverlappingRangeMap<T>::End() const {
  return ranges_.end();
}

template<class T>
bool NonOverlappingRangeMap<T>::RangeStrictlyContains(
    const AddressRangeList::Range& outer,
    const AddressRangeList::Range& inner) {
  return (outer.first <= inner.first) && (outer.second >= inner.second) &&
      ((outer.first != inner.first) || (outer.second != inner.second));
}

template<class T>
void NonOverlappingRangeMap<T>::SplitRange(Iterator split, uint64 low,
                                           uint64 high, const T& value) {
  const AddressRangeList::Range old_range = split->first;
  const T old_value = split->second;
  pair<Iterator, bool> insert_status;

  ranges_.erase(split);

  if (low != old_range.first) {
    insert_status =
        ranges_.insert(make_pair(make_pair(old_range.first, low), old_value));
    CHECK(insert_status.second);
  }

  insert_status = ranges_.insert(make_pair(make_pair(low, high), value));
  CHECK(insert_status.second);

  if (high != old_range.second) {
    insert_status =
        ranges_.insert(make_pair(make_pair(high, old_range.second), old_value));
    CHECK(insert_status.second);
  }
}

}  // namespace autofdo

#endif  // AUTOFDO_SYMBOLIZE_NONOVERLAPPING_RANGE_MAP_H_
