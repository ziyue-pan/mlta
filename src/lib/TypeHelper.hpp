#pragma once

#include "TypeGraph.hpp"

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
        return !typeset->empty() && !typeset->isOpaque() && !typeset->isGenericPtr();
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
                    if (auto store = dyn_cast<StoreInst>(&inst)) {
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

    void dumpOpaque(Module *module, TypeGraph *tg) {
        errs() << "dumping ...\n";
        int cnt = 0;
        for (auto &global : module->globals()) {
            Value *v = dyn_cast<Value>(&global);
            cnt++;
            if (!tg->get(nullptr, v)) {
                errs() << "[MISS] " << *v << "\n";
            }
        }

        for (auto &F : *module) {
            for (auto &BB : F) {
                for (auto &inst : BB) {
                    if (inst.getType()->isVoidTy())
                        continue;
                    if (isa<StoreInst>(&inst))
                        continue;
                    Value *v = dyn_cast<Value>(&inst);
                    cnt++;

                    if (!tg->get(&F, v)) {
                        errs() << "[MISS] " << *v << "\n";
                    }
                }
            }
        }

        errs() << "total: " << cnt << "\n";
    }
};
