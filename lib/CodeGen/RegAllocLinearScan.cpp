//===-- RegAllocLinearScan.cpp - Linear Scan register allocator -----------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file was developed by the LLVM research group and is distributed under
// the University of Illinois Open Source License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implements a linear scan register allocator.
//
//===----------------------------------------------------------------------===//
#define DEBUG_TYPE "regalloc"
#include "llvm/Function.h"
#include "llvm/CodeGen/LiveIntervals.h"
#include "llvm/CodeGen/LiveVariables.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/Passes.h"
#include "llvm/CodeGen/SSARegMap.h"
#include "llvm/Target/MRegisterInfo.h"
#include "llvm/Target/TargetInstrInfo.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Support/CFG.h"
#include "Support/Debug.h"
#include "Support/DepthFirstIterator.h"
#include "Support/Statistic.h"
#include "Support/STLExtras.h"
#include <algorithm>
using namespace llvm;

namespace {
    Statistic<> numStores("ra-linearscan", "Number of stores added");
    Statistic<> numLoads ("ra-linearscan", "Number of loads added");

    class PhysRegTracker {
    private:
        const MRegisterInfo* mri_;
        std::vector<unsigned> regUse_;

    public:
        PhysRegTracker(MachineFunction* mf)
            : mri_(mf ? mf->getTarget().getRegisterInfo() : NULL) {
            if (mri_) {
                regUse_.assign(mri_->getNumRegs(), 0);
            }
        }

        PhysRegTracker(const PhysRegTracker& rhs)
            : mri_(rhs.mri_),
              regUse_(rhs.regUse_) {
        }

        const PhysRegTracker& operator=(const PhysRegTracker& rhs) {
            mri_ = rhs.mri_;
            regUse_ = rhs.regUse_;
            return *this;
        }

        void addPhysRegUse(unsigned physReg) {
            ++regUse_[physReg];
            for (const unsigned* as = mri_->getAliasSet(physReg); *as; ++as) {
                physReg = *as;
                ++regUse_[physReg];
            }
        }

        void delPhysRegUse(unsigned physReg) {
            assert(regUse_[physReg] != 0);
            --regUse_[physReg];
            for (const unsigned* as = mri_->getAliasSet(physReg); *as; ++as) {
                physReg = *as;
                assert(regUse_[physReg] != 0);
                --regUse_[physReg];
            }
        }

        bool isPhysRegAvail(unsigned physReg) const {
            return regUse_[physReg] == 0;
        }
    };

    class RA : public MachineFunctionPass {
    private:
        MachineFunction* mf_;
        const TargetMachine* tm_;
        const MRegisterInfo* mri_;
        LiveIntervals* li_;
        typedef std::list<LiveIntervals::Interval*> IntervalPtrs;
        IntervalPtrs unhandled_, fixed_, active_, inactive_, handled_;

        PhysRegTracker prt_;

        typedef std::map<unsigned, unsigned> Virt2PhysMap;
        Virt2PhysMap v2pMap_;

        typedef std::map<unsigned, int> Virt2StackSlotMap;
        Virt2StackSlotMap v2ssMap_;

        int instrAdded_;

        typedef std::vector<float> SpillWeights;
        SpillWeights spillWeights_;

    public:
        RA()
            : prt_(NULL) {

        }

        virtual const char* getPassName() const {
            return "Linear Scan Register Allocator";
        }

        virtual void getAnalysisUsage(AnalysisUsage &AU) const {
            AU.addRequired<LiveVariables>();
            AU.addRequired<LiveIntervals>();
            MachineFunctionPass::getAnalysisUsage(AU);
        }

        /// runOnMachineFunction - register allocate the whole function
        bool runOnMachineFunction(MachineFunction&);

        void releaseMemory();

    private:
        /// initIntervalSets - initializa the four interval sets:
        /// unhandled, fixed, active and inactive
        void initIntervalSets(LiveIntervals::Intervals& li);

        /// processActiveIntervals - expire old intervals and move
        /// non-overlapping ones to the incative list
        void processActiveIntervals(IntervalPtrs::value_type cur);

