#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/Value.h"
#include "llvm/IR/Verifier.h"

#include <llvm/ExecutionEngine/Orc/ExecutionUtils.h>
#include <llvm/ExecutionEngine/Orc/ThreadSafeModule.h>
#include <llvm/IR/PassInstrumentation.h>
#include <llvm/IR/PassManager.h>
#include <llvm/Support/Error.h>

#include <llvm/IR/PassManager.h>
#include <llvm/Analysis/LoopAnalysisManager.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Passes/StandardInstrumentations.h>
#include <llvm/Pass.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Support/TargetSelect.h>

#include <llvm/Transforms/InstCombine/InstCombine.h>
#include <llvm/Transforms/Scalar.h>
#include <llvm/Transforms/Scalar/Reassociate.h>
#include <llvm/Transforms/Scalar/GVN.h>
#include <llvm/Transforms/Scalar/SimplifyCFG.h>

#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include <cctype>
#include <cstdio>
#include <cstdlib>

#include "parser.h"
#include "lexer.h"
#include "KaleidoscopeJIT.h"

using namespace llvm;
using namespace llvm::orc;
using namespace kale;

extern int CurTok;
extern std::map<char, int> BinopPrecedence;

//===----------------------------------------------------------------------===//
// Code Generation
//===----------------------------------------------------------------------===//

static std::unique_ptr<LLVMContext> TheContext;
static std::unique_ptr<Module> TheModule;
static std::unique_ptr<IRBuilder<>> Builder;
static std::map<std::string, Value*> NamedValues;

static std::unique_ptr<KaleidoscopeJIT> TheJIT;
static std::unique_ptr<FunctionPassManager> TheFPM;
static std::unique_ptr<LoopAnalysisManager> TheLAM;
static std::unique_ptr<FunctionAnalysisManager> TheFAM;
static std::unique_ptr<CGSCCAnalysisManager> TheCGAM;
static std::unique_ptr<ModuleAnalysisManager> TheMAM;
static std::unique_ptr<PassInstrumentationCallbacks> ThePIC;
static std::unique_ptr<StandardInstrumentations> TheSI;
static std::map<std::string, std::unique_ptr<PrototypeAST>> FunctionProtos;
static ExitOnError ExitOnErr;

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

//===----------------------------------------------------------------------===//
// Top-Level parsing and JIT Drive
//===----------------------------------------------------------------------===//

static void InitializeModuleAndManagers() {
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

static void HandleDefinition() {
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

static void HandleExtern() {
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

static void HandleTopLevelExpression() {
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

/// top ::= definition | external | expression | ';'
static void MainLoop() {
  while (true) {
    fprintf(stderr, "ready> ");
    switch (CurTok) {
    case tok_eof:
      return;
    case ';': // ignore top-level semicolons.
      getNextToken();
      break;
    case tok_def:
      HandleDefinition();
      break;
    case tok_extern:
      HandleExtern();
      break;
    default:
      HandleTopLevelExpression();
      break;
    }
  }
}

//===----------------------------------------------------------------------===//
// "Library" functions that can be "extern'd" from user code.
//===----------------------------------------------------------------------===//

#ifdef _WIN32
#define DLLEXPORT __declspec(dllexport)
#else
#define DLLEXPORT
#endif

/// putchard - putchar that takes a double and returns 0.
extern "C" DLLEXPORT double putchard(double X) {
  fputc((char)X, stderr);
  return 0;
}

/// printd - printf that takes a double prints it as "%f\n", returning 0.
extern "C" DLLEXPORT double printd(double X) {
  fprintf(stderr, "%f\n", X);
  return 0;
}

//===----------------------------------------------------------------------===//
// Main driver code.
//===----------------------------------------------------------------------===//

int main() {
  InitializeNativeTarget();
  InitializeNativeTargetAsmPrinter();
  InitializeNativeTargetAsmParser();
  // Install standard binary operators.
  // 1 is lowest precedence.
  BinopPrecedence['<'] = 10;
  BinopPrecedence['+'] = 20;
  BinopPrecedence['-'] = 20;
  BinopPrecedence['*'] = 40; // highest.

  // Prime the first token.
  fprintf(stderr, "ready> ");
  getNextToken();

  TheJIT = ExitOnErr(KaleidoscopeJIT::Create());

  InitializeModuleAndManagers();

  // Run the main "interpreter loop" now.
  MainLoop();

  TheModule->print(errs(), nullptr);

  return 0;
}
