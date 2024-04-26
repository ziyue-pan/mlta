#pragma once

#include <llvm/Support/VersionTuple.h>
#include <llvm/Support/raw_ostream.h>

#include <set>
#include <string>
#include <vector>

using namespace std;
using namespace llvm;

class TypeSet {
  private:
    set<string> types;

  public:
    void dump() {
        // iterate types and print them with a comma
        auto it = types.begin();
        for (; it != types.end(); it++) {
            errs() << *it;
            if (it != types.end())
                errs() << ", ";
        }
    }

    void insert(string type) {
        types.insert(type);
        erasePtr();
    }

    void insert(TypeSet *other) {
        if (!other)
            return;

        for (auto it = other->types.begin(); it != other->types.end(); it++)
            types.insert(*it);
        erasePtr();
    }

    bool empty() { return types.empty(); }

    int count(string type) { return types.count(type); }

    bool hasPtr() { return types.count("ptr"); }

    bool isOpaque() { return types.size() == 1 && hasPtr(); }

    void erasePtr() {
        if (types.size() > 1 && types.count("ptr"))
            types.erase("ptr");
    }

    vector<string> getTypes() {
        vector<string> result;
        for (auto it = types.begin(); it != types.end(); it++)
            result.push_back(*it);
        return result;
    }

    typename set<string>::iterator begin() { return types.begin(); }
    typename set<string>::iterator end() { return types.end(); }

    bool isStructTy() {
        if (types.empty())
            return false;

        for (auto it = types.begin(); it != types.end(); it++) {
            if (it->find("struct") != string::npos)
                return true;
        }
        return false;
    }

    bool isIntegerTy() {
        if (types.empty())
            return false;
        for (auto it = types.begin(); it != types.end(); it++)
            if (it->find("int") != string::npos)
                return true;
        return false;
    }

    bool equalByStruct(TypeSet *other) {
        // outs() << "[DEBUG] reach equalByStruct\n";
        // outs() << "[DEBUG] this: ";
        // dump();
        // outs() << "\n[DEBUG] other: ";
        // other->dump();
        // outs() << "\n";
        
        if (!isStructTy() || !other->isStructTy())
            return false;


        for (auto it1 = types.begin(); it1 != types.end(); it1++)
            for (auto it2 = other->types.begin(); it2 != other->types.end(); it2++)
                if (it1->find("struct") != string::npos 
                    && it2->find("struct") != string:: npos 
                    && *it1 == *it2) {
                        outs() << "[DEBUG] equalByStruct: " << *it1 << " == " << *it2 << "\n";
                        return true;
                    }

        return false;
    }

    bool equalbyInteger(TypeSet *other) {
        // outs() << "[DEBUG] reach equalByInteger\n";
        // outs() << "[DEBUG] this: ";
        // dump();
        // outs() << "\n[DEBUG] other: ";
        // other->dump();
        // outs() << "\n----";

        if (!isIntegerTy() || !other->isIntegerTy())
            return false;

        for (auto it1 = types.begin(); it1 != types.end(); it1++)
            for (auto it2 = other->types.begin(); it2 != other->types.end(); it2++) {
                int size1 = integerSize(*it1);
                int size2 = integerSize(*it2);
                
                if (size1 == size2) {
                    outs() << "[DEBUG] equalByInteger: " << *it1 << " == " << *it2 << "\n";
                    return true;
                }
            }

        return false;
    }

    int integerSize(const string &ty) {
        if (ty == "i32")
            return 32;
        else if (ty == "i64")
            return 64;
        else if (ty == "i1")
            return 1;
        else if (ty == "i8")
            return 8;
        else if (ty == "i16")
            return 16;
        else if (ty == "i128")
            return 128;
        else if (ty == "int")
            return 32;
        else
            return 0;
    }

    bool isPointerTy() {
        if (types.empty())
            return false;
        for (auto it = types.begin(); it != types.end(); it++)
            if (*it == "ptr" || it->find("*") != string::npos)
                return true;
        return false;
    }

    bool equalByPointer(TypeSet *other) {
        // outs() << "[DEBUG] reach equalByPointer\n";
        // outs() << "[DEBUG] this: ";
        // dump();
        // outs() << "\n[DEBUG] other: ";
        // other->dump();
        // outs() << "\n";

        return isPointerTy() && other->isPointerTy();
    }
};