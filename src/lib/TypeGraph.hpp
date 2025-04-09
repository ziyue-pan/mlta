#pragma once

#include <llvm/ADT/StringExtras.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/Value.h>
#include <llvm/Support/Casting.h>

#include <iomanip>
#include <iostream>
#include <map>
#include <set>

#include <execinfo.h>

#include "TypeSet.hpp"

using namespace std;
using namespace llvm;

inline void print_stacktrace(void) {
    errs() << "stack trace:\n";
    char **strings;
    size_t i, size;
    enum Constexpr { MAX_SIZE = 1024 };
    void *array[MAX_SIZE];
    size = backtrace(array, MAX_SIZE);
    strings = backtrace_symbols(array, size);
    for (i = 0; i < size; i++)
        errs() << strings[i] << "\n";
    free(strings);
}

/// This class maintains a map from llvm values to the possible types.
class TypeGraph {
    using TypeMap = map<Value *, TypeSet *>;

  private:
    const bool DEBUG = false;

    bool canFlow(string type) { return !type.empty() && type != "ptr"; }

    bool canFlow(set<string> typeset) {
        return !typeset.empty() && !typeset.count("ptr");
    }

    bool isInternal(Value *v) {
        auto name = v->getName();
        if (name.startswith("."))
            return true;
        if (name.count(".") > 1)
            return true;
        if (name.count(".") == 1) {
            auto subname = name.split(".").second;
            for (auto ch : subname) {
                if (!isDigit(ch))
                    return true;
            }
        }

        return false;
    }

  public:
    TypeMap globalMap;
    map<Function *, TypeMap *> localMap;

    ~TypeGraph() {
        for (auto &pair : globalMap) {
            delete pair.second;
        }
        for (auto &pair : localMap) {
            for (auto &pair2 : *pair.second) {
                delete pair2.second;
            }
            delete pair.second;
        }
    }

    /// get the type of a value
    TypeSet *get(Function *scope, Value *key) {
        // first check local type
        if (scope && localMap.find(scope) != localMap.end()) {
            TypeMap *localTypeMap = localMap[scope];
            auto it = localTypeMap->find(key);
            if (it != localTypeMap->end())
                return it->second;
        }

        // check global type
        auto it = globalMap.find(key);
        if (it != globalMap.end())
            return it->second;

        return nullptr;
    }

    /// merge multiple types into one value's type
    // return true if the type is updated, false if not changed
    bool put(Function *scope, Value *key, TypeSet *value, bool isFunc = false) {
        // no value, quick return
        if (!value)
            return false;

        TypeSet *to_add = new TypeSet();
        to_add->insert(value);

        // find existing type
        auto old = get(scope, key);

        if (old) {
            // filter out ptr* type
            if (DEBUG && to_add->count("ptr**")) {
                key->dump();
                errs() << "current type: ";
                old->dump();
                errs() << "\n";
                print_stacktrace();
            }

            // filter out subtype problems
            for (auto &type : to_add->getTypes()) {
                // if old contains type.reference(), then skip
                if (old->count(type + "*")) {
                    to_add->erase(type);
                } else if (type.back() == '*' &&
                           old->count(type.substr(0, type.size() - 1))) {
                    // if old contains type.dereference(), then skip
                    to_add->erase(type);
                }
            }
        }

        if (old == nullptr)
            old = new TypeSet();

        if (to_add->empty()) {
            delete to_add;
            return false;
        }

        // check if old == value
        if (old->equals(to_add)) {
            delete to_add;
            return false;
        }

        // update type
        old->insert(to_add);
        delete to_add;

        if (isFunc)
            old->isFunc = true;

        if (scope) { // test scope
            auto it = localMap.find(scope);
            if (it != localMap.end()) // update local map
                (*it->second)[key] = old;
            else {
                localMap[scope] = new TypeMap();
                (*localMap[scope])[key] = old;
            }
        } else { // no scope, update global map
            globalMap[key] = old;
        }

        return true;
    }

