// Task 4: the lexer and the parser (code -> syntax tree).
#include <string>

#include "dxg5/lang_ast.h"
#include "test.h"

using namespace dx5;

namespace {

// the tree as text: (op a b), call f(a, n=b), member a.b
std::string Show(const Expr& e)
{
    switch (e.kind) {
    case Expr::Num: return e.text;
    case Expr::Str: return "\"" + e.text + "\"";
    case Expr::Name: return e.text;
    case Expr::Member: return Show(*e.args[0]) + "." + e.text;
    case Expr::MemberExpr: return Show(*e.args[0]) + ".(" + Show(*e.args[1]) + ")";
    case Expr::Unary: return "(" + e.text + " " + Show(*e.args[0]) + ")";
    case Expr::Binary: return "(" + e.text + " " + Show(*e.args[0]) + " " + Show(*e.args[1]) + ")";
    case Expr::Cond: return "(? " + Show(*e.args[0]) + " " + Show(*e.args[1]) + " " + Show(*e.args[2]) + ")";
    case Expr::Lambda: return "(" + e.text + ") => " + Show(*e.args[0]);
    case Expr::Call: {
        std::string s = e.text + "(";
        bool first = true;
        for (const auto& a : e.args) s += (first ? "" : ", ") + Show(*a), first = false;
        for (const auto& a : e.named) s += (first ? "" : ", ") + a.first + "=" + Show(*a.second), first = false;
        return s + ")";
    }
    }
    return "?";
}

std::string ExprOf(const std::string& src)
{
    std::unique_ptr<Expr> e;
    Diag d;
    if (!ParseExpr(src, &e, &d)) return "ERR " + std::to_string(d.pos.line) + ":" + std::to_string(d.pos.col) + " " + d.msg;
    return Show(*e);
}

}  // namespace

TEST(parse_precedence)
{
    CHECK(ExprOf("a + b * c ** 2 ** 3") == "(+ a (* b (** c (** 2 3))))");
    CHECK(ExprOf("-2 ** 2") == "(- (** 2 2))");
    CHECK(ExprOf("a < b && !c || d == e") == "(|| (&& (< a b) (! c)) (== d e))");
    CHECK(ExprOf("c ? a + 1 : b") == "(? c (+ a 1) b)");
    CHECK(ExprOf("(a + b) % 3 / 2") == "(/ (% (+ a b) 3) 2)");
    CHECK(ExprOf("0x3fa1 + 1e3 + 1.5") == "(+ (+ 0x3fa1 1e3) 1.5)");
}

TEST(parse_floordiv_vs_comment)
{
    Program p;
    Diag d;
    CHECK(ParseProgram("x = floordiv(a, b)\ny = 1 // note\n// line\nz = 2 /* inline */ + 3\n", &p, &d));
    CHECK(p.stmts.size() == 3);
    if (p.stmts.size() == 3) {
        CHECK(Show(*p.stmts[0].value) == "floordiv(a, b)");
        CHECK(Show(*p.stmts[1].value) == "1");
        CHECK(Show(*p.stmts[2].value) == "(+ 2 3)");
        CHECK(p.stmts[1].pos.line == 2);
    }
}

TEST(parse_each_if)
{
    Program p;
    Diag d;
    CHECK(ParseProgram("g2 = each g {\n  if t < 0.5 { scale *= 0.5 } else { opacity = 0 }\n  position += vec(0, 1, 0)\n}\n", &p, &d));
    CHECK(p.stmts.size() == 1);
    if (p.stmts.size() != 1) return;
    const Stmt& s = p.stmts[0];
    CHECK(s.kind == Stmt::Each && s.names.size() == 1 && s.names[0] == "g2" && s.input == "g");
    CHECK(s.body.size() == 2);
    if (s.body.size() == 2) {
        const Stmt& i = s.body[0];
        CHECK(i.kind == Stmt::If && Show(*i.value) == "(< t 0.5)");
        CHECK(i.body.size() == 1 && i.body[0].kind == Stmt::Compound && i.body[0].op == "*=" && i.body[0].names[0] == "scale");
        CHECK(i.elseBody.size() == 1 && i.elseBody[0].kind == Stmt::Assign && i.elseBody[0].names[0] == "opacity");
        CHECK(s.body[1].kind == Stmt::Compound && s.body[1].op == "+=");
    }
    Program q;
    CHECK(ParseProgram("each h { opacity = 0.5 }", &q, &d) && q.stmts.size() == 1 && q.stmts[0].kind == Stmt::Each &&
          q.stmts[0].names.empty() && q.stmts[0].input == "h");
}

TEST(parse_lambda_member_call)
{
    CHECK(ExprOf("(e) => e.position + e.parent.index") == "(e) => (+ e.position e.parent.index)");
    CHECK(ExprOf("g.position") == "g.position");
    CHECK(ExprOf("g.(index * 2)") == "g.((* index 2))");
    CHECK(ExprOf("set_position(g, position + vec(0, 1, 0), where: index % 2 == 0)") ==
          "set_position(g, (+ position vec(0, 1, 0)), where=(== (% index 2) 0))");
    CHECK(ExprOf("circle(count: 36, radius: 100 + index * 5)") == "circle(count=36, radius=(+ 100 (* index 5)))");
    CHECK(ExprOf("p(e)") == "p(e)");
    CHECK(ExprOf("f(\"ix\", a ? b : c)") == "f(\"ix\", (? a b c))");
    Program p;
    Diag d;
    CHECK(ParseProgram("g_b, b = capture(g, position)\n", &p, &d));
    CHECK(p.stmts.size() == 1 && p.stmts[0].names.size() == 2 && p.stmts[0].names[0] == "g_b" && p.stmts[0].names[1] == "b");
    // a call spanning lines
    CHECK(ParseProgram("g = grid(cx: 3,\n   cy: 4)\noutput(g)\n", &p, &d) && p.stmts.size() == 2);
}

TEST(parse_group)
{
    Program p;
    Diag d;
    CHECK(ParseProgram("group grid(cx: int structural = 5, sx = 100, g: geometry) {\n  h = points(cx)\n  return h\n}\n", &p, &d));
    CHECK(p.stmts.size() == 1);
    if (p.stmts.size() != 1) return;
    const Stmt& s = p.stmts[0];
    CHECK(s.kind == Stmt::Group && s.names[0] == "grid" && s.params.size() == 3);
    if (s.params.size() == 3) {
        CHECK(s.params[0].name == "cx" && s.params[0].type == "int" && s.params[0].structural && Show(*s.params[0].dflt) == "5");
        CHECK(s.params[1].name == "sx" && s.params[1].type.empty() && !s.params[1].structural);
        CHECK(s.params[2].type == "geometry" && !s.params[2].dflt);
    }
    CHECK(s.body.size() == 2 && s.body[1].kind == Stmt::Return && Show(*s.body[1].value) == "h");
}

TEST(parse_errors)
{
    Program p;
    Diag d;
    CHECK(!ParseProgram("x = (1 +\n", &p, &d));
    CHECK(d.pos.line >= 1 && !d.msg.empty());
    CHECK(!ParseProgram("each { }\n", &p, &d));
    CHECK(!ParseProgram("x = 1 +\ny = 2\n", &p, &d) && d.pos.line == 1);
    CHECK(!ParseProgram("x = @\n", &p, &d) && d.pos.col == 5);
    CHECK(!ParseProgram("x = \"open\n", &p, &d));
    CHECK(!ParseProgram("f(a b)\n", &p, &d));
}
