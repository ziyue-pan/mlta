#pragma once

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
#include <regex>
#include <string>
#include <utility>

#include "TypeGraph.hpp"
#include "TypeHelper.hpp"

using namespace llvm;
using namespace std;

map<string, string> type_trans_map = {
    {"bool", "i1"},           {"short", "i16"},
    {"char", "i8"},           {"int", "i32"},
    {"long", "i64"},          {"long long", "i64"},
    {"unsigned char", "i8"},  {"unsigned short", "i16"},
    {"unsigned long", "i64"}, {"unsigned long long", "i64"},
    {"unsigned int", "i32"},
};
string di_to_ir_type(string &di_type) {
    string ir_type = di_type;

    // trim the ending multiple *
    int ptr_level = 0;
    while (ir_type.back() == '*') {
        ir_type.pop_back();
        ptr_level++;
    }

    // general types, in the table
    auto iter = type_trans_map.find(ir_type);
    if (iter != type_trans_map.end()) {
        ir_type = iter->second;
    } else {
        // di_type starts with "struct"
        if (ir_type.find("struct") == 0) {
            ir_type = "\%struct." + ir_type.substr(7);
        } else if (ir_type.find("enum") == 0) {
            ir_type = "i32";
        }
    }

    ir_type += string(ptr_level, '*');
    return ir_type;
}

// trim suffixing digits from ir types
string trim_ir_suffix(string &ir_type) {
    regex pattern(R"((%struct\.[a-zA-Z_]\w*)\.\d+(\*?))");
    return regex_replace(ir_type, pattern, "$1$2");
}

class LLVMHelper {
  public:
    TypeHelper tyHelper;
    virtual void initialize(Module *module, TypeGraph *tg) = 0;
};

