//===-- LiveIntervalAnalysis.cpp - Live Interval Analysis -----------------===//
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
#include "LiveIntervalAnalysis.h"
#include "llvm/Value.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/CodeGen/LiveVariables.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/Passes.h"
#include "llvm/CodeGen/SSARegMap.h"
#include "llvm/Target/MRegisterInfo.h"
#include "llvm/Target/TargetInstrInfo.h"
#include "llvm/Target/TargetMachine.h"
#include "Support/CommandLine.h"
#include "Support/Debug.h"
#include "Support/Statistic.h"
#include "Support/STLExtras.h"
#include "VirtRegMap.h"
#include <cmath>

using namespace llvm;

namespace {
    RegisterAnalysis<LiveIntervals> X("liveintervals",
                                      "Live Interval Analysis");

    Statistic<> numIntervals
    ("liveintervals", "Number of original intervals");

    Statistic<> numIntervalsAfter
    ("liveintervals", "Number of intervals after coalescing");

    Statistic<> numJoins
    ("liveintervals", "Number of interval joins performed");

    Statistic<> numPeep
    ("liveintervals", "Number of identity moves eliminated after coalescing");

    Statistic<> numFolded
    ("liveintervals", "Number of loads/stores folded into instructions");

    cl::opt<bool>
    EnableJoining("join-liveintervals",
                  cl::desc("Join compatible live intervals"),
                  cl::init(true));
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

void LiveIntervals::releaseMemory()
{
    mi2iMap_.clear();
    i2miMap_.clear();
    r2iMap_.clear();
    r2rMap_.clear();
    intervals_.clear();
}


/// runOnMachineFunction - Register allocate the whole function
///
bool LiveIntervals::runOnMachineFunction(MachineFunction &fn) {
    mf_ = &fn;
    tm_ = &fn.getTarget();
    mri_ = tm_->getRegisterInfo();
    lv_ = &getAnalysis<LiveVariables>();

    // number MachineInstrs
    unsigned miIndex = 0;
    for (MachineFunction::iterator mbb = mf_->begin(), mbbEnd = mf_->end();
         mbb != mbbEnd; ++mbb)
        for (MachineBasicBlock::iterator mi = mbb->begin(), miEnd = mbb->end();
             mi != miEnd; ++mi) {
            bool inserted = mi2iMap_.insert(std::make_pair(mi, miIndex)).second;
            assert(inserted && "multiple MachineInstr -> index mappings");
            i2miMap_.push_back(mi);
            miIndex += InstrSlots::NUM;
        }

    computeIntervals();

    numIntervals += intervals_.size();

    // join intervals if requested
    if (EnableJoining) joinIntervals();

    numIntervalsAfter += intervals_.size();

    // perform a final pass over the instructions and compute spill
    // weights, coalesce virtual registers and remove identity moves
    const LoopInfo& loopInfo = getAnalysis<LoopInfo>();
    const TargetInstrInfo& tii = *tm_->getInstrInfo();

    for (MachineFunction::iterator mbbi = mf_->begin(), mbbe = mf_->end();
         mbbi != mbbe; ++mbbi) {
        MachineBasicBlock* mbb = mbbi;
        unsigned loopDepth = loopInfo.getLoopDepth(mbb->getBasicBlock());

        for (MachineBasicBlock::iterator mii = mbb->begin(), mie = mbb->end();
             mii != mie; ) {
            // if the move will be an identity move delete it
            unsigned srcReg, dstReg;
            if (tii.isMoveInstr(*mii, srcReg, dstReg) &&
                rep(srcReg) == rep(dstReg)) {
                // remove from def list
                LiveInterval& interval = getOrCreateInterval(rep(dstReg));
                // remove index -> MachineInstr and
                // MachineInstr -> index mappings
                Mi2IndexMap::iterator mi2i = mi2iMap_.find(mii);
                if (mi2i != mi2iMap_.end()) {
                    i2miMap_[mi2i->second/InstrSlots::NUM] = 0;
                    mi2iMap_.erase(mi2i);
                }
                mii = mbbi->erase(mii);
                ++numPeep;
            }
            else {
                for (unsigned i = 0; i < mii->getNumOperands(); ++i) {
                    const MachineOperand& mop = mii->getOperand(i);
                    if (mop.isRegister() && mop.getReg() &&
                        MRegisterInfo::isVirtualRegister(mop.getReg())) {
                        // replace register with representative register
                        unsigned reg = rep(mop.getReg());
                        mii->SetMachineOperandReg(i, reg);

                        Reg2IntervalMap::iterator r2iit = r2iMap_.find(reg);
                        assert(r2iit != r2iMap_.end());
                        r2iit->second->weight +=
                            (mop.isUse() + mop.isDef()) * pow(10.0F, loopDepth);
                    }
                }
                ++mii;
            }
        }
    }

    DEBUG(std::cerr << "********** INTERVALS **********\n");
    DEBUG(std::copy(intervals_.begin(), intervals_.end(),
                    std::ostream_iterator<LiveInterval>(std::cerr, "\n")));
    DEBUG(std::cerr << "********** MACHINEINSTRS **********\n");
    DEBUG(
        for (MachineFunction::iterator mbbi = mf_->begin(), mbbe = mf_->end();
             mbbi != mbbe; ++mbbi) {
            std::cerr << ((Value*)mbbi->getBasicBlock())->getName() << ":\n";
            for (MachineBasicBlock::iterator mii = mbbi->begin(),
                     mie = mbbi->end(); mii != mie; ++mii) {
                std::cerr << getInstructionIndex(mii) << '\t';
                mii->print(std::cerr, tm_);
            }
        });

    return true;
}

std::vector<LiveInterval*> LiveIntervals::addIntervalsForSpills(
    const LiveInterval& li,
    VirtRegMap& vrm,
    int slot)
{
    std::vector<LiveInterval*> added;

    assert(li.weight != HUGE_VAL &&
           "attempt to spill already spilled interval!");

    DEBUG(std::cerr << "\t\t\t\tadding intervals for spills for interval: "
          << li << '\n');

    const TargetRegisterClass* rc = mf_->getSSARegMap()->getRegClass(li.reg);

    for (LiveInterval::Ranges::const_iterator
              i = li.ranges.begin(), e = li.ranges.end(); i != e; ++i) {
        unsigned index = getBaseIndex(i->start);
        unsigned end = getBaseIndex(i->end-1) + InstrSlots::NUM;
        for (; index != end; index += InstrSlots::NUM) {
            // skip deleted instructions
            while (index != end && !getInstructionFromIndex(index))
                index += InstrSlots::NUM;
            if (index == end) break;

            MachineBasicBlock::iterator mi = getInstructionFromIndex(index);

        for_operand:
            for (unsigned i = 0; i != mi->getNumOperands(); ++i) {
                MachineOperand& mop = mi->getOperand(i);
                if (mop.isRegister() && mop.getReg() == li.reg) {
                    if (MachineInstr* fmi =
                        mri_->foldMemoryOperand(mi, i, slot)) {
                        lv_->instructionChanged(mi, fmi);
                        vrm.virtFolded(li.reg, mi, fmi);
                        mi2iMap_.erase(mi);
                        i2miMap_[index/InstrSlots::NUM] = fmi;
                        mi2iMap_[fmi] = index;
                        MachineBasicBlock& mbb = *mi->getParent();
                        mi = mbb.insert(mbb.erase(mi), fmi);
                        ++numFolded;
                        goto for_operand;
                    }
                    else {
                        // This is tricky. We need to add information in
                        // the interval about the spill code so we have to
                        // use our extra load/store slots.
                        //
                        // If we have a use we are going to have a load so
                        // we start the interval from the load slot
                        // onwards. Otherwise we start from the def slot.
                        unsigned start = (mop.isUse() ?
                                          getLoadIndex(index) :
                                          getDefIndex(index));
                        // If we have a def we are going to have a store
                        // right after it so we end the interval after the
                        // use of the next instruction. Otherwise we end
                        // after the use of this instruction.
                        unsigned end = 1 + (mop.isDef() ?
                                            getStoreIndex(index) :
                                            getUseIndex(index));

                        // create a new register for this spill
                        unsigned nReg =
                            mf_->getSSARegMap()->createVirtualRegister(rc);
                        mi->SetMachineOperandReg(i, nReg);
                        vrm.grow();
                        vrm.assignVirt2StackSlot(nReg, slot);
                        LiveInterval& nI = getOrCreateInterval(nReg);
                        assert(nI.empty());
                        // the spill weight is now infinity as it
                        // cannot be spilled again
                        nI.weight = HUGE_VAL;
                        DEBUG(std::cerr << " +" << LiveRange(start, end));
                        nI.addRange(LiveRange(start, end));
                        added.push_back(&nI);
                        // update live variables
                        lv_->addVirtualRegisterKilled(nReg, mi);
                        DEBUG(std::cerr << "\t\t\t\tadded new interval: "
                              << nI << '\n');
                    }
                }
            }
        }
    }

    return added;
}

void LiveIntervals::printRegName(unsigned reg) const
{
    if (MRegisterInfo::isPhysicalRegister(reg))
        std::cerr << mri_->getName(reg);
    else
        std::cerr << "%reg" << reg;
}

void LiveIntervals::handleVirtualRegisterDef(MachineBasicBlock* mbb,
                                             MachineBasicBlock::iterator mi,
                                             LiveInterval& interval)
{
    DEBUG(std::cerr << "\t\tregister: "; printRegName(interval.reg));
    LiveVariables::VarInfo& vi = lv_->getVarInfo(interval.reg);

    // Virtual registers may be defined multiple times (due to phi 
    // elimination and 2-addr elimination).  Much of what we do only has to be 
    // done once for the vreg.  We use an empty interval to detect the first 
    // time we see a vreg.
    if (interval.empty()) {
       // Assume this interval is singly defined until we find otherwise.
       interval.isDefinedOnce = true;

       // Get the Idx of the defining instructions.
       unsigned defIndex = getDefIndex(getInstructionIndex(mi));

       // Loop over all of the blocks that the vreg is defined in.  There are
       // two cases we have to handle here.  The most common case is a vreg
       // whose lifetime is contained within a basic block.  In this case there
       // will be a single kill, in MBB, which comes after the definition.
       if (vi.Kills.size() == 1 && vi.Kills[0]->getParent() == mbb) {
           // FIXME: what about dead vars?
           unsigned killIdx;
           if (vi.Kills[0] != mi)
               killIdx = getUseIndex(getInstructionIndex(vi.Kills[0]))+1;
           else
               killIdx = defIndex+1;

           // If the kill happens after the definition, we have an intra-block
           // live range.
           if (killIdx > defIndex) {
              assert(vi.AliveBlocks.empty() && 
                     "Shouldn't be alive across any blocks!");
              interval.addRange(LiveRange(defIndex, killIdx));
              DEBUG(std::cerr << " +" << LiveRange(defIndex, killIdx) << "\n");
              return;
           }
       }

       // The other case we handle is when a virtual register lives to the end
       // of the defining block, potentially live across some blocks, then is
       // live into some number of blocks, but gets killed.  Start by adding a
       // range that goes from this definition to the end of the defining block.
       LiveRange NewLR(defIndex, getInstructionIndex(&mbb->back()) +
                                                   InstrSlots::NUM);
       DEBUG(std::cerr << " +" << NewLR);
       interval.addRange(NewLR);

       // Iterate over all of the blocks that the variable is completely
       // live in, adding [insrtIndex(begin), instrIndex(end)+4) to the
       // live interval.
       for (unsigned i = 0, e = vi.AliveBlocks.size(); i != e; ++i) {
           if (vi.AliveBlocks[i]) {
               MachineBasicBlock* mbb = mf_->getBlockNumbered(i);
               if (!mbb->empty()) {
                 LiveRange LR(getInstructionIndex(&mbb->front()),
                             getInstructionIndex(&mbb->back())+InstrSlots::NUM);
                 interval.addRange(LR);
                 DEBUG(std::cerr << " +" << LR);
               }
           }
       }

       // Finally, this virtual register is live from the start of any killing
       // block to the 'use' slot of the killing instruction.
       for (unsigned i = 0, e = vi.Kills.size(); i != e; ++i) {
           MachineInstr *Kill = vi.Kills[i];
           LiveRange LR(getInstructionIndex(Kill->getParent()->begin()),
                        getUseIndex(getInstructionIndex(Kill))+1);
           interval.addRange(LR);
           DEBUG(std::cerr << " +" << LR);
       }

    } else {
       // If this is the second time we see a virtual register definition, it
       // must be due to phi elimination or two addr elimination.  If this is
       // the result of two address elimination, then the vreg is the first
       // operand, and is a def-and-use.
       if (mi->getOperand(0).isRegister() && 
           mi->getOperand(0).getReg() == interval.reg &&
           mi->getOperand(0).isDef() && mi->getOperand(0).isUse()) {
         // If this is a two-address definition, just ignore it.
       } else {
         // Otherwise, this must be because of phi elimination.  In this case, 
         // the defined value will be live until the end of the basic block it
         // is defined in.
         unsigned defIndex = getDefIndex(getInstructionIndex(mi));
         LiveRange LR(defIndex, 
                      getInstructionIndex(&mbb->back()) +InstrSlots::NUM);
         interval.addRange(LR);
         DEBUG(std::cerr << " +" << LR);
       }
       interval.isDefinedOnce = false;
    }

    DEBUG(std::cerr << '\n');
}

void LiveIntervals::handlePhysicalRegisterDef(MachineBasicBlock *MBB,
                                              MachineBasicBlock::iterator mi,
                                              LiveInterval& interval)
{
    // A physical register cannot be live across basic block, so its
    // lifetime must end somewhere in its defining basic block.
    DEBUG(std::cerr << "\t\tregister: "; printRegName(interval.reg));
    typedef LiveVariables::killed_iterator KillIter;

    unsigned baseIndex = getInstructionIndex(mi);
    unsigned start = getDefIndex(baseIndex);
    unsigned end = start;

    // If it is not used after definition, it is considered dead at
    // the instruction defining it. Hence its interval is:
    // [defSlot(def), defSlot(def)+1)
    for (KillIter ki = lv_->dead_begin(mi), ke = lv_->dead_end(mi);
         ki != ke; ++ki) {
        if (interval.reg == ki->second) {
            DEBUG(std::cerr << " dead");
            end = getDefIndex(start) + 1;
            goto exit;
        }
    }

    // If it is not dead on definition, it must be killed by a
    // subsequent instruction. Hence its interval is:
    // [defSlot(def), useSlot(kill)+1)
    while (1) {
        ++mi;
        assert(mi != MBB->end() && "physreg was not killed in defining block!");
        baseIndex += InstrSlots::NUM;
        for (KillIter ki = lv_->killed_begin(mi), ke = lv_->killed_end(mi);
             ki != ke; ++ki) {
            if (interval.reg == ki->second) {
                DEBUG(std::cerr << " killed");
                end = getUseIndex(baseIndex) + 1;
                goto exit;
            }
        }
    }

exit:
    assert(start < end && "did not find end of interval?");
    interval.addRange(LiveRange(start, end));
    DEBUG(std::cerr << " +" << LiveRange(start, end) << '\n');
}

void LiveIntervals::handleRegisterDef(MachineBasicBlock *MBB,
                                      MachineBasicBlock::iterator MI,
                                      unsigned reg) {
  if (MRegisterInfo::isVirtualRegister(reg))
    handleVirtualRegisterDef(MBB, MI, getOrCreateInterval(reg));
  else if (lv_->getAllocatablePhysicalRegisters()[reg]) {
    handlePhysicalRegisterDef(MBB, MI, getOrCreateInterval(reg));
    for (const unsigned* AS = mri_->getAliasSet(reg); *AS; ++AS)
      handlePhysicalRegisterDef(MBB, MI, getOrCreateInterval(*AS));
  }
}

/// computeIntervals - computes the live intervals for virtual
/// registers. for some ordering of the machine instructions [1,N] a
/// live interval is an interval [i, j) where 1 <= i <= j < N for
/// which a variable is live
void LiveIntervals::computeIntervals()
{
    DEBUG(std::cerr << "********** COMPUTING LIVE INTERVALS **********\n");
    DEBUG(std::cerr << "********** Function: "
          << ((Value*)mf_->getFunction())->getName() << '\n');

    for (MachineFunction::iterator I = mf_->begin(), E = mf_->end(); 
         I != E; ++I) {
        MachineBasicBlock* mbb = I;
        DEBUG(std::cerr << ((Value*)mbb->getBasicBlock())->getName() << ":\n");

        for (MachineBasicBlock::iterator mi = mbb->begin(), miEnd = mbb->end();
             mi != miEnd; ++mi) {
            const TargetInstrDescriptor& tid =
                tm_->getInstrInfo()->get(mi->getOpcode());
            DEBUG(std::cerr << getInstructionIndex(mi) << "\t";
                  mi->print(std::cerr, tm_));

            // handle implicit defs
            for (const unsigned* id = tid.ImplicitDefs; *id; ++id)
                handleRegisterDef(mbb, mi, *id);

            // handle explicit defs
            for (int i = mi->getNumOperands() - 1; i >= 0; --i) {
                MachineOperand& mop = mi->getOperand(i);
                // handle register defs - build intervals
                if (mop.isRegister() && mop.getReg() && mop.isDef())
                    handleRegisterDef(mbb, mi, mop.getReg());
            }
        }
    }
}

void LiveIntervals::joinIntervalsInMachineBB(MachineBasicBlock *MBB) {
    DEBUG(std::cerr << ((Value*)MBB->getBasicBlock())->getName() << ":\n");
    const TargetInstrInfo& tii = *tm_->getInstrInfo();

    for (MachineBasicBlock::iterator mi = MBB->begin(), mie = MBB->end();
         mi != mie; ++mi) {
        const TargetInstrDescriptor& tid = tii.get(mi->getOpcode());
        DEBUG(std::cerr << getInstructionIndex(mi) << '\t';
              mi->print(std::cerr, tm_););

        // we only join virtual registers with allocatable
        // physical registers since we do not have liveness information
        // on not allocatable physical registers
        unsigned regA, regB;
        if (tii.isMoveInstr(*mi, regA, regB) &&
            (MRegisterInfo::isVirtualRegister(regA) ||
             lv_->getAllocatablePhysicalRegisters()[regA]) &&
            (MRegisterInfo::isVirtualRegister(regB) ||
             lv_->getAllocatablePhysicalRegisters()[regB])) {

            // get representative registers
            regA = rep(regA);
            regB = rep(regB);

            // if they are already joined we continue
            if (regA == regB)
                continue;

            Reg2IntervalMap::iterator r2iA = r2iMap_.find(regA);
            assert(r2iA != r2iMap_.end() &&
                   "Found unknown vreg in 'isMoveInstr' instruction");
            Reg2IntervalMap::iterator r2iB = r2iMap_.find(regB);
            assert(r2iB != r2iMap_.end() &&
                   "Found unknown vreg in 'isMoveInstr' instruction");

            Intervals::iterator intA = r2iA->second;
            Intervals::iterator intB = r2iB->second;

            DEBUG(std::cerr << "\t\tInspecting " << *intA << " and " << *intB 
                            << ": ");

            // both A and B are virtual registers
            if (MRegisterInfo::isVirtualRegister(intA->reg) &&
                MRegisterInfo::isVirtualRegister(intB->reg)) {

                const TargetRegisterClass *rcA, *rcB;
                rcA = mf_->getSSARegMap()->getRegClass(intA->reg);
                rcB = mf_->getSSARegMap()->getRegClass(intB->reg);

                // if they are not of the same register class we continue
                if (rcA != rcB) {
                    DEBUG(std::cerr << "Differing reg classes.\n");
                    continue;
                }

                // if their intervals do not overlap we join them.
                if ((intA->containsOneValue() && intB->containsOneValue()) ||
                    !intB->overlaps(*intA)) {
                    intA->join(*intB);
                    ++numJoins;
                    DEBUG(std::cerr << "Joined.  Result = " << *intA << "\n");
                    r2iB->second = r2iA->second;
                    r2rMap_.insert(std::make_pair(intB->reg, intA->reg));
                    intervals_.erase(intB);
                } else {
                    DEBUG(std::cerr << "Interference!\n");
                }
            } else if (!MRegisterInfo::isPhysicalRegister(intA->reg) ||
                       !MRegisterInfo::isPhysicalRegister(intB->reg)) {
                if (MRegisterInfo::isPhysicalRegister(intB->reg)) {
                    std::swap(regA, regB);
                    std::swap(intA, intB);
                    std::swap(r2iA, r2iB);
                }

                assert(MRegisterInfo::isPhysicalRegister(intA->reg) &&
                       MRegisterInfo::isVirtualRegister(intB->reg) &&
                       "A must be physical and B must be virtual");

                const TargetRegisterClass *rcA, *rcB;
                rcA = mri_->getRegClass(intA->reg);
                rcB = mf_->getSSARegMap()->getRegClass(intB->reg);
                // if they are not of the same register class we continue
                if (rcA != rcB) {
                    DEBUG(std::cerr << "Differing reg classes.\n");
                    continue;
                }

                if (!intA->overlaps(*intB) &&
                    !overlapsAliases(*intA, *intB)) {
                    intA->join(*intB);
                    ++numJoins;
                    DEBUG(std::cerr << "Joined.  Result = " << *intA << "\n");
                    r2iB->second = r2iA->second;
                    r2rMap_.insert(std::make_pair(intB->reg, intA->reg));
                    intervals_.erase(intB);
                } else {
                    DEBUG(std::cerr << "Interference!\n");
                }
            } else {
                DEBUG(std::cerr << "Cannot join physregs.\n");
            }
        }
    }
}

namespace {
  // DepthMBBCompare - Comparison predicate that sort first based on the loop
  // depth of the basic block (the unsigned), and then on the MBB number.
  struct DepthMBBCompare {
    typedef std::pair<unsigned, MachineBasicBlock*> DepthMBBPair;
    bool operator()(const DepthMBBPair &LHS, const DepthMBBPair &RHS) const {
      if (LHS.first > RHS.first) return true;   // Deeper loops first
      return LHS.first == RHS.first && 
             LHS.second->getNumber() < RHS.second->getNumber();
    }
  };
}

void LiveIntervals::joinIntervals() {
  DEBUG(std::cerr << "********** JOINING INTERVALS ***********\n");

  const LoopInfo &LI = getAnalysis<LoopInfo>();
  if (LI.begin() == LI.end()) {
    // If there are no loops in the function, join intervals in function order.
    for (MachineFunction::iterator I = mf_->begin(), E = mf_->end();
         I != E; ++I)
      joinIntervalsInMachineBB(I);
  } else {
    // Otherwise, join intervals in inner loops before other intervals.
    // Unfortunately we can't just iterate over loop hierarchy here because
    // there may be more MBB's than BB's.  Collect MBB's for sorting.
    std::vector<std::pair<unsigned, MachineBasicBlock*> > MBBs;
    for (MachineFunction::iterator I = mf_->begin(), E = mf_->end();
         I != E; ++I)
      MBBs.push_back(std::make_pair(LI.getLoopDepth(I->getBasicBlock()), I));

    // Sort by loop depth.
    std::sort(MBBs.begin(), MBBs.end(), DepthMBBCompare());

    // Finally, join intervals in loop nest order. 
    for (unsigned i = 0, e = MBBs.size(); i != e; ++i)
      joinIntervalsInMachineBB(MBBs[i].second);
  }
}

bool LiveIntervals::overlapsAliases(const LiveInterval& lhs,
                                    const LiveInterval& rhs) const
{
    assert(MRegisterInfo::isPhysicalRegister(lhs.reg) &&
           "first interval must describe a physical register");

    for (const unsigned* as = mri_->getAliasSet(lhs.reg); *as; ++as) {
        Reg2IntervalMap::const_iterator r2i = r2iMap_.find(*as);
        assert(r2i != r2iMap_.end() && "alias does not have interval?");
        if (rhs.overlaps(*r2i->second))
            return true;
    }

    return false;
}

LiveInterval& LiveIntervals::getOrCreateInterval(unsigned reg)
{
    Reg2IntervalMap::iterator r2iit = r2iMap_.lower_bound(reg);
    if (r2iit == r2iMap_.end() || r2iit->first != reg) {
        float Weight = MRegisterInfo::isPhysicalRegister(reg) ?  HUGE_VAL :0.0F;
        intervals_.push_back(LiveInterval(reg, Weight));
        r2iit = r2iMap_.insert(r2iit, std::make_pair(reg, --intervals_.end()));
    }

    return *r2iit->second;
}