        /// processInactiveIntervals - expire old intervals and move
        /// overlapping ones to the active list
        void processInactiveIntervals(IntervalPtrs::value_type cur);

        /// updateSpillWeights - updates the spill weights of the
        /// specifed physical register and its weight
        void updateSpillWeights(unsigned reg, SpillWeights::value_type weight);

        /// assignRegOrStackSlotAtInterval - assign a register if one
        /// is available, or spill.
        void assignRegOrStackSlotAtInterval(IntervalPtrs::value_type cur);

        /// addSpillCode - adds spill code for interval. The interval
        /// must be modified by LiveIntervals::updateIntervalForSpill.
        void addSpillCode(IntervalPtrs::value_type li, int slot);

        ///
        /// register handling helpers
        ///

        /// getFreePhysReg - return a free physical register for this
        /// virtual register interval if we have one, otherwise return
        /// 0
        unsigned getFreePhysReg(IntervalPtrs::value_type cur);

        /// assignVirt2PhysReg - assigns the free physical register to
        /// the virtual register passed as arguments
        Virt2PhysMap::iterator
        assignVirt2PhysReg(unsigned virtReg, unsigned physReg);

        /// clearVirtReg - free the physical register associated with this
        /// virtual register and disassociate virtual->physical and
        /// physical->virtual mappings
        void clearVirtReg(Virt2PhysMap::iterator it);

        /// assignVirt2StackSlot - assigns this virtual register to a
        /// stack slot. returns the stack slot
        int assignVirt2StackSlot(unsigned virtReg);

        /// getStackSlot - returns the offset of the specified
        /// register on the stack
        int getStackSlot(unsigned virtReg);

        void printVirtRegAssignment() const {
            std::cerr << "register assignment:\n";

            for (Virt2PhysMap::const_iterator
                     i = v2pMap_.begin(), e = v2pMap_.end(); i != e; ++i) {
                assert(i->second != 0);
                std::cerr << '[' << i->first << ','
                          << mri_->getName(i->second) << "]\n";
            }
            for (Virt2StackSlotMap::const_iterator
                     i = v2ssMap_.begin(), e = v2ssMap_.end(); i != e; ++i) {
                std::cerr << '[' << i->first << ",ss#" << i->second << "]\n";
            }
            std::cerr << '\n';
        }

        void printIntervals(const char* const str,
                            RA::IntervalPtrs::const_iterator i,
                            RA::IntervalPtrs::const_iterator e) const {
            if (str) std::cerr << str << " intervals:\n";
            for (; i != e; ++i) {
                std::cerr << "\t\t" << **i << " -> ";
                unsigned reg = (*i)->reg;
                if (MRegisterInfo::isVirtualRegister(reg)) {
                    Virt2PhysMap::const_iterator it = v2pMap_.find(reg);
                    reg = (it == v2pMap_.end() ? 0 : it->second);
                }
                std::cerr << mri_->getName(reg) << '\n';
            }
        }

        void verifyAssignment() const {
            for (Virt2PhysMap::const_iterator i = v2pMap_.begin(),
                     e = v2pMap_.end(); i != e; ++i)
                for (Virt2PhysMap::const_iterator i2 = i; i2 != e; ++i2)
                    if (mri_->areAliases(i->second, i2->second)) {
                        const LiveIntervals::Interval
                            &in = li_->getInterval(i->second),
                            &in2 = li_->getInterval(i2->second);
                        assert(!in.overlaps(in2) &&
                               "overlapping intervals for same register!");
                    }
        }
    };
}

void RA::releaseMemory()
{
    v2pMap_.clear();
    v2ssMap_.clear();
    unhandled_.clear();
    active_.clear();
    inactive_.clear();
    fixed_.clear();
    handled_.clear();
}

