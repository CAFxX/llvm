//===-- Support/StatisticReporter.h - Easy way to expose stats ---*- C++ -*-==//
//
// This file defines the 'Statistic' class, which is designed to be an easy way
// to expose various success metrics from passes.  These statistics are printed
// at the end of a run, when the -stats command line option is enabled on the
// command line.
//
// This is useful for reporting information like the number of instructions
// simplified, optimized or removed by various transformations, like this:
//
// static Statistic<> NumInstEliminated("GCSE - Number of instructions killed");
//
// Later, in the code: ++NumInstEliminated;
//
//===----------------------------------------------------------------------===//

#ifndef SUPPORT_STATISTIC_REPORTER_H
#define SUPPORT_STATISTIC_REPORTER_H

#include <iosfwd>

// StatisticBase - Nontemplated base class for Statistic<> class...
class StatisticBase {
  const char *Name;
protected:
  StatisticBase(const char *name) : Name(name) {}
  virtual ~StatisticBase() {}

  // destroy - Called by subclass dtor so that we can still invoke virtual
  // functions on the subclass.
  void destroy() const;

  // printValue - Overridden by template class to print out the value type...
  virtual void printValue(ostream &o) const = 0;

  // hasSomeData - Return true if some data has been aquired.  Avoid printing
  // lots of zero counts.
  //
  virtual bool hasSomeData() const = 0;
};

// Statistic Class - templated on the data type we are monitoring...
template <typename DataType=unsigned>
class Statistic : private StatisticBase {
  DataType Value;

  virtual void printValue(ostream &o) const { o << Value; }
  virtual bool hasSomeData() const { return Value != DataType(); }
public:
  // Normal constructor, default initialize data item...
  Statistic(const char *name) : StatisticBase(name), Value(DataType()) {}

  // Constructor to provide an initial value...
  Statistic(const DataType &Val, const char *name)
    : StatisticBase(name), Value(Val) {}

  // Print information when destroyed, iff command line option is specified
  ~Statistic() { destroy(); }

  // Allow use of this class as the value itself...
  inline operator DataType() const { return Value; }
  inline const DataType &operator=(DataType Val) { Value = Val; return Value; }
  inline const DataType &operator++() { return ++Value; }
  inline DataType operator++(int) { return Value++; }
  inline const DataType &operator+=(const DataType &V) { return Value += V; }
  inline const DataType &operator-=(const DataType &V) { return Value -= V; }
};

#endif
