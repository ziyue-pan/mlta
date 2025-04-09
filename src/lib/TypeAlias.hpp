#pragma once

#include <llvm/Support/CommandLine.h>

#include "LLVMHelper.hpp"
#include "TypeGraph.hpp"
#include "TypeHelper.hpp"
#include "WorkList.hpp"

using namespace std;
using namespace llvm;

extern cl::opt<string> TypeSrc;

class TypeAlias {
  private:
    Module *m;
    TypeGraph *tg;
    WorkList *worklist;
    TypeHelper *tyHelper;
    LLVMHelper *llHelper;

  public:
    TypeAlias(Module *m, TypeGraph *tg, WorkList *worklist,
              LLVMHelper *llHelper) {
        this->m = m;
        this->tg = tg;
        this->worklist = worklist;
        this->llHelper = llHelper;
    }

    void processPhi(Function *scope, PHINode &phi) {
        bool r_updated = false;
        Value *r = dyn_cast<Value>(&phi);

        for (int i = 0; i < phi.getNumIncomingValues(); ++i) {
            Value *v = phi.getIncomingValue(i);
            if (!tg->isOpaque(scope, v) && tg->isOpaque(scope, r)) {
                if (tg->put(scope, r, tg->get(scope, v)))
                    r_updated = true;
            }
        }

        if (r_updated)
            worklist->push_user(r);
    }

    void processSelect(Function *scope, SelectInst &select) {
        bool r_updated = false;
        Value *r = dyn_cast<Value>(&select);
        Value *a = select.getTrueValue();
        Value *b = select.getFalseValue();

        auto typeB = tg->get(scope, b);
        if (tyHelper->canFlow(typeB)) {
            if (tg->put(scope, a, tg->get(scope, b)))
                worklist->push_user(a);

            if (tg->put(scope, r, tg->get(scope, b)))
                r_updated = true;
        }

        auto typeA = tg->get(scope, a);
        if (tyHelper->canFlow(typeA)) {
            if (tg->put(scope, b, tg->get(scope, a)))
                worklist->push_user(b);

            if (tg->put(scope, r, tg->get(scope, a)))
                r_updated = true;
        }

        if (r_updated)
            worklist->push_user(r);
    }

