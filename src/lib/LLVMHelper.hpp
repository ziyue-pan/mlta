#pragma once

#include "TypeGraph.hpp"

#include <llvm/ADT/SmallVector.h>
#include <llvm/BinaryFormat/Dwarf.h>
#include <llvm/IR/DebugInfo.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/Value.h>
#include <llvm/Support/Casting.h>

#include <map>
#include <string>
#include <utility>

using namespace llvm;
using namespace std;

/// Helper class for TypeGraph
class TypeHelper {
  private:
    // drop struct layout
    void dropLayout(string &type) {
        if (type.find("\%struct.") == 0 || type.find("\%union.") == 0) {
            type = type.substr(0, type.find(" "));
        }
    }

    // drop array size
    void dropArray(string &type) {
        if (type.find("[") == 0) {
            auto pos = type.find(" ") + 3;
            type = type.substr(pos, type.size() - pos - 1);

            if (type.find("[") == 0)
                dropArray(type);

            if (!isOpaque(type))
                type += "*";
        }
    }

  public:
    /// get the name of a type
    inline string getTypeName(Type *type) {
        string str;
        raw_string_ostream rso(str);
        type->print(rso);

        // TODO add hook to adjust type
        dropArray(str);
        dropLayout(str);

        return str;
    }

    /// check if a type is an opaque pointer
    bool isOpaque(string &type) { return type == "ptr"; }
    bool isOpaque(set<string> &typeset) { return typeset.count("ptr"); }
    bool isOpaque(Type *type) { return getTypeName(type) == "ptr"; }

    // check if a type is ptr to opaque pointer
    bool isPtrToOpaque(string &type) {
        return type == "ptr*" || type == "ptr**";
    }
    bool isPtrToOpaque(set<string> &typeset) {
        return typeset.count("ptr*") || typeset.count("ptr**");
    }

    string getReference(Type *type) {
        auto str = getTypeName(type);

        if (isOpaque(str)) {
            return str;
        }
        return str + "*";
    }

    string getReference(string &type) {
        if (isOpaque(type)) {
            return type;
        }
        return type + "*";
    }

    bool canFlow(string &type) { return !type.empty() && !isOpaque(type); }

    bool canFlow(TypeSet *typeset) {
        if (!typeset)
            return false;
        return !typeset->empty() && !typeset->isOpaque();
    }

    /// count total number of instructions and number of opaque pointers
    void count(Module *module, TypeGraph *tg) {
        long total = 0, opaque = 0;

        for (auto &global : module->globals()) {
            total++;
            if (tg->isOpaque(nullptr, &global)) {
                opaque++;
            }
        }

        for (auto &func : *module) {
            for (auto &basic_block : func) {
                for (auto &inst : basic_block) {
                    // skip store inst
                    if (auto store = dyn_cast_or_null<StoreInst>(&inst)) {
                        continue;
                    }

                    total++;
                    if (tg->isOpaque(&func, &inst)) {
                        opaque++;
                    }
                }
            }
        }

        errs() << "total: " << total << ", opaque: " << opaque << "\n";
    }

    // TODO remove this after debugging
    void dumpOpaque(Module *module, TypeGraph *tg) {
        vector<string> libFuncs{
            "gettext",          "fopen",          "calloc",
            "localtime",        "strchr",         "gmtime_r",
            "malloc",           "__ctype_b_loc",  "reallocarray",
            "__errno_location", "getenv",         "textdomain",
            "setlocale",        "bindtextdomain", "realloc",
            "nl_langinfo"};

        for (auto typeMap : tg->getAllMap()) {
            for (auto &[key, value] : *typeMap) {
                if (value->isOpaque()) {
                    bool isLibFunc = false;
                    for (auto libFunc : libFuncs) {
                        // declare
                        if (key->getName().contains(libFunc)) {
                            isLibFunc = true;
                            break;
                        }
                    }
                    if (isLibFunc) {
                        continue;
                    }

                    errs() << "[INFO] opaque value:\n";
                    key->dump();

                    continue;

                    if (auto *inst = dyn_cast_or_null<Instruction>(key)) {
                        errs() << "operands:\n";
                        for (auto &operand : inst->operands()) {
                            operand->dump();
                        }
                    }

                    errs() << "users:\n";
                    for (auto user : key->users()) {
                        user->dump();
                    }
                    errs() << "\n";
                }
            }
        }
    }
};

