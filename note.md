# Kaleidoscope

## Lexer

### Tokens
* EOF
* def
* extern
* identifier
* number

### Get tokens
* 遇到空格跳过
* 使用两个静态变量存值
  * IdentifierStr ： 
  * NumVal

## Abstract Syntax Tree
* ExprAST
  * NumberExprAST
  * VariableExprAST
  * BinaryExprAST
  * CallExprAST
* PrototypeAST
* FunctionAST

##  Parser

## Code Generatotr

### Binary Operator
1. Recursively emit code for the left-hand side of the expression ,then the right-hand side , then compute the result of the expresion.
2. specify what instruction to create according to the opcode
    * In  kaleidoscope there  is only one type **double**
    * LLVM instructions are constrained by strict rules:
        1. left-hand side and right-hand side must have the same type
        2. result type must match the oper  and types
3. For compare operator