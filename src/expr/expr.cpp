// Lexer, parser and evaluator of expressions (spec/expr.md §3–§4).
#include "expr/expr.h"

#include "base/charconv.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <numbers>

namespace construe::expr {

// ---------------------------------------------------------------- units and dimensions

std::string dimToString(Dim d) {
    if (d.dimensionless()) return "1";
    std::string s;
    if (d.L != 0) s += "L^" + std::to_string(d.L);
    if (d.A != 0) {
        if (!s.empty()) s += "*";
        s += "A^" + std::to_string(d.A);
    }
    return s;
}

double lengthFactor(LengthUnit u) {
    switch (u) {
        case LengthUnit::mm: return 1.0;
        case LengthUnit::cm: return 10.0;
        case LengthUnit::m: return 1000.0;
        case LengthUnit::in: return 25.4;
        case LengthUnit::ft: return 304.8;
    }
    return 1.0;
}

double angleFactor(AngleUnit u) { return u == AngleUnit::deg ? std::numbers::pi / 180.0 : 1.0; }

const char* lengthUnitName(LengthUnit u) {
    switch (u) {
        case LengthUnit::mm: return "mm";
        case LengthUnit::cm: return "cm";
        case LengthUnit::m: return "m";
        case LengthUnit::in: return "in";
        case LengthUnit::ft: return "ft";
    }
    return "mm";
}

const char* angleUnitName(AngleUnit u) { return u == AngleUnit::deg ? "deg" : "rad"; }

std::optional<LengthUnit> parseLengthUnit(const std::string& s) {
    for (auto u : {LengthUnit::mm, LengthUnit::cm, LengthUnit::m, LengthUnit::in, LengthUnit::ft})
        if (s == lengthUnitName(u)) return u;
    return std::nullopt;
}

std::optional<AngleUnit> parseAngleUnit(const std::string& s) {
    if (s == "deg") return AngleUnit::deg;
    if (s == "rad") return AngleUnit::rad;
    return std::nullopt;
}

namespace {

struct UnitInfo {
    const char* name;
    Dim dim;
    double factor;
};

const UnitInfo* findUnit(const std::string& s) {
    static const UnitInfo units[] = {
        {"mm", LENGTH, 1.0},   {"cm", LENGTH, 10.0},  {"m", LENGTH, 1000.0},
        {"in", LENGTH, 25.4},  {"ft", LENGTH, 304.8}, {"deg", ANGLE, std::numbers::pi / 180.0},
        {"rad", ANGLE, 1.0},
    };
    for (const auto& u : units)
        if (s == u.name) return &u;
    return nullptr;
}

enum class Fn { Sin, Cos, Tan, Asin, Acos, Atan, Atan2, Sqrt, Abs, Min, Max, Floor, Ceil, Round };

struct FnInfo {
    const char* name;
    Fn fn;
    int minArgs;
    int maxArgs;  // -1 — unlimited
};

const FnInfo* findFunction(const std::string& s) {
    static const FnInfo fns[] = {
        {"sin", Fn::Sin, 1, 1},     {"cos", Fn::Cos, 1, 1},     {"tan", Fn::Tan, 1, 1},
        {"asin", Fn::Asin, 1, 1},   {"acos", Fn::Acos, 1, 1},   {"atan", Fn::Atan, 1, 1},
        {"atan2", Fn::Atan2, 2, 2}, {"sqrt", Fn::Sqrt, 1, 1},   {"abs", Fn::Abs, 1, 1},
        {"min", Fn::Min, 1, -1},    {"max", Fn::Max, 1, -1},    {"floor", Fn::Floor, 1, 1},
        {"ceil", Fn::Ceil, 1, 1},   {"round", Fn::Round, 1, 1},
    };
    for (const auto& f : fns)
        if (s == f.name) return &f;
    return nullptr;
}

}  // namespace

bool isReservedName(const std::string& name) {
    return findUnit(name) != nullptr || findFunction(name) != nullptr || name == "pi";
}

bool isQualifiedName(const std::string& name) { return name.find('.') != std::string::npos; }

const char* varTypeName(VarType t) {
    switch (t) {
        case VarType::Float: return "float";
        case VarType::Int: return "int";
        case VarType::String: return "string";
        case VarType::List: return "list";
    }
    return "float";
}

std::optional<VarType> parseVarType(const std::string& s) {
    for (auto t : {VarType::Float, VarType::Int, VarType::String, VarType::List})
        if (s == varTypeName(t)) return t;
    return std::nullopt;
}

// ---------------------------------------------------------------- parse tree

enum class NodeKind { Number, Variable, Pi, Unary, Binary, Call };

struct Node {
    NodeKind kind;
    int pos = 0;  // position of the node's token (code points)
    double value = 0.0;
    Dim dim;
    std::string name;  // variable
    char op = 0;       // '+', '-', '*', '/', '^'
    const FnInfo* fn = nullptr;
    std::vector<std::shared_ptr<const Node>> children;
};

namespace {

using NodePtr = std::shared_ptr<const Node>;

// ---------------------------------------------------------------- lexer

enum class Tok { Number, Ident, Op, LParen, RParen, Comma, End };

struct Token {
    Tok type;
    int pos;
    std::string text;
    double number = 0.0;
    char op = 0;
};

Error syntaxError(int pos, const std::string& expected) {
    Error e("EXPR_SYNTAX");
    e.with("position", static_cast<double>(pos));
    if (!expected.empty()) e.with("expected", expected);
    return e;
}

bool isDigit(char c) { return c >= '0' && c <= '9'; }
bool isLetter(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }

Result<std::vector<Token>> lex(const std::string& s) {
    std::vector<Token> out;
    size_t i = 0;
    int cp = 0;  // index of the current code point
    bool leadingEqAllowed = true;

    auto advance = [&](size_t n) {
        i += n;
        cp += static_cast<int>(n);  // called only for ASCII
    };

    while (i < s.size()) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c >= 0x80) return syntaxError(cp, "");  // any non-ASCII character is not allowed
        if (c == ' ' || c == '\t') {
            advance(1);
            continue;
        }
        if (c == '=' && leadingEqAllowed) {
            leadingEqAllowed = false;
            advance(1);
            continue;
        }
        leadingEqAllowed = false;