class LLVMHelper {
  public:
    TypeHelper tyHelper;
    virtual void initialize(Module *module, TypeGraph *tg) = 0;
};

/// Helper function for LLVM migration instructions
class MigrationHelper : public LLVMHelper {
  public:
    void initialize(Module *module, TypeGraph *tg) {
        // initialize global value
        for (auto &global : module->globals()) {
            auto type = global.getValueType();
            // all global variables should be pointers
            tg->put(nullptr, &global, tyHelper.getReference(type));
        }

        // initialize insts
        for (auto &func : *module) {
            for (auto &basic_block : func) {
                for (auto &inst : basic_block) {
                    auto type = getType(inst);

                    // get value
                    Value *value = dyn_cast<Value>(&inst);
                    if (auto store = dyn_cast_or_null<StoreInst>(&inst)) {
                        value = store->getValueOperand();
                    }

                    // put it into type graph
                    tg->put(&func, value, type);
                }
            }
        }

        // initialize function type
        for (auto &func : *module) {
            auto type = func.getFunctionType();
            auto retType = type->getReturnType();
            Value *funcValue = dyn_cast<Value>(&func);
            tg->put(nullptr, funcValue, tyHelper.getTypeName(retType));
        }
    }

    string getType(Instruction &inst) {
        auto type = inst.getType();
        if (auto *load = dyn_cast_or_null<LoadInst>(&inst)) {
            type = load->getType();
        }
        if (auto *store = dyn_cast_or_null<StoreInst>(&inst)) {
            type = store->getValueOperand()->getType();
        }

        // handle gep instruction
        // if (auto* gep = dyn_cast_or_null<GetElementPtrInst>(&inst)) {
        //     type = gep->getSourceElementType();
        //     return getTypeName(type) + "*";
        // }

        // handle alloca instruction
        if (auto *alloca = dyn_cast_or_null<AllocaInst>(&inst)) {
            type = alloca->getAllocatedType();

            if (!tyHelper.isOpaque(type))
                return tyHelper.getReference(type);
        }

        // handle call instruction
        if (auto *call = dyn_cast_or_null<CallInst>(&inst)) {
            type = call->getFunctionType();

            // trim parameters' type by erasing parantheses
            auto funcType = tyHelper.getTypeName(type);
            funcType.erase(funcType.find_first_of('('));
            funcType.erase(funcType.find_last_of(' '));
            return funcType;
        }

        return tyHelper.getTypeName(type);
    }
};

class DebugInfoHelper : public LLVMHelper {
  private:
    map<StructType *, DIType *> diTypeMap;

