#include <cstddef>
#include <llvm/ADT/APFloat.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Value.h>
#include <llvm/Passes/PassBuilder.h>
#include <memory>

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/IRBuilder.h"
#include <llvm/IR/PassManager.h>
#include "llvm/IR/Verifier.h"

#include <llvm/Analysis/LoopAnalysisManager.h>
#include <llvm/Analysis/CGSCCPassManager.h> 
#include <llvm/Passes/StandardInstrumentations.h>

#include <llvm/Transforms/Scalar/GVN.h>
#include <llvm/Transforms/InstCombine/InstCombine.h>
#include <llvm/Transforms/Scalar.h>
#include <llvm/Transforms/Scalar/Reassociate.h>
#include <llvm/Transforms/Scalar/SimplifyCFG.h>

#include <llvm/Target/TargetMachine.h>
#include <llvm/Support/TargetSelect.h>

#include "KaleidoscopeJIT.h"
#include <map>

#include "ast.h"
#include "lexer.h"
#include "codegen.h"
#include "parser.h"

using namespace llvm;
using namespace llvm::orc;
using namespace kale;

//===----------------------------------------------------------------------===//
// Code Generation
//===----------------------------------------------------------------------===//

std::unique_ptr<LLVMContext> TheContext;
std::unique_ptr<Module> TheModule;
std::unique_ptr<IRBuilder<>> Builder;
std::map<std::string, Value*> NamedValues;
std::unique_ptr<KaleidoscopeJIT> TheJIT;
std::unique_ptr<FunctionPassManager> TheFPM;
std::unique_ptr<LoopAnalysisManager> TheLAM;
std::unique_ptr<FunctionAnalysisManager> TheFAM;
std::unique_ptr<CGSCCAnalysisManager> TheCGAM;
std::unique_ptr<ModuleAnalysisManager> TheMAM;
std::unique_ptr<PassInstrumentationCallbacks> ThePIC;
std::unique_ptr<StandardInstrumentations> TheSI;
std::map<std::string, std::unique_ptr<PrototypeAST>> FunctionProtos;
ExitOnError ExitOnErr;

Value *LogErrorV(const char *Str) {
  LogError(Str);
  return nullptr;
}

Function *getFunction(std::string Name) {
  // First, see if the function has already been added to the current module.
  if(auto *F = TheModule->getFunction(Name))
    return F;

  // If not , check whether we can codegen the declaration from some existing prototype.
  auto FI = FunctionProtos.find(Name);
  if(FI !=  FunctionProtos.end()) {
    return FI->second->codegen();
  }
  return nullptr;
}

Value *NumberExprAST::codegen() {
  return ConstantFP::get(*TheContext, APFloat(Val));
}

Value *VariableExprAST::codegen() {
  Value *V = NamedValues[Name];
  if(!V) {
    LogErrorV("Unknown variable name");
  }
  return V;
}

Value *BinaryExprAST::codegen() {
  Value *L = LHS->codegen();
  Value *R = RHS->codegen();

  if(!L || !R)  {
    return nullptr;
  }

  switch(Op) {
    /// specify what instruction to create
    case '+': return Builder->CreateFAdd(L, R, "addtmp");
    case '-': return Builder->CreateFSub(L, R, "subtmp");
    case '*': return Builder->CreateFMul(L, R, "multmp");
    case '>': 
      L = Builder->CreateFCmpULT(L, R, "cmptmp");
      // unsigned int to float point UIToFP
      return Builder->CreateUIToFP(L, Type::getDoubleTy(*TheContext), "booltmp");
    default :
      return LogErrorV("invaild binary operator");
  }
}

Value *CallExprAST::codegen() {
  Function *CalleeF = TheModule->getFunction(Callee);

  if(!CalleeF) {
    return LogErrorV("Unknown function referenced");
  }

  if(CalleeF->arg_size() != Args.size()) {
    return LogErrorV("Incorrect # arguments passed");
  }

  std::vector<Value *> ArgsV;

  for(unsigned i = 0, e = Args.size(); i!=e ; ++i ) {
    ArgsV.push_back(Args[i]->codegen());
    if(!ArgsV.back()) {
      return nullptr;
    }
  }
  return Builder->CreateCall(CalleeF  , ArgsV, "calltmp");
}

