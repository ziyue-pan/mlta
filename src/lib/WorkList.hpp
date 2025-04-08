#pragma once

#include <llvm/IR/Instruction.h>
#include <llvm/IR/Module.h>

#include <queue>
#include <unordered_set>

using namespace std;
using namespace llvm;

class WorkList {
  private:
    queue<Instruction *> worklist;
    unordered_set<Instruction *> visited;

  public:
    WorkList(Module *module) {
        for (auto &func : *module) {
            for (auto &bb : func) {
                for (auto &inst : bb) {
                    worklist.push(&inst);
                }
            }
        }
    }

    void push(Instruction *inst) { 
        if (visited.count(inst)) {
            return;
        }

        visited.insert(inst);
        worklist.push(inst); 
    }

    void push_user(Value *value) {
        return;
        for (auto user : value->users()) {
            if (auto inst = dyn_cast<Instruction>(user)) {
                worklist.push(inst);
            } 
        }
    }

    Instruction *pop() {
        auto inst = worklist.front();
        worklist.pop();
        visited.erase(inst);
        return inst;
    }

    bool empty() { return worklist.empty(); }
};