  public:
    void initialize(Module *module, TypeGraph *tg) {
        // map structure type to DIType
        DebugInfoFinder finder;
        finder.processModule(*module);

        for (auto s : module->getIdentifiedStructTypes()) {
            if (!s->hasName())
                continue;

            auto structName = s->getName();
            structName.consume_front("struct.");
            for (auto type : finder.types()) {
                // typedef
                if (auto *derived = dyn_cast_or_null<DIDerivedType>(type)) {
                    if (derived->getTag() == dwarf::DW_TAG_typedef) {
                        if (derived->getName().equals(structName)) {
                            diTypeMap.insert({s, derived->getBaseType()});
                            break;
                        }
                    }
                }
                // struct
                else if (auto *composite =
                             dyn_cast_or_null<DICompositeType>(type)) {
                    if (composite->getTag() == dwarf::DW_TAG_structure_type) {
                        if (composite->getName().equals(structName)) {
                            // empty elements, skip
                            if (composite->getElements().empty())
                                continue;

                            diTypeMap.insert({s, composite});
                            break;
                        }
                    }
                }
            }
        }

        for (auto &global : module->globals()) {
            SmallVector<DIGlobalVariableExpression *> di_global_exps;
            global.getDebugInfo(di_global_exps);

            if (di_global_exps.empty()) {
                // FIXME tg->put(nullptr, &global, "ptr");
                continue;
            }

            for (auto di_global_exp : di_global_exps) {
                auto di_global = di_global_exp->getVariable();
                tg->put(nullptr, &global, getDITypeName(di_global->getType()));
            }
        }

        for (auto &func : *module) {
            for (auto &basic_block : func) {
                for (auto &inst : basic_block) {

                    // get value
                    Value *value = dyn_cast<Value>(&inst);
                    if (auto store = dyn_cast_or_null<StoreInst>(&inst)) {
                        value = store->getValueOperand();
                    }

                    // alloca adds one more level of ptr
                    if (auto di_local = getDILocalVar(inst)) {
                        tg->put(&func, value,
                                getDITypeName(di_local->getType()));
                    } else {
                        // FIXME no debug info, default to opaque pointer
                        // tg->put(&func, value, "ptr");
                    }
                }
            }
        }

        // handle function prototype accurately
        for (auto &func : *module) {
            Value *funcValue = dyn_cast<Value>(&func);

            // get DISubprogram
            auto *subprogram = func.getSubprogram();

            if (!subprogram) {
                auto type = func.getFunctionType();
                auto retType = type->getReturnType();
                tg->put(nullptr, funcValue, tyHelper.getTypeName(retType));
            } else {
                auto *subroutinetype = subprogram->getType();
                auto typearray = subroutinetype->getTypeArray();

                // process return type
                tg->put(nullptr, funcValue, getDITypeName(typearray[0]));

                // process parameters
                for (int i = 1; i < typearray.size(); ++i) {
                    auto type = typearray[i];

                    if (!type) // type can be null in variable func parameters
                        continue;

                    if (i - 1 < func.arg_size()) { // avoid overflow
                        auto param = func.getArg(i - 1);
                        tg->put(&func, param, getDITypeName(type));
                    }
                }
            }
        }
    }

    bool hasDebugInfo(Module &M) {
        for (auto &F : M)
            for (auto &BB : F)
                for (auto &I : BB)
                    if (auto DILoc = I.getDebugLoc())
                        return true;
        return false;
    }

    DILocalVariable *getDILocalVar(Instruction &inst) {
        DILocalVariable *local = nullptr;

        // check if is call inst
        if (auto *call = dyn_cast_or_null<CallInst>(&inst)) {

            auto call_func = call->getCalledFunction();

            // check invoked function is intrinsic llvm debug function
            if (call_func && call_func->isIntrinsic() &&
                call_func->getName().starts_with("llvm.dbg.declare")) {

                // get the second argument, which stores `DILocalVariable`
                if (auto arg = call->getArgOperand(1)) {
                    auto metadata =
                        dyn_cast<MetadataAsValue>(arg)->getMetadata();
                    local = dyn_cast_or_null<DILocalVariable>(metadata);
                }
            }
        }

        return local;
    }

    // DIType *getDIStructType(StructType *structType) { return pos->second; }

    string getDIStructField(StructType *structType, uint64_t index) {
        // iterate over all DICompositeType
        auto pos = diTypeMap.find(structType);
        if (pos == diTypeMap.end()) {
            return "";
        }

        auto diStructType = static_cast<DICompositeType *>(pos->second);
        auto elements = diStructType->getElements();
        if (index >= elements.size()) {
            return "";
        }

        auto element = elements[index];
        if (auto *derivedType = dyn_cast<DIDerivedType>(element)) {
            return getDITypeName(derivedType->getBaseType());
        }
        return "";
    }

