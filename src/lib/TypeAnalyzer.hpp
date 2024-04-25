#pragma once

#include "LLVMHelper.hpp"
#include "TypeGraph.hpp"

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

using namespace std;
using namespace llvm;


class TypeAnalyzer {
  private:
    Module *module;

    TypeGraph *tg;
    LLVMHelper *llHelper;
    TypeHelper *tyHelper;

    void log(string msg) {
        errs() << msg;
    }

    // void processFunctionSummary(Function &func);

    void processAddrOf(Function *scope, AllocaInst &alloca);

    void processFieldOf(Function *scope, GetElementPtrInst &gep) {
        // infer base value's type
        Value *base = gep.getPointerOperand();
        Type *baseType = gep.getSourceElementType();

        // base type
        auto baseName = tyHelper->getTypeName(baseType);
        if (tg->isOpaque(scope, base) && tyHelper->canFlow(baseName)) {
            tg->put(scope, base, tyHelper->getReference(baseName));
        }

        // infer left hand side value's type
        string typeName;
        Value *lhs = dyn_cast<Value>(&gep);

        // start from 2, skip the first index
        for (int i = 2; i < gep.getNumOperands(); ++i) {
            Value *index = gep.getOperand(i);
            if (auto *constIndex = dyn_cast<ConstantInt>(index)) {
                // if the index is a constant, adjust the type
                uint64_t index = constIndex->getZExtValue();

                if (auto *structType = dyn_cast<StructType>(baseType)) {
                    if (index < structType->getNumElements()) {
                        baseType = structType->getElementType(index);
                        typeName = tyHelper->getTypeName(baseType);

                        // if baseType is op, divert to DIType
                        if (tyHelper->isOpaque(typeName) &&
                            i == gep.getNumOperands() - 1) {
                            auto diHelper =
                                static_cast<DebugInfoHelper *>(llHelper);
                            typeName =
                                diHelper->getDIStructField(structType, index);
                        }
                    }
                } else if (auto *arrayType = dyn_cast<ArrayType>(baseType)) {
                    baseType = arrayType->getElementType();
                } else if (auto *vectorType = dyn_cast<VectorType>(baseType)) {
                    baseType = vectorType->getElementType();
                } else {
                    // should not reach here
                    errs() << "[ERR] unknown type: ";
                    baseType->dump();
                }
            } else {
                if (auto *arrayType = dyn_cast<ArrayType>(baseType)) {
                    baseType = arrayType->getElementType();
                } else if (auto *vectorType = dyn_cast<VectorType>(baseType)) {
                    baseType = vectorType->getElementType();
                } else {
                    // should not reach here
                    errs() << "[ERR] unknown index: ";
                    index->dump();
                    exit(1);
                }
            }
        }

        if (tg->isOpaque(scope, lhs) && tyHelper->canFlow(typeName)) {
            tg->put(scope, lhs, tyHelper->getReference(typeName));
        }
    }

    void processCast(Function *scope, CastInst &cast) {
        // Value* src = cast.getOperand(0);
        Value *dst = dyn_cast<Value>(&cast);
        auto dstType = tyHelper->getTypeName(cast.getDestTy());

        tg->put(scope, dst, dstType);
    }

    void processCopy();

    void processCall(Function *scope, CallInst &call) {
        Function *calledFunc = call.getCalledFunction();

        if (calledFunc) {
            if (calledFunc->hasName() &&
                calledFunc->getName().starts_with("llvm.")) {
                return;
            }

            // Get the number of parameters in the called function
            FunctionType *FTy = calledFunc->getFunctionType();
            unsigned numParams = FTy->getNumParams();

            // handle parameters
            for (unsigned i = 0; i < numParams; ++i) {
                Value *argValue = call.getArgOperand(i);
                Value *paramValue = calledFunc->getArg(i);

                // argValue flows to paramValue
                auto argType = tg->get(scope, argValue);
                if (tyHelper->canFlow(argType))
                    tg->put(scope, paramValue, argType);
            }
        }

        // process return value
        Value *dst = dyn_cast<Value>(&call);
        auto dstType = tg->get(scope, calledFunc);
        if (tyHelper->canFlow(dstType))
            tg->put(scope, dst, dstType);
    }

    void processLoad(Function *scope, LoadInst &load) {
        Value *src = load.getPointerOperand();
        Value *dst = dyn_cast<Value>(&load);

        auto deref = tg->dereference(scope, src);
        if (tyHelper->canFlow(deref))
            tg->put(scope, dst, deref);
        delete deref;

        auto ref = tg->reference(scope, dst);
        if (tyHelper->canFlow(ref))
            tg->put(scope, src, ref);
        delete ref;
    }

