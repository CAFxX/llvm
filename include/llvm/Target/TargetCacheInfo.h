// $Id$ -*-c++-*-
//***************************************************************************
// File:
//	MachineCacheInfo.h
// 
// Purpose:
//      Describes properties of the target cache architecture.
//**************************************************************************/

#ifndef LLVM_TARGET_MACHINECACHEINFO_H
#define LLVM_TARGET_MACHINECACHEINFO_H

#include "llvm/Target/TargetMachine.h"
#include "llvm/Support/DataTypes.h"
#include <vector>


//---------------------------------------------------------------------------
// class MachineCacheInfo 
// 
// Purpose:
//   Describes properties of the target cache architecture.
//---------------------------------------------------------------------------

class MachineCacheInfo : public NonCopyableV {
public:
  const TargetMachine& target;
  
protected:
  unsigned int           numLevels;
  vector<unsigned short> cacheLineSizes;
  vector<unsigned int>   cacheSizes;
  vector<unsigned short> cacheAssoc;
  
public:
  /*ctor*/          MachineCacheInfo    (const TargetMachine& tgt);
  /*dtor*/ virtual ~MachineCacheInfo    () {}
  
  // Default parameters are:
  //    NumLevels    = 2
  //    L1: LineSize 16, Cache Size 32KB, Direct-mapped (assoc = 1)
  //    L2: LineSize 32, Cache Size 1 MB, 4-way associative
  // NOTE: Cache levels are numbered from 1 as above, not from 0.
  // 
  virtual  void     Initialize          (); // subclass to override defaults
  
  unsigned int      getNumCacheLevels   () const {
    return numLevels;
  }
  unsigned short    getCacheLineSize    (unsigned level)  const {
    assert(level <= cacheLineSizes.size() && "Invalid cache level");
    return cacheLineSizes[level-1];
  }
  unsigned int      getCacheSize        (unsigned level)  const {
    assert(level <= cacheSizes.size() && "Invalid cache level");
    return cacheSizes[level-1];
  }
  unsigned short    getCacheAssoc       (unsigned level)  const {
    assert(level <= cacheAssoc.size() && "Invalid cache level");
    return cacheAssoc[level];
  }
};


//---------------------------------------------------------------------------

#endif