    string getDITypeName(DIType *ditype) {
        if (!ditype)
            return "void";

        string name = "";

        // dump DIType according to DITag
        auto tag = ditype->getTag();
        switch (tag) {
        case dwarf::DW_TAG_base_type:
            name = ditype->getName().equals("_Bool") ? "bool"
                                                     : ditype->getName().str();
            break;
        case dwarf::DW_TAG_enumeration_type:
            name = "enum " + ditype->getName().str();
            break;
        case dwarf::DW_TAG_array_type: {
            auto *composite = dyn_cast_or_null<DICompositeType>(ditype);
            auto basename = composite->getBaseType() != nullptr
                                ? getDITypeName(composite->getBaseType())
                                : "void";
            // infer subrange
            int subrangeCount = 0;
            auto elements = composite->getElements(); // get elements
            for (auto element : elements) {
                if (auto *sub = dyn_cast_or_null<DISubrange>(element))
                    subrangeCount++;
            }
            name =
                basename + string(subrangeCount, '*'); // multi-dimension array
        } break;
        case dwarf::DW_TAG_pointer_type: {
            auto *derived = dyn_cast_or_null<DIDerivedType>(ditype);
            auto basename = derived->getBaseType() != nullptr
                                ? getDITypeName(derived->getBaseType())
                                : "void";
            if (basename == "void")
                name = "ptr";
            else
                name = basename + "*";
        } break;
        case dwarf::DW_TAG_structure_type:
            name = "struct " + ditype->getName().str();
            break;
        case dwarf::DW_TAG_typedef:
            name = ditype->getName().str();
            break;
        case dwarf::DW_TAG_volatile_type:
        case dwarf::DW_TAG_restrict_type:
        case dwarf::DW_TAG_const_type: {
            auto *derived = dyn_cast_or_null<DIDerivedType>(ditype);
            auto basename = derived->getBaseType() != nullptr
                                ? getDITypeName(derived->getBaseType())
                                : "void";
            name = basename;
        } break;
        case dwarf::DW_TAG_union_type: {
            name = "union " + ditype->getName().str();
        } break;
        case dwarf::DW_TAG_subroutine_type: {
            auto *subroutine = dyn_cast_or_null<DISubroutineType>(ditype);
            name = subroutine->getName();
        } break;
        default:
            errs() << "[ERR] should handle " << tag << "\n";
            break;
        }

        return name;
    }
};

class TBAAHelper : public LLVMHelper {
  public:
    void initialize(Module *module, TypeGraph *tg) {
        for (auto &func : *module) {
            for (auto &basic_block : func) {
                for (auto &inst : basic_block) {
                    // handle TBAA
                    auto aamd = inst.getAAMetadata();

                    if (aamd.TBAA) {
                        auto *tbaa = aamd.TBAA;
                        auto tbaaTypeName = parseTypeName(tbaa);

                        if (tbaaTypeName.empty())
                            continue;

                        // handle store ptr
                        if (auto *store = dyn_cast_or_null<StoreInst>(&inst)) {
                            auto *ptr = store->getPointerOperand();
                            tg->put(&func, ptr, tbaaTypeName);
                        } else if (auto *load =
                                       dyn_cast_or_null<LoadInst>(&inst)) {
                            auto *ptr = load->getPointerOperand();
                            tg->put(&func, &inst, tbaaTypeName);
                        }
                    }
                }
            }
        }
    }

    string getTypeName(MDNode *tbaaType) {
        auto *baseTyName = dyn_cast_or_null<MDString>(tbaaType->getOperand(0));

        // if accessTy is an omnipotent char
        auto *accessTy = dyn_cast_or_null<MDNode>(tbaaType->getOperand(1));
        if (isOmnipotentChar(accessTy)) {
            return baseTyName->getString().str();
        }

        return getTypeName(accessTy);
    }

    // parse TBAA type name
    string parseTypeName(MDNode *tbaa) {

        auto *baseTy = dyn_cast_or_null<MDNode>(tbaa->getOperand(0));
        auto *accessTy = dyn_cast_or_null<MDNode>(tbaa->getOperand(1));

        if (isOmnipotentChar(accessTy))
            return "";

        auto name = getTypeName(accessTy);
        if (name == "any pointer")
            return "";

        return name + "*";
    }