    void processStore(Function *scope, StoreInst &store) {
        Value *src = store.getValueOperand();
        Value *dst = store.getPointerOperand();

        auto ref = tg->reference(scope, src);
        if (tyHelper->canFlow(ref))
            tg->put(scope, dst, ref);
        delete ref;

        auto deref = tg->dereference(scope, dst);
        if (tyHelper->canFlow(deref))
            tg->put(scope, src, deref);
        delete deref;
    }

    // may get unsigned int from debuginfo
    void processBinary(Function *scope, BinaryOperator &binop) {
        // get value a and b
        Value *a = binop.getOperand(0);
        Value *b = binop.getOperand(1);
        Value *r = dyn_cast<Value>(&binop);

        auto typeB = tg->get(scope, b);
        if (tyHelper->canFlow(typeB)) {
            tg->put(scope, a, tg->get(scope, b));
            tg->put(scope, r, tg->get(scope, b));
        }

        auto typeA = tg->get(scope, a);
        if (tyHelper->canFlow(typeA)) {
            tg->put(scope, b, tg->get(scope, a));
            tg->put(scope, r, tg->get(scope, a));
        }
    }

    // FIXME may be removed
    void processCmp(Function *scope, CmpInst &cmp) {
        // get value a and b
        Value *a = cmp.getOperand(0);
        Value *b = cmp.getOperand(1);

        // type mismatch between a and b
        if (tg->isOpaque(scope, a) && !tg->isOpaque(scope, b))
            tg->put(scope, a, tg->get(scope, b));
        else if (tg->isOpaque(scope, b) && !tg->isOpaque(scope, a))
            tg->put(scope, b, tg->get(scope, a));
    }

    void processPhi(Function *scope, PHINode &phi) {
        // get value a and b
        Value *r = dyn_cast<Value>(&phi);

        for (int i = 0; i < phi.getNumIncomingValues(); ++i) {
            Value *v = phi.getIncomingValue(i);
            if (!tg->isOpaque(scope, v) && tg->isOpaque(scope, r)) {
                tg->put(scope, r, tg->get(scope, v));
            }
        }
    }

    void processSelect(Function *scope, SelectInst &select) {
        Value *r = dyn_cast<Value>(&select);
        Value *a = select.getTrueValue();
        Value *b = select.getFalseValue();

        auto typeB = tg->get(scope, b);
        if (tyHelper->canFlow(typeB)) {
            tg->put(scope, a, tg->get(scope, b));
            tg->put(scope, r, tg->get(scope, b));
        }

        auto typeA = tg->get(scope, a);
        if (tyHelper->canFlow(typeA)) {
            tg->put(scope, b, tg->get(scope, a));
            tg->put(scope, r, tg->get(scope, a));
        }
    }

    /// real process function
    void process() {
        for (auto &func : *module) {
            for (auto &basic_block : func) {
                for (auto &inst : basic_block) {

                    if (auto *cast = dyn_cast_or_null<CastInst>(&inst)) {
                        processCast(&func, *cast);
                    } else if (auto *load = dyn_cast_or_null<LoadInst>(&inst)) {
                        processLoad(&func, *load);
                    } else if (auto *store =
                                   dyn_cast_or_null<StoreInst>(&inst)) {
                        processStore(&func, *store);
                    } else if (auto *binop =
                                   dyn_cast_or_null<BinaryOperator>(&inst)) {
                        processBinary(&func, *binop);
                    } else if (auto *phi = dyn_cast_or_null<PHINode>(&inst)) {
                        processPhi(&func, *phi);
                    } else if (auto *gep =
                                   dyn_cast_or_null<GetElementPtrInst>(&inst)) {
                        processFieldOf(&func, *gep);
                    } else if (auto *cmp = dyn_cast_or_null<CmpInst>(&inst)) {
                        processCmp(&func, *cmp);
                    } else if (auto *call = dyn_cast_or_null<CallInst>(&inst)) {
                        processCall(&func, *call);
                    } else if (auto *select =
                                   dyn_cast_or_null<SelectInst>(&inst)) {
                        processSelect(&func, *select);
                    }
                }
            }
        }
    }

  public:
    explicit TypeAnalyzer(Module *m) { module = m; }

    TypeGraph* analyze() {
        log("[INFO] using combination\n");
        CombHelper *combHelper = new CombHelper();

        if (!combHelper->hasDebugInfo(*module)) {
            errs() << "[ERR] no debug info found\n";
            return nullptr;
        }

        llHelper = combHelper;

        // do initialization
        log("[INFO] initializing ...\n");
        tg = new TypeGraph();
        llHelper->initialize(module, tg);


        // do inference
        int iterNum = 5;
        log("[INFO] running " + to_string(iterNum) + " iterations\n");

        for (int i = 0; i < iterNum; i++) {
            log("[INFO] doing iteration " + to_string(i + 1) + "\n");
            process();
        }

        log("[INFO] done\n");

        return tg;
    }
};