        const int start = cp;
        if (isDigit(static_cast<char>(c)) || (c == '.' && i + 1 < s.size() && isDigit(s[i + 1]))) {
            size_t j = i;
            while (j < s.size() && isDigit(s[j])) ++j;
            if (j < s.size() && s[j] == '.') {
                if (j + 1 < s.size() && isDigit(s[j + 1])) {
                    ++j;
                    while (j < s.size() && isDigit(s[j])) ++j;
                } else if (j == i) {
                    return syntaxError(start, "digit");
                }
            }
            if (j < s.size() && (s[j] == 'e' || s[j] == 'E')) {
                size_t k = j + 1;
                if (k < s.size() && (s[k] == '+' || s[k] == '-')) ++k;
                if (k < s.size() && isDigit(s[k])) {
                    while (k < s.size() && isDigit(s[k])) ++k;
                    j = k;
                }
            }
            Token t{Tok::Number, start, s.substr(i, j - i)};
            // from_chars does not depend on the process locale (strtod would read "," in a Russian locale).
            parseDouble(t.text.data(), t.text.data() + t.text.size(), t.number);
            out.push_back(t);
            advance(j - i);
            continue;
        }
        if (isLetter(static_cast<char>(c)) || c == '_') {
            size_t j = i;
            auto identStart = [&](size_t k) { return k < s.size() && (isLetter(s[k]) || s[k] == '_'); };
            while (j < s.size() && (isLetter(s[j]) || isDigit(s[j]) || s[j] == '_')) ++j;
            // Qualified name "ident.ident" (§3.1): exactly one dot.
            if (j < s.size() && s[j] == '.' && identStart(j + 1)) {
                ++j;
                while (j < s.size() && (isLetter(s[j]) || isDigit(s[j]) || s[j] == '_')) ++j;
                if (j < s.size() && s[j] == '.' && identStart(j + 1)) return syntaxError(cp + static_cast<int>(j - i), "");
            }
            out.push_back(Token{Tok::Ident, start, s.substr(i, j - i)});
            advance(j - i);
            continue;
        }
        switch (c) {
            case '+': case '-': case '*': case '/': case '^': {
                Token t{Tok::Op, start, std::string(1, static_cast<char>(c))};
                t.op = static_cast<char>(c);
                out.push_back(t);
                advance(1);
                continue;
            }
            case '(': out.push_back(Token{Tok::LParen, start, "("}); advance(1); continue;
            case ')': out.push_back(Token{Tok::RParen, start, ")"}); advance(1); continue;
            case ',': out.push_back(Token{Tok::Comma, start, ","}); advance(1); continue;
            default: return syntaxError(start, "");  // invalid character (incl. '=' not at the start, '.')
        }
    }
    out.push_back(Token{Tok::End, cp, ""});
    return out;
}