bool RA::runOnMachineFunction(MachineFunction &fn) {
    mf_ = &fn;
    tm_ = &fn.getTarget();
    mri_ = tm_->getRegisterInfo();
    li_ = &getAnalysis<LiveIntervals>();
    prt_ = PhysRegTracker(mf_);

    initIntervalSets(li_->getIntervals());

    // linear scan algorithm
    DEBUG(std::cerr << "Machine Function\n");

    DEBUG(printIntervals("\tunhandled", unhandled_.begin(), unhandled_.end()));
    DEBUG(printIntervals("\tfixed", fixed_.begin(), fixed_.end()));
    DEBUG(printIntervals("\tactive", active_.begin(), active_.end()));
    DEBUG(printIntervals("\tinactive", inactive_.begin(), inactive_.end()));

    while (!unhandled_.empty() || !fixed_.empty()) {
        // pick the interval with the earliest start point
        IntervalPtrs::value_type cur;
        if (fixed_.empty()) {
            cur = unhandled_.front();
            unhandled_.pop_front();
        }
        else if (unhandled_.empty()) {
            cur = fixed_.front();
            fixed_.pop_front();
        }
        else if (unhandled_.front()->start() < fixed_.front()->start()) {
            cur = unhandled_.front();
            unhandled_.pop_front();
        }
        else {
            cur = fixed_.front();
            fixed_.pop_front();
        }

        DEBUG(std::cerr << *cur << '\n');

        processActiveIntervals(cur);
        processInactiveIntervals(cur);

        // if this register is fixed we are done
        if (MRegisterInfo::isPhysicalRegister(cur->reg)) {
            prt_.addPhysRegUse(cur->reg);
            active_.push_back(cur);
            handled_.push_back(cur);
        }
        // otherwise we are allocating a virtual register. try to find
        // a free physical register or spill an interval in order to
        // assign it one (we could spill the current though).
        else {
            assignRegOrStackSlotAtInterval(cur);
        }

        DEBUG(printIntervals("\tactive", active_.begin(), active_.end()));
        DEBUG(printIntervals("\tinactive", inactive_.begin(), inactive_.end()));    }

    // expire any remaining active intervals
    for (IntervalPtrs::iterator i = active_.begin(); i != active_.end(); ++i) {
        unsigned reg = (*i)->reg;
        DEBUG(std::cerr << "\t\tinterval " << **i << " expired\n");
        if (MRegisterInfo::isVirtualRegister(reg)) {
            reg = v2pMap_[reg];
        }
        prt_.delPhysRegUse(reg);
    }

    DEBUG(printVirtRegAssignment());
    DEBUG(std::cerr << "finished register allocation\n");
    // this is a slow operations do not uncomment
    // DEBUG(verifyAssignment());

    const TargetInstrInfo& tii = tm_->getInstrInfo();

    DEBUG(std::cerr << "Rewrite machine code:\n");
    for (MachineFunction::iterator mbbi = mf_->begin(), mbbe = mf_->end();
         mbbi != mbbe; ++mbbi) {
        instrAdded_ = 0;

        for (MachineBasicBlock::iterator mii = mbbi->begin(), mie = mbbi->end();
             mii != mie; ++mii) {
            DEBUG(std::cerr << '\t'; mii->print(std::cerr, *tm_));

            // use our current mapping and actually replace every
            // virtual register with its allocated physical registers
            DEBUG(std::cerr << "\t\treplacing virtual registers with mapped "
                  "physical registers:\n");
            for (unsigned i = 0, e = mii->getNumOperands();
                 i != e; ++i) {
                MachineOperand& op = mii->getOperand(i);
                if (op.isRegister() &&
                    MRegisterInfo::isVirtualRegister(op.getReg())) {
                    unsigned virtReg = op.getReg();
                    Virt2PhysMap::iterator it = v2pMap_.find(virtReg);
                    assert(it != v2pMap_.end() &&
                           "all virtual registers must be allocated");
                    unsigned physReg = it->second;
                    assert(MRegisterInfo::isPhysicalRegister(physReg));
                    DEBUG(std::cerr << "\t\t\t%reg" << virtReg
                          << " -> " << mri_->getName(physReg) << '\n');
                    mii->SetMachineOperandReg(i, physReg);
                }
            }
        }
    }

    return true;
}

