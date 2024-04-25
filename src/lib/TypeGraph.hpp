#pragma once

#include <llvm/ADT/StringExtras.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/Value.h>
#include <llvm/Support/Casting.h>

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
    TypeMap globalMap;
    map<Function *, TypeMap *> localMap;

    // FIXME add filter, support for more accurate type information
    const bool UNARY = false;
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

    // FIXME add filter, support for more accurate type information
    /// merge multiple types into one value's type
    void put(Function *scope, Value *key, TypeSet *value) {
        // no value, quick return
        if (!value)
            return;

        // find existing type
        auto old = get(scope, key);

        if (old) {
            // filter out ptr* type
            if (DEBUG && value->count("ptr**")) {
                key->dump();
                errs() << "current type: ";
                old->dump();
                errs() << "\n";
                print_stacktrace();
            }

            // filter out type conflict
            // FIXME old and value should define a new operator
            if (UNARY && !old->empty() && !old->count("ptr") && old != value) {
                errs() << "[ERR] type conflict when put ";
                value->dump();
                errs() << " to ";
                key->dump();
                errs() << "current type: ";
                old->dump();
                errs() << "\n";
                // print_stacktrace();
            }
        }

        if (old == nullptr)
            old = new TypeSet();

        // update type
        old->insert(value);
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
    }

    // FIXME add filter, support for more accurate type information
    /// merge a single type into one value's type
    void put(Function *scope, Value *key, string value) {
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

            // filter out type conflict
            if (UNARY && value != "ptr" && !old->empty() && !old->hasPtr() &&
                !old->count(value)) {
                errs() << "[ERR] type conflict when put " << value << " to ";
                key->dump();
                errs() << "current type: ";
                old->dump();
                errs() << "\n";
            }
        }

        if (old == nullptr)
            old = new TypeSet();

        // update type
        old->insert(value);

        // should not be a global value
        if (!dyn_cast_or_null<GlobalValue>(key) && scope) {
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
        // filter out internal values
        if (!key->hasName() || isInternal(key))
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

    vector<TypeMap *> getAllMap() {
        vector<TypeMap *> ret;
        ret.push_back(&globalMap);
        for (auto &pair : localMap) {
            ret.push_back(pair.second);
        }
        return ret;
    }
};