// ---------------------------------------------------------------- parser

class Parser {
public:
    explicit Parser(std::vector<Token> t) : toks_(std::move(t)) {}

    Result<NodePtr> parseAll() {
        if (peek().type == Tok::End) return syntaxError(peek().pos, "expression");
        auto e = sum();
        if (!e) return e;
        if (peek().type != Tok::End) return syntaxError(peek().pos, "end");
        return e;
    }

private:
    std::vector<Token> toks_;
    size_t p_ = 0;

    const Token& peek() const { return toks_[p_]; }
    const Token& take() { return toks_[p_++]; }
    bool isOp(char c) const { return peek().type == Tok::Op && peek().op == c; }

    static NodePtr makeBinary(char op, int pos, NodePtr l, NodePtr r) {
        auto n = std::make_shared<Node>();
        n->kind = NodeKind::Binary;
        n->op = op;
        n->pos = pos;
        n->children = {std::move(l), std::move(r)};
        return n;
    }

    Result<NodePtr> sum() {
        auto l = product();
        if (!l) return l;
        NodePtr left = *l;
        while (isOp('+') || isOp('-')) {
            const Token& t = take();
            auto r = product();
            if (!r) return r;
            left = makeBinary(t.op, t.pos, left, *r);
        }
        return left;
    }

    Result<NodePtr> product() {
        auto l = unary();
        if (!l) return l;
        NodePtr left = *l;
        while (isOp('*') || isOp('/')) {
            const Token& t = take();
            auto r = unary();
            if (!r) return r;
            left = makeBinary(t.op, t.pos, left, *r);
        }
        return left;
    }

    Result<NodePtr> unary() {
        if (isOp('+') || isOp('-')) {
            const Token& t = take();
            auto c = unary();
            if (!c) return c;
            auto n = std::make_shared<Node>();
            n->kind = NodeKind::Unary;
            n->op = t.op;
            n->pos = t.pos;
            n->children = {*c};
            return NodePtr(n);
        }
        return power();
    }

    Result<NodePtr> power() {
        auto b = primary();
        if (!b) return b;
        if (isOp('^')) {
            const Token& t = take();
            auto e = unary();
            if (!e) return e;
            return makeBinary('^', t.pos, *b, *e);
        }
        return b;
    }

    Result<NodePtr> primary() {
        const Token& t = peek();
        switch (t.type) {
            case Tok::Number: {
                take();
                auto n = std::make_shared<Node>();
                n->kind = NodeKind::Number;
                n->pos = t.pos;
                n->value = t.number;
                if (peek().type == Tok::Ident) {
                    if (const UnitInfo* u = findUnit(peek().text)) {
                        take();
                        n->value = t.number * u->factor;
                        n->dim = u->dim;
                    }
                }
                return NodePtr(n);
            }
            case Tok::Ident: {
                if (findUnit(t.text)) return syntaxError(t.pos, "operand");
                if (const FnInfo* f = findFunction(t.text)) return call(f);
                take();
                auto n = std::make_shared<Node>();
                n->pos = t.pos;
                if (t.text == "pi") {
                    n->kind = NodeKind::Pi;
                } else {
                    n->kind = NodeKind::Variable;
                    n->name = t.text;
                }
                return NodePtr(n);
            }
            case Tok::LParen: {
                take();
                auto e = sum();
                if (!e) return e;
                if (peek().type != Tok::RParen) return syntaxError(peek().pos, ")");
                take();
                return e;
            }
            default: return syntaxError(t.pos, "operand");
        }
    }