void RA::initIntervalSets(LiveIntervals::Intervals& li)
{
    assert(unhandled_.empty() && fixed_.empty() &&
           active_.empty() && inactive_.empty() &&
           "interval sets should be empty on initialization");

    for (LiveIntervals::Intervals::iterator i = li.begin(), e = li.end();
         i != e; ++i) {
        if (MRegisterInfo::isPhysicalRegister(i->reg))
            fixed_.push_back(&*i);
        else
            unhandled_.push_back(&*i);
    }
}

void RA::processActiveIntervals(IntervalPtrs::value_type cur)
{
    DEBUG(std::cerr << "\tprocessing active intervals:\n");
    for (IntervalPtrs::iterator i = active_.begin(); i != active_.end();) {
        unsigned reg = (*i)->reg;
        // remove expired intervals
        if ((*i)->expiredAt(cur->start())) {
            DEBUG(std::cerr << "\t\tinterval " << **i << " expired\n");
            if (MRegisterInfo::isVirtualRegister(reg)) {
                reg = v2pMap_[reg];
            }
            prt_.delPhysRegUse(reg);
            // remove from active
            i = active_.erase(i);
        }
        // move inactive intervals to inactive list
        else if (!(*i)->liveAt(cur->start())) {
            DEBUG(std::cerr << "\t\t\tinterval " << **i << " inactive\n");
            if (MRegisterInfo::isVirtualRegister(reg)) {
                reg = v2pMap_[reg];
            }
            prt_.delPhysRegUse(reg);
            // add to inactive
            inactive_.push_back(*i);
            // remove from active
            i = active_.erase(i);
        }
        else {
            ++i;
        }
    }
}

void RA::processInactiveIntervals(IntervalPtrs::value_type cur)
{
    DEBUG(std::cerr << "\tprocessing inactive intervals:\n");
    for (IntervalPtrs::iterator i = inactive_.begin(); i != inactive_.end();) {
        unsigned reg = (*i)->reg;

        // remove expired intervals
        if ((*i)->expiredAt(cur->start())) {
            DEBUG(std::cerr << "\t\t\tinterval " << **i << " expired\n");
            // remove from inactive
            i = inactive_.erase(i);
        }
        // move re-activated intervals in active list
        else if ((*i)->liveAt(cur->start())) {
            DEBUG(std::cerr << "\t\t\tinterval " << **i << " active\n");
            if (MRegisterInfo::isVirtualRegister(reg)) {
                reg = v2pMap_[reg];
            }
            prt_.addPhysRegUse(reg);
            // add to active
            active_.push_back(*i);
            // remove from inactive
            i = inactive_.erase(i);
        }
        else {
            ++i;
        }
    }
}

void RA::updateSpillWeights(unsigned reg, SpillWeights::value_type weight)
{
    spillWeights_[reg] += weight;
    for (const unsigned* as = mri_->getAliasSet(reg); *as; ++as)
        spillWeights_[*as] += weight;
}

