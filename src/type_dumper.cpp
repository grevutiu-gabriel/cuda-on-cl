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

#include "type_dumper.h"

#include "mutations.h"
#include "EasyCL/util/easycl_stringhelper.h"

#include "llvm/IR/Instructions.h"
#include "llvm/Support/raw_ostream.h"
// #include "llvm/IR/Type.h"
// #include "llvm/IR/FunctionType.h"

#include <iostream>
#include <set>

using namespace std;
using namespace llvm;
using namespace cocl;

namespace cocl {

std::string TypeDumper::dumpAddressSpace(llvm::Type *type) {
    if(PointerType *ptr = dyn_cast<PointerType>(type)) {
        int addressspace = ptr->getAddressSpace();
        switch(addressspace) {
            case 0:
                return "";
            case 1:
                return "global";
            case 3:
                return "local";
            default:
                throw runtime_error("not implemented, addressspace " + easycl::toString(addressspace));
        }
    } else {
        return "";
    }
}

std::string TypeDumper::dumpPointerType(PointerType *ptr, bool decayArraysToPointer) {
    string gencode = "";
    Type *elementType = ptr->getPointerElementType();
    string elementTypeString = dumpType(elementType, decayArraysToPointer);
    int addressspace = ptr->getAddressSpace();
    string addressspacestr = "";
    if(addressspace == 1) {
        addressspacestr = "global";
    }
    if(addressspace == 3) {
        addressspacestr = "local";
    }
    if(addressspace == 4) {
        addressspacestr = "constant";
    }
    // we're just going to hackily assume that anything that is `global **` should be `global * global *`
    // if(isa<PointerType>(ptr->getPointerElementType())) {
        // return "global " + elementTypeString + addressspacestr + " *";
        // return elementTypeString + addressspacestr + " *";
        // return "global " + elementTypeString + addressspacestr + " *";
    // }
    if(addressspacestr != "") {
        gencode += addressspacestr + " ";
    }
    gencode += elementTypeString + "*";
    return gencode;
}

std::string TypeDumper::dumpIntegerType(IntegerType *type) {
    switch(type->getPrimitiveSizeInBits()) {
        case 32:
            return "int";
        case 64:
            return "long";
        case 16:
            return "short";
        case 8:
            return "char";
        case 1:
            return "bool";
        default:
            outs() << "integer size " << type->getPrimitiveSizeInBits() << "\n";
            throw runtime_error("unrecognized size");
    }
}

std::string TypeDumper::addStructToGlobalNames(StructType *type) {
    // outs() << "dumpstructtype" << "\n";
    if(globalNames->hasName(type)) {
        return globalNames->getName(type);
    }
    // cout << "typedumper:;addstructtoglobalnames" << endl;
    // type->dump();
    if(type->hasName()) {
        string name = type->getName();
        // outs() << "name " << name << "\n";
        name = easycl::replaceGlobal(name, ".", "_");
        name = easycl::replaceGlobal(name, ":", "_");
        // cout << "typedumper::dumpstructtype, name=" << name << endl;
        if(name == "struct_float4") {
            name = "float4";
            name = globalNames->getOrCreateName(type, name);
            return "float4";
        } else {
            if(name.find("struct_") == 0 || name.find("struct ") == 0) {
                name[6] = ' ';
                name = globalNames->getOrCreateName(type, name);
                // structsToDefine[type] = name;
                structsToDefine.insert(type);
                return name;
            } else if(name.find("class_") != string::npos) {
                name = "struct " + name;
                name = globalNames->getOrCreateName(type, name);
                // structsToDefine[type] = name;
                structsToDefine.insert(type);
                return name;
            } else {
                name = "struct " + name;
                name = globalNames->getOrCreateName(type, name);
                structsToDefine.insert(type);
                // structsToDefine[type] = name;
                return name;
            }
        }
    } else {
        type->dump();
        throw runtime_error("not implemented: anonymous struct types");
    }
}

std::string TypeDumper::dumpStructType(StructType *type) {
    return addStructToGlobalNames(type);
}

std::string TypeDumper::dumpArrayType(ArrayType *type, bool decayArraysToPointer) {
    int length = type->getNumElements();
    Type *elementType = type->getElementType();
    if(decayArraysToPointer) {
        return dumpType(elementType, decayArraysToPointer) + "*";
    } else {
        return dumpType(elementType, decayArraysToPointer) + "[" + easycl::toString(length) + "]";
    }
}

std::string TypeDumper::dumpFunctionType(FunctionType *fn) {
    // throw runtime_error("not implemented");
    // // outs() << "function" << "\n";
    std::string params_str = "";
    int i = 0;
    for(auto it=fn->param_begin(); it != fn->param_end(); it++) {
        Type * paramType = *it;
        if(i > 0) {
            params_str += ", ";
        }
        params_str += dumpType(paramType);
        i++;
    }
    // outs() << "params_str " << params_str << "\n";
    return params_str;
}

std::string TypeDumper::dumpType(Type *type, bool decayArraysToPointer) {
    Type::TypeID typeID = type->getTypeID();
    switch(typeID) {
        case Type::VoidTyID:
            return "void";
        case Type::FloatTyID:
            return "float";
        // case Type::UnionTyID:
        //     throw runtime_error("not implemented: union type");
        case Type::StructTyID:
            return dumpStructType(cast<StructType>(type));
        case Type::VectorTyID:
            throw runtime_error("not implemented: vector type");
        case Type::ArrayTyID:
            return dumpArrayType(cast<ArrayType>(type), decayArraysToPointer);
        case Type::DoubleTyID:
            if(forceSingle) {
                return "float";
            }
            return "double";
        case Type::FunctionTyID:
            return dumpFunctionType(cast<FunctionType>(type));
        case Type::PointerTyID:
            return dumpPointerType(cast<PointerType>(type), decayArraysToPointer);
        case Type::IntegerTyID:
            return dumpIntegerType(cast<IntegerType>(type));
        default:
            outs() << "type id " << typeID << "\n";
            throw runtime_error("unrecognized type");
    }
}

std::string TypeDumper::dumpStructDefinition(StructType *type, string name) {
    std::string declaration = "";
    // structDeclarations.insert(name);
    // cout << "dumping struct " << name << endl;
    declaration += name + " {\n";
    int i = 0;
    for(auto it=type->element_begin(); it != type->element_end(); it++) {
        Type *elementType = *it;
        bool isPtrPtrFunction = false;
        if(PointerType *ptr = dyn_cast<PointerType>(elementType)) {
            Type *ptrElementType = ptr->getPointerElementType();
            if(PointerType *ptrptr = dyn_cast<PointerType>(ptrElementType)) {
                Type *ptrptrElementType = ptrptr->getPointerElementType();
                isPtrPtrFunction = isa<FunctionType>(ptrptrElementType);
            }
        }
        if(isPtrPtrFunction) {
            // we can ignore functions, whilst dumping structs
            continue;
        }
        std::string memberName = "f" + easycl::toString(i);
        if(ArrayType *arraytype = dyn_cast<ArrayType>(elementType)) {
            Type *arrayelementtype = arraytype->getPointerElementType();
            // outs() << "arrayelementtype " << dumpType(arrayelementtype) << "\n";
            int numElements = arraytype->getNumElements();
            // outs() << "numelements " << numElements << "\n";
            declaration += "    " + dumpType(arrayelementtype) + " ";
            declaration += memberName + "[" + easycl::toString(numElements) + "];\n";
            // throw runtime_error("not implemented declarestruct for arraytype elements");
        } else {
            declaration += "    ";
            // if its a pointer, lets assume its global, for now
            if(isa<PointerType>(elementType)) {
                // updateAddressSpace(ptr, 1);
                declaration += "global ";

            }
            declaration += dumpType(elementType) + " " + memberName + ";\n";
        }
        i++;
    }
    declaration += "};\n";

    return declaration;
}

std::string TypeDumper::dumpStructDefinitions() {
    string gencode = "";
    set<StructType *>dumped;
    while(dumped.size() < structsToDefine.size()) {
        for(auto it=structsToDefine.begin(); it != structsToDefine.end(); it++) {
            StructType *structType = *it;
            if(dumped.find(structType) != dumped.end()) {
                continue;
            }
            // cout << structType->getName().str() << endl;
            // check if we already defined its members
            bool dumpable = true;
            // cout << "    dumping elements of " << structType->getName().str() << endl;
            // cout << "    num elements " << structType->getNumElements() << endl;
            for(auto it2=structType->element_begin(); it2 != structType->element_end(); it2++) {
                // Type *structElementType = *it2;
                // (*it2)->dump();
                // outs << "\n";
                Type *subType = (*it2);
                // cout << "array type? " << dyn_cast<ArrayType>(*it2) << endl;
                if(ArrayType *elementArrayType = dyn_cast<ArrayType>(subType)) {
                    subType = elementArrayType->getElementType();
                }
                if(StructType *elementStructType = dyn_cast<StructType>(subType)) {
                    if(dumped.find(elementStructType) == dumped.end()) {
                        // cout << "    needs: " << elementStructType->getName().str() << endl;
                        // cout << "    adding to structs to define" << endl;
                        // elementStructType->dump();
                        // if(elementStructType->getName().str() == "") {
                        //     throw runtime_error("anonymous struct");
                        // }
                        structsToDefine.insert(elementStructType);
                        dumpable = false;
                        break;
                    } else {
                        // cout << "    ok: " << elementStructType->getName().str() << endl;
                    }
                }
            }
            if(!dumpable) {
                // cout << "    skip" << std::endl;
                continue;
            }
            dumped.insert(structType);
            addStructToGlobalNames(structType);
            // cout << "    dump" << endl;
            gencode += dumpStructDefinition(structType, globalNames->getName(structType));
        }
    }
    // cout << "all structs dumped" << endl;
    return gencode;
}

// void TypeDumper::dumpStructDeclarations(std::ostream &os) {
//     for(auto it=structDeclarations.begin(); it != structDeclarations.end(); it++) {
//         os << *it << ";\n";
//     }
// }

} // namespace cocl;