    Result<NodePtr> call(const FnInfo* f) {
        const Token& nameTok = take();
        if (peek().type != Tok::LParen) return syntaxError(peek().pos, "(");
        take();
        auto n = std::make_shared<Node>();
        n->kind = NodeKind::Call;
        n->pos = nameTok.pos;
        n->fn = f;
        if (peek().type == Tok::RParen) return syntaxError(nameTok.pos, "arity");  // zero arguments — arity violation
        while (true) {
            auto a = sum();
            if (!a) return a;
            n->children.push_back(*a);
            if (peek().type == Tok::Comma) {
                take();
                continue;
            }
            if (peek().type == Tok::RParen) {
                take();
                break;
            }
            return syntaxError(peek().pos, ")");
        }
        const int argc = static_cast<int>(n->children.size());
        if (argc < f->minArgs || (f->maxArgs >= 0 && argc > f->maxArgs))
            return syntaxError(nameTok.pos, "arity");
        return NodePtr(n);
    }
};

// ---------------------------------------------------------------- evaluation

Error dimError(Dim expected, Dim actual, int pos) {
    Error e("EXPR_DIMENSION");
    e.with("expected", dimToString(expected)).with("actual", dimToString(actual));
    e.with("position", static_cast<double>(pos));
    return e;
}

Error dimErrorS(const std::string& expected, Dim actual, int pos) {
    Error e("EXPR_DIMENSION");
    e.with("expected", expected).with("actual", dimToString(actual));
    e.with("position", static_cast<double>(pos));
    return e;
}

Error domainError(const std::string& name, int pos) {
    Error e("EXPR_DOMAIN");
    e.with("name", name).with("position", static_cast<double>(pos));
    return e;
}

class Evaluator {
public:
    Evaluator(const Env& env, AngleUnit au) : env_(env), angle_(au) {}

    Result<Quantity> eval(const Node& n) {
        auto r = evalRaw(n);
        if (r && !std::isfinite(r->value)) return domainError(nodeName(n), n.pos);
        return r;
    }

private:
    const Env& env_;
    AngleUnit angle_;

    static std::string nodeName(const Node& n) {
        switch (n.kind) {
            case NodeKind::Call: return n.fn->name;
            case NodeKind::Binary:
            case NodeKind::Unary: return std::string(1, n.op);
            case NodeKind::Variable: return n.name;
            default: return "";
        }
    }

    Result<Quantity> evalRaw(const Node& n) {
        switch (n.kind) {
            case NodeKind::Number: return Quantity{n.value, n.dim};
            case NodeKind::Pi: return Quantity{std::numbers::pi, DIMLESS};
            case NodeKind::Variable: {
                EnvValue v = env_ ? env_(n.name) : std::nullopt;
                if (!v) {
                    Error e("EXPR_UNKNOWN_NAME");
                    e.with("name", n.name).with("position", static_cast<double>(n.pos));
                    return e;
                }
                if (!v->ok() && v->error().code == "EXPR_TYPE") {  // reference to a string variable (§5а)
                    Error e("EXPR_TYPE");
                    e.with("expected", std::string("number")).with("actual", std::string("string"));
                    e.with("name", n.name).with("position", static_cast<double>(n.pos));
                    return e;
                }
                if (!v->ok()) {
                    Error e("EXPR_DEPENDS_ON_ERROR");
                    const std::string* root = v->error().stringParam("name");
                    e.with("name", root ? *root : n.name).with("position", static_cast<double>(n.pos));
                    return e;
                }
                return v->value();
            }
            case NodeKind::Unary: {
                auto c = eval(*n.children[0]);
                if (!c) return c;
                if (n.op == '-') c->value = -c->value;
                return c;
            }
            case NodeKind::Binary: return binary(n);
            case NodeKind::Call: return call(n);
        }
        return Error("EXPR_SYNTAX");
    }