Function *PrototypeAST::codegen() {
  std::vector<Type*> Doubles(Args.size(),
                             Type::getDoubleTy(*TheContext));

  FunctionType *FT =
    FunctionType::get(Type::getDoubleTy(*TheContext), Doubles, false);

  Function *F =
    Function::Create(FT, Function::ExternalLinkage, Name, TheModule.get());

  unsigned Idx = 0;
  for(auto &Arg : F->args()) {
    Arg.setName(Args[Idx++]);
  }
  return F;
}

Function *FunctionAST::codegen() {
  Function *TheFunction = TheModule->getFunction(Proto->getName());

  if(!TheFunction) {
    TheFunction = Proto->codegen();
  }
  
  if(!TheFunction) {
    return nullptr;
  }

  if(!TheFunction->empty()) {
    return (Function*)LogErrorV("Function cannot be redefined.");
  }

  BasicBlock *BB = BasicBlock::Create(*TheContext, "entry", TheFunction);
  Builder->SetInsertPoint(BB);

  NamedValues.clear();
  for(auto &Arg : TheFunction->args()) 
    NamedValues[std::string(Arg.getName())] = &Arg; 

  if(Value *RetVal = Body->codegen()) {
    Builder->CreateRet(RetVal);

    // Validate the generated code , checking for consistency
    verifyFunction(*TheFunction);

    // Run the optimizer on the function .
    TheFPM->run(*TheFunction, *TheFAM);

    return TheFunction;
  }

  TheFunction->eraseFromParent();
  return nullptr;
}

Value *IfExprAST::codegen() {
  Value *CondV = Cond->codegen();
  if(!CondV) {
    return nullptr;
  }

  // Convert condition to a bool by comparing non-equal to 0.0
  CondV = Builder->CreateFCmpONE(CondV, ConstantFP::get(*TheContext, APFloat(0.0)), "ifcond");

  Function *TheFunction = Builder->GetInsertBlock()->getParent();

  // Create blocks for the then ans else cases. Insert the 'then' block at the end of the function
  BasicBlock *ThenBB = 
    BasicBlock::Create(*TheContext, "then", TheFunction);
  BasicBlock *ElseBB = BasicBlock::Create(*TheContext, "else");
  BasicBlock *MergeBB = BasicBlock::Create(*TheContext, "ifcont");
  
  Builder->CreateCondBr(CondV, ThenBB, ElseBB);

  // Emit then value
  Builder->SetInsertPoint(ThenBB);

  Value *ThenV = Then->codegen();
  if(!Then) {
    return nullptr;
  }

  Builder->CreateBr(MergeBB);

  // Codegen of 'Then' can change the current block, update ThenBB for the PHI
  ThenBB = Builder->GetInsertBlock();

  // Emit else block.
  TheFunction->getBasicBlockList().push_back(ElseBB);
  Builder->SetInsertPoint(ElseBB);

  Value *ElseV = Else->codegen();
  if(!ElseV) 
    return nullptr;

  Builder->CreateBr(MergeBB);
  // codegen of 'Else' can change the current block, update ElseBB for the PHI
  ElseBB = Builder->GetInsertBlock();

  // emit merge block
  TheFunction->getBasicBlockList().push_back(MergeBB);
  Builder->SetInsertPoint(MergeBB);
  PHINode *PN = Builder->CreatePHI(Type::getDoubleTy(*TheContext), 2, "iftmp");

  PN->addIncoming(ThenV, ThenBB);
  PN->addIncoming(ElseV, ElseBB);
  return PN;
}

//===----------------------------------------------------------------------===//
// Top-Level parsing and JIT Drive
//===----------------------------------------------------------------------===//

