//===-- LiveIntervals.cpp - Live Interval Analysis ------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file was developed by the LLVM research group and is distributed under
// the University of Illinois Open Source License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implements the LiveInterval analysis pass which is used
// by the Linear Scan Register allocator. This pass linearizes the
// basic blocks of the function in DFS order and uses the
// LiveVariables pass to conservatively compute live intervals for
// each virtual and physical register.
//
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "liveintervals"
#include "llvm/CodeGen/LiveIntervals.h"
#include "llvm/Function.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/CodeGen/LiveVariables.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/Passes.h"
#include "llvm/CodeGen/SSARegMap.h"
#include "llvm/Target/MRegisterInfo.h"
#include "llvm/Target/TargetInstrInfo.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetRegInfo.h"
#include "llvm/Support/CFG.h"
#include "Support/Debug.h"
#include "Support/DepthFirstIterator.h"
#include "Support/Statistic.h"
#include <cmath>
#include <iostream>
#include <limits>

using namespace llvm;

namespace {
    RegisterAnalysis<LiveIntervals> X("liveintervals",
                                      "Live Interval Analysis");

    Statistic<> numIntervals("liveintervals", "Number of intervals");
};

void LiveIntervals::getAnalysisUsage(AnalysisUsage &AU) const
{
    AU.addPreserved<LiveVariables>();
    AU.addRequired<LiveVariables>();
    AU.addPreservedID(PHIEliminationID);
    AU.addRequiredID(PHIEliminationID);
    AU.addRequiredID(TwoAddressInstructionPassID);
    AU.addRequired<LoopInfo>();
    MachineFunctionPass::getAnalysisUsage(AU);
}

/// runOnMachineFunction - Register allocate the whole function
///
bool LiveIntervals::runOnMachineFunction(MachineFunction &fn) {
    DEBUG(std::cerr << "Machine Function\n");
    mf_ = &fn;
    tm_ = &fn.getTarget();
    mri_ = tm_->getRegisterInfo();
    lv_ = &getAnalysis<LiveVariables>();
    mbbi2mbbMap_.clear();
    mi2iMap_.clear();
    r2iMap_.clear();
    r2iMap_.clear();
    intervals_.clear();

    // number MachineInstrs
    unsigned miIndex = 0;
    for (MachineFunction::iterator mbb = mf_->begin(), mbbEnd = mf_->end();
         mbb != mbbEnd; ++mbb) {
        const std::pair<MachineBasicBlock*, unsigned>& entry =
            lv_->getMachineBasicBlockInfo(&*mbb);
        bool inserted = mbbi2mbbMap_.insert(std::make_pair(entry.second,
                                                           entry.first)).second;
        assert(inserted && "multiple index -> MachineBasicBlock");

        for (MachineBasicBlock::iterator mi = mbb->begin(), miEnd = mbb->end();
             mi != miEnd; ++mi) {
            inserted = mi2iMap_.insert(std::make_pair(*mi, miIndex)).second;
            assert(inserted && "multiple MachineInstr -> index mappings");
            ++miIndex;
        }
    }

    computeIntervals();

    // compute spill weights
    const LoopInfo& loopInfo = getAnalysis<LoopInfo>();
    const TargetInstrInfo& tii = tm_->getInstrInfo();

    for (MbbIndex2MbbMap::iterator
             it = mbbi2mbbMap_.begin(), itEnd = mbbi2mbbMap_.end();
         it != itEnd; ++it) {
        MachineBasicBlock* mbb = it->second;

        unsigned loopDepth = loopInfo.getLoopDepth(mbb->getBasicBlock());

        for (MachineBasicBlock::iterator mi = mbb->begin(), miEnd = mbb->end();
             mi != miEnd; ++mi) {
            MachineInstr* instr = *mi;
            for (int i = instr->getNumOperands() - 1; i >= 0; --i) {
                MachineOperand& mop = instr->getOperand(i);

                if (!mop.isVirtualRegister())
                    continue;

                unsigned reg = mop.getAllocatedRegNum();
                Reg2IntervalMap::iterator r2iit = r2iMap_.find(reg);
                assert(r2iit != r2iMap_.end());
                intervals_[r2iit->second].weight += pow(10.0F, loopDepth);
            }
        }
    }

    return true;
}

void LiveIntervals::printRegName(unsigned reg) const
{
    if (reg < MRegisterInfo::FirstVirtualRegister)
        std::cerr << mri_->getName(reg);
    else
        std::cerr << '%' << reg;
}

