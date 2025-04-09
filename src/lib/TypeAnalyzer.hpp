#pragma once

#include "LLVMHelper.hpp"
#include "TypeAlias.hpp"
#include "TypeGraph.hpp"
#include "WorkList.hpp"

#include <llvm/IR/Constants.h>
#include <llvm/IR/DebugInfo.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/Value.h>
#include <llvm/Support/Casting.h>
#include <llvm/Support/TypeName.h>
#include <llvm/Support/raw_ostream.h>

using namespace std;
using namespace llvm;


class TypeAnalyzer {
  private:
    Module *module;

    // TypeGraph stores the results
    TypeGraph *tg;

    // Type Alias Rules
    TypeAlias *alias;

    WorkList *worklist;

    // helper classes
    LLVMHelper *llHelper;
    TypeHelper *tyHelper;

    void log(string msg) {
        errs() << msg;
    }


    /// real process function
    void process() {
        while (!worklist->empty()) {
            auto inst = worklist->pop();

            if (auto *cast = dyn_cast<CastInst>(inst)) {
                alias->processCast(cast->getFunction(), *cast);
            } else if (auto *load = dyn_cast<LoadInst>(inst)) {
                alias->processLoad(load->getFunction(), *load);
            } else if (auto *store = dyn_cast<StoreInst>(inst)) {
                alias->processStore(store->getFunction(), *store);
            } else if (auto *binop = dyn_cast<BinaryOperator>(inst)) {
                alias->processBinary(binop->getFunction(), *binop);
            } else if (auto *phi = dyn_cast<PHINode>(inst)) {
                alias->processPhi(phi->getFunction(), *phi);
            } else if (auto *gep = dyn_cast<GetElementPtrInst>(inst)) {
                alias->processFieldOf(gep->getFunction(), *gep);
            } else if (auto *cmp = dyn_cast<CmpInst>(inst)) {
                alias->processCmp(cmp->getFunction(), *cmp);
            } else if (auto *call = dyn_cast<CallInst>(inst)) {
                alias->processCall(call->getFunction(), *call);
            } else if (auto *select = dyn_cast<SelectInst>(inst)) {
                alias->processSelect(select->getFunction(), *select);
            }
        }
    }

  public:
    explicit TypeAnalyzer(Module *m) { module = m; }

    // ===================
    // initialize analyzer
    // ===================
    TypeGraph *init() {
        CombHelper *combHelper = new CombHelper();

        if (!combHelper->hasDebugInfo(*module)) {
            errs() << "[ERR] no debug info found\n";
            return nullptr;
        }

        llHelper = combHelper;

        // do initialization
        tg = new TypeGraph();

        llHelper->initialize(module, tg);

        worklist = new WorkList(module);

        alias = new TypeAlias(module, tg, worklist, llHelper);

        return tg;
    }

    // ==============
    // start analysis
    // ==============
    TypeGraph *analyze() {
        // do inference
        process();

        return tg;
    }
};