    Result<Quantity> binary(const Node& n) {
        auto a = eval(*n.children[0]);
        if (!a) return a;
        auto b = eval(*n.children[1]);
        if (!b) return b;
        Quantity x = *a, y = *b;
        switch (n.op) {
            case '+':
            case '-':
                if (!(x.dim == y.dim)) return dimError(x.dim, y.dim, n.pos);
                return Quantity{n.op == '+' ? x.value + y.value : x.value - y.value, x.dim};
            case '*': return Quantity{x.value * y.value, {x.dim.L + y.dim.L, x.dim.A + y.dim.A}};
            case '/':
                if (std::abs(y.value) < 1e-300) {
                    Error e("EXPR_DIV_ZERO");
                    e.with("position", static_cast<double>(n.pos));
                    return e;
                }
                return Quantity{x.value / y.value, {x.dim.L - y.dim.L, x.dim.A - y.dim.A}};
            case '^': {
                if (!y.dim.dimensionless()) return dimError(DIMLESS, y.dim, n.pos);
                if (x.dim.dimensionless()) return Quantity{std::pow(x.value, y.value), DIMLESS};
                const double r = std::round(y.value);
                if (std::abs(y.value - r) > 1e-12) return dimErrorS("integer", x.dim, n.pos);
                const int k = static_cast<int>(r);
                return Quantity{std::pow(x.value, r), {x.dim.L * k, x.dim.A * k}};
            }
        }
        return Error("EXPR_SYNTAX");
    }

    Result<Quantity> call(const Node& n) {
        std::vector<Quantity> args;
        for (const auto& c : n.children) {
            auto r = eval(*c);
            if (!r) return r;
            args.push_back(*r);
        }
        const int pos = n.pos;
        auto angleArg = [&](const Quantity& q) -> Result<Quantity> {
            if (q.dim.dimensionless()) return Quantity{q.value * angleFactor(angle_), ANGLE};
            if (!(q.dim == ANGLE)) return dimError(ANGLE, q.dim, pos);
            return q;
        };
        switch (n.fn->fn) {
            case Fn::Sin:
            case Fn::Cos:
            case Fn::Tan: {
                auto a = angleArg(args[0]);
                if (!a) return a;
                const double v = a->value;
                const double r = n.fn->fn == Fn::Sin ? std::sin(v) : n.fn->fn == Fn::Cos ? std::cos(v) : std::tan(v);
                return Quantity{r, DIMLESS};
            }
            case Fn::Asin:
            case Fn::Acos:
            case Fn::Atan: {
                const Quantity& x = args[0];
                if (!x.dim.dimensionless()) return dimError(DIMLESS, x.dim, pos);
                if (n.fn->fn != Fn::Atan && std::abs(x.value) > 1.0) return domainError(n.fn->name, pos);
                const double r = n.fn->fn == Fn::Asin ? std::asin(x.value)
                               : n.fn->fn == Fn::Acos ? std::acos(x.value)
                                                      : std::atan(x.value);
                return Quantity{r, ANGLE};
            }
            case Fn::Atan2: {
                const Quantity &y = args[0], &x = args[1];
                if (!(y.dim == x.dim)) return dimError(y.dim, x.dim, pos);
                if (y.value == 0.0 && x.value == 0.0) return domainError(n.fn->name, pos);
                return Quantity{std::atan2(y.value, x.value), ANGLE};
            }
            case Fn::Sqrt: {
                const Quantity& x = args[0];
                if (x.dim.L % 2 != 0 || x.dim.A % 2 != 0) return dimErrorS("even", x.dim, pos);
                if (x.value < 0) return domainError(n.fn->name, pos);
                return Quantity{std::sqrt(x.value), {x.dim.L / 2, x.dim.A / 2}};
            }
            case Fn::Abs: return Quantity{std::abs(args[0].value), args[0].dim};
            case Fn::Floor: return Quantity{std::floor(args[0].value), args[0].dim};
            case Fn::Ceil: return Quantity{std::ceil(args[0].value), args[0].dim};
            case Fn::Round: return Quantity{std::round(args[0].value), args[0].dim};
            case Fn::Min:
            case Fn::Max: {
                Quantity best = args[0];
                for (size_t i = 1; i < args.size(); ++i) {
                    if (!(args[i].dim == args[0].dim)) return dimError(args[0].dim, args[i].dim, pos);
                    if (n.fn->fn == Fn::Min ? args[i].value < best.value : args[i].value > best.value)
                        best.value = args[i].value;
                }
                return best;
            }
        }
        return Error("EXPR_SYNTAX");
    }
};

