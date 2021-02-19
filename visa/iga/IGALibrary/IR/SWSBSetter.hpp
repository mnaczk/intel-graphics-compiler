/*===================== begin_copyright_notice ==================================

Copyright (c) 2017 Intel Corporation

Permission is hereby granted, free of charge, to any person obtaining a
copy of this software and associated documentation files (the
"Software"), to deal in the Software without restriction, including
without limitation the rights to use, copy, modify, merge, publish,
distribute, sublicense, and/or sell copies of the Software, and to
permit persons to whom the Software is furnished to do so, subject to
the following conditions:

The above copyright notice and this permission notice shall be included
in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.


======================= end_copyright_notice ==================================*/

#ifndef _IGA_SWSBSETTER_H_
#define _IGA_SWSBSETTER_H_
#include "Instruction.hpp"
#include "Block.hpp"
#include "Kernel.hpp"
#include "Operand.hpp"
#include "../ErrorHandler.hpp"
#include "RegDeps.hpp"

namespace iga
{
    // Bucket represents a GRF and maps to all instructions that access it
    class Bucket
    {
    public:
        Bucket() { dependencies.reserve(5); }
        void clearDependency() { dependencies.clear(); }
        bool isEmpty() const { return dependencies.empty(); }
        size_t getNumDependencies() const { return dependencies.size(); }
        DepSet * getDepSet(uint32_t index) { return dependencies[index]; }
        void clearDepSet(uint32_t index) { dependencies[index] = nullptr; }
        //Most of the time dependecy vector will have 1 or two entries
        void addDepSet(DepSet *dep) {
            bool depSet = false;
            for (size_t i = 0; i < dependencies.size(); ++i)
            {
                if (!dependencies[i])
                {
                    dependencies[i] = dep;
                    depSet = true;
                    break;
                }
            }
            if (!depSet)
            {
                dependencies.push_back(dep);
            }
        }
    private:
        std::vector<DepSet*> dependencies;
    };

    class SWSBAnalyzer
    {
    public:
        typedef DepSet::InstIDs InstIDs;
    public:
        //Blocks have already been created
        SWSBAnalyzer(Kernel &k, ErrorHandler &errHandler,
            SWSB_ENCODE_MODE encode_mode, int sbid_count)
                      : m_kernel(k),
                        m_errorHandler(errHandler),
                        m_SBIDRRCounter(0),
                        m_initPoint(false),
                        MAX_VALID_DISTANCE(k.getModel().getSWSBMaxValidDistance())
        {
            // Set SWSB_ENCODE_MODE
            if (encode_mode != SWSB_ENCODE_MODE::SWSBInvalidMode)
                m_swsbMode = encode_mode;
            else
                m_swsbMode = k.getModel().getSWSBEncodeMode();


            m_DB = new DepSetBuilder(k.getModel());
            m_buckets = new Bucket[m_DB->getTOTAL_BUCKETS()];

            // set sbid count to 16 if not given
            if (sbid_count)
                m_SBIDCount = sbid_count;
            else
                m_SBIDCount = 16;
            m_freeSBIDList.resize(m_SBIDCount);
        }

        ~SWSBAnalyzer()
        {
            delete m_DB;
            delete[] m_buckets;
        }

        void run();

    private:
        // postProcess - last step of "run"
        void postProcess();

    private:
        // activeSBID: input list that the sbid this dep has dependency on will be added into. This list
        // will later on be pass to processActiveSBID to set the swsb id dependency to inst accordingly
        // needSyncForShootDownInst: if the sync to the sbid on the instruction is required. If the instruction
        // is possiblely being shoot down, we have to add a sync to the id is synced with because we will
        // clear the dependency
        void calculateDependence(DepSet &dep,
                                 SWSB &distanceDependency,
                                 const Instruction &currInst,
                                 std::vector<SBID> &activeSBID,
                                 bool &needSyncForShootDownInst);
        void processActiveSBID(SWSB &distanceDependency,
                               const DepSet* input,
                               Block *bb,
                               InstList::iterator iter,
                               std::vector<SBID>& activeSBID);

        // helper function to pick a free SBID and set it to distanceDependency.
        // This function only set SBID to distanceDependency, will not assign distanceDependency to
        // the inst
        SBID& assignSBID(DepSet* input, DepSet* output, Instruction& inst, SWSB& distanceDependency,
            InstList::iterator insertPoint, Block *curBB, bool needSyncForShootDown);

        // helper fuction to set out-of-order dependecy. Called by calculateDependence.
        // This function create SBID with depedency to given dep, and add it to activeSBID
        // it also set needSyncForShootDownInst if required.
        void setSbidDependency(DepSet& dep, const Instruction& currInst,
            bool& needSyncForShootDownInst, std::vector<SBID>& activeSBID);

        // clear dependency of the given dep
        void clearDepBuckets(DepSet &dep);
        // clear all sbid, set ids to all free and insert sync to sync with all pipes
        void clearSBIDDependence(InstList::iterator insertPoint, Instruction *lastInst, Block *bb);
        // clear given input and output dependency in the buckets (for in-order pipes only)
        void clearBuckets(DepSet* input, DepSet* output);

        // helper function to insert sync.allrd and sync.allwr before the given inserPoint of given bb
        void insertSyncAllRdWr(InstList::iterator insertPoint, Block *bb);

        // a helper function to increase inst id counter based on the current encoding mode
        void advanceInorderInstCounter(DEP_PIPE dep_pipe);

        // get number of dist pipe according to SWSB_ENCODE_MODE
        uint32_t getNumOfDistPipe();


    private:
        const uint32_t MAX_GRF_BUCKETS = 128;
        int m_LatencyInOrderPipe = 10;

    private:
        // m_SBIDCound - number of SBID can be used
        uint32_t m_SBIDCount;

        // m_InstIdCounter - record the current instruction state
        InstIDs m_InstIdCounter;

        Kernel &m_kernel;
        ErrorHandler &m_errorHandler;

        Bucket* m_buckets = nullptr;
        DepSetBuilder* m_DB = nullptr;

        // This is the list to recored all sbid, if it's free or not
        std::vector<SBID> m_freeSBIDList;

        // m_SBIDRRCounter - the round robin counter for SB id reuse
        unsigned int m_SBIDRRCounter;

        // id to dep set mapping, this tracks for which instructions' dependency that this id
        // is currently on. While we're re-using id, we clean up the dependency
        std::map<uint32_t, std::pair<DepSet*, DepSet*>> m_IdToDepSetMap;

        // m_distanceTracker - Track the DepSet of in-order instructions to see if their latency
        // is satisfied. If the distance to current instruction is larger then the latency, then
        // we no need to track the dependency anymore, remove the node from m_distanceTracker
        struct distanceTrackerNode {
            distanceTrackerNode(DepSet *in, DepSet *out)
                : input(in), output(out)
            {}
            DepSet *input;
            DepSet *output;
        };
        std::list<distanceTrackerNode> m_distanceTracker;

        bool m_initPoint;

        SWSB_ENCODE_MODE m_swsbMode;

        const int MAX_VALID_DISTANCE;
    };
}
#endif
