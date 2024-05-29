#ifndef __PARSER_H__
#define __PARSER_H__

#include "lexer.h"
#include "ast.h"

#include <map>
#include <memory>

namespace kale {
    int GetTokPrecedence();
    int getNextToken();

    /// LogError* - These are little helper functions for error handling.
    std::unique_ptr<ExprAST> LogError(const char *Str) ;

    std::unique_ptr<PrototypeAST> LogErrorP(const char *Str);



    std::unique_ptr<ExprAST> ParseExpression();

    /// numberexpr ::= number
    std::unique_ptr<ExprAST> ParseNumberExpr();

    /// parenexpr ::= '(' expression ')'
    std::unique_ptr<ExprAST> ParseParenExpr() ;

    /// identifierexpr
    ///   ::= identifier
    ///   ::= identifier '(' expression* ')'
    std::unique_ptr<ExprAST> ParseIdentifierExpr() ;

    /// primary
    ///   ::= identifierexpr
    ///   ::= numberexpr
    ///   ::= parenexpr
    std::unique_ptr<ExprAST> ParsePrimary() ;

    /// binoprhs
    ///   ::= ('+' primary)*
    std::unique_ptr<ExprAST> ParseBinOpRHS(int ExprPrec,
                                                std::unique_ptr<ExprAST> LHS) ;

    /// expression
    ///   ::= primary binoprhs
    ///
    std::unique_ptr<ExprAST> ParseExpression() ;

    /// prototype
    ///   ::= id '(' id* ')'
    std::unique_ptr<PrototypeAST> ParsePrototype() ;

    /// definition ::= 'def' prototype expression
    std::unique_ptr<FunctionAST> ParseDefinition() ;

    /// toplevelexpr ::= expression
    std::unique_ptr<FunctionAST> ParseTopLevelExpr() ;

    /// external ::= 'extern' prototype
    std::unique_ptr<PrototypeAST> ParseExtern() ;
}


#endif
