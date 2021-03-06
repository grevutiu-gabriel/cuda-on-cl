// Copyright Hugh Perkins 2016

// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at

//     http://www.apache.org/licenses/LICENSE-2.0

// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.


// This is going to patch the cuda launch instrutions, in the hostside ir. hopefully

#include "mutations.h"
#include "struct_clone.h"
// #include "ir-to-opencl-common.h"
#include "argparsecpp.h"
#include "type_dumper.h"
#include "GlobalNames.h"
#include "EasyCL/util/easycl_stringhelper.h"

#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/STLExtras.h"

#include "llvm/IR/AssemblyAnnotationWriter.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/LLVMContext.h"
// #include "llvm/IR/IRBuilder.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/ValueSymbolTable.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/DebugInfoMetadata.h"

#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_os_ostream.h"

#include "llvm/Transforms/Utils/BasicBlockUtils.h"

#include <cstdio>
#include <cstdlib>
#include <vector>
#include <map>
#include <set>
#include <stdexcept>
#include <memory>
#include <sstream>
#include <fstream>

using namespace llvm;
using namespace std;
using namespace cocl;

namespace cocl {

static llvm::LLVMContext context;
// static std::string sourcecode_stringname;
static std::string devicellcode_stringname;
static string devicellfilename;

static GlobalNames globalNames;
static TypeDumper typeDumper(&globalNames);
static StructCloner structCloner(&typeDumper, &globalNames);

// static string deviceclfilename;
// static string clfilenamesimple;

// bool single_precision = true;

class LaunchCallInfo {
public:
    LaunchCallInfo() {
        grid_xy_value = 0;
        grid_z_value = 0;
        block_xy_value = 0;
        block_z_value = 0;
    }
    std::string kernelName = "";
    vector<Type *> callTypes;
    vector<Value *> callValuesByValue;
    vector<Value *> callValuesAsPointers;
    Value *stream;
    Value *grid_xy_value;
    Value *grid_z_value;
    Value *block_xy_value;
    Value *block_z_value;
};

static unique_ptr<LaunchCallInfo> launchCallInfo(new LaunchCallInfo);

class GenericCallInst {
    // its children can hold a CallInst or an InvokeInst
public:
    // GenericCallInst() {}
    virtual ~GenericCallInst() {}
    static unique_ptr<GenericCallInst> create(InvokeInst *inst);
    static unique_ptr<GenericCallInst> create(CallInst *inst);
    virtual Value *getArgOperand(int idx) = 0;
    virtual Value *getOperand(int idx) = 0;
    virtual Module *getModule() = 0;
    virtual Instruction *getInst() = 0;
    virtual void dump() = 0;
};

class GenericCallInst_Call : public GenericCallInst {
public:
    GenericCallInst_Call(CallInst *inst) : inst(inst) {}
    CallInst *inst;
    virtual Value *getArgOperand(int idx) override {
        return inst->getArgOperand(idx);
    }
    virtual Value *getOperand(int idx) override {
        return inst->getArgOperand(idx);
    }
    virtual Module *getModule() {
        return inst->getModule();
    }
    virtual Instruction *getInst() {
        return inst;
    }
    virtual void dump() {
        inst->dump();
    }
};

class GenericCallInst_Invoke : public GenericCallInst {
public:
    GenericCallInst_Invoke(InvokeInst *inst) : inst(inst) {}
    InvokeInst *inst;
    virtual Value *getArgOperand(int idx) override {
        return inst->getArgOperand(idx);
    }
    virtual Value *getOperand(int idx) override {
        return inst->getArgOperand(idx);
    }
    virtual Module *getModule() {
        return inst->getModule();
    }
    virtual Instruction *getInst() {
        return inst;
    }
    virtual void dump() {
        inst->dump();
    }
};

unique_ptr<GenericCallInst> GenericCallInst::create(InvokeInst *inst) {
    return unique_ptr<GenericCallInst>(new GenericCallInst_Invoke(inst));
}
unique_ptr<GenericCallInst> GenericCallInst::create(CallInst *inst) {
    return unique_ptr<GenericCallInst>(new GenericCallInst_Call(inst));
}

ostream &operator<<(ostream &os, const LaunchCallInfo &info) {
    raw_os_ostream my_raw_os_ostream(os);
    my_raw_os_ostream << "LaunchCallInfo " << info.kernelName;
    my_raw_os_ostream << "<<<";

    my_raw_os_ostream << ">>>";
    my_raw_os_ostream << "(";
    int i = 0;
    for(auto it=info.callTypes.begin(); it != info.callTypes.end(); it++) {
        if(i > 0){
            my_raw_os_ostream << ", ";
        }
        Type *type = *it;
        type->print(my_raw_os_ostream);
        i++;
    }
    my_raw_os_ostream << ");\n";
    my_raw_os_ostream << "value types: ";
    i = 0;
    for(auto it=info.callValuesByValue.begin(); it != info.callValuesByValue.end(); it++) {
        Value *value = *it;
        if(i > 0) {
            my_raw_os_ostream << ", ";
        }
        my_raw_os_ostream << typeDumper.dumpType(value->getType());
        i ++;
    }
    return os;
}

void getLaunchTypes(GenericCallInst *inst, LaunchCallInfo *info) {
    // input to this is a cudaLaunch instruction
    // sideeffect is to populate in info:
    // - name of the kernel
    // - type of each of the kernel parameters (without the actual Value's)
    info->callTypes.clear();
    // outs() << "getLaunchTypes()\n";
    Value *argOperand = inst->getArgOperand(0);
    if(ConstantExpr *expr = dyn_cast<ConstantExpr>(argOperand)) {
        Instruction *instr = expr->getAsInstruction();
        Type *op0type = instr->getOperand(0)->getType();
        Type *op0typepointed = op0type->getPointerElementType();
        if(FunctionType *fn = dyn_cast<FunctionType>(op0typepointed)) {
            for(auto it=fn->param_begin(); it != fn->param_end(); it++) {
                Type * paramType = *it;
                info->callTypes.push_back(paramType);
            }
        }
        info->kernelName = instr->getOperand(0)->getName();
        // outs() << "got kernel name " << info->kernelName << "\n";
    } else {
        throw runtime_error("getlaunchtypes, didnt get ConstantExpr");
    }
}

void getLaunchArgValue(GenericCallInst *inst, LaunchCallInfo *info) {
    // input to this is:
    // - inst is cudaSetupArgument instruction, with:
    //   - first operand is a value pointing to the value we want to send to the kernel
    //
    // - output of this method is
    //    populate info with a Value holding the actual concrete value w ewant to send to the kernel
    //    (note a pointer to it, since we Load the pointer)
    // Notes:
    // - the first operand of inst was created as bitcast(i8*)(alloca (type-of-arg))
    // - the alloca instruction is inst->getOperand(0)->getOperand(0)
    // - so if we load from the alloca instruction, we should have the value we want?
    // outs() << "getLaunchArgValue " << "\n";
    if(!isa<Instruction>(inst->getOperand(0))) {
        outs() << "getlaunchvalue, first operatnd of inst is not an instruction..." << "\n";
        inst->dump();
        outs() << "\n";
        inst->getOperand(0)->dump();
        outs() << "\n";
        throw runtime_error("getlaunchvalue, first operatnd of inst is not an instruction...");
    }
    Instruction *bitcast = cast<Instruction>(inst->getOperand(0));
    Value *alloca = bitcast;
    if(isa<BitCastInst>(bitcast)) {
        alloca = bitcast->getOperand(0);
    } else {
        alloca = bitcast;
    }
    Instruction *load = new LoadInst(alloca, "loadCudaArg");
    load->insertBefore(inst->getInst());
    info->callValuesByValue.push_back(load);
    info->callValuesAsPointers.push_back(alloca);
}

ostream &operator<<(ostream &os, const PointerInfo &pointerInfo) {
    os << "PointerInfo(offset=" << pointerInfo.offset << ", type=" << typeDumper.dumpType(pointerInfo.type);
    os << " indices=";
    int i = 0;
    for(auto it=pointerInfo.indices.begin(); it != pointerInfo.indices.end(); it++) {
        if(i > 0) {
            os << ",";
        }
        os << *it;
        i++;
    }
    os << ")";
    return os;
}

Instruction *addSetKernelArgInst_int(Instruction *lastInst, Value *value, IntegerType *intType) {
    Module *M = lastInst->getModule();

    int bitLength = intType->getBitWidth();
    string mangledName = "";
    if(bitLength == 32) {
        mangledName = "_Z17setKernelArgInt32i";
    } else if(bitLength == 64) {
        #ifdef __APPLE__
        mangledName = "_Z17setKernelArgInt64x";
        #else
        mangledName = "_Z17setKernelArgInt64l";
        #endif
    } else if(bitLength == 8) {
        mangledName = "_Z16setKernelArgInt8c";
    } else {
        throw runtime_error("bitlength " + easycl::toString(bitLength) + " not implemented");
    }
    Function *setKernelArgInt = cast<Function>(M->getOrInsertFunction(
        mangledName,
        Type::getVoidTy(context),
        IntegerType::get(context, bitLength),
        NULL));
    CallInst *call = CallInst::Create(setKernelArgInt, value);
    call->insertAfter(lastInst);
    lastInst = call;
    return lastInst;
}

Instruction *addSetKernelArgInst_float(Instruction *lastInst, Value *value) {
    Module *M = lastInst->getModule();

    Function *setKernelArgFloat = cast<Function>(M->getOrInsertFunction(
        "_Z17setKernelArgFloatf",
        Type::getVoidTy(context),
        Type::getFloatTy(context),
        NULL));
    CallInst *call = CallInst::Create(setKernelArgFloat, value);
    call->insertAfter(lastInst);
    return call;
}

Instruction *addSetKernelArgInst_pointer(Instruction *lastInst, Value *value) {
    Module *M = lastInst->getModule();

    Type *elementType = value->getType()->getPointerElementType();
    // cout << "addSetKernelArgInst_pointer elementType:" << endl;
    // elementType->dump();
    // we can probably generalize these to all just send as a pointer to char
    // we'll need to cast them somehow first

    BitCastInst *bitcast = new BitCastInst(value, PointerType::get(IntegerType::get(context, 8), 0));
    bitcast->insertAfter(lastInst);
    lastInst = bitcast;

    const DataLayout *dataLayout = &M->getDataLayout();
    int allocSize = dataLayout->getTypeAllocSize(elementType);
    // cout << "allocsize " << allocSize << endl;
    int32_t elementSize = allocSize;

    Function *setKernelArgFloatStar = cast<Function>(M->getOrInsertFunction(
        "_Z20setKernelArgCharStarPci",
        Type::getVoidTy(context),
        PointerType::get(IntegerType::get(context, 8), 0),
        IntegerType::get(context, 32),
        NULL));
    Value *args[] = {bitcast, createInt32Constant(&context, elementSize)};
    CallInst *call = CallInst::Create(setKernelArgFloatStar, ArrayRef<Value *>(args));
    call->insertAfter(lastInst);
    lastInst = call;
    return lastInst;
}

Instruction *addSetKernelArgInst_pointerstruct(Instruction *lastInst, Value *value) {
    Module *M = lastInst->getModule();

    // outs() << "addSetKernelArgInst_pointerstruct()\n";
    // outs() << "value\n";
    // value->dump();
    Type *valueType = value->getType();
    // outs() << "valueType\n";
    // valueType->dump();
    StructType *structType = cast<StructType>(valueType->getPointerElementType());

    // outs() << "got a byvalue struct" << "\n";
    unique_ptr<StructInfo> structInfo(new StructInfo());
    StructCloner::walkStructType(M, structInfo.get(), 0, 0, vector<int>(), "", cast<StructType>(structType));

    bool structHasPointers = structInfo->pointerInfos.size() > 0;
    // outs() << "struct has pointers? " << structHasPointers << "\n";

    // if it doesnt contain pointers, we can just send it as a char *, after creating a pointer
    // to it
    // actually, we have a pointer already, ie valueAsPointerInstr
    if(!structHasPointers) {
        return addSetKernelArgInst_pointer(lastInst, value);
    }

    // StructType *structType = cast<StructType>(value->getType());
    string name = globalNames.getOrCreateName(structType);
    Type *newType = structCloner.cloneNoPointers(structType);

    const DataLayout *dataLayout = &M->getDataLayout();
    int allocSize = dataLayout->getTypeAllocSize(newType);
    // outs() << "original typeallocsize " << dataLayout->getTypeAllocSize(value->getType()) << "\n";
    // outs() << "pointerfree typeallocsize " << allocSize << "\n";

    Function *setKernelArgStruct = cast<Function>(M->getOrInsertFunction(
        "_Z18setKernelArgStructPci",
        Type::getVoidTy(context),
        PointerType::get(IntegerType::get(context, 8), 0),
        IntegerType::get(context, 32),
        NULL));

    AllocaInst *alloca = new AllocaInst(newType, "newalloca");
    alloca->insertAfter(lastInst);
    lastInst = alloca;

    lastInst = structCloner.createHostsideIrCopyPtrfullToNoptr(lastInst, structType, value, alloca);

    BitCastInst *bitcast = new BitCastInst(alloca, PointerType::get(IntegerType::get(context, 8), 0));
    bitcast->insertAfter(lastInst);
    lastInst = bitcast;

    Value *args[2];
    args[0] = bitcast;
    args[1] = createInt32Constant(&context, allocSize);

    CallInst *call = CallInst::Create(setKernelArgStruct, ArrayRef<Value *>(args));
    call->insertAfter(lastInst);
    lastInst = call;

    // outs() << "pointers in struct:" << "\n";
    for(auto pointerit=structInfo->pointerInfos.begin(); pointerit != structInfo->pointerInfos.end(); pointerit++) {
        PointerInfo *pointerInfo = pointerit->get();
        vector<Value *> indices;
        indices.push_back(createInt32Constant(&context, 0));
        for(auto idxit = pointerInfo->indices.begin(); idxit != pointerInfo->indices.end(); idxit++) {
            int idx = *idxit;
            // outs() << "idx " << idx << "\n";
            indices.push_back(createInt32Constant(&context, idx));
        }
        GetElementPtrInst *gep = GetElementPtrInst::CreateInBounds(structType, value, ArrayRef<Value *>(&indices[0], &indices[indices.size()]), "getfloatstaraddr");
        gep->insertAfter(lastInst);
        lastInst = gep;

        LoadInst *loadgep = new LoadInst(gep, "loadgep");
        loadgep->insertAfter(lastInst);
        lastInst = loadgep;

        lastInst = addSetKernelArgInst_pointer(lastInst, loadgep);
    }

    return lastInst;
}

Instruction *addSetKernelArgInst_byvaluestruct(Instruction *lastInst, Value *value, Value *valueAsPointerInstr) {
    Module *M = lastInst->getModule();

    // outs() << "got a byvalue struct" << "\n";
    unique_ptr<StructInfo> structInfo(new StructInfo());
    StructCloner::walkStructType(M, structInfo.get(), 0, 0, vector<int>(), "", cast<StructType>(value->getType()));

    bool structHasPointers = structInfo->pointerInfos.size() > 0;
    // outs() << "struct has pointers? " << structHasPointers << "\n";

    // if it doesnt contain pointers, we can just send it as a char *, after creating a pointer
    // to it
    // actually, we have a pointer already, ie valueAsPointerInstr
    if(!structHasPointers) {
        return addSetKernelArgInst_pointer(lastInst, valueAsPointerInstr);
    }

    StructType *structType = cast<StructType>(value->getType());
    string name = globalNames.getOrCreateName(structType);
    Type *newType = structCloner.cloneNoPointers(structType);

    const DataLayout *dataLayout = &M->getDataLayout();
    int allocSize = dataLayout->getTypeAllocSize(newType);
    // outs() << "original typeallocsize " << dataLayout->getTypeAllocSize(value->getType()) << "\n";
    // outs() << "pointerfree typeallocsize " << allocSize << "\n";

    Function *setKernelArgStruct = cast<Function>(M->getOrInsertFunction(
        "_Z18setKernelArgStructPci",
        Type::getVoidTy(context),
        PointerType::get(IntegerType::get(context, 8), 0),
        IntegerType::get(context, 32),
        NULL));

    AllocaInst *alloca = new AllocaInst(newType, "newalloca");
    alloca->insertAfter(lastInst);
    lastInst = alloca;

    lastInst = structCloner.createHostsideIrCopyPtrfullToNoptr(lastInst, structType, valueAsPointerInstr, alloca);

    BitCastInst *bitcast = new BitCastInst(alloca, PointerType::get(IntegerType::get(context, 8), 0));
    bitcast->insertAfter(lastInst);
    lastInst = bitcast;

    Value *args[2];
    args[0] = bitcast;
    args[1] = createInt32Constant(&context, allocSize);

    CallInst *call = CallInst::Create(setKernelArgStruct, ArrayRef<Value *>(args));
    call->insertAfter(lastInst);
    lastInst = call;

    // outs() << "pointers in struct:" << "\n";
    for(auto pointerit=structInfo->pointerInfos.begin(); pointerit != structInfo->pointerInfos.end(); pointerit++) {
        PointerInfo *pointerInfo = pointerit->get();
        vector<Value *> indices;
        indices.push_back(createInt32Constant(&context, 0));
        for(auto idxit = pointerInfo->indices.begin(); idxit != pointerInfo->indices.end(); idxit++) {
            int idx = *idxit;
            // outs() << "idx " << idx << "\n";
            indices.push_back(createInt32Constant(&context, idx));
        }
        GetElementPtrInst *gep = GetElementPtrInst::CreateInBounds(value->getType(), valueAsPointerInstr, ArrayRef<Value *>(&indices[0], &indices[indices.size()]), "getfloatstaraddr");
        gep->insertAfter(lastInst);
        lastInst = gep;

        LoadInst *loadgep = new LoadInst(gep, "loadgep");
        loadgep->insertAfter(lastInst);
        lastInst = loadgep;

        lastInst = addSetKernelArgInst_pointer(lastInst, loadgep);
    }
    return lastInst;
}

Instruction *addSetKernelArgInst(Instruction *lastInst, Value *value, Value *valueAsPointerInstr) {
    if(IntegerType *intType = dyn_cast<IntegerType>(value->getType())) {
        lastInst = addSetKernelArgInst_int(lastInst, value, intType);
    } else if(value->getType()->isFloatingPointTy()) {
        lastInst = addSetKernelArgInst_float(lastInst, value);
    } else if(value->getType()->isPointerTy()) {
        // cout << "pointer" << endl;
        Type *elementType = dyn_cast<PointerType>(value->getType())->getPointerElementType();
        if(isa<StructType>(elementType)) {
            // cout << "pointer to struct" << endl;
            lastInst = addSetKernelArgInst_pointerstruct(lastInst, value);
        } else {
            // cout << "pointer to non-struct" << endl;
            lastInst = addSetKernelArgInst_pointer(lastInst, value);
        }
    } else if(isa<StructType>(value->getType())) {
        // cout << "structtype" << endl;
        lastInst = addSetKernelArgInst_byvaluestruct(lastInst, value, valueAsPointerInstr);
    } else {
        value->dump();
        outs() << "\n";
        throw runtime_error("kernel arg type type not implemented " + typeDumper.dumpType(value->getType()));
    }
    return lastInst;
}

void patchCudaLaunch(Function *F, GenericCallInst *inst, vector<Instruction *> &to_replace_with_zero) {
    // outs() << "============\n";
    // outs() << "cudaLaunch\n";

    Module *M = inst->getModule();

    getLaunchTypes(inst, launchCallInfo.get());
    to_replace_with_zero.push_back(inst->getInst());
    outs() << "patching launch in " << string(F->getName()) << "\n";

    string kernelName = launchCallInfo->kernelName;
    Instruction *kernelNameValue = addStringInstr(M, "s_" + ::devicellcode_stringname + "_" + kernelName, kernelName);
    kernelNameValue->insertBefore(inst->getInst());

    // this isnt actually needed for running, but hopefully useful for debugging
    Instruction *llSourcecodeValue = addStringInstrExistingGlobal(M, devicellcode_stringname);
    llSourcecodeValue->insertBefore(inst->getInst());

    // Instruction *clSourcecodeValue = addStringInstrExistingGlobal(M, sourcecode_stringname);
    // clSourcecodeValue->insertBefore(inst);

    Function *configureKernel = cast<Function>(F->getParent()->getOrInsertFunction(
        "configureKernel",
        Type::getVoidTy(context),
        PointerType::get(IntegerType::get(context, 8), 0),
        PointerType::get(IntegerType::get(context, 8), 0),
        // PointerType::get(IntegerType::get(context, 8), 0),
        NULL));
    Value *args[] = {kernelNameValue, llSourcecodeValue};
    CallInst *callConfigureKernel = CallInst::Create(configureKernel, ArrayRef<Value *>(&args[0], &args[2]));
    callConfigureKernel->insertBefore(inst->getInst());
    Instruction *lastInst = callConfigureKernel;

    // pass args now
    int i = 0;
    for(auto argit=launchCallInfo->callValuesByValue.begin(); argit != launchCallInfo->callValuesByValue.end(); argit++) {
        Value *value = *argit;
        Value *valueAsPointerInstr = launchCallInfo->callValuesAsPointers[i];
        lastInst = addSetKernelArgInst(lastInst, value, valueAsPointerInstr);
        i++;
    }
    // trigger the kernel...
    Function *kernelGo = cast<Function>(F->getParent()->getOrInsertFunction(
        "_Z8kernelGov",
        Type::getVoidTy(context),
        NULL));
    CallInst *kernelGoInst = CallInst::Create(kernelGo);
    kernelGoInst->insertAfter(lastInst);
    lastInst = kernelGoInst;

    launchCallInfo->callValuesByValue.clear();
    launchCallInfo->callValuesAsPointers.clear();
    // launchCallInfo.reset(new LaunchCallInfo);
}

void patchFunction(Function *F) {
    bool is_main = (string(F->getName().str()) == "main");
    if(is_main) cout << "patching " << F->getName().str() << endl;    
    vector<Instruction *> to_replace_with_zero;
    IntegerType *inttype = IntegerType::get(context, 32);
    ConstantInt *constzero = ConstantInt::getSigned(inttype, 0);
    for(auto it=F->begin(); it != F->end(); it++) {
        BasicBlock *basicBlock = &*it;
        for(auto insit=basicBlock->begin(); insit != basicBlock->end(); insit++) {
            Instruction *inst = &*insit;
            if(!isa<CallInst>(inst) && !isa<InvokeInst>(inst)) {
                continue;
            }
            Function *called = 0;
            unique_ptr<GenericCallInst> genCallInst;
            if(CallInst *callInst = dyn_cast<CallInst>(inst)) {
                called = callInst->getCalledFunction();
                genCallInst = GenericCallInst::create(callInst);
            } else if(InvokeInst *callInst = dyn_cast<InvokeInst>(inst)) {
                called = callInst->getCalledFunction();
                genCallInst = GenericCallInst::create(callInst);
            }
            if(called == 0) {
                continue;
            }
            if(!called->hasName()) {
                continue;
            }
            string calledFunctionName = called->getName();
            // if(is_main && calledFunctionName.find("cuda") != string::npos) cout << "calledfunctionname " << calledFunctionName << endl;
            if(calledFunctionName == "cudaLaunch") {
                patchCudaLaunch(F, genCallInst.get(), to_replace_with_zero);
            } else if(calledFunctionName == "cudaSetupArgument") {
                getLaunchArgValue(genCallInst.get(), launchCallInfo.get());
                to_replace_with_zero.push_back(inst);
            }
        }
    }
    for(auto it=to_replace_with_zero.begin(); it != to_replace_with_zero.end(); it++) {
        Instruction *inst = *it;
        BasicBlock::iterator ii(inst);
        if(InvokeInst *invoke = dyn_cast<InvokeInst>(inst)) {
            // need to add an uncondtioinal branch, after the old invoke locaiton
            // cout << "replacing an invoke, need to patch in a branch" << endl;
            BasicBlock *oldTarget = invoke->getNormalDest();
            BranchInst *branch = BranchInst::Create(oldTarget);
            branch->insertAfter(inst);
        }
        // AllocaInst *alloca = new AllocaInst(IntegerType::get(context, 32));
        // alloca->insertBefore(inst);
        // StoreInst *store = new StoreInst(constzero, alloca);
        // store->insertBefore(inst);
        // LoadInst *load = new LoadInst(alloca);
        // load->insertBefore(inst);
        // ReplaceInstWithValue(inst->getParent()->getInstList(), ii, load);
        ReplaceInstWithValue(inst->getParent()->getInstList(), ii, constzero);
    }
}

string getBasename(string path) {
    // grab anything after final / ,or whole string
    size_t slash_pos = path.rfind('/');
    if(slash_pos == string::npos) {
        return path;
    }
    return path.substr(slash_pos + 1);
}

void patchModule(Module *M) {
    // ifstream f_incl(::deviceclfilename);
    // string cl_sourcecode(
    //     (std::istreambuf_iterator<char>(f_incl)),
    //     (std::istreambuf_iterator<char>()));

    ifstream f_inll(::devicellfilename);
    string devicell_sourcecode(
        (std::istreambuf_iterator<char>(f_inll)),
        (std::istreambuf_iterator<char>()));

    // ::sourcecode_stringname = "__opencl_sourcecode" + ::deviceclfilename;
    ::devicellcode_stringname = "__devicell_sourcecode" + ::devicellfilename;

    // addGlobalVariable(M, sourcecode_stringname, cl_sourcecode);
    addGlobalVariable(M, devicellcode_stringname, devicell_sourcecode);

    vector<Function *> functionsToRemove;
    for(auto it = M->begin(); it != M->end(); it++) {
        Function *F = &*it;
        string name = F->getName();
            patchFunction(F);
            verifyFunction(*F);
    }
}

} // namespace cocl