    bool isOmnipotentChar(MDNode *tbaa) {
        auto *accessTyName = dyn_cast_or_null<MDString>(tbaa->getOperand(0));
        return accessTyName->getString().equals("omnipotent char");
    }
};

class CombHelper : public LLVMHelper {
    // TODO combine migration and debug info (and maybe TBAA)
  private:
    MigrationHelper *migHelper;
    DebugInfoHelper *diHelper;
    map<StructType *, DIType *> diTypeMap;

  public:
    void initialize(Module *module, TypeGraph *tg) {
        // create the inner helpers
        migHelper = new MigrationHelper();
        diHelper = new DebugInfoHelper();
        
        // map structure type to DIType
        DebugInfoFinder finder;
        finder.processModule(*module);

        for (auto s : module->getIdentifiedStructTypes()) {
            if (!s->hasName())
                continue;

            auto structName = s->getName();
            structName.consume_front("struct.");
            for (auto type : finder.types()) {
                // typedef
                if (auto *derived = dyn_cast_or_null<DIDerivedType>(type)) {
                    if (derived->getTag() == dwarf::DW_TAG_typedef) {
                        if (derived->getName().equals(structName)) {
                            diTypeMap.insert({s, derived->getBaseType()});
                            break;
                        }
                    }
                }
                // struct
                else if (auto *composite =
                             dyn_cast_or_null<DICompositeType>(type)) {
                    if (composite->getTag() == dwarf::DW_TAG_structure_type) {
                        if (composite->getName().equals(structName)) {
                            // empty elements, skip
                            if (composite->getElements().empty())
                                continue;

                            diTypeMap.insert({s, composite});
                            break;
                        }
                    }
                }
            }
        }

        for (auto &global : module->globals()) {
            SmallVector<DIGlobalVariableExpression *> di_global_exps;
            global.getDebugInfo(di_global_exps);

            if (di_global_exps.empty()) {
                auto type = global.getValueType();
                tg->put(nullptr, &global, tyHelper.getReference(type));
                continue;
            }

            for (auto di_global_exp : di_global_exps) {
                auto di_global = di_global_exp->getVariable();
                tg->put(nullptr, &global,
                        diHelper->getDITypeName(di_global->getType()));
            }
        }

        for (auto &func : *module) {
            for (auto &basic_block : func) {
                for (auto &inst : basic_block) {

                    // get value
                    Value *value = dyn_cast<Value>(&inst);
                    if (auto store = dyn_cast_or_null<StoreInst>(&inst)) {
                        value = store->getValueOperand();
                    }

                    // alloca adds one more level of ptr
                    if (auto di_local = diHelper->getDILocalVar(inst)) {
                        tg->put(&func, value,
                                diHelper->getDITypeName(di_local->getType()));
                    } else {
                        // no debug info, default to migration instruction 
                        auto type = migHelper->getType(inst);
                        tg->put(&func, value, type);
                    }
                }
            }
        }

        // handle function prototype accurately
        for (auto &func : *module) {
            Value *funcValue = dyn_cast<Value>(&func);

            // get DISubprogram
            auto *subprogram = func.getSubprogram();

            if (!subprogram) {
                auto type = func.getFunctionType();
                auto retType = type->getReturnType();
                tg->put(nullptr, funcValue, tyHelper.getTypeName(retType));
            } else {
                auto *subroutinetype = subprogram->getType();
                auto typearray = subroutinetype->getTypeArray();

                // process return type
                tg->put(nullptr, funcValue,
                        diHelper->getDITypeName(typearray[0]));

                // process parameters
                for (int i = 1; i < typearray.size(); ++i) {
                    auto type = typearray[i];

                    if (!type) // type can be null in variable func parameters
                        continue;

                    if (i - 1 < func.arg_size()) { // avoid overflow
                        auto param = func.getArg(i - 1);
                        tg->put(&func, param, diHelper->getDITypeName(type));
                    }
                }
            }
        }
    }

    bool hasDebugInfo(Module &M) {
        return diHelper->hasDebugInfo(M);
    }
};