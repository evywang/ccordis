#ifndef CCORDIS_VALUE_H
#define CCORDIS_VALUE_H

#include <cstdint>
#include <initializer_list>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace ccordis {

/**
 * @brief JSON-shaped configuration/payload value — the kernel's only
 *        structured data type (design §4.1). std::variant-based; purely a
 *        value type: parsing lives in host adapters, never in the kernel.
 *
 * Object preserves insertion order (vector-of-pairs — review F13) with
 * linear lookup: config payloads are small; order stability matters for
 * diffs and topology serialization.
 */
class Value
{
public:
    using Array = std::vector<Value>;
    using Object = std::vector<std::pair<std::string, Value>>;
    using Storage = std::variant<std::monostate,     // null
                                 bool, std::int64_t, double,
                                 std::string, Array, Object>;

    Value() = default;
    Value(std::nullptr_t) {}
    Value(bool b) : m_v(b) {}
    Value(int i) : m_v(static_cast<std::int64_t>(i)) {}
    Value(std::int64_t i) : m_v(i) {}
    Value(double d) : m_v(d) {}
    Value(const char *s) : m_v(std::string(s)) {}
    Value(std::string s) : m_v(std::move(s)) {}
    Value(Array a) : m_v(std::move(a)) {}
    Value(Object o) : m_v(std::move(o)) {}

    bool isNull() const { return std::holds_alternative<std::monostate>(m_v); }
    bool isBool() const { return std::holds_alternative<bool>(m_v); }
    bool isInt() const { return std::holds_alternative<std::int64_t>(m_v); }
    bool isDouble() const { return std::holds_alternative<double>(m_v); }
    bool isString() const { return std::holds_alternative<std::string>(m_v); }
    bool isArray() const { return std::holds_alternative<Array>(m_v); }
    bool isObject() const { return std::holds_alternative<Object>(m_v); }

    bool toBool(bool dflt = false) const;
    std::int64_t toInt(std::int64_t dflt = 0) const;
    double toDouble(double dflt = 0.0) const;     // int values widen
    std::string toString() const;                 // "" when not a string
    /** String access with fallback: null/mismatched values yield `dflt`
     *  (idiomatic `cfg.at("host").toString("127.0.0.1")`). */
    std::string toString(const std::string &dflt) const;

    const Array *asArray() const;
    const Object *asObject() const;

    /** Element count for arrays/objects, 0 otherwise. */
    std::size_t size() const;

    /** Object member access; missing key (or non-object) → null Value. */
    Value at(const std::string &key) const;
    bool has(const std::string &key) const;

    static Value object(std::initializer_list<Object::value_type> items);
    static Value array(std::initializer_list<Value> items);

private:
    Storage m_v;
};

inline bool Value::toBool(bool dflt) const
{
    const bool *p = std::get_if<bool>(&m_v);
    return p ? *p : dflt;
}

inline std::int64_t Value::toInt(std::int64_t dflt) const
{
    const std::int64_t *p = std::get_if<std::int64_t>(&m_v);
    return p ? *p : dflt;
}

inline double Value::toDouble(double dflt) const
{
    if (const double *p = std::get_if<double>(&m_v))
        return *p;
    if (const std::int64_t *p = std::get_if<std::int64_t>(&m_v))
        return static_cast<double>(*p);
    return dflt;
}

inline std::string Value::toString() const
{
    const std::string *p = std::get_if<std::string>(&m_v);
    return p ? *p : std::string();
}

inline std::string Value::toString(const std::string &dflt) const
{
    const std::string *p = std::get_if<std::string>(&m_v);
    return p ? *p : dflt;
}

inline const Value::Array *Value::asArray() const
{
    return std::get_if<Array>(&m_v);
}

inline const Value::Object *Value::asObject() const
{
    return std::get_if<Object>(&m_v);
}

inline std::size_t Value::size() const
{
    if (const Array *a = std::get_if<Array>(&m_v))
        return a->size();
    if (const Object *o = std::get_if<Object>(&m_v))
        return o->size();
    return 0;
}

inline Value Value::at(const std::string &key) const
{
    if (const Object *o = std::get_if<Object>(&m_v)) {
        for (const auto &kv : *o) {
            if (kv.first == key)
                return kv.second;
        }
    }
    return Value();
}

inline bool Value::has(const std::string &key) const
{
    if (const Object *o = std::get_if<Object>(&m_v)) {
        for (const auto &kv : *o) {
            if (kv.first == key)
                return true;
        }
    }
    return false;
}

inline Value Value::object(std::initializer_list<Object::value_type> items)
{
    return Value(Object(items));
}

inline Value Value::array(std::initializer_list<Value> items)
{
    return Value(Array(items));
}

} // namespace ccordis

#endif // CCORDIS_VALUE_H