void LiveIntervals::handleVirtualRegisterDef(MachineBasicBlock* mbb,
                                             MachineBasicBlock::iterator mi,
                                             unsigned reg)
{
    DEBUG(std::cerr << "\t\tregister: ";printRegName(reg); std::cerr << '\n');

    unsigned instrIndex = getInstructionIndex(*mi);

    LiveVariables::VarInfo& vi = lv_->getVarInfo(reg);

    Interval* interval = 0;
    Reg2IntervalMap::iterator r2iit = r2iMap_.find(reg);
    if (r2iit == r2iMap_.end()) {
        // add new interval
        intervals_.push_back(Interval(reg));
        // update interval index for this register
        r2iMap_[reg] = intervals_.size() - 1;
        interval = &intervals_.back();
    }
    else {
        interval = &intervals_[r2iit->second];
    }

    for (MbbIndex2MbbMap::iterator
             it = mbbi2mbbMap_.begin(), itEnd = mbbi2mbbMap_.end();
         it != itEnd; ++it) {
        unsigned liveBlockIndex = it->first;
        MachineBasicBlock* liveBlock = it->second;
        if (liveBlockIndex < vi.AliveBlocks.size() &&
            vi.AliveBlocks[liveBlockIndex] &&
            !liveBlock->empty()) {
            unsigned start =  getInstructionIndex(liveBlock->front());
            unsigned end = getInstructionIndex(liveBlock->back()) + 1;
            interval->addRange(start, end);
        }
    }

    bool killedInDefiningBasicBlock = false;
    for (int i = 0, e = vi.Kills.size(); i != e; ++i) {
        MachineBasicBlock* killerBlock = vi.Kills[i].first;
        MachineInstr* killerInstr = vi.Kills[i].second;
        unsigned start = (mbb == killerBlock ?
                          instrIndex :
                          getInstructionIndex(killerBlock->front()));
        unsigned end = getInstructionIndex(killerInstr) + 1;
        if (start < end) {
            killedInDefiningBasicBlock |= mbb == killerBlock;
            interval->addRange(start, end);
        }
    }

    if (!killedInDefiningBasicBlock) {
        unsigned end = getInstructionIndex(mbb->back()) + 1;
        interval->addRange(instrIndex, end);
    }
}

void LiveIntervals::handlePhysicalRegisterDef(MachineBasicBlock* mbb,
                                              MachineBasicBlock::iterator mi,
                                              unsigned reg)
{
    DEBUG(std::cerr << "\t\tregister: "; printRegName(reg));

    unsigned start = getInstructionIndex(*mi);
    unsigned end = start;

    // register can be dead by the instruction defining it but it can
    // only be killed by subsequent instructions

    for (LiveVariables::killed_iterator
             ki = lv_->dead_begin(*mi),
             ke = lv_->dead_end(*mi);
         ki != ke; ++ki) {
        if (reg == ki->second) {
            end = getInstructionIndex(ki->first) + 1;
            DEBUG(std::cerr << " dead\n");
            goto exit;
        }
    }
    ++mi;

    for (MachineBasicBlock::iterator e = mbb->end(); mi != e; ++mi) {
        for (LiveVariables::killed_iterator
                 ki = lv_->dead_begin(*mi),
                 ke = lv_->dead_end(*mi);
             ki != ke; ++ki) {
            if (reg == ki->second) {
                end = getInstructionIndex(ki->first) + 1;
                DEBUG(std::cerr << " dead\n");
                goto exit;
            }
        }

        for (LiveVariables::killed_iterator
                 ki = lv_->killed_begin(*mi),
                 ke = lv_->killed_end(*mi);
             ki != ke; ++ki) {
            if (reg == ki->second) {
                end = getInstructionIndex(ki->first) + 1;
                DEBUG(std::cerr << " killed\n");
                goto exit;
            }
        }
    }
exit:
    assert(start < end && "did not find end of interval?");

    Reg2IntervalMap::iterator r2iit = r2iMap_.find(reg);
    if (r2iit != r2iMap_.end()) {
        unsigned ii = r2iit->second;
        Interval& interval = intervals_[ii];
        interval.addRange(start, end);
    }
    else {
        intervals_.push_back(Interval(reg));
        Interval& interval = intervals_.back();
        // update interval index for this register
        r2iMap_[reg] = intervals_.size() - 1;
        interval.addRange(start, end);
    }
}

void LiveIntervals::handleRegisterDef(MachineBasicBlock* mbb,
                                      MachineBasicBlock::iterator mi,
                                      unsigned reg)
{
    if (reg < MRegisterInfo::FirstVirtualRegister) {
        if (lv_->getAllocatablePhysicalRegisters()[reg]) {
            handlePhysicalRegisterDef(mbb, mi, reg);
            for (const unsigned* as = mri_->getAliasSet(reg); *as; ++as)
                handlePhysicalRegisterDef(mbb, mi, *as);
        }
    }
    else {
        handleVirtualRegisterDef(mbb, mi, reg);
    }
}