int main(int argc, char *argv[]) {
    SMDiagnostic smDiagnostic;
    argparsecpp::ArgumentParser parser;

    // string devicellfilename;
    string rawhostfilename;
    string patchedhostfilename;

    parser.add_string_argument("--hostrawfile", &rawhostfilename)->required()->help("input file");
    // parser.add_string_argument("--deviceclfile", &::deviceclfilename)->required()->help("input file");
    parser.add_string_argument("--devicellfile", &::devicellfilename)->required()->help("input file");
    parser.add_string_argument("--hostpatchedfile", &patchedhostfilename)->required()->help("output file");
    if(!parser.parse_args(argc, argv)) {
        return -1;
    }

    std::unique_ptr<llvm::Module> module = parseIRFile(rawhostfilename, smDiagnostic, context);
    if(!module) {
        smDiagnostic.print(argv[0], errs());
        return 1;
    }

    try {
        patchModule(module.get());
    } catch(const runtime_error &e) {
        outs() << "exception whilst doing:\n";
        outs() << "reading rawhost ll file " << rawhostfilename << "\n";
        // outs() << "reading device cl file " << deviceclfilename << "\n";
        outs() << "outputing to hostpatched file " << patchedhostfilename << "\n";
        throw e;
    }

    AssemblyAnnotationWriter assemblyAnnotationWriter;
    ofstream ofile;
    ofile.open(patchedhostfilename);
    raw_os_ostream my_raw_os_ostream(ofile);
    verifyModule(*module);
    module->print(my_raw_os_ostream, &assemblyAnnotationWriter);
    ofile.close();
    return 0;
}
