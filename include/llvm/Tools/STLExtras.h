//===-- STLExtras.h - Useful functions when working with the STL --*- C++ -*--=//
//
// This file contains some templates that are useful if you are working with the
// STL at all.
//
// No library is required when using these functinons.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TOOLS_STL_EXTRAS_H
#define LLVM_TOOLS_STL_EXTRAS_H

#include <functional>

//===----------------------------------------------------------------------===//
//     Extra additions to <iterator>
//===----------------------------------------------------------------------===//

// mapped_iterator - This is a simple iterator adapter that causes a function to
// be dereferenced whenever operator* is invoked on the iterator.
//
// It turns out that this is disturbingly similar to boost::transform_iterator
//
#if 1
template <class RootIt, class UnaryFunc>
class mapped_iterator {
  RootIt current;
public:
  typedef typename iterator_traits<RootIt>::iterator_category
          iterator_category;
  typedef typename iterator_traits<RootIt>::difference_type
          difference_type;
  typedef typename UnaryFunc::result_type value_type;
  typedef typename UnaryFunc::result_type *pointer;
  typedef void reference;        // Can't modify value returned by fn

  typedef RootIt iterator_type;
  typedef mapped_iterator<RootIt, UnaryFunc> _Self;

  inline RootIt &getCurrent() const { return current; }

  inline explicit mapped_iterator(const RootIt &I) : current(I) {}
  inline mapped_iterator(const mapped_iterator &It) : current(It.current) {}

  inline value_type operator*() const {   // All this work to do this 
    return UnaryFunc()(*current);         // little change
  }

  _Self& operator++() { ++current; return *this; }
  _Self& operator--() { --current; return *this; }
  _Self  operator++(int) { _Self __tmp = *this; ++current; return __tmp; }
  _Self  operator--(int) { _Self __tmp = *this; --current; return __tmp; }
  _Self  operator+    (difference_type n) const { return _Self(current + n); }
  _Self& operator+=   (difference_type n) { current += n; return *this; }
  _Self  operator-    (difference_type n) const { return _Self(current - n); }
  _Self& operator-=   (difference_type n) { current -= n; return *this; }
  reference operator[](difference_type n) const { return *(*this + n); }  

  inline bool operator==(const _Self &X) const { return current == X.current; }
  inline bool operator< (const _Self &X) const { return current <  X.current; }

  inline difference_type operator-(const _Self &X) const {
    return current - X.current;
  }
};

template <class _Iterator, class Func>
inline mapped_iterator<_Iterator, Func> 
operator+(typename mapped_iterator<_Iterator, Func>::difference_type N,
          const mapped_iterator<_Iterator, Func>& X) {
  return mapped_iterator<_Iterator, Func>(X.getCurrent() - N);
}

#else

// This fails to work, because some iterators are not classes, for example
// vector iterators are commonly value_type **'s
template <class RootIt, class UnaryFunc>
class mapped_iterator : public RootIt {
public:
  typedef typename UnaryFunc::result_type value_type;
  typedef typename UnaryFunc::result_type *pointer;
  typedef void reference;        // Can't modify value returned by fn

  typedef mapped_iterator<RootIt, UnaryFunc> _Self;
  typedef RootIt super;
  inline explicit mapped_iterator(const RootIt &I) : super(I) {}
  inline mapped_iterator(const super &It) : super(It) {}

  inline value_type operator*() const {     // All this work to do 
    return UnaryFunc(super::operator*());   // this little thing
  }
};
#endif

// map_iterator - Provide a convenient way to create mapped_iterators, just like
// make_pair is useful for creating pairs...
//
template <class ItTy, class FuncTy>
inline mapped_iterator<ItTy, FuncTy> map_iterator(const ItTy &I, FuncTy F) {
  return mapped_iterator<ItTy, FuncTy>(I);
}



//===----------------------------------------------------------------------===//
//     Extra additions to <algorithm>
//===----------------------------------------------------------------------===//

// reduce - Reduce a sequence values into a single value, given an initial
// value and an operator.
//
template <class InputIt, class Function, class ValueType>
ValueType reduce(InputIt First, InputIt Last, Function Func, ValueType Value) {
  for ( ; First != Last; ++First)
    Value = Func(*First, Value);
  return Value;
}

#if 1   // This is likely to be more efficient

// reduce_apply - Reduce the result of applying a function to each value in a
// sequence, given an initial value, an operator, a function, and a sequence.
//
template <class InputIt, class Function, class ValueType, class TransFunc>
ValueType reduce_apply(InputIt First, InputIt Last, Function Func, 
		       ValueType Value, TransFunc XForm) {
  for ( ; First != Last; ++First)
    Value = Func(XForm(*First), Value);
  return Value;
}

#else  // This is arguably more elegant

// reduce_apply - Reduce the result of applying a function to each value in a
// sequence, given an initial value, an operator, a function, and a sequence.
//
template <class InputIt, class Function, class ValueType, class TransFunc>
ValueType reduce_apply2(InputIt First, InputIt Last, Function Func, 
		       ValueType Value, TransFunc XForm) {
  return reduce(map_iterator(First, XForm), 
		map_iterator(Last, XForm),
		Func, Value);
}
#endif



//===----------------------------------------------------------------------===//
//     Extra additions to <functional>
//===----------------------------------------------------------------------===//

// bitwise_or - This is a simple functor that applys operator| on its two 
// arguments to get a boolean result.
//
template<class Ty>
struct bitwise_or : public binary_function<Ty, Ty, bool> {
  bool operator()(const Ty& left, const Ty& right) const {
    return left | right;
  }
};

#endif
