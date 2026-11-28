// DupliX 2 (design v5, 8.1): the code's syntax tree. The code is the graph written out: a call is a node, an
// argument a wire, a name a wire's name; `each` and `group` are the only blocks (and `if` inside `each`).
#pragma once

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace dx5 {

struct Pos {
    int line = 1, col = 1;
};
struct Diag {
    Pos pos;
    std::string msg;
};

struct Expr {
    enum Kind { Num, Str, Name, Member, MemberExpr, Call, Unary, Binary, Cond, Lambda };
    Kind kind = Num;
    Pos pos;
    // Num: the number as written; Str: the string; Name: the name; Member: the member's name; Call: the function's
    // name; Unary / Binary: the operator ("-", "!", "+", "**", "&&", ...); Lambda: its parameter's name
    std::string text;
    // Member: [base]; MemberExpr: [base, expr]; Unary: [a]; Binary: [a, b]; Cond: [c, a, b]; Lambda: [body];
    // Call: the positional arguments
    std::vector<std::unique_ptr<Expr>> args;
    std::vector<std::pair<std::string, std::unique_ptr<Expr>>> named;  // Call: the named arguments, in order
};

struct Param {
    std::string name;
    std::string type;  // "" (any), int, float, bool, vector, rotation, geometry
    bool structural = false;
    std::unique_ptr<Expr> dflt;
    Pos pos;
};

struct Stmt {
    enum Kind { Assign, Compound, ExprStmt, Each, If, Group, Return };
    Kind kind = ExprStmt;
    Pos pos;
    // Assign: the targets (1 or 2: `g_b, b = capture(...)`); Compound: [target]; Each: [output] or []; Group: [name]
    std::vector<std::string> names;
    std::string op;                // Compound: "+=", "-=", "*=", "/="
    std::unique_ptr<Expr> value;   // Assign / Compound / ExprStmt / Return: the value; If: the condition
    std::string input;             // Each: the geometry it runs over
    std::vector<Stmt> body;        // Each / If / Group
    std::vector<Stmt> elseBody;    // If
    std::vector<Param> params;     // Group
};

struct Program {
    std::vector<Stmt> stmts;
};

bool ParseProgram(const std::string& src, Program* out, Diag* err);
bool ParseExpr(const std::string& src, std::unique_ptr<Expr>* out, Diag* err);

// ---- the lexer (dx5_lex.cpp) ----
struct Token {
    enum Kind { Num, Str, Ident, Op, Newline, End };
    Kind kind = End;
    std::string text;
    Pos pos;
};
// Newlines inside ( ) are dropped (a call may span lines). `//` and `/* */` are comments.
bool Lex(const std::string& src, std::vector<Token>* out, Diag* err);

}  // namespace dx5
