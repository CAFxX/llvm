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
#include "llvm/CodeGen/LiveVariables.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/Passes.h"
#include "llvm/CodeGen/SSARegMap.h"
#include "llvm/Target/MRegisterInfo.h"
#include "llvm/Target/TargetMachine.h"
#include "Support/Debug.h"
#include "Support/Statistic.h"
#include "Support/STLExtras.h"
#include "LiveIntervalAnalysis.h"
#include "PhysRegTracker.h"
#include "VirtRegMap.h"
#include <algorithm>
#include <cmath>
#include <set>
#include <queue>

using namespace llvm;

namespace {

    Statistic<double> efficiency
    ("regalloc", "Ratio of intervals processed over total intervals");

    static unsigned numIterations = 0;
    static unsigned numIntervals = 0;

    class RA : public MachineFunctionPass {
    private:
        MachineFunction* mf_;
        const TargetMachine* tm_;
        const MRegisterInfo* mri_;
        LiveIntervals* li_;
        typedef std::vector<LiveInterval*> IntervalPtrs;
        IntervalPtrs handled_, fixed_, active_, inactive_;
        typedef std::priority_queue<LiveInterval*,
                                    IntervalPtrs,
                                    greater_ptr<LiveInterval> > IntervalHeap;
        IntervalHeap unhandled_;
        std::auto_ptr<PhysRegTracker> prt_;
        std::auto_ptr<VirtRegMap> vrm_;
        std::auto_ptr<Spiller> spiller_;

        typedef std::vector<float> SpillWeights;
        SpillWeights spillWeights_;

    public:
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
        /// linearScan - the linear scan algorithm
        void linearScan();

        /// initIntervalSets - initializa the four interval sets:
        /// unhandled, fixed, active and inactive
        void initIntervalSets();

        /// processActiveIntervals - expire old intervals and move
        /// non-overlapping ones to the incative list
        void processActiveIntervals(LiveInterval* cur);

        /// processInactiveIntervals - expire old intervals and move
        /// overlapping ones to the active list
        void processInactiveIntervals(LiveInterval* cur);

        /// updateSpillWeights - updates the spill weights of the
        /// specifed physical register and its weight
        void updateSpillWeights(unsigned reg, SpillWeights::value_type weight);

        /// assignRegOrStackSlotAtInterval - assign a register if one
        /// is available, or spill.
        void assignRegOrStackSlotAtInterval(LiveInterval* cur);

        ///
        /// register handling helpers
        ///

        /// getFreePhysReg - return a free physical register for this
        /// virtual register interval if we have one, otherwise return
        /// 0
        unsigned getFreePhysReg(LiveInterval* cur);

        /// assignVirt2StackSlot - assigns this virtual register to a
        /// stack slot. returns the stack slot
        int assignVirt2StackSlot(unsigned virtReg);

        template <typename ItTy>
        void printIntervals(const char* const str, ItTy i, ItTy e) const {
            if (str) std::cerr << str << " intervals:\n";
            for (; i != e; ++i) {
                std::cerr << "\t" << **i << " -> ";
                unsigned reg = (*i)->reg;
                if (MRegisterInfo::isVirtualRegister(reg)) {
                    reg = vrm_->getPhys(reg);
                }
                std::cerr << mri_->getName(reg) << '\n';
            }
        }
    };
}

void RA::releaseMemory()
{
    while (!unhandled_.empty()) unhandled_.pop();
    fixed_.clear();
    active_.clear();
    inactive_.clear();
    handled_.clear();
}

bool RA::runOnMachineFunction(MachineFunction &fn) {
    mf_ = &fn;
    tm_ = &fn.getTarget();
    mri_ = tm_->getRegisterInfo();
    li_ = &getAnalysis<LiveIntervals>();
    if (!prt_.get()) prt_.reset(new PhysRegTracker(*mri_));
    vrm_.reset(new VirtRegMap(*mf_));
    if (!spiller_.get()) spiller_.reset(createSpiller());

    initIntervalSets();

    linearScan();

    spiller_->runOnMachineFunction(*mf_, *vrm_);

    return true;
}

