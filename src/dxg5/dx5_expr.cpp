// DupliX 2 (design v5, 8.3): the Expression node: its formula, read once per distinct text (a cache shared by the
// render threads), evaluated column by column over the geometry it is used on. Names, in order: the node's own
// inputs (variables and nodes the reader linked), true / false, the built-in attributes, index / id / count /
// time / seed, the macros (m1..m32, i1..i32), the named attributes of that geometry ("parent.ix" for parent.ix).
#include <memory>
#include <mutex>
#include <unordered_map>

#include "dxg5/eval_internal.h"
#include "dxg5/lang.h"
#include "graph/eval.h"

namespace dx5 {

namespace {

using Lines = std::vector<FormulaLine>;

std::shared_ptr<const Lines> Parsed(const std::string& text, std::string* err)
{
    static std::mutex mu;
    static std::unordered_map<std::string, std::shared_ptr<const Lines>> cache;
    std::lock_guard<std::mutex> lock(mu);
    auto it = cache.find(text);
    if (it != cache.end()) return it->second;
    auto lines = std::make_shared<Lines>();
    Diag d;
    std::shared_ptr<const Lines> r;
    if (ParseFormula(text, lines.get(), &d)) r = lines;
    else if (err) *err = d.msg;
    if (cache.size() > 4096) cache.clear();
    cache[text] = r;
    return r;
}

Column One(const Value& v) { return MakeColumn(v.type, 1, v); }

bool MacroName(const std::string& s, bool* isInt, int* k)
{
    if (s.size() < 2 || (s[0] != 'm' && s[0] != 'i')) return false;
    int v = 0;
    for (size_t j = 1; j < s.size(); j++) {
        if (s[j] < '0' || s[j] > '9' || v > 100) return false;
        v = v * 10 + (s[j] - '0');
    }
    if (v < 1 || v > 32 || (s[1] == '0')) return false;
    *isInt = s[0] == 'i', *k = v - 1;
    return true;
}

struct ExprEval {
    Evaluator& ev;
    const Node& n;
    const GeoPtr& ctx;
    std::map<std::string, Column> locals;  // the formula's earlier lines

    Column Values(const std::string& op, std::vector<Column>& in)
    {
        size_t size = 1;
        for (const Column& c : in)
            if (c.size() != 1) size = ctx->count;
        std::vector<Value> out(size);
        Value a[3];
        for (size_t e = 0; e < size; e++) {
            for (size_t k = 0; k < in.size() && k < 3; k++) a[k] = in[k].Get(e);
            std::string err;
            if (!MathOp(op, a, (int)std::min<size_t>(in.size(), 3), &out[e], &err)) {
                ev.Warn(n.key + ": " + err);
                out[e] = Value::F(0);
            }
        }
        Type t = out.empty() ? Type::Float : out[0].type;
        for (const Value& v : out)
            if (v.type != t) t = (v.type == Type::Vector || t == Type::Vector) ? Type::Vector : Type::Float;
        Column c = MakeColumn(t, size, Value::F(0));
        for (size_t e = 0; e < size; e++) c.Set(e, out[e]);
        return c;
    }

    Column Name(const std::string& s)
    {
        auto l = locals.find(s);
        if (l != locals.end()) return l->second;
        if (n.In(s)) return ev.Input(n, s, ctx);
        if (s == "true" || s == "false") return One(Value::B(s == "true"));
        const size_t cnt = ctx->count;
        if (const BuiltinAttr* b = FindBuiltin(s)) {
            auto a = ctx->attrs.find(s);
            return a != ctx->attrs.end() ? a->second : MakeColumn(b->type, cnt, b->dflt);
        }
        if (s == "index" || s == "id") {
            Column c = MakeColumn(Type::Int, cnt, Value::I(0));
            for (size_t k = 0; k < cnt; k++) c.i[k] = s == "index" ? (int64_t)k : (int64_t)ctx->id[k];
            return c;
        }
        if (s == "count") return One(Value::I((int64_t)cnt));
        if (s == "time") return One(Value::F(ev.Ctx().time));
        if (s == "seed") return One(Value::I(ev.Ctx().seed));
        bool isInt = false;
        int k = 0;
        if (MacroName(s, &isInt, &k)) {
            const EvalContext& c = ev.Ctx();
            if (isInt) return One(Value::I(c.intMacro ? c.intMacro(k, c.time) : 0));
            return One(Value::F(c.macro ? c.macro(k, c.time) : 0));
        }
        auto a = ctx->attrs.find(s);
        if (a != ctx->attrs.end() && a->second.size() == cnt) return a->second;
        if (cnt) ev.Warn(n.key + ": no attribute '" + s + "'");
        return MakeColumn(Type::Float, cnt, Value::F(0));
    }