/// Helper class for LLVM migration instructions
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
            for (auto &arg : func.args()) {
                Value *value = dyn_cast<Value>(&arg);
                auto type = tyHelper.getTypeName(value->getType());
                tg->put(&func, value, type);
            }

            for (auto &basic_block : func) {
                for (auto &inst : basic_block) {
                    auto type = getType(inst);

                    // get value
                    Value *value = dyn_cast<Value>(&inst);
                    if (auto store = dyn_cast<StoreInst>(&inst)) {
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
            tg->put(nullptr, funcValue, tyHelper.getTypeName(retType), true);
        }
    }

    string getType(Instruction &inst) {
        auto type = inst.getType();
        if (auto *load = dyn_cast<LoadInst>(&inst)) {
            type = load->getType();
        }
        if (auto *store = dyn_cast<StoreInst>(&inst)) {
            type = store->getValueOperand()->getType();
        }

        // handle gep instruction
        // if (auto* gep = dyn_cast<GetElementPtrInst>(&inst)) {
        //     type = gep->getSourceElementType();
        //     return getTypeName(type) + "*";
        // }

        // handle alloca instruction
        if (auto *alloca = dyn_cast<AllocaInst>(&inst)) {
            type = alloca->getAllocatedType();

            if (!tyHelper.isOpaque(type))
                return tyHelper.getReference(type);
        }

        // handle call instruction
        if (auto *call = dyn_cast<CallInst>(&inst)) {
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
    const bool RESOLVE_TYPEDEF = true;

    map<StructType *, DIType *> structMap;
    map<Value *, vector<DILocalVariable *>> diLocalMap;

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
                if (auto *derived = dyn_cast<DIDerivedType>(type)) {
                    if (derived->getTag() == dwarf::DW_TAG_typedef) {
                        if (derived->getName().equals(structName)) {
                            structMap.insert({s, derived->getBaseType()});
                            break;
                        }
                    }
                }
                // struct
                else if (auto *composite = dyn_cast<DICompositeType>(type)) {
                    if (composite->getTag() == dwarf::DW_TAG_structure_type) {
                        if (composite->getName().equals(structName)) {
                            // empty elements, skip
                            if (composite->getElements().empty())
                                continue;

                            structMap.insert({s, composite});
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
                continue;
            }

            for (auto di_global_exp : di_global_exps) {
                auto di_global = di_global_exp->getVariable();
                auto di_type_name = getDITypeName(di_global->getType()) + "*";
                tg->put(nullptr, &global, di_to_ir_type(di_type_name));
            }
        }

        // parse di local variables
        for (auto &F : *module) {
            for (auto &BB : F) {
                for (auto &I : BB) {
                    parseDILocalVar(I, diLocalMap);
                }
            }
        }

        for (auto &func : *module) {
            for (auto &basic_block : func) {
                for (auto &inst : basic_block) {

                    // get value
                    Value *value = dyn_cast<Value>(&inst);
                    if (auto store = dyn_cast<StoreInst>(&inst)) {
                        value = store->getValueOperand();
                    }

                    if (diLocalMap.find(value) != diLocalMap.end()) {
                        for (auto di_local : diLocalMap[value]) {
                            auto di_type_name =
                                getDITypeName(di_local->getType());

                            if (AllocaInst *alloca =
                                    dyn_cast<AllocaInst>(value)) {
                                di_type_name += "*";
                            }
                            tg->put(&func, value, di_to_ir_type(di_type_name));
                        }
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
                tg->put(nullptr, funcValue, tyHelper.getTypeName(retType),
                        true);
            } else {
                auto *subroutinetype = subprogram->getType();
                auto typearray = subroutinetype->getTypeArray();

                // process return type
                auto di_type_name = getDITypeName(typearray[0]);
                tg->put(nullptr, funcValue, di_to_ir_type(di_type_name), true);

                // process parameters
                for (int i = 1; i < typearray.size(); ++i) {
                    auto type = typearray[i];

                    if (!type) // type can be null in variable func parameters
                        continue;

                    if (i - 1 < func.arg_size()) { // avoid overflow
                        // type from DI subprogram
                        auto param = func.getArg(i - 1);
                        auto di_type_name = getDITypeName(type);
                        tg->put(&func, param, di_to_ir_type(di_type_name));

                        // type from DI local var
                        if (diLocalMap.find(param) != diLocalMap.end()) {
                            for (auto di_local : diLocalMap[param]) {
                                auto di_type_name =
                                    getDITypeName(di_local->getType());
                                tg->put(&func, param,
                                        di_to_ir_type(di_type_name));
                            }
                        }
                    }
                }
            }
        }
    }

    void parseDILocalVar(Instruction &inst,
                         map<Value *, vector<DILocalVariable *>> &diLocalMap) {
        // check if is call inst
        if (auto *call = dyn_cast<CallInst>(&inst)) {
            auto call_func = call->getCalledFunction();
            if (!call_func)
                return;
            if (call_func->arg_size() < 2)
                return;

            // check invoked function is intrinsic llvm debug function
            if (call_func && call_func->isIntrinsic() &&
                call_func->getName().startswith("llvm.dbg")) {

                auto val = call->getArgOperand(0);
                MetadataAsValue *mtv0 = dyn_cast_or_null<MetadataAsValue>(val);
                Metadata *mt0 = mtv0->getMetadata();
                ValueAsMetadata *vmt = dyn_cast_or_null<ValueAsMetadata>(mt0);
                if (!vmt)
                    return;
                Value *real_val = vmt->getValue();

                // get the second argument, which stores `DILocalVariable`
                if (auto arg = call->getArgOperand(1)) {
                    auto metadata =
                        dyn_cast<MetadataAsValue>(arg)->getMetadata();
                    DILocalVariable *di_value =
                        dyn_cast<DILocalVariable>(metadata);
                    // one value can map to multiple values, so save in vectors
                    if (diLocalMap.find(real_val) != diLocalMap.end()) {
                        diLocalMap[real_val].push_back(di_value);
                    } else {
                        diLocalMap[real_val] = {di_value};
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

    string getDIStructField(StructType *structType, uint64_t index) {
        // iterate over all DICompositeType
        auto pos = structMap.find(structType);

        // if (pos == structMap.end()) {
        //     // try to search by struct name
        //     if (!structType->hasName()) {
        //         return "";
        //     }

        //     auto structName = structType->getName().str();
        //     for (auto it = structMap.begin(); it != structMap.end(); it++) {
        //         errs() << "[DBG] it: " << it->first << "\n";
        //         auto itName = it->first->getName().str();
        //         if (itName == structName) {
        //             pos = it;
        //             break;
        //         }
        //     }
        // }

        if (pos == structMap.end()) {
            return "";
        }

        auto diStructType = static_cast<DICompositeType *>(pos->second);
        auto elements = diStructType->getElements();
        if (index >= elements.size()) {
            return "";
        }

        auto element = elements[index];
        if (auto *derivedType = dyn_cast<DIDerivedType>(element)) {
            auto baseTypeName = getDITypeName(derivedType->getBaseType());
            return di_to_ir_type(baseTypeName);
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
            auto *composite = dyn_cast<DICompositeType>(ditype);
            auto basename = composite->getBaseType() != nullptr
                                ? getDITypeName(composite->getBaseType())
                                : "void";
            // infer subrange
            int subrangeCount = 0;
            auto elements = composite->getElements(); // get elements
            for (auto element : elements) {
                if (auto *sub = dyn_cast<DISubrange>(element))
                    subrangeCount++;
            }
            name =
                basename + string(subrangeCount, '*'); // multi-dimension array
        } break;
        case dwarf::DW_TAG_pointer_type: {
            auto *derived = dyn_cast<DIDerivedType>(ditype);
            auto basename = derived->getBaseType() != nullptr
                                ? getDITypeName(derived->getBaseType())
                                : "void";
            // if (basename == "void")
            //     name = "ptr";
            // else
            name = basename + "*";
        } break;
        case dwarf::DW_TAG_structure_type:
            name = "struct " + ditype->getName().str();
            break;
        case dwarf::DW_TAG_typedef:
            if (RESOLVE_TYPEDEF) {
                auto *derived = dyn_cast<DIDerivedType>(ditype);
                auto basetype = derived->getBaseType();
                if (basetype) {
                    name = getDITypeName(basetype);
                    if (name == "") {
                        name = ditype->getName().str();
                    }
                } else {
                    name = ditype->getName().str();
                }
            } else {
                name = ditype->getName().str();
            }
            break;
        case dwarf::DW_TAG_volatile_type:
        case dwarf::DW_TAG_restrict_type:
        case dwarf::DW_TAG_const_type: {
            auto *derived = dyn_cast<DIDerivedType>(ditype);
            auto basename = derived->getBaseType() != nullptr
                                ? getDITypeName(derived->getBaseType())
                                : "void";
            name = basename;
        } break;
        case dwarf::DW_TAG_union_type: {
            name = "union " + ditype->getName().str();
        } break;
        case dwarf::DW_TAG_subroutine_type: {
            auto *subroutine = dyn_cast<DISubroutineType>(ditype);
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

                    if (aamd && aamd.TBAA) {
                        // get tbaa type name
                        auto tbaa_type = getTBAAType(aamd.TBAA, func, inst);

                        // skip empty type
                        if (tbaa_type.empty()) {
                            continue;
                        }

                        Value *ld_st_ptr = nullptr; // tbaa annotated pointer

                        // bond to load or store instruction
                        if (LoadInst *load = dyn_cast<LoadInst>(&inst)) {
                            ld_st_ptr = load->getPointerOperand();
                        } else if (StoreInst *store =
                                       dyn_cast<StoreInst>(&inst)) {
                            ld_st_ptr = store->getPointerOperand();
                        }

                        if (ld_st_ptr) {
                            if (tbaa_type != "any pointer" && !isScalarType(tbaa_type)) {
                                tbaa_type = "%struct." + tbaa_type;
                            }

                            // if is scala type
                            if (isScalarType(tbaa_type)) {
                                tg->put(&func, ld_st_ptr, tbaa_type);
                            } else if (GlobalValue *gv =
                                           dyn_cast<GlobalValue>(ld_st_ptr)) {
                                tg->put(nullptr, gv, tbaa_type);
                            } else if (GetElementPtrInst *gep =
                                           dyn_cast<GetElementPtrInst>(
                                               ld_st_ptr)) {
                                tg->put(&func, gep->getPointerOperand(),
                                        tbaa_type);
                            } else if (LoadInst *load =
                                           dyn_cast<LoadInst>(ld_st_ptr)) {
                                tg->put(&func, load->getPointerOperand(),
                                        tbaa_type);
                            } else if (StoreInst *store =
                                           dyn_cast<StoreInst>(ld_st_ptr)) {
                                tg->put(&func, store->getPointerOperand(),
                                        tbaa_type);
                            }
                        }
                    }

                    // if (aamd && aamd.TBAA) {
                    //     auto *tbaa = aamd.TBAA;
                    //     auto tbaaTypeName = parseTypeName(tbaa);

                    //     if (tbaaTypeName.empty())
                    //         continue;

                    //     auto trans_type_name = di_to_ir_type(tbaaTypeName);
                    //     // handle store ptr
                    //     if (auto *store = dyn_cast<StoreInst>(&inst)) {
                    //         auto *ptr = store->getPointerOperand();
                    //         tg->put(&func, ptr, trans_type_name);
                    //     } else if (auto *load = dyn_cast<LoadInst>(&inst)) {
                    //         auto *ptr = load->getPointerOperand();
                    //         tg->put(&func, ptr, trans_type_name);
                    //     }
                    // }
                }
            }
        }
    }

    bool isScalarType(string &type) {
        return type == "i1" || type == "i8" || type == "i16" || type == "i32" ||
               type == "i64" || type == "float" || type == "double";
    }

    string getTBAAType(MDNode *tbaa, Function &func, Instruction &inst) {
        // get first field
        auto *baseTy = dyn_cast<MDNode>(tbaa->getOperand(0));

        // get baseTy's type name
        if (!baseTy) {
            return "";
        }

        auto *baseTyName = dyn_cast<MDString>(baseTy->getOperand(0));
        if (!baseTyName) {
            return "";
        }

        auto baseTypeName = baseTyName->getString().str();

        if (baseTypeName.empty() || baseTypeName == "omnipotent char") {
            return "";
        }

        return di_to_ir_type(baseTypeName);
    }

    string getTypeName(MDNode *tbaaType) {
        auto *baseTyName = dyn_cast<MDString>(tbaaType->getOperand(0));

        // if accessTy is an omnipotent char
        auto *accessTy = dyn_cast<MDNode>(tbaaType->getOperand(1));
        if (isOmnipotentChar(accessTy)) {
            return baseTyName->getString().str();
        }

        return getTypeName(accessTy);
    }

    // parse TBAA type name
    string parseTypeName(MDNode *tbaa) {

        auto *baseTy = dyn_cast<MDNode>(tbaa->getOperand(0));
        auto *accessTy = dyn_cast<MDNode>(tbaa->getOperand(1));

        if (isOmnipotentChar(accessTy))
            return "";

        auto name = getTypeName(accessTy);
        if (name == "any pointer")
            return "";

        return name + "*";
    }

    bool isOmnipotentChar(MDNode *tbaa) {
        auto *accessTyName = dyn_cast<MDString>(tbaa->getOperand(0));
        return accessTyName->getString().equals("omnipotent char");
    }
};

class CombHelper : public LLVMHelper {
  private:
    MigrationHelper *migHelper;
    DebugInfoHelper *diHelper;
    map<StructType *, DIType *> structMap;
    map<Value *, vector<DILocalVariable *>> diLocalMap;

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
                if (auto *derived = dyn_cast<DIDerivedType>(type)) {
                    if (derived->getTag() == dwarf::DW_TAG_typedef) {
                        if (derived->getName().equals(structName)) {
                            structMap.insert({s, derived->getBaseType()});
                            break;
                        }
                    }
                }
                // struct
                else if (auto *composite = dyn_cast<DICompositeType>(type)) {
                    if (composite->getTag() == dwarf::DW_TAG_structure_type) {
                        if (composite->getName().equals(structName)) {
                            // empty elements, skip
                            if (composite->getElements().empty())
                                continue;

                            structMap.insert({s, composite});
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
                auto di_type_name =
                    diHelper->getDITypeName(di_global->getType()) + "*";
                tg->put(nullptr, &global, di_to_ir_type(di_type_name));
            }
        }

        // parse di local variables
        for (auto &F : *module) {
            for (auto &BB : F) {
                for (auto &I : BB) {
                    diHelper->parseDILocalVar(I, diLocalMap);
                }
            }
        }

        for (auto &func : *module) {
            for (auto &basic_block : func) {
                for (auto &inst : basic_block) {

                    // get value
                    Value *value = dyn_cast<Value>(&inst);
                    if (auto store = dyn_cast<StoreInst>(&inst)) {
                        value = store->getValueOperand();
                    }

                    // type from DI local var
                    if (diLocalMap.find(value) != diLocalMap.end()) {
                        for (auto di_local : diLocalMap[value]) {
                            auto di_type_name =
                                diHelper->getDITypeName(di_local->getType());

                            // handle alloca instruction
                            if (AllocaInst *alloca =
                                    dyn_cast<AllocaInst>(value)) {
                                di_type_name += "*";
                            }
                            tg->put(&func, value, di_to_ir_type(di_type_name));
                        }
                    }
                    // no debug info, default to migration instruction
                    auto type = migHelper->getType(inst);
                    tg->put(&func, value, type);
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
                tg->put(nullptr, funcValue, tyHelper.getTypeName(retType),
                        true);
            } else {
                auto *subroutinetype = subprogram->getType();
                auto typearray = subroutinetype->getTypeArray();

                // process return type
                auto di_type_name = diHelper->getDITypeName(typearray[0]);
                tg->put(nullptr, funcValue, di_to_ir_type(di_type_name), true);

                // process parameters
                for (int i = 1; i < typearray.size(); ++i) {
                    auto type = typearray[i];

                    if (!type) // type can be null in variable func parameters
                        continue;

                    if (i - 1 < func.arg_size()) { // avoid overflow
                        // type from DI subprogram
                        auto param = func.getArg(i - 1);
                        auto di_type_name = diHelper->getDITypeName(type);
                        tg->put(&func, param, di_to_ir_type(di_type_name));

                        // type from DI local var
                        if (diLocalMap.find(param) != diLocalMap.end()) {
                            for (auto di_local : diLocalMap[param]) {
                                auto di_type_name = diHelper->getDITypeName(
                                    di_local->getType());
                                tg->put(&func, param,
                                        di_to_ir_type(di_type_name));
                            }
                        }
                    }
                }
            }
        }
    }

    bool hasDebugInfo(Module &M) { return diHelper->hasDebugInfo(M); }

    string getDIStructField(StructType *structType, uint64_t index) {
        // iterate over all DICompositeType
        auto pos = structMap.find(structType);

        // if (pos == structMap.end()) {
        //     // try to search by struct name
        //     if (!structType->hasName()) {
        //         return "";
        //     }

        //     auto structName = structType->getName().str();
        //     for (auto it = structMap.begin(); it != structMap.end(); it++) {
        //         auto itName = it->first->getName().str();
        //         if (itName == structName) {
        //             pos = it;
        //             break;
        //         }
        //     }
        // }

        if (pos == structMap.end()) {
            return "";
        }

        auto diStructType = static_cast<DICompositeType *>(pos->second);
        auto elements = diStructType->getElements();
        if (index >= elements.size()) {
            return "";
        }

        auto element = elements[index];
        if (auto *derivedType = dyn_cast<DIDerivedType>(element)) {
            auto baseTypeName =
                diHelper->getDITypeName(derivedType->getBaseType());
            return di_to_ir_type(baseTypeName);
        }
        return "";
    }
};