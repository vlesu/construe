// Expressions, units, variables (spec/expr.md).
#pragma once

#include "base/error.h"

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace construe::expr {

// Dimension: powers of length and angle (spec/expr.md §2.1).
struct Dim {
    int L = 0;
    int A = 0;
    bool operator==(const Dim&) const = default;
    bool dimensionless() const { return L == 0 && A == 0; }
};

inline constexpr Dim DIMLESS{0, 0};
inline constexpr Dim LENGTH{1, 0};
inline constexpr Dim ANGLE{0, 1};

std::string dimToString(Dim d);  // canonical notation: "1", "L^1", "L^2*A^-1"

struct Quantity {
    double value = 0.0;  // in internal units: mm, rad
    Dim dim;
};

enum class LengthUnit { mm, cm, m, in, ft };
enum class AngleUnit { deg, rad };

struct DocumentUnits {
    LengthUnit length = LengthUnit::mm;
    AngleUnit angle = AngleUnit::deg;
};

double lengthFactor(LengthUnit u);
double angleFactor(AngleUnit u);
const char* lengthUnitName(LengthUnit u);
const char* angleUnitName(AngleUnit u);
std::optional<LengthUnit> parseLengthUnit(const std::string& s);
std::optional<AngleUnit> parseAngleUnit(const std::string& s);

// Expected dimension of a field: Dim or Any (nullopt).
using Expected = std::optional<Dim>;

struct Node;  // parse tree node (internal)

class Ast {
public:
    Ast() = default;
    explicit Ast(std::shared_ptr<const Node> root) : root_(std::move(root)) {}
    const Node* root() const { return root_.get(); }

private:
    std::shared_ptr<const Node> root_;
};

// Value of a variable in the environment: nullopt — no such name (EXPR_UNKNOWN_NAME);
// Error — the variable is erroneous (parameter "name" — the root cause).
using EnvValue = std::optional<Result<Quantity>>;
using Env = std::function<EnvValue(const std::string& name)>;

Result<Ast> parse(const std::string& text);
Result<Quantity> evaluate(const Ast& ast, const Env& env, AngleUnit angleUnit);
Result<Quantity> evaluateField(const std::string& text, Expected expected, const Env& env,
                               const DocumentUnits& units);

// Variable names in evaluation order (§4.1), without repeats.
std::vector<std::string> referencesInOrder(const Ast& ast);
std::set<std::string> references(const Ast& ast);
// Position (code points) of the first occurrence of a name in evaluation order.
std::optional<int> referencePosition(const Ast& ast, const std::string& name);

// Position (code points) of the root node of the parse tree (spec/expr.md §5а, int).
int rootPosition(const Ast& ast);
// Every reference to the variable from in text replaced by to (whole name tokens by the parse tree, not parts of other
// names or text in strings; FX9-VAR-001); text that does not parse or has no reference — unchanged.
std::string renameReference(const std::string& text, const std::string& from, const std::string& to);

bool isReservedName(const std::string& name);
// Qualified name "a.b" (§3.1).
bool isQualifiedName(const std::string& name);

inline constexpr double INT_VALUE_TOL = 1e-9;  // §8
inline constexpr int SUBST_PRECISION = 6;      // §8

// Variable type (§5а).
enum class VarType { Float, Int, String, List };
const char* varTypeName(VarType t);  // float, int, string, list
std::optional<VarType> parseVarType(const std::string& s);

struct VariableEntry {
    std::string id;
    std::string name;
    VarType type = VarType::Float;
    Result<Quantity> value = Error("EXPR_SYNTAX");  // string: EXPR_TYPE (not a number)
    std::string text;                                // string: the value
    // Has a string value (§5а): a string variable without an error of its own (e.g. a reserved name).
    bool isString() const { return type == VarType::String && !value.ok() && value.error().code == "EXPR_TYPE"; }
};

struct VariableInput {
    std::string id;
    std::string name;        // full name (with scope for instance variables, §5б)
    std::string expression;
    VarType type = VarType::Float;
    std::vector<std::string> values;  // list (§5а)
    std::string scope;                // §5б: "" — document scope, otherwise a prefix "plate1."
    std::optional<VariableEntry> fixed;  // §5б: a given value (or error), not evaluated
};

class VariableTable {
public:
    std::vector<VariableEntry> entries;  // in the order of the variables array

    const VariableEntry* find(const std::string& name) const;
    Env env() const;  // environment for evaluateField (§5)
};

VariableTable evaluateVariables(const std::vector<VariableInput>& variables, const DocumentUnits& units);

// Value-field input (§9, FX9-SK-004, FX9-SK-005).
// Names of the table similar to an unknown name (a typo): edit distance by code points, ASCII letters case-insensitive,
// at most SIMILAR_NAME_DISTANCE, or one name is a prefix of the other; nearest first, ties by name; at most max names.
inline constexpr int SIMILAR_NAME_DISTANCE = 2;
std::vector<std::string> similarNames(const std::string& name, const VariableTable& table, size_t max = 3);

// Value display (§6.1).
std::string format(const Quantity& q, const DocumentUnits& units, int precision);
// Number display without a unit: the number part of §6.1 in document units (§5а.1).
std::string formatPlain(const Quantity& q, const DocumentUnits& units, int precision);
// Template substitution "{name}" (§5а.1).
std::string substitute(const std::string& templ, const VariableTable& table, const DocumentUnits& units);

}  // namespace construe::expr