void RA::assignRegOrStackSlotAtInterval(IntervalPtrs::value_type cur)
{
    DEBUG(std::cerr << "\tallocating current interval:\n");

    PhysRegTracker backupPrt = prt_;

    spillWeights_.assign(mri_->getNumRegs(), 0.0);

    // for each interval in active update spill weights
    for (IntervalPtrs::const_iterator i = active_.begin(), e = active_.end();
         i != e; ++i) {
        unsigned reg = (*i)->reg;
        if (MRegisterInfo::isVirtualRegister(reg))
            reg = v2pMap_[reg];
        updateSpillWeights(reg, (*i)->weight);
    }

    // for every interval in inactive we overlap with, mark the
    // register as not free and update spill weights
    for (IntervalPtrs::const_iterator i = inactive_.begin(),
             e = inactive_.end(); i != e; ++i) {
        if (cur->overlaps(**i)) {
            unsigned reg = (*i)->reg;
            if (MRegisterInfo::isVirtualRegister(reg))
                reg = v2pMap_[reg];
            prt_.addPhysRegUse(reg);
            updateSpillWeights(reg, (*i)->weight);
        }
    }

    // for every interval in fixed we overlap with,
    // mark the register as not free and update spill weights
    for (IntervalPtrs::const_iterator i = fixed_.begin(),
             e = fixed_.end(); i != e; ++i) {
        if (cur->overlaps(**i)) {
            unsigned reg = (*i)->reg;
            prt_.addPhysRegUse(reg);
            updateSpillWeights(reg, (*i)->weight);
        }
    }

    unsigned physReg = getFreePhysReg(cur);
    // restore the physical register tracker
    prt_ = backupPrt;
    // if we find a free register, we are done: assign this virtual to
    // the free physical register and add this interval to the active
    // list.
    if (physReg) {
        assignVirt2PhysReg(cur->reg, physReg);
        active_.push_back(cur);
        handled_.push_back(cur);
        return;
    }

    DEBUG(std::cerr << "\t\tassigning stack slot at interval "<< *cur << ":\n");
    // push the current interval back to unhandled since we are going
    // to re-run at least this iteration
    unhandled_.push_front(cur);

    float minWeight = std::numeric_limits<float>::infinity();
    unsigned minReg = 0;
    const TargetRegisterClass* rc = mf_->getSSARegMap()->getRegClass(cur->reg);
    for (TargetRegisterClass::iterator i = rc->allocation_order_begin(*mf_);
         i != rc->allocation_order_end(*mf_); ++i) {
        unsigned reg = *i;
        if (minWeight > spillWeights_[reg]) {
            minWeight = spillWeights_[reg];
            minReg = reg;
        }
    }
    DEBUG(std::cerr << "\t\t\tregister with min weight: "
          << mri_->getName(minReg) << " (" << minWeight << ")\n");

    // if the current has the minimum weight, we need to modify it,
    // push it back in unhandled and let the linear scan algorithm run
    // again
    if (cur->weight < minWeight) {
        DEBUG(std::cerr << "\t\t\t\tspilling(c): " << *cur;);
        int slot = assignVirt2StackSlot(cur->reg);
        li_->updateSpilledInterval(*cur);
        addSpillCode(cur, slot);
        DEBUG(std::cerr << "[ " << *cur << " ]\n");
        return;
    }

    // otherwise we spill all intervals aliasing the register with
    // minimum weight, rollback to the interval with the earliest
    // start point and let the linear scan algorithm run again
    std::vector<bool> toSpill(mri_->getNumRegs(), false);
    toSpill[minReg] = true;
    for (const unsigned* as = mri_->getAliasSet(minReg); *as; ++as)
        toSpill[*as] = true;
    unsigned earliestStart = cur->start();

    for (IntervalPtrs::iterator i = active_.begin();
         i != active_.end(); ++i) {
        unsigned reg = (*i)->reg;
        if (MRegisterInfo::isVirtualRegister(reg) &&
            toSpill[v2pMap_[reg]] &&
            cur->overlaps(**i)) {
            DEBUG(std::cerr << "\t\t\t\tspilling(a): " << **i);
            int slot = assignVirt2StackSlot((*i)->reg);
            li_->updateSpilledInterval(**i);
            addSpillCode(*i, slot);
            DEBUG(std::cerr << "[ " << **i << " ]\n");
            earliestStart = std::min(earliestStart, (*i)->start());
        }
    }
    for (IntervalPtrs::iterator i = inactive_.begin();
         i != inactive_.end(); ++i) {
        unsigned reg = (*i)->reg;
        if (MRegisterInfo::isVirtualRegister(reg) &&
            toSpill[v2pMap_[reg]] &&
            cur->overlaps(**i)) {
            DEBUG(std::cerr << "\t\t\t\tspilling(i): " << **i << '\n');
            int slot = assignVirt2StackSlot((*i)->reg);
            li_->updateSpilledInterval(**i);
            addSpillCode(*i, slot);
            DEBUG(std::cerr << "[ " << **i << " ]\n");
            earliestStart = std::min(earliestStart, (*i)->start());
        }
    }

    DEBUG(std::cerr << "\t\t\t\trolling back to: " << earliestStart << '\n');
    // scan handled in reverse order and undo each one, restoring the
    // state of unhandled and fixed
    while (!handled_.empty()) {
        IntervalPtrs::value_type i = handled_.back();
        // if this interval starts before t we are done
        if (i->start() < earliestStart)
            break;
        DEBUG(std::cerr << "\t\t\t\t\tundo changes for: " << *i << '\n');
        handled_.pop_back();
        IntervalPtrs::iterator it;
        if ((it = find(active_.begin(), active_.end(), i)) != active_.end()) {
            active_.erase(it);
            if (MRegisterInfo::isPhysicalRegister(i->reg)) {
                fixed_.push_front(i);
                prt_.delPhysRegUse(i->reg);
            }
            else {
                Virt2PhysMap::iterator v2pIt = v2pMap_.find(i->reg);
                clearVirtReg(v2pIt);
                unhandled_.push_front(i);
                prt_.delPhysRegUse(v2pIt->second);
            }
        }
        else if ((it = find(inactive_.begin(), inactive_.end(), i)) != inactive_.end()) {
            inactive_.erase(it);
            if (MRegisterInfo::isPhysicalRegister(i->reg))
                fixed_.push_front(i);
            else {
                Virt2PhysMap::iterator v2pIt = v2pMap_.find(i->reg);
                clearVirtReg(v2pIt);
                unhandled_.push_front(i);
            }
        }
        else {
            if (MRegisterInfo::isPhysicalRegister(i->reg))
                fixed_.push_front(i);
            else {
                Virt2PhysMap::iterator v2pIt = v2pMap_.find(i->reg);
                clearVirtReg(v2pIt);
                unhandled_.push_front(i);
            }
        }
    }

    // scan the rest and undo each interval that expired after t and
    // insert it in active (the next iteration of the algorithm will
    // put it in inactive if required)
    IntervalPtrs::iterator i = handled_.begin(), e = handled_.end();
    for (; i != e; ++i) {
        if (!(*i)->expiredAt(earliestStart) && (*i)->expiredAt(cur->start())) {
            DEBUG(std::cerr << "\t\t\t\t\tundo changes for: " << **i << '\n');
            active_.push_back(*i);
            if (MRegisterInfo::isPhysicalRegister((*i)->reg))
                prt_.addPhysRegUse((*i)->reg);
            else {
                assert(v2pMap_.count((*i)->reg));
                prt_.addPhysRegUse(v2pMap_.find((*i)->reg)->second);
            }
        }
    }
}