const Node* findRef(const Node& n, const std::string& name) {
    if (n.kind == NodeKind::Variable) return n.name == name ? &n : nullptr;
    for (const auto& c : n.children)
        if (const Node* r = findRef(*c, name)) return r;
    return nullptr;
}

void collectRefs(const Node& n, std::vector<std::string>& out) {
    if (n.kind == NodeKind::Variable) {
        for (const auto& s : out)
            if (s == n.name) return;
        out.push_back(n.name);
        return;
    }
    for (const auto& c : n.children) collectRefs(*c, out);
}

void collectPositions(const Node& n, const std::string& name, std::vector<int>& out) {
    if (n.kind == NodeKind::Variable && n.name == name) out.push_back(n.pos);
    for (const auto& c : n.children) collectPositions(*c, name, out);
}

}  // namespace

Result<Ast> parse(const std::string& text) {
    auto toks = lex(text);
    if (!toks) return toks.error();
    Parser p(std::move(*toks));
    auto root = p.parseAll();
    if (!root) return root.error();
    return Ast(*root);
}

Result<Quantity> evaluate(const Ast& ast, const Env& env, AngleUnit angleUnit) {
    Evaluator ev(env, angleUnit);
    return ev.eval(*ast.root());
}

Result<Quantity> evaluateField(const std::string& text, Expected expected, const Env& env,
                               const DocumentUnits& units) {
    auto ast = parse(text);
    if (!ast) return ast.error();
    auto q = evaluate(*ast, env, units.angle);
    if (!q || !expected) return q;
    Quantity v = *q;
    // Dimensionless — in the document unit; unrepresentable after conversion — EXPR_DOMAIN (§4).
    auto converted = [&](double factor, const char* unit, Dim dim) -> Result<Quantity> {
        const double x = v.value * factor;
        if (!std::isfinite(x)) return Error("EXPR_DOMAIN").with("name", std::string(unit)).with("position", 0.0);
        return Quantity{x, dim};
    };
    if (v.dim.dimensionless() && *expected == LENGTH) return converted(lengthFactor(units.length), lengthUnitName(units.length), LENGTH);
    if (v.dim.dimensionless() && *expected == ANGLE) return converted(angleFactor(units.angle), angleUnitName(units.angle), ANGLE);
    if (!(v.dim == *expected)) {
        Error e("EXPR_DIMENSION");
        e.with("expected", dimToString(*expected)).with("actual", dimToString(v.dim));
        return e;
    }
    return v;
}

std::vector<std::string> referencesInOrder(const Ast& ast) {
    std::vector<std::string> out;
    if (ast.root()) collectRefs(*ast.root(), out);
    return out;
}

std::optional<int> referencePosition(const Ast& ast, const std::string& name) {
    if (!ast.root()) return std::nullopt;
    const Node* n = findRef(*ast.root(), name);
    if (!n) return std::nullopt;
    return n->pos;
}

int rootPosition(const Ast& ast) { return ast.root() ? ast.root()->pos : 0; }

std::string renameReference(const std::string& text, const std::string& from, const std::string& to) {
    auto ast = parse(text);
    if (!ast || !ast->root()) return text;
    std::vector<int> pos;
    collectPositions(*ast->root(), from, pos);
    if (pos.empty()) return text;
    // Code point → byte offsets of the text.
    std::vector<size_t> byteAt;
    for (size_t i = 0; i < text.size(); ++i)
        if ((static_cast<unsigned char>(text[i]) & 0xC0) != 0x80) byteAt.push_back(i);
    byteAt.push_back(text.size());
    std::sort(pos.begin(), pos.end());
    std::string out = text;
    for (auto it = pos.rbegin(); it != pos.rend(); ++it) {
        if (*it < 0 || size_t(*it) >= byteAt.size()) return text;
        const size_t b = byteAt[size_t(*it)];
        if (out.compare(b, from.size(), from) != 0) return text;  // the name as written must be there
        out.replace(b, from.size(), to);
    }
    return out;
}

std::set<std::string> references(const Ast& ast) {
    auto v = referencesInOrder(ast);
    return {v.begin(), v.end()};
}

}  // namespace construe::expr