    void processFieldOf(Function *scope, GetElementPtrInst &gep) {
        // infer base value's type
        Value *base = gep.getPointerOperand();
        Type *baseType = gep.getSourceElementType();

        // base type
        auto baseName = tyHelper->getTypeName(baseType);
        if (tg->isOpaque(scope, base) && tyHelper->canFlow(baseName)) {
            if (tg->put(scope, base, tyHelper->getReference(baseName)))
                worklist->push_user(base);
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

                            auto combHelper =
                                static_cast<CombHelper *>(llHelper);
                            typeName = combHelper->getDIStructField(
                                structType, index);
                        }
                    }
                } else if (auto *arrayType = dyn_cast<ArrayType>(baseType)) {
                    baseType = arrayType->getElementType();
                } else if (auto *vectorType = dyn_cast<VectorType>(baseType)) {
                    baseType = vectorType->getElementType();
                } else {
                    // should not reach here
                    baseType->dump();
                }
            } else {
                if (auto *arrayType = dyn_cast<ArrayType>(baseType)) {
                    baseType = arrayType->getElementType();
                } else if (auto *vectorType = dyn_cast<VectorType>(baseType)) {
                    baseType = vectorType->getElementType();
                } else {
                    // should not reach here
                    index->dump();
                    exit(1);
                }
            }
        }

        if (tg->isOpaque(scope, lhs) && tyHelper->canFlow(typeName)) {
            if (tg->put(scope, lhs, tyHelper->getReference(typeName)))
                worklist->push_user(lhs);
        }
    }

    void processCast(Function *scope, CastInst &cast) {
        // Value* src = cast.getOperand(0);
        Value *dst = dyn_cast<Value>(&cast);
        auto dstType = tyHelper->getTypeName(cast.getDestTy());

        if (tg->put(scope, dst, dstType))
            worklist->push_user(dst);
    }

    void processCopy(Function *scope, CallInst &call) {
        Value *dst = call.getArgOperand(0);
        Value *src = call.getArgOperand(1);

        auto dstType = tg->get(scope, dst);
        if (tyHelper->canFlow(dstType)) {
            if (tg->put(scope, src, dstType))
                worklist->push_user(src);
        }

        auto srcType = tg->get(scope, src);
        if (tyHelper->canFlow(srcType)) {
            if (tg->put(scope, dst, srcType))
                worklist->push_user(dst);
        }
    }

    void processCall(Function *scope, CallInst &call) {
        Function *calledFunc = call.getCalledFunction();

        // is direct call
        if (calledFunc) {
            if (calledFunc->hasName()) {
                auto funcName = calledFunc->getName();
                if (funcName.startswith("llvm.memcpy") ||
                    funcName.startswith("llvm.memmove"))
                    processCopy(scope, call);
                else if (funcName.startswith("llvm.")) // llvm instrinsic
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
                    if (tg->put(calledFunc, paramValue, argType))
                        worklist->push_user(paramValue);
            }
        } else {
            // TODO: indirect call
            // Value *calledValue = call.getCalledOperand();
            // errs() << "[DBG] Indirect call found: ";
            // auto xx = calledValue->stripPointerCasts();
            // errs() << *xx << "\n";
            // Function *f =
            // dyn_cast<Function>(calledValue->stripPointerCasts()); if (f) {
            //     errs() << "[DBG] Indirect call to " << f->getName() << "\n";
            // }
        }

        // process return value
        Value *dst = dyn_cast<Value>(&call);
        auto dstType = tg->get(nullptr, calledFunc);
        if (tyHelper->canFlow(dstType))
            if (tg->put(scope, dst, dstType))
                worklist->push_user(dst);
    }

    void processLoad(Function *scope, LoadInst &load) {
        Value *src = load.getPointerOperand();
        Value *dst = dyn_cast<Value>(&load);

        auto deref = tg->dereference(scope, src);
        if (tyHelper->canFlow(deref))
            if (tg->put(scope, dst, deref))
                worklist->push_user(dst);
        delete deref;

        auto ref = tg->reference(scope, dst);
        if (tyHelper->canFlow(ref))
            if (tg->put(scope, src, ref))
                worklist->push_user(src);
        delete ref;
    }

    void processStore(Function *scope, StoreInst &store) {
        Value *src = store.getValueOperand();
        Value *dst = store.getPointerOperand();

        auto ref = tg->reference(scope, src);
        if (tyHelper->canFlow(ref))
            if (tg->put(scope, dst, ref))
                worklist->push_user(dst);
        delete ref;

        auto deref = tg->dereference(scope, dst);
        if (tyHelper->canFlow(deref))
            if (tg->put(scope, src, deref))
                worklist->push_user(src);
        delete deref;
    }

    void processBinary(Function *scope, BinaryOperator &binop) {
        bool r_updated = false;
        Value *a = binop.getOperand(0);
        Value *b = binop.getOperand(1);
        Value *r = dyn_cast<Value>(&binop);

        auto typeB = tg->get(scope, b);
        if (tyHelper->canFlow(typeB)) {
            if (tg->put(scope, a, tg->get(scope, b)))
                worklist->push_user(a);
            if (tg->put(scope, r, tg->get(scope, b)))
                r_updated = true;
        }

        auto typeA = tg->get(scope, a);
        if (tyHelper->canFlow(typeA)) {
            if (tg->put(scope, b, tg->get(scope, a)))
                worklist->push_user(b);
            if (tg->put(scope, r, tg->get(scope, a)))
                r_updated = true;
        }

        if (r_updated)
            worklist->push_user(r);
    }

    void processCmp(Function *scope, CmpInst &cmp) {
        // get value a and b
        Value *a = cmp.getOperand(0);
        Value *b = cmp.getOperand(1);

        // type mismatch between a and b
        if (tg->isOpaque(scope, a) && !tg->isOpaque(scope, b)) {
            if (tg->put(scope, a, tg->get(scope, b)))
                worklist->push_user(a);
        } else if (tg->isOpaque(scope, b) && !tg->isOpaque(scope, a)) {
            if (tg->put(scope, b, tg->get(scope, a)))
                worklist->push_user(b);
        }
    }
};