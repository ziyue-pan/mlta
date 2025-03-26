#pragma once

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
    bool isFunc = false;

    ~TypeSet() { types.clear(); }

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

    void erase(string type) { types.erase(type); }

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

    bool isGenericPtr() { return types.size() == 1 && count("void*"); }

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

    bool equals(TypeSet *given) {
        for (auto it = given->begin(); it != given->end(); it++) {
            if (count(*it))
                return true;
        }

        return false;
    }

    bool equalsBase(TypeSet *given) {
        // ground truth
        for (auto it = given->begin(); it != given->end(); it++) {
            for (auto it2 = types.begin(); it2 != types.end(); it2++) {
                string type = *it2;
                while (type.back() == '*')
                    type.pop_back();
                if (type == *it)
                    return true;
            }
        }
        return false;
    }

    int size() { return types.size(); }

    string at(int index) { return *next(types.begin(), index); }

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

    bool isIntegerTy(const string &ty) {
        if (ty.find("i") == 0) {
            for (auto it2 = ty.begin() + 1; it2 != ty.end(); it2++)
                if (!isdigit(*it2))
                    break;
            return true;
        }

        return false;
    }

    bool isVoid() { return types.count("void"); }

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
            for (auto it2 = other->types.begin(); it2 != other->types.end(); it2++) {
                if (it1->find("struct") != string::npos 
                    && it2->find("struct") != string:: npos 
                    && *it1 == *it2) {
                        // trim ending *
                        string type1 = *it1;
                        while (type1.back() == '*')
                            type1.pop_back();
                        
                        if (type1 == "%struct.") {
                            continue;
                        }

                        outs() << "[DEBUG] equalByStruct: " << *it1 << " == " << *it2 << "\n";
                        return true;
                    }
        }

        return false;
    }

    bool equalByInteger(TypeSet *other) {
        // outs() << "[DEBUG] reach equalByInteger\n";
        // outs() << "[DEBUG] this: ";
        // dump();
        // outs() << "\n[DEBUG] other: ";
        // other->dump();
        // outs() << "\n----";

        for (auto it1 = types.begin(); it1 != types.end(); it1++)
            for (auto it2 = other->types.begin(); it2 != other->types.end(); it2++) {
                if (!isIntegerTy(*it1) || !isIntegerTy(*it2))
                    continue;

                int size1 = integerSize(*it1);
                int size2 = integerSize(*it2);
                
                if (size1 != 0 && size2 != 0 && size1 == size2) {
                    // outs() << "[DEBUG] equalByInteger: " << *it1 << " == " << *it2 << "\n";
                    return true;
                }

                if ((size1 == 32 && size2 == 64) || (size1 == 64 && size2 == 32)) {
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