void RA::linearScan()
{
    // linear scan algorithm
    DEBUG(std::cerr << "********** LINEAR SCAN **********\n");
    DEBUG(std::cerr << "********** Function: "
          << mf_->getFunction()->getName() << '\n');

    // DEBUG(printIntervals("unhandled", unhandled_.begin(), unhandled_.end()));
    DEBUG(printIntervals("fixed", fixed_.begin(), fixed_.end()));
    DEBUG(printIntervals("active", active_.begin(), active_.end()));
    DEBUG(printIntervals("inactive", inactive_.begin(), inactive_.end()));

    while (!unhandled_.empty()) {
        // pick the interval with the earliest start point
        LiveInterval* cur = unhandled_.top();
        unhandled_.pop();
        ++numIterations;
        DEBUG(std::cerr << "\n*** CURRENT ***: " << *cur << '\n');

        processActiveIntervals(cur);
        processInactiveIntervals(cur);

        // if this register is fixed we are done
        if (MRegisterInfo::isPhysicalRegister(cur->reg)) {
            prt_->addRegUse(cur->reg);
            active_.push_back(cur);
            handled_.push_back(cur);
        }
        // otherwise we are allocating a virtual register. try to find
        // a free physical register or spill an interval in order to
        // assign it one (we could spill the current though).
        else {
            assignRegOrStackSlotAtInterval(cur);
        }

        DEBUG(printIntervals("active", active_.begin(), active_.end()));
        DEBUG(printIntervals("inactive", inactive_.begin(), inactive_.end()));
    }
    numIntervals += li_->getNumIntervals();
    efficiency = double(numIterations) / double(numIntervals);

    // expire any remaining active intervals
    for (IntervalPtrs::reverse_iterator
             i = active_.rbegin(); i != active_.rend(); ) {
        unsigned reg = (*i)->reg;
        DEBUG(std::cerr << "\tinterval " << **i << " expired\n");
        if (MRegisterInfo::isVirtualRegister(reg))
            reg = vrm_->getPhys(reg);
        prt_->delRegUse(reg);
        i = IntervalPtrs::reverse_iterator(active_.erase(i.base()-1));
    }

    // expire any remaining inactive intervals
    for (IntervalPtrs::reverse_iterator
             i = inactive_.rbegin(); i != inactive_.rend(); ) {
        DEBUG(std::cerr << "\tinterval " << **i << " expired\n");
        i = IntervalPtrs::reverse_iterator(inactive_.erase(i.base()-1));
    }

    DEBUG(std::cerr << *vrm_);
}

void RA::initIntervalSets()
{
    assert(unhandled_.empty() && fixed_.empty() &&
           active_.empty() && inactive_.empty() &&
           "interval sets should be empty on initialization");

    for (LiveIntervals::iterator i = li_->begin(), e = li_->end(); i != e; ++i){
        unhandled_.push(&i->second);
        if (MRegisterInfo::isPhysicalRegister(i->second.reg))
            fixed_.push_back(&i->second);
    }
}

void RA::processActiveIntervals(IntervalPtrs::value_type cur)
{
    DEBUG(std::cerr << "\tprocessing active intervals:\n");
    for (IntervalPtrs::reverse_iterator
             i = active_.rbegin(); i != active_.rend();) {
        unsigned reg = (*i)->reg;
        // remove expired intervals
        if ((*i)->expiredAt(cur->start())) {
            DEBUG(std::cerr << "\t\tinterval " << **i << " expired\n");
            if (MRegisterInfo::isVirtualRegister(reg))
                reg = vrm_->getPhys(reg);
            prt_->delRegUse(reg);
            // remove from active
            i = IntervalPtrs::reverse_iterator(active_.erase(i.base()-1));
        }
        // move inactive intervals to inactive list
        else if (!(*i)->liveAt(cur->start())) {
            DEBUG(std::cerr << "\t\tinterval " << **i << " inactive\n");
            if (MRegisterInfo::isVirtualRegister(reg))
                reg = vrm_->getPhys(reg);
            prt_->delRegUse(reg);
            // add to inactive
            inactive_.push_back(*i);
            // remove from active
            i = IntervalPtrs::reverse_iterator(active_.erase(i.base()-1));
        }
        else {
            ++i;
        }
    }
}

