#include "llvm/Pass.h"
#include "llvm/Function.h"
#include "llvm/Module.h"
#include "llvm/Instructions.h"
#include "llvm/GlobalAlias.h"
#include "llvm/GlobalVariable.h"
#include "llvm/Constants.h"
#include "llvm/Analysis/Verifier.h"
#include "llvm/DefaultPasses.h"
#include "ObjectiveCOpts.h"
#include <string>

using namespace GNUstep;
using namespace llvm;
using std::string;

namespace {

  class GNUNonfragileIvarPass : public FunctionPass {

    public:
    static char ID;
    GNUNonfragileIvarPass() : FunctionPass(ID) {}

    Module *M;
    size_t PointerSize;
    virtual bool doInitialization(Module &Mod) {
      M = &Mod;
      PointerSize = 8;
      if (M->getPointerSize() == Module::Pointer32) 
        PointerSize = 4;
      return false;  
    }

    std::string getSuperName(Constant *ClsStruct) {
      User *super = cast<User>(ClsStruct->getOperand(1));
      if (isa<ConstantPointerNull>(super)) return "";
      GlobalVariable *name = cast<GlobalVariable>(super->getOperand(0));
      return cast<ConstantArray>(name->getInitializer())->getAsString();
    }

    size_t sizeOfClass(const std::string &className) {
      // This is a root class
      if ("" == className) { return 0; }
      // These root classes are assumed to only have one ivar: isa
      if (className.compare(0, 8, "NSObject") == 0 || 
          className.compare(0, 6, "Object") == 0) {
        return PointerSize;
      }
      GlobalVariable *Cls = M->getGlobalVariable("_OBJC_CLASS_" + className);
      if (!Cls) return 0;
      Constant *ClsStruct = Cls->getInitializer();
      // Size is initialized to be negative.
      ConstantInt *Size = cast<ConstantInt>(ClsStruct->getOperand(5));
      return sizeOfClass(getSuperName(ClsStruct)) - Size->getSExtValue();
    }

    size_t hardCodedOffset(const StringRef &className, 
                           const StringRef &ivarName) {
      GlobalVariable *Cls = M->getGlobalVariable(("_OBJC_CLASS_" + className).str(), true);
      if (!Cls) return 0;
      Constant *ClsStruct = Cls->getInitializer();
      size_t superSize = sizeOfClass(getSuperName(ClsStruct));
      if (!superSize) return 0;
      ConstantStruct *IvarStruct = cast<ConstantStruct>(
          cast<GlobalVariable>(ClsStruct->getOperand(6))->getInitializer());
      int ivarCount = cast<ConstantInt>(IvarStruct->getOperand(0))->getSExtValue();
      Constant *ivars = IvarStruct->getOperand(1);
      for (int i=0 ; i<ivarCount ; i++) {
        Constant *ivar = cast<Constant>(ivars->getOperand(i));
        GlobalVariable *name =
          cast<GlobalVariable>(
              cast<User>(ivar->getOperand(0))->getOperand(0));
        std::string ivarNameStr = 
          cast<ConstantArray>(name->getInitializer())->getAsString();
        if (ivarNameStr.compare(0, ivarName.size(), ivarName.str()) == 0)
          return superSize +
            cast<ConstantInt>(ivar->getOperand(2))->getSExtValue();
      }
      return 0;
    }

    virtual bool runOnFunction(Function &F) {
      bool modified = false;
      typedef std::pair<Instruction*, Value*> Replacement;
      llvm::SmallVector<Replacement, 16> replacements;
      //llvm::cerr << "IvarPass: " << F.getName() << "\n";
      for (Function::iterator i=F.begin(), end=F.end() ;
          i != end ; ++i) {
        for (BasicBlock::iterator b=i->begin(), last=i->end() ;
            b != last ; ++b) {
          if (LoadInst *indirectload = dyn_cast<LoadInst>(b)) {
            if (LoadInst *load = dyn_cast<LoadInst>(indirectload->getOperand(0))) {
              if (GlobalVariable *ivar =
                  dyn_cast<GlobalVariable>(load->getOperand(0))) {
                StringRef variableName = ivar->getName();

                if (!variableName.startswith("__objc_ivar_offset_")) break;

                static size_t prefixLength = strlen("__objc_ivar_offset_");

                StringRef suffix = variableName.substr(prefixLength,
                    variableName.size()-prefixLength);

                std::pair<StringRef,StringRef> parts = suffix.split('.');
                StringRef className = parts.first;
                StringRef ivarName = parts.second;

                // If the class, and all superclasses, are visible in this module
                // then we can hard-code the ivar offset
                if (size_t offset = hardCodedOffset(className, ivarName)) {
                  replacements.push_back(Replacement(load, 0));
                  replacements.push_back(Replacement(indirectload,
                              ConstantInt::get(indirectload->getType(), offset)));
                  modified = true;
                } else {
                  // If the class was compiled with the new ABI, then we have a
                  // direct offset variable that we can use
                  if (Value *offset = M->getGlobalVariable(
                              ("__objc_ivar_offset_value_" + suffix).str())) {
                    replacements.push_back(Replacement(load, offset));
                    modified = true;
                  }
                }
              }
            }
          }
        }
      }
      for (SmallVector<Replacement, 16>::iterator i=replacements.begin(),
              end=replacements.end() ; i != end ; ++i) {
        if (i->second) 
          i->first->replaceAllUsesWith(i->second);
        i->first->removeFromParent();
      }
      verifyFunction(F);
      return modified;
    }
  };

  char GNUNonfragileIvarPass::ID = 0;
  RegisterPass<GNUNonfragileIvarPass> X("gnu-nonfragile-ivar", "Ivar fragility pass");
#if LLVM_MAJOR > 2
  StandardPass::RegisterStandardPass<GNUNonfragileIvarPass> D(
        StandardPass::Module, &DefaultStandardPasses::LoopUnrollID,
        StandardPass::OptimzationFlags(1), &NonfragileIvarID);
  StandardPass::RegisterStandardPass<GNUNonfragileIvarPass> L(StandardPass::LTO,
      &DefaultStandardPasses::JumpThreadingID,
      StandardPass::OptimzationFlags(0), &NonfragileIvarID);
#endif
}

FunctionPass *createGNUNonfragileIvarPass(void)
{
  return new GNUNonfragileIvarPass();
}
