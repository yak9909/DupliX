// DupliX 2 (design v5, 8): expressions as text, the math functions, constant folding.
#include <charconv>
#include <cstdlib>

#include "dxg5/eval_internal.h"
#include "dxg5/lang.h"

namespace dx5 {

namespace {

int Level(const Expr& e)
{
    switch (e.kind) {
    case Expr::Cond: return 0;
    case Expr::Binary: {
        const std::string& o = e.text;
        if (o == "||") return 1;
        if (o == "&&") return 2;
        if (o == "==" || o == "!=") return 3;
        if (o == "<" || o == "<=" || o == ">" || o == ">=") return 4;
        if (o == "+" || o == "-") return 5;
        if (o == "**") return 8;
        return 6;
    }
    case Expr::Unary: return 7;
    case Expr::Lambda: return -1;
    default: return 9;
    }
}

std::string Wrap(const Expr& e, bool paren) { return paren ? "(" + PrintExpr(e) + ")" : PrintExpr(e); }

std::string Quote(const std::string& s)
{
    std::string o = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') o += '\\';
        o += c;
    }
    return o + "\"";
}

}  // namespace

std::string PrintExpr(const Expr& e)
{
    switch (e.kind) {
    case Expr::Num: return e.text;
    case Expr::Str: return Quote(e.text);
    case Expr::Name: return e.text;
    case Expr::Member: return Wrap(*e.args[0], Level(*e.args[0]) < 9) + "." + e.text;
    case Expr::MemberExpr: return Wrap(*e.args[0], Level(*e.args[0]) < 9) + ".(" + PrintExpr(*e.args[1]) + ")";
    case Expr::Unary: return e.text + Wrap(*e.args[0], Level(*e.args[0]) < 7);
    case Expr::Binary: {
        const int lv = Level(e);
        if (e.text == "**") return Wrap(*e.args[0], Level(*e.args[0]) < 9) + " ** " + Wrap(*e.args[1], Level(*e.args[1]) < 7);
        return Wrap(*e.args[0], Level(*e.args[0]) < lv) + " " + e.text + " " + Wrap(*e.args[1], Level(*e.args[1]) <= lv);
    }
    case Expr::Cond:
        return Wrap(*e.args[0], Level(*e.args[0]) <= 0) + " ? " + Wrap(*e.args[1], Level(*e.args[1]) < 0) + " : " +
               Wrap(*e.args[2], Level(*e.args[2]) < 0);
    case Expr::Lambda: return "(" + e.text + ") => " + PrintExpr(*e.args[0]);
    case Expr::Call: {
        std::string s = e.text + "(";
        bool first = true;
        for (const auto& a : e.args) s += (first ? "" : ", ") + PrintExpr(*a), first = false;
        for (const auto& a : e.named) s += (first ? "" : ", ") + a.first + ": " + PrintExpr(*a.second), first = false;
        return s + ")";
    }
    }
    return "";
}

bool IsMathFunction(const std::string& n)
{
    static const char* fns[] = {"sin",  "cos",   "tan", "asin",  "acos",   "atan2",     "sqrt",     "abs",     "floor",
                                "ceil", "round", "min", "max",   "clamp",  "mix",       "smoothstep", "len",   "normalize",
                                "dot",  "cross", "vec", "rot",   "inverse", "floordiv", "rot_mul",  "rot_apply", "sep_x",
                                "sep_y", "sep_z"};
    for (const char* f : fns)
        if (n == f) return true;
    return false;
}

std::string OpName(const std::string& s, bool unary)
{
    if (unary) return s == "-" ? "neg" : "not";
    static const struct {
        const char* sym;
        const char* op;
    } m[] = {{"+", "add"}, {"-", "sub"}, {"*", "mul"}, {"/", "div"}, {"%", "mod"}, {"**", "pow"}, {"==", "eq"}, {"!=", "ne"},
             {"<", "lt"},  {"<=", "le"}, {">", "gt"},  {">=", "ge"}, {"&&", "and"}, {"||", "or"}};
    for (const auto& x : m)
        if (s == x.sym) return x.op;
    return "";
}

Value NumberValue(const std::string& t)
{
    if (t.size() > 2 && t[0] == '0' && (t[1] == 'x' || t[1] == 'X')) return Value::I((int64_t)std::strtoull(t.c_str() + 2, nullptr, 16));
    if (t.find_first_of(".eE") != std::string::npos) {
        double v = 0;
        std::from_chars(t.data(), t.data() + t.size(), v);
        return Value::F(v);
    }
    int64_t v = 0;
    std::from_chars(t.data(), t.data() + t.size(), v);
    return Value::I(v);
}

bool EvalConst(const Expr& e, Value* out)
{
    std::string err;
    switch (e.kind) {
    case Expr::Num: *out = NumberValue(e.text); return true;
    case Expr::Name:
        if (e.text == "true" || e.text == "false") {
            *out = Value::B(e.text == "true");
            return true;
        }
        return false;
    case Expr::Unary: {
        Value a;
        return EvalConst(*e.args[0], &a) && MathOp(OpName(e.text, true), &a, 1, out, &err);
    }
    case Expr::Binary: {
        Value a[2];
        return EvalConst(*e.args[0], &a[0]) && EvalConst(*e.args[1], &a[1]) && MathOp(OpName(e.text, false), a, 2, out, &err);
    }
    case Expr::Cond: {
        Value a[3];
        for (int k = 0; k < 3; k++)
            if (!EvalConst(*e.args[k], &a[k])) return false;
        return MathOp("switch", a, 3, out, &err);
    }
    case Expr::Member: {
        Value a;
        if (!EvalConst(*e.args[0], &a) || a.type != Type::Vector) return false;
        const std::string& m = e.text;
        if (m != "x" && m != "y" && m != "z") return false;
        *out = Value::F(m == "x" ? a.v.x : m == "y" ? a.v.y : a.v.z);
        return true;
    }
    case Expr::Call: {
        if (!IsMathFunction(e.text) || !e.named.empty() || e.args.size() > 3) return false;
        Value a[3];
        for (size_t k = 0; k < e.args.size(); k++)
            if (!EvalConst(*e.args[k], &a[k])) return false;
        return MathOp(e.text, a, (int)e.args.size(), out, &err);
    }
    default: return false;
    }
}

}  // namespace dx5