    Column Eval(const Expr& e)
    {
        switch (e.kind) {
        case Expr::Num: return One(NumberValue(e.text));
        case Expr::Name: return Name(e.text);
        case Expr::Member: {
            if (e.args[0]->kind == Expr::Name && e.args[0]->text == "parent") return Name("parent." + e.text);
            if (e.text == "x" || e.text == "y" || e.text == "z") {
                std::vector<Column> in = {Eval(*e.args[0])};
                return Values(std::string("sep_") + e.text, in);
            }
            ev.Warn(n.key + ": cannot read ." + e.text);
            return MakeColumn(Type::Float, ctx->count, Value::F(0));
        }
        case Expr::Unary: {
            std::vector<Column> in = {Eval(*e.args[0])};
            return Values(OpName(e.text, true), in);
        }
        case Expr::Binary: {
            std::vector<Column> in = {Eval(*e.args[0]), Eval(*e.args[1])};
            return Values(OpName(e.text, false), in);
        }
        case Expr::Cond: {
            std::vector<Column> in = {Eval(*e.args[0]), Eval(*e.args[1]), Eval(*e.args[2])};
            return Values("switch", in);
        }
        case Expr::Call: {
            if (IsMathFunction(e.text) && e.named.empty()) {
                std::vector<Column> in;
                for (const auto& a : e.args) in.push_back(Eval(*a));
                return Values(e.text, in);
            }
            if (e.text == "attr" && e.args.size() == 1 && e.args[0]->kind == Expr::Str) {
                const size_t cnt = ctx->count;
                auto a = ctx->attrs.find(e.args[0]->text);
                if (a != ctx->attrs.end() && a->second.size() == cnt) return a->second;
                if (cnt) ev.Warn(n.key + ": no attribute '" + e.args[0]->text + "'");
                return MakeColumn(Type::Float, cnt, Value::F(0));
            }
            if (e.text == "random" && e.args.empty()) {
                uint32_t salt = (uint32_t)HashStr(n.key);
                for (const auto& a : e.named)
                    if (a.first == "salt") salt = (uint32_t)AsInt(NumberValue(a.second->text));
                const size_t cnt = ctx->count;
                Column c = MakeColumn(Type::Float, cnt, Value::F(0));
                for (size_t k = 0; k < cnt; k++) c.f[k] = dxg::Rand01(ctx->id[k], ev.Ctx().seed, salt);
                return c;
            }
            // a variable holding a function, called on the element: p(e)
            if (n.In(e.text)) return ev.Input(n, e.text, ctx);
            ev.Warn(n.key + ": " + e.text + "() is not a math function");
            return MakeColumn(Type::Float, ctx->count, Value::F(0));
        }
        default:
            ev.Warn(n.key + ": cannot evaluate " + PrintExpr(e));
            return MakeColumn(Type::Float, ctx->count, Value::F(0));
        }
    }
};

}  // namespace

Column EvalExpressionNode(Evaluator& ev, const Node& n, const GeoPtr& ctx, int output)
{
    std::string err;
    const std::shared_ptr<const Lines> lines = Parsed(n.text, &err);
    if (!lines) {
        ev.Warn(n.key + ": " + (err.empty() ? "cannot read the expression" : err));
        return MakeColumn(Type::Float, ctx->count, Value::F(0));
    }
    // the lines in order up to the output asked for (`_` lines are not outputs)
    ExprEval x{ev, n, ctx, {}};
    int out = 0;
    for (const FormulaLine& l : *lines) {
        Column c = x.Eval(*l.expr);
        if (l.name[0] != '_') {
            if (out == output) return c;
            out++;
        }
        x.locals[l.name] = std::move(c);
    }
    ev.Warn(n.key + ": no output " + std::to_string(output));
    return MakeColumn(Type::Float, ctx->count, Value::F(0));
}

}  // namespace dx5