    /// merge a single type into one value's type
    bool put(Function *scope, Value *key, string value, bool isFunc = false) {
        // find existing type
        auto old = get(scope, key);

        if (old) {
            // filter out ptr* type
            if (DEBUG && value == "ptr**") {
                key->dump();
                errs() << "current type: ";
                old->dump();
                errs() << "\n";
                print_stacktrace();
            }

            // filter out subtype problems
            if (old->count(value + "*")) {
                return false;
            } else if (value.back() == '*' &&
                       old->count(value.substr(0, value.size() - 1))) {
                return false;
            }
        }

        if (old == nullptr)
            old = new TypeSet();

        // check if old == value
        if (old->count(value))
            return false;

        // update type
        old->insert(value);

        if (isFunc)
            old->isFunc = true;

        // should not be a global value
        if (!dyn_cast<GlobalValue>(key) && scope) {
            auto it = localMap.find(scope);
            if (it != localMap.end()) // update local map
                (*it->second)[key] = old;
            else {
                localMap[scope] = new TypeMap();
                (*localMap[scope])[key] = old;
            }
        } else { // no scope, update global map
            globalMap[key] = old;
        }

        return true;
    }

    /// check if a value is an opaque pointer
    bool isOpaque(Function *scope, Value *key) {
        auto typeSet = get(scope, key);
        return typeSet && typeSet->count("ptr");
    }

    /// get the pointer type of a value
    TypeSet *reference(Function *scope, Value *key) {
        auto ret = new TypeSet();
        auto old = get(scope, key);

        if (!old)
            return ret;

        for (auto &type : old->getTypes()) {
            // if type ends with "**", skip ***p
            if (type.size() > 2 && type.substr(type.size() - 2) == "**") {
                continue;
            }

            if (type != "ptr")
                ret->insert(type + "*");
        }

        return ret;
    }

    /// get the dereferenced type of a value
    TypeSet *dereference(Function *scope, Value *key) {
        auto ret = new TypeSet();
        auto old = get(scope, key);

        if (!old)
            return ret;

        for (auto &type : old->getTypes()) {
            if (type.back() == '*') {
                ret->insert(type.substr(0, type.size() - 1));
            }
        }

        return ret;
    }

    /// print the type of a value
    void dumpType(Function *scope, Value *key, TypeSet *value) {
        if (!key->hasName())
            return;

        if (scope)
            errs() << scope->getName() << ", ";
        else
            errs() << "(global), ";

        errs() << key->getName() << ", { ";

        if (!value->empty()) {
            set<string>::iterator iter = value->begin();
            errs() << *iter;
            while (++iter != value->end())
                errs() << ", " << *iter;
        }
        errs() << " }\n";
    }

    void dumpAllType() {
        for (auto &pair : globalMap) {
            dumpType(nullptr, pair.first, pair.second);
        }
        for (auto &pair : localMap) {
            for (auto &pair2 : *pair.second) {
                dumpType(pair.first, pair2.first, pair2.second);
            }
        }
    }

    void coverage(Module *module) {
        int total_cnt = 0;
        int cover_cnt = 0;

        // globals
        for (auto &global : module->globals()) {
            // skip if global is a function
            // if (global.getType()->isFunctionTy())
            //     continue;
            total_cnt++;

            Value *v = dyn_cast<Value>(&global);
            auto type = get(nullptr, v);
            if (type && !type->isOpaque())
                cover_cnt++;
        }

        // arguments
        for (auto &F : *module) {
            Function *scope = dyn_cast<Function>(&F);

            for (auto &arg : F.args()) {
                total_cnt++;
                auto type = get(scope, &arg);
                if (type && !type->isOpaque()) {
                    cover_cnt++;
                } 
            }
            for (auto &BB : F) {
                for (auto &inst : BB) {

                    if (auto store = dyn_cast<StoreInst>(&inst))
                        continue;

                    total_cnt++;
                    auto type = get(scope, dyn_cast<Value>(&inst));
                    if (type && !type->isOpaque()) {
                        cover_cnt++;
                    } 
                }
            }
        }

        errs() << "[RESULT] total count: " << total_cnt << "\n";
        errs() << "[RESULT] cover count: " << cover_cnt << "\n";
        double coverage = (double)cover_cnt / total_cnt;

        ostringstream oss;
        oss << fixed << std::setprecision(2) << (coverage * 100) << "%";
        errs() << "[RESULT] coverage: " << oss.str() << "\n";
    }

    vector<TypeMap *> getAllMap() {
        vector<TypeMap *> ret;
        ret.push_back(&globalMap);
        for (auto &pair : localMap) {
            ret.push_back(pair.second);
        }
        return ret;
    }
};