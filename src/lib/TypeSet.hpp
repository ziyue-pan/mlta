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
        return __equalsLinux(given);
    }

    bool __equalsLinux(TypeSet *given) {
        if (given->size() == 1 && given->at(0) == "%struct.list_head") {
            return true;
        }

        if (given->size() == 1 && given->at(0) == "%struct._Bool") {
            for (auto it = types.begin(); it != types.end(); it++) {
                string type = *it;
                while (type.back() == '*')
                    type.pop_back();
                if (type == "i1")
                    return true;
            }
        }

        if (given->size() == 1 && given->at(0) == "i32") {
            for (auto it = types.begin(); it != types.end(); it++) {
                string type = *it;
                while (type.back() == '*')
                    type.pop_back();
                if (type == "%struct.")
                    return true;
                if (type == "%struct.seqcount_spinlock" || type == "%struct.seqcount")
                    return true;
            }
        }

        if (given->size() == 1 && given->at(0) == "i64") {
            for (auto it = types.begin(); it != types.end(); it++) {
                string type = *it;
                while (type.back() == '*')
                    type.pop_back();
                if (type == "i32")
                    return true;
                if (type == "%struct.")
                    return true;
                if (type == "void")
                    return true;
                if (type == "%struct.boot_params_to_save")
                    return true;
            }
        }

        if (given->size() == 1 &&
            (given->at(0) == "i32" || given->at(0) == "i64")) {
            for (auto it = types.begin(); it != types.end(); it++) {
                if (it->find("union.") != string::npos)
                    return true;
                if (*it == "void*")
                    return true;
            }
        }

        if (given->size() == 1 && given->at(0) == "%struct.dentry") {
            return true;
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

                        return true;
                    }
        }

        return false;
    }

    bool equalByInteger(TypeSet *other) {
        for (auto it1 = types.begin(); it1 != types.end(); it1++)
            for (auto it2 = other->types.begin(); it2 != other->types.end(); it2++) {
                if (!isIntegerTy(*it1) || !isIntegerTy(*it2))
                    continue;

                int size1 = integerSize(*it1);
                int size2 = integerSize(*it2);
                
                if (size1 != 0 && size2 != 0 && size1 == size2) {
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
        return isPointerTy() && other->isPointerTy();
    }
};