void RA::addSpillCode(IntervalPtrs::value_type li, int slot)
{
    // We scan the instructions corresponding to each range. We load
    // when we have a use and spill at end of basic blocks or end of
    // ranges only if the register was modified.
    const TargetRegisterClass* rc = mf_->getSSARegMap()->getRegClass(li->reg);

    for (LiveIntervals::Interval::Ranges::iterator i = li->ranges.begin(),
             e = li->ranges.end(); i != e; ++i) {
        unsigned index = i->first & ~1;
        unsigned end = i->second;

    entry:
        bool dirty = false, loaded = false;

        // skip deleted instructions. getInstructionFromIndex returns
        // null if the instruction was deleted (because of coalescing
        // for example)
        while (!li_->getInstructionFromIndex(index)) index += 2;
        MachineBasicBlock::iterator mi = li_->getInstructionFromIndex(index);
        MachineBasicBlock* mbb = mi->getParent();

        for (; index < end; index += 2) {
            // ignore deleted instructions
            while (!li_->getInstructionFromIndex(index)) index += 2;

            // if we changed basic block we need to start over
            mi = li_->getInstructionFromIndex(index);
            if (mbb != mi->getParent()) {
                if (dirty) {
                    mi = li_->getInstructionFromIndex(index-2);
                    assert(mbb == mi->getParent() &&
                           "rewound to wrong instruction?");
                    DEBUG(std::cerr << "add store for reg" << li->reg << " to "
                          "stack slot " << slot << " after: ";
                          mi->print(std::cerr, *tm_));
                    ++numStores;
                    mri_->storeRegToStackSlot(*mi->getParent(),
                                              next(mi), li->reg, slot, rc);
                }
                goto entry;
            }

            // if it is used in this instruction load it
            for (unsigned i = 0; i < mi->getNumOperands(); ++i) {
                MachineOperand& mop = mi->getOperand(i);
                if (mop.isRegister() && mop.getReg() == li->reg &&
                    mop.isUse() && !loaded) {
                    loaded = true;
                    DEBUG(std::cerr << "add load for reg" << li->reg
                          << " from stack slot " << slot << " before: ";
                          mi->print(std::cerr, *tm_));
                    ++numLoads;
                    mri_->loadRegFromStackSlot(*mi->getParent(),
                                               mi, li->reg, slot, rc);
                }
            }

            // if it is defined in this instruction mark as dirty
            for (unsigned i = 0; i < mi->getNumOperands(); ++i) {
                MachineOperand& mop = mi->getOperand(i);
                if (mop.isRegister() && mop.getReg() == li->reg &&
                    mop.isDef())
                    dirty = loaded = true;
            }
        }
        if (dirty) {
            mi = li_->getInstructionFromIndex(index-2);
            assert(mbb == mi->getParent() &&
                   "rewound to wrong instruction?");
            DEBUG(std::cerr << "add store for reg" << li->reg << " to "
                  "stack slot " << slot << " after: ";
                  mi->print(std::cerr, *tm_));
            ++numStores;
            mri_->storeRegToStackSlot(*mi->getParent(),
                                      next(mi), li->reg, slot, rc);
        }
    }
}