void InitializeModuleAndManagers() {
  // Open a new context and module.
  TheContext = std::make_unique<LLVMContext>();
  TheModule = std::make_unique<Module>("my cool jit", *TheContext);
  TheModule->setDataLayout(TheJIT->getDataLayout());

  // Create a new builder for the module
  Builder = std::make_unique<IRBuilder<>>(*TheContext);
  
  // Create new pass and analysis managers.
   TheFPM = std::make_unique<FunctionPassManager>();
  TheLAM = std::make_unique<LoopAnalysisManager>();
  TheFAM = std::make_unique<FunctionAnalysisManager>();
  TheCGAM = std::make_unique<CGSCCAnalysisManager>();
  TheMAM = std::make_unique<ModuleAnalysisManager>();
  ThePIC = std::make_unique<PassInstrumentationCallbacks>();
  TheSI = std::make_unique<StandardInstrumentations>(true);

  TheSI->registerCallbacks(*ThePIC, TheFAM.get());

  // Add transform passes .
  // Do simple "peepholoe" optimizations and bit-twiddling optzns.
  TheFPM->addPass(InstCombinePass());
  // Reassociate expressions.
  TheFPM->addPass(ReassociatePass());
  // Eliminate Common SubExpressions.
  // need to create GVNPass to get The GVN pass
  TheFPM->addPass(GVN());
  // Simplify the control flow graph (deleting unreachable blocks, etc).
  TheFPM->addPass(SimplifyCFGPass());

  // Register analysis passes used in these transform passes.
  PassBuilder PB;
  PB.registerModuleAnalyses(*TheMAM);
  PB.registerFunctionAnalyses(*TheFAM);
  PB.crossRegisterProxies(*TheLAM, *TheFAM, *TheCGAM, *TheMAM);
}

void HandleDefinition() {
  if (auto FnAST = ParseDefinition()) {
    if(auto *FnIR = FnAST->codegen()) {
      fprintf(stderr, "Read function definition: \n");
      FnIR->print(errs());
      fprintf(stderr, "\n");
      ExitOnErr(TheJIT->addModule(
        ThreadSafeModule(std::move(TheModule), std::move(TheContext))
      ));
      InitializeModuleAndManagers();
    }
  } else {
    // Skip token for error recovery.
    getNextToken();
  }
}

void HandleExtern() {
  if (auto ProtoAST = ParseExtern()) {
    if(auto FnIR = ProtoAST->codegen()) {
      fprintf(stderr, "Read extern: \n");
      FnIR->print(errs());
      fprintf(stderr, "\n");
      FunctionProtos[ProtoAST->getName()] = std::move(ProtoAST);
    }
  } else {
    // Skip token for error recovery.
    getNextToken();
  }
}

void HandleTopLevelExpression() {
  // Evaluate a top-level expression into an anonymous function.
  if (auto FnAST = ParseTopLevelExpr()) {
    if( FnAST->codegen()) {
      // Create a ResourceTracker to track JIT'd memory allocated to our
      // anonymous expression -- that way we can free it after executing.
      auto RT = TheJIT->getMainJITDylib().createResourceTracker();

      auto TSM = ThreadSafeModule(std::move(TheModule), std::move(TheContext));
      ExitOnErr(TheJIT->addModule(std::move(TSM), RT));
      InitializeModuleAndManagers();

      // Search the JIT for the __anon_expr symbol.
      auto ExprSymbol = ExitOnErr(TheJIT->lookup("__anon_expr"));

      // Get the symbol's address and cast it to the right type (takes on arguments, returns a double)
      // so we can call it as a native function.
      auto addr = ExprSymbol.getAddress();
      double (*FP)() = reinterpret_cast<double (*)()>(addr);
      fprintf(stderr, "Evaluated to %f\n", FP());

      // Delete the ananymous expression module from the JIT.
      ExitOnErr(RT->remove());
    }
  } else {
    getNextToken();
  }
}