//===- JITMemoryManagerTest.cpp - Unit tests for the JIT memory manager ---===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"
#include "llvm/ADT/OwningPtr.h"
#include "llvm/ExecutionEngine/JITMemoryManager.h"
#include "llvm/DerivedTypes.h"
#include "llvm/Function.h"
#include "llvm/GlobalValue.h"

using namespace llvm;

namespace {

Function *makeFakeFunction() {
  std::vector<const Type*> params;
  const FunctionType *FTy = FunctionType::get(Type::VoidTy, params, false);
  return Function::Create(FTy, GlobalValue::ExternalLinkage);
}

// Allocate three simple functions that fit in the initial slab.  This exercises
// the code in the case that we don't have to allocate more memory to store the
// function bodies.
TEST(JITMemoryManagerTest, NoAllocations) {
  OwningPtr<JITMemoryManager> MemMgr(
      JITMemoryManager::CreateDefaultMemManager());
  uintptr_t size;
  uint8_t *start;
  std::string Error;

  // Allocate the functions.
  OwningPtr<Function> F1(makeFakeFunction());
  size = 1024;
  start = MemMgr->startFunctionBody(F1.get(), size);
  memset(start, 0xFF, 1024);
  MemMgr->endFunctionBody(F1.get(), start, start + 1024);
  EXPECT_TRUE(MemMgr->CheckInvariants(Error)) << Error;

  OwningPtr<Function> F2(makeFakeFunction());
  size = 1024;
  start = MemMgr->startFunctionBody(F2.get(), size);
  memset(start, 0xFF, 1024);
  MemMgr->endFunctionBody(F2.get(), start, start + 1024);
  EXPECT_TRUE(MemMgr->CheckInvariants(Error)) << Error;

  OwningPtr<Function> F3(makeFakeFunction());
  size = 1024;
  start = MemMgr->startFunctionBody(F3.get(), size);
  memset(start, 0xFF, 1024);
  MemMgr->endFunctionBody(F3.get(), start, start + 1024);
  EXPECT_TRUE(MemMgr->CheckInvariants(Error)) << Error;

  // Deallocate them out of order, in case that matters.
  MemMgr->deallocateMemForFunction(F2.get());
  EXPECT_TRUE(MemMgr->CheckInvariants(Error)) << Error;
  MemMgr->deallocateMemForFunction(F1.get());
  EXPECT_TRUE(MemMgr->CheckInvariants(Error)) << Error;
  MemMgr->deallocateMemForFunction(F3.get());
  EXPECT_TRUE(MemMgr->CheckInvariants(Error)) << Error;
}

// Make three large functions that take up most of the space in the slab.  Then
// try allocating three smaller functions that don't require additional slabs.
TEST(JITMemoryManagerTest, TestCodeAllocation) {
  OwningPtr<JITMemoryManager> MemMgr(
      JITMemoryManager::CreateDefaultMemManager());
  uintptr_t size;
  uint8_t *start;
  std::string Error;

  // Big functions are a little less than the largest block size.
  const uintptr_t smallFuncSize = 1024;
  const uintptr_t bigFuncSize = (MemMgr->GetDefaultCodeSlabSize() -
                                 smallFuncSize * 2);

  // Allocate big functions
  OwningPtr<Function> F1(makeFakeFunction());
  size = bigFuncSize;
  start = MemMgr->startFunctionBody(F1.get(), size);
  ASSERT_LE(bigFuncSize, size);
  memset(start, 0xFF, bigFuncSize);
  MemMgr->endFunctionBody(F1.get(), start, start + bigFuncSize);
  EXPECT_TRUE(MemMgr->CheckInvariants(Error)) << Error;

  OwningPtr<Function> F2(makeFakeFunction());
  size = bigFuncSize;
  start = MemMgr->startFunctionBody(F2.get(), size);
  ASSERT_LE(bigFuncSize, size);
  memset(start, 0xFF, bigFuncSize);
  MemMgr->endFunctionBody(F2.get(), start, start + bigFuncSize);
  EXPECT_TRUE(MemMgr->CheckInvariants(Error)) << Error;

  OwningPtr<Function> F3(makeFakeFunction());
  size = bigFuncSize;
  start = MemMgr->startFunctionBody(F3.get(), size);
  ASSERT_LE(bigFuncSize, size);
  memset(start, 0xFF, bigFuncSize);
  MemMgr->endFunctionBody(F3.get(), start, start + bigFuncSize);
  EXPECT_TRUE(MemMgr->CheckInvariants(Error)) << Error;

  // Check that each large function took it's own slab.
  EXPECT_EQ(3U, MemMgr->GetNumCodeSlabs());

  // Allocate small functions
  OwningPtr<Function> F4(makeFakeFunction());
  size = smallFuncSize;
  start = MemMgr->startFunctionBody(F4.get(), size);
  ASSERT_LE(smallFuncSize, size);
  memset(start, 0xFF, smallFuncSize);
  MemMgr->endFunctionBody(F4.get(), start, start + smallFuncSize);
  EXPECT_TRUE(MemMgr->CheckInvariants(Error)) << Error;

  OwningPtr<Function> F5(makeFakeFunction());
  size = smallFuncSize;
  start = MemMgr->startFunctionBody(F5.get(), size);
  ASSERT_LE(smallFuncSize, size);
  memset(start, 0xFF, smallFuncSize);
  MemMgr->endFunctionBody(F5.get(), start, start + smallFuncSize);
  EXPECT_TRUE(MemMgr->CheckInvariants(Error)) << Error;

  OwningPtr<Function> F6(makeFakeFunction());
  size = smallFuncSize;
  start = MemMgr->startFunctionBody(F6.get(), size);
  ASSERT_LE(smallFuncSize, size);
  memset(start, 0xFF, smallFuncSize);
  MemMgr->endFunctionBody(F6.get(), start, start + smallFuncSize);
  EXPECT_TRUE(MemMgr->CheckInvariants(Error)) << Error;

  // Check that the small functions didn't allocate any new slabs.
  EXPECT_EQ(3U, MemMgr->GetNumCodeSlabs());

  // Deallocate them out of order, in case that matters.
  MemMgr->deallocateMemForFunction(F2.get());
  EXPECT_TRUE(MemMgr->CheckInvariants(Error)) << Error;
  MemMgr->deallocateMemForFunction(F1.get());
  EXPECT_TRUE(MemMgr->CheckInvariants(Error)) << Error;
  MemMgr->deallocateMemForFunction(F4.get());
  EXPECT_TRUE(MemMgr->CheckInvariants(Error)) << Error;
  MemMgr->deallocateMemForFunction(F3.get());
  EXPECT_TRUE(MemMgr->CheckInvariants(Error)) << Error;
  MemMgr->deallocateMemForFunction(F5.get());
  EXPECT_TRUE(MemMgr->CheckInvariants(Error)) << Error;
  MemMgr->deallocateMemForFunction(F6.get());
  EXPECT_TRUE(MemMgr->CheckInvariants(Error)) << Error;
}

// Allocate five global ints of varying widths and alignment, and check their
// alignment and overlap.
TEST(JITMemoryManagerTest, TestSmallGlobalInts) {
  OwningPtr<JITMemoryManager> MemMgr(
      JITMemoryManager::CreateDefaultMemManager());
  uint8_t  *a = (uint8_t *)MemMgr->allocateGlobal(8,  0);
  uint16_t *b = (uint16_t*)MemMgr->allocateGlobal(16, 2);
  uint32_t *c = (uint32_t*)MemMgr->allocateGlobal(32, 4);
  uint64_t *d = (uint64_t*)MemMgr->allocateGlobal(64, 8);

  // Check the alignment.
  EXPECT_EQ(0U, ((uintptr_t)b) & 0x1);
  EXPECT_EQ(0U, ((uintptr_t)c) & 0x3);
  EXPECT_EQ(0U, ((uintptr_t)d) & 0x7);

  // Initialize them each one at a time and make sure they don't overlap.
  *a = 0xff;
  *b = 0U;
  *c = 0U;
  *d = 0U;
  EXPECT_EQ(0xffU, *a);
  EXPECT_EQ(0U, *b);
  EXPECT_EQ(0U, *c);
  EXPECT_EQ(0U, *d);
  *a = 0U;
  *b = 0xffffU;
  EXPECT_EQ(0U, *a);
  EXPECT_EQ(0xffffU, *b);
  EXPECT_EQ(0U, *c);
  EXPECT_EQ(0U, *d);
  *b = 0U;
  *c = 0xffffffffU;
  EXPECT_EQ(0U, *a);
  EXPECT_EQ(0U, *b);
  EXPECT_EQ(0xffffffffU, *c);
  EXPECT_EQ(0U, *d);
  *c = 0U;
  *d = 0xffffffffffffffffU;
  EXPECT_EQ(0U, *a);
  EXPECT_EQ(0U, *b);
  EXPECT_EQ(0U, *c);
  EXPECT_EQ(0xffffffffffffffffU, *d);

  // Make sure we didn't allocate any extra slabs for this tiny amount of data.
  EXPECT_EQ(1U, MemMgr->GetNumDataSlabs());
}

// Allocate a small global, a big global, and a third global, and make sure we
// only use two slabs for that.
TEST(JITMemoryManagerTest, TestLargeGlobalArray) {
  OwningPtr<JITMemoryManager> MemMgr(
      JITMemoryManager::CreateDefaultMemManager());
  size_t Size = 4 * MemMgr->GetDefaultDataSlabSize();
  uint64_t *a = (uint64_t*)MemMgr->allocateGlobal(64, 8);
  uint8_t *g = MemMgr->allocateGlobal(Size, 8);
  uint64_t *b = (uint64_t*)MemMgr->allocateGlobal(64, 8);

  // Check the alignment.
  EXPECT_EQ(0U, ((uintptr_t)a) & 0x7);
  EXPECT_EQ(0U, ((uintptr_t)g) & 0x7);
  EXPECT_EQ(0U, ((uintptr_t)b) & 0x7);

  // Initialize them to make sure we don't segfault and make sure they don't
  // overlap.
  memset(a, 0x1, 8);
  memset(g, 0x2, Size);
  memset(b, 0x3, 8);
  EXPECT_EQ(0x0101010101010101U, *a);
  // Just check the edges.
  EXPECT_EQ(0x02U, g[0]);
  EXPECT_EQ(0x02U, g[Size - 1]);
  EXPECT_EQ(0x0303030303030303U, *b);

  // Check the number of slabs.
  EXPECT_EQ(2U, MemMgr->GetNumDataSlabs());
}

// Allocate lots of medium globals so that we can test moving the bump allocator
// to a new slab.
TEST(JITMemoryManagerTest, TestManyGlobals) {
  OwningPtr<JITMemoryManager> MemMgr(
      JITMemoryManager::CreateDefaultMemManager());
  size_t SlabSize = MemMgr->GetDefaultDataSlabSize();
  size_t Size = 128;
  int Iters = (SlabSize / Size) + 1;

  // We should start with one slab.
  EXPECT_EQ(1U, MemMgr->GetNumDataSlabs());

  // After allocating a bunch of globals, we should have two.
  for (int I = 0; I < Iters; ++I)
    MemMgr->allocateGlobal(Size, 8);
  EXPECT_EQ(2U, MemMgr->GetNumDataSlabs());

  // And after much more, we should have three.
  for (int I = 0; I < Iters; ++I)
    MemMgr->allocateGlobal(Size, 8);
  EXPECT_EQ(3U, MemMgr->GetNumDataSlabs());
}

// Allocate lots of function stubs so that we can test moving the stub bump
// allocator to a new slab.
TEST(JITMemoryManagerTest, TestManyStubs) {
  OwningPtr<JITMemoryManager> MemMgr(
      JITMemoryManager::CreateDefaultMemManager());
  size_t SlabSize = MemMgr->GetDefaultStubSlabSize();
  size_t Size = 128;
  int Iters = (SlabSize / Size) + 1;

  // We should start with one slab.
  EXPECT_EQ(1U, MemMgr->GetNumStubSlabs());

  // After allocating a bunch of stubs, we should have two.
  for (int I = 0; I < Iters; ++I)
    MemMgr->allocateStub(NULL, Size, 8);
  EXPECT_EQ(2U, MemMgr->GetNumStubSlabs());

  // And after much more, we should have three.
  for (int I = 0; I < Iters; ++I)
    MemMgr->allocateStub(NULL, Size, 8);
  EXPECT_EQ(3U, MemMgr->GetNumStubSlabs());
}

}