unsigned RA::getFreePhysReg(IntervalPtrs::value_type cur)
{
    DEBUG(std::cerr << "\t\tgetting free physical register: ");
    const TargetRegisterClass* rc = mf_->getSSARegMap()->getRegClass(cur->reg);

    for (TargetRegisterClass::iterator i = rc->allocation_order_begin(*mf_);
         i != rc->allocation_order_end(*mf_); ++i) {
        unsigned reg = *i;
        if (prt_.isPhysRegAvail(reg)) {
            DEBUG(std::cerr << mri_->getName(reg) << '\n');
            return reg;
        }
    }

    DEBUG(std::cerr << "no free register\n");
    return 0;
}

RA::Virt2PhysMap::iterator
RA::assignVirt2PhysReg(unsigned virtReg, unsigned physReg)
{
    bool inserted;
    Virt2PhysMap::iterator it;
    tie(it, inserted) = v2pMap_.insert(std::make_pair(virtReg, physReg));
    assert(inserted && "attempting to assign a virt->phys mapping to an "
           "already mapped register");
    prt_.addPhysRegUse(physReg);
    return it;
}

void RA::clearVirtReg(Virt2PhysMap::iterator it)
{
    assert(it != v2pMap_.end() &&
           "attempting to clear a not allocated virtual register");
    unsigned physReg = it->second;
    v2pMap_.erase(it);
    DEBUG(std::cerr << "\t\t\tcleared register " << mri_->getName(physReg)
          << "\n");
}


int RA::assignVirt2StackSlot(unsigned virtReg)
{
    const TargetRegisterClass* rc = mf_->getSSARegMap()->getRegClass(virtReg);
    int frameIndex = mf_->getFrameInfo()->CreateStackObject(rc);

    bool inserted = v2ssMap_.insert(std::make_pair(virtReg, frameIndex)).second;
    assert(inserted && "attempt to assign stack slot to spilled register!");
    return frameIndex;
}

int RA::getStackSlot(unsigned virtReg)
{
    assert(v2ssMap_.count(virtReg) &&
           "attempt to get stack slot for a non spilled register");
    return v2ssMap_.find(virtReg)->second;
}

FunctionPass* llvm::createLinearScanRegisterAllocator() {
    return new RA();
}