void RA::processInactiveIntervals(IntervalPtrs::value_type cur)
{
    DEBUG(std::cerr << "\tprocessing inactive intervals:\n");
    for (IntervalPtrs::reverse_iterator
             i = inactive_.rbegin(); i != inactive_.rend();) {
        unsigned reg = (*i)->reg;

        // remove expired intervals
        if ((*i)->expiredAt(cur->start())) {
            DEBUG(std::cerr << "\t\tinterval " << **i << " expired\n");
            // remove from inactive
            i = IntervalPtrs::reverse_iterator(inactive_.erase(i.base()-1));
        }
        // move re-activated intervals in active list
        else if ((*i)->liveAt(cur->start())) {
            DEBUG(std::cerr << "\t\tinterval " << **i << " active\n");
            if (MRegisterInfo::isVirtualRegister(reg))
                reg = vrm_->getPhys(reg);
            prt_->addRegUse(reg);
            // add to active
            active_.push_back(*i);
            // remove from inactive
            i = IntervalPtrs::reverse_iterator(inactive_.erase(i.base()-1));
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

void RA::assignRegOrStackSlotAtInterval(LiveInterval* cur)
{
    DEBUG(std::cerr << "\tallocating current interval: ");

    PhysRegTracker backupPrt = *prt_;

    spillWeights_.assign(mri_->getNumRegs(), 0.0);

    // for each interval in active update spill weights
    for (IntervalPtrs::const_iterator i = active_.begin(), e = active_.end();
         i != e; ++i) {
        unsigned reg = (*i)->reg;
        if (MRegisterInfo::isVirtualRegister(reg))
            reg = vrm_->getPhys(reg);
        updateSpillWeights(reg, (*i)->weight);
    }

    // for every interval in inactive we overlap with, mark the
    // register as not free and update spill weights
    for (IntervalPtrs::const_iterator i = inactive_.begin(),
             e = inactive_.end(); i != e; ++i) {
        if (cur->overlaps(**i)) {
            unsigned reg = (*i)->reg;
            if (MRegisterInfo::isVirtualRegister(reg))
                reg = vrm_->getPhys(reg);
            prt_->addRegUse(reg);
            updateSpillWeights(reg, (*i)->weight);
        }
    }

    // for every interval in fixed we overlap with,
    // mark the register as not free and update spill weights
    for (IntervalPtrs::const_iterator i = fixed_.begin(),
             e = fixed_.end(); i != e; ++i) {
        if (cur->overlaps(**i)) {
            unsigned reg = (*i)->reg;
            prt_->addRegUse(reg);
            updateSpillWeights(reg, (*i)->weight);
        }
    }

    unsigned physReg = getFreePhysReg(cur);
    // restore the physical register tracker
    *prt_ = backupPrt;
    // if we find a free register, we are done: assign this virtual to
    // the free physical register and add this interval to the active
    // list.
    if (physReg) {
        DEBUG(std::cerr <<  mri_->getName(physReg) << '\n');
        vrm_->assignVirt2Phys(cur->reg, physReg);
        prt_->addRegUse(physReg);
        active_.push_back(cur);
        handled_.push_back(cur);
        return;
    }
    DEBUG(std::cerr << "no free registers\n");

    DEBUG(std::cerr << "\tassigning stack slot at interval "<< *cur << ":\n");

    float minWeight = HUGE_VAL;
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
    DEBUG(std::cerr << "\t\tregister with min weight: "
          << mri_->getName(minReg) << " (" << minWeight << ")\n");

    // if the current has the minimum weight, we need to spill it and
    // add any added intervals back to unhandled, and restart
    // linearscan.
    if (cur->weight <= minWeight) {
        DEBUG(std::cerr << "\t\t\tspilling(c): " << *cur << '\n';);
        int slot = vrm_->assignVirt2StackSlot(cur->reg);
        std::vector<LiveInterval*> added =
            li_->addIntervalsForSpills(*cur, *vrm_, slot);
        if (added.empty())
          return;  // Early exit if all spills were folded.

        // Merge added with unhandled.  Note that we know that 
        // addIntervalsForSpills returns intervals sorted by their starting
        // point.
        for (unsigned i = 0, e = added.size(); i != e; ++i)
            unhandled_.push(added[i]);
        return;
    }

    // push the current interval back to unhandled since we are going
    // to re-run at least this iteration. Since we didn't modify it it
    // should go back right in the front of the list
    unhandled_.push(cur);

    // otherwise we spill all intervals aliasing the register with
    // minimum weight, rollback to the interval with the earliest
    // start point and let the linear scan algorithm run again
    std::vector<LiveInterval*> added;
    assert(MRegisterInfo::isPhysicalRegister(minReg) &&
           "did not choose a register to spill?");
    std::vector<bool> toSpill(mri_->getNumRegs(), false);
    toSpill[minReg] = true;
    for (const unsigned* as = mri_->getAliasSet(minReg); *as; ++as)
        toSpill[*as] = true;
    unsigned earliestStart = cur->start();

    std::set<unsigned> spilled;

    for (IntervalPtrs::iterator
             i = active_.begin(); i != active_.end(); ++i) {
        unsigned reg = (*i)->reg;
        if (MRegisterInfo::isVirtualRegister(reg) &&
            toSpill[vrm_->getPhys(reg)] &&
            cur->overlaps(**i)) {
            DEBUG(std::cerr << "\t\t\tspilling(a): " << **i << '\n');
            earliestStart = std::min(earliestStart, (*i)->start());
            int slot = vrm_->assignVirt2StackSlot((*i)->reg);
            std::vector<LiveInterval*> newIs =
                li_->addIntervalsForSpills(**i, *vrm_, slot);
            std::copy(newIs.begin(), newIs.end(), std::back_inserter(added));
            spilled.insert(reg);
        }
    }
    for (IntervalPtrs::iterator
             i = inactive_.begin(); i != inactive_.end(); ++i) {
        unsigned reg = (*i)->reg;
        if (MRegisterInfo::isVirtualRegister(reg) &&
            toSpill[vrm_->getPhys(reg)] &&
            cur->overlaps(**i)) {
            DEBUG(std::cerr << "\t\t\tspilling(i): " << **i << '\n');
            earliestStart = std::min(earliestStart, (*i)->start());
            int slot = vrm_->assignVirt2StackSlot((*i)->reg);
            std::vector<LiveInterval*> newIs =
                li_->addIntervalsForSpills(**i, *vrm_, slot);
            std::copy(newIs.begin(), newIs.end(), std::back_inserter(added));
            spilled.insert(reg);
        }
    }

    DEBUG(std::cerr << "\t\trolling back to: " << earliestStart << '\n');
    // scan handled in reverse order and undo each one, restoring the
    // state of unhandled
    while (!handled_.empty()) {
        LiveInterval* i = handled_.back();
        // if this interval starts before t we are done
        if (i->start() < earliestStart)
            break;
        DEBUG(std::cerr << "\t\t\tundo changes for: " << *i << '\n');
        handled_.pop_back();
        IntervalPtrs::iterator it;
        if ((it = find(active_.begin(), active_.end(), i)) != active_.end()) {
            active_.erase(it);
            if (MRegisterInfo::isPhysicalRegister(i->reg)) {
                prt_->delRegUse(i->reg);
                unhandled_.push(i);
            }
            else {
                if (!spilled.count(i->reg))
                    unhandled_.push(i);
                prt_->delRegUse(vrm_->getPhys(i->reg));
                vrm_->clearVirt(i->reg);
            }
        }
        else if ((it = find(inactive_.begin(), inactive_.end(), i)) != inactive_.end()) {
            inactive_.erase(it);
            if (MRegisterInfo::isPhysicalRegister(i->reg))
                unhandled_.push(i);
            else {
                if (!spilled.count(i->reg))
                    unhandled_.push(i);
                vrm_->clearVirt(i->reg);
            }
        }
        else {
            if (MRegisterInfo::isVirtualRegister(i->reg))
                vrm_->clearVirt(i->reg);
            unhandled_.push(i);
        }
    }

    // scan the rest and undo each interval that expired after t and
    // insert it in active (the next iteration of the algorithm will
    // put it in inactive if required)
    IntervalPtrs::iterator i = handled_.begin(), e = handled_.end();
    for (; i != e; ++i) {
        if (!(*i)->expiredAt(earliestStart) && (*i)->expiredAt(cur->start())) {
            DEBUG(std::cerr << "\t\t\tundo changes for: " << **i << '\n');
            active_.push_back(*i);
            if (MRegisterInfo::isPhysicalRegister((*i)->reg))
                prt_->addRegUse((*i)->reg);
            else
                prt_->addRegUse(vrm_->getPhys((*i)->reg));
        }
    }

    std::sort(added.begin(), added.end(), less_ptr<LiveInterval>());
    // merge added with unhandled
    for (unsigned i = 0, e = added.size(); i != e; ++i)
        unhandled_.push(added[i]);
}

unsigned RA::getFreePhysReg(LiveInterval* cur)
{
    const TargetRegisterClass* rc = mf_->getSSARegMap()->getRegClass(cur->reg);

    for (TargetRegisterClass::iterator i = rc->allocation_order_begin(*mf_);
         i != rc->allocation_order_end(*mf_); ++i) {
        unsigned reg = *i;
        if (prt_->isRegAvail(reg))
            return reg;
    }
    return 0;
}

FunctionPass* llvm::createLinearScanRegisterAllocator() {
    return new RA();
}