unsigned LiveIntervals::getInstructionIndex(MachineInstr* instr) const
{
    assert(mi2iMap_.find(instr) != mi2iMap_.end() &&
           "instruction not assigned a number");
    return mi2iMap_.find(instr)->second;
}

/// computeIntervals - computes the live intervals for virtual
/// registers. for some ordering of the machine instructions [1,N] a
/// live interval is an interval [i, j] where 1 <= i <= j <= N for
/// which a variable is live
void LiveIntervals::computeIntervals()
{
    DEBUG(std::cerr << "computing live intervals:\n");

    for (MbbIndex2MbbMap::iterator
             it = mbbi2mbbMap_.begin(), itEnd = mbbi2mbbMap_.end();
         it != itEnd; ++it) {
        MachineBasicBlock* mbb = it->second;
        DEBUG(std::cerr << "machine basic block: "
              << mbb->getBasicBlock()->getName() << "\n");

        for (MachineBasicBlock::iterator mi = mbb->begin(), miEnd = mbb->end();
             mi != miEnd; ++mi) {
            MachineInstr* instr = *mi;
            const TargetInstrDescriptor& tid =
                tm_->getInstrInfo().get(instr->getOpcode());
            DEBUG(std::cerr << "\t[" << getInstructionIndex(instr) << "] ";
                  instr->print(std::cerr, *tm_););

            // handle implicit defs
            for (const unsigned* id = tid.ImplicitDefs; *id; ++id)
                handleRegisterDef(mbb, mi, *id);

            // handle explicit defs
            for (int i = instr->getNumOperands() - 1; i >= 0; --i) {
                MachineOperand& mop = instr->getOperand(i);

                if (!mop.isRegister())
                    continue;

                // handle defs - build intervals
                if (mop.isDef())
                    handleRegisterDef(mbb, mi, mop.getAllocatedRegNum());
            }
        }
    }

    std::sort(intervals_.begin(), intervals_.end(), StartPointComp());
    DEBUG(std::copy(intervals_.begin(), intervals_.end(),
                    std::ostream_iterator<Interval>(std::cerr, "\n")));
}

LiveIntervals::Interval::Interval(unsigned r)
    : reg(r), hint(0),
      weight((r < MRegisterInfo::FirstVirtualRegister ?
              std::numeric_limits<float>::max() : 0.0F))
{

}

void LiveIntervals::Interval::addRange(unsigned start, unsigned end)
{
    DEBUG(std::cerr << "\t\t\tadding range: [" << start <<','<< end << ") -> ");
    //assert(start < end && "invalid range?");
    Range range = std::make_pair(start, end);
    Ranges::iterator it =
        ranges.insert(std::upper_bound(ranges.begin(), ranges.end(), range),
                      range);

    mergeRangesForward(it);
    mergeRangesBackward(it);
    DEBUG(std::cerr << *this << '\n');
}

void LiveIntervals::Interval::mergeRangesForward(Ranges::iterator it)
{
    for (Ranges::iterator next = it + 1;
         next != ranges.end() && it->second >= next->first; ) {
        it->second = std::max(it->second, next->second);
        next = ranges.erase(next);
    }
}

void LiveIntervals::Interval::mergeRangesBackward(Ranges::iterator it)
{
    for (Ranges::iterator prev = it - 1;
         it != ranges.begin() && it->first <= prev->second; ) {
        it->first = std::min(it->first, prev->first);
        it->second = std::max(it->second, prev->second);
        it = ranges.erase(prev);
        prev = it - 1;
    }
}

bool LiveIntervals::Interval::liveAt(unsigned index) const
{
    Ranges::const_iterator r = ranges.begin();
    while (r != ranges.end() && index < (r->second - 1)) {
        if (index >= r->first)
            return true;
        ++r;
    }
    return false;
}

bool LiveIntervals::Interval::overlaps(const Interval& other) const
{
    Ranges::const_iterator i = ranges.begin();
    Ranges::const_iterator j = other.ranges.begin();

    while (i != ranges.end() && j != other.ranges.end()) {
        if (i->first < j->first) {
            if ((i->second - 1) > j->first) {
                return true;
            }
            else {
                ++i;
            }
        }
        else if (j->first < i->first) {
            if ((j->second - 1) > i->first) {
                return true;
            }
            else {
                ++j;
            }
        }
        else {
            return true;
        }
    }

    return false;
}

std::ostream& llvm::operator<<(std::ostream& os,
                               const LiveIntervals::Interval& li)
{
    os << "%reg" << li.reg << ',' << li.weight << " = ";
    for (LiveIntervals::Interval::Ranges::const_iterator
             i = li.ranges.begin(), e = li.ranges.end(); i != e; ++i) {
        os << "[" << i->first << "," << i->second << ")";
    }
    return os;
}
