#include <ccordis/Json.h>

#include <cstdint>
#include <cstring>
#include <fstream>
#include <sstream>

namespace ccordis {

namespace {

constexpr int kMaxDepth = 64;

class Parser
{
public:
    Parser(const char *begin, const char *end, std::string *error)
        : m_p(begin), m_end(end), m_begin(begin), m_error(error) {}

    bool run(Value &out)
    {
        if (!parseValue(out))
            return false;
        skipWs();
        if (m_p != m_end)
            return fail("trailing characters after JSON document");
        return true;
    }

private:
    const char *m_p;
    const char *m_end;
    const char *m_begin;
    std::string *m_error;
    int m_depth = 0;

    void skipWs()
    {
        while (m_p < m_end && (*m_p == ' ' || *m_p == '\t' || *m_p == '\n' || *m_p == '\r'))
            ++m_p;
    }

    bool fail(const char *msg)
    {
        if (m_error && m_error->empty())
            *m_error = std::string(msg) + " (offset "
                     + std::to_string(m_p - m_begin) + ")";
        return false;
    }

    bool parseValue(Value &out)
    {
        if (++m_depth > kMaxDepth) {
            --m_depth;
            return fail("nesting too deep");
        }
        const bool ok = parseValueInner(out);
        --m_depth;
        return ok;
    }

    bool parseValueInner(Value &out)
    {
        skipWs();
        if (m_p >= m_end)
            return fail("unexpected end of input");
        switch (*m_p) {
        case '{': return parseObject(out);
        case '[': return parseArray(out);
        case '"': {
            std::string s;
            if (!parseString(s))
                return false;
            out = Value(std::move(s));
            return true;
        }
        case 't':
            if (m_end - m_p >= 4 && std::memcmp(m_p, "true", 4) == 0) {
                m_p += 4;
                out = Value(true);
                return true;
            }
            return fail("bad literal");
        case 'f':
            if (m_end - m_p >= 5 && std::memcmp(m_p, "false", 5) == 0) {
                m_p += 5;
                out = Value(false);
                return true;
            }
            return fail("bad literal");
        case 'n':
            if (m_end - m_p >= 4 && std::memcmp(m_p, "null", 4) == 0) {
                m_p += 4;
                out = Value(nullptr);
                return true;
            }
            return fail("bad literal");
        default:
            return parseNumber(out);
        }
    }

    static void appendUtf8(std::string &s, std::uint32_t cp)
    {
        if (cp < 0x80) {
            s.push_back(char(cp));
        } else if (cp < 0x800) {
            s.push_back(char(0xC0 | (cp >> 6)));
            s.push_back(char(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            s.push_back(char(0xE0 | (cp >> 12)));
            s.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
            s.push_back(char(0x80 | (cp & 0x3F)));
        } else {
            s.push_back(char(0xF0 | (cp >> 18)));
            s.push_back(char(0x80 | ((cp >> 12) & 0x3F)));
            s.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
            s.push_back(char(0x80 | (cp & 0x3F)));
        }
    }

    bool parseHex4(unsigned &cp)
    {
        if (m_end - m_p < 4)
            return fail("truncated \\u escape");
        cp = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = *m_p++;
            cp <<= 4;
            if (c >= '0' && c <= '9')      cp |= unsigned(c - '0');
            else if (c >= 'a' && c <= 'f') cp |= unsigned(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') cp |= unsigned(c - 'A' + 10);
            else return fail("bad hex digit in \\u escape");
        }
        return true;
    }

    bool parseString(std::string &s)
    {
        ++m_p;   // opening quote
        while (m_p < m_end) {
            const unsigned char c = static_cast<unsigned char>(*m_p);
            if (c == '"') {
                ++m_p;
                return true;
            }
            if (c == '\\') {
                ++m_p;
                if (m_p >= m_end)
                    return fail("truncated escape");
                switch (*m_p) {
                case '"':  s.push_back('"');  ++m_p; break;
                case '\\': s.push_back('\\'); ++m_p; break;
                case '/':  s.push_back('/');  ++m_p; break;
                case 'b':  s.push_back('\b'); ++m_p; break;
                case 'f':  s.push_back('\f'); ++m_p; break;
                case 'n':  s.push_back('\n'); ++m_p; break;
                case 'r':  s.push_back('\r'); ++m_p; break;
                case 't':  s.push_back('\t'); ++m_p; break;
                case 'u': {
                    ++m_p;
                    unsigned cp = 0;
                    if (!parseHex4(cp))
                        return false;
                    if (cp >= 0xD800 && cp <= 0xDBFF) {           // high surrogate
                        if (m_end - m_p >= 6 && m_p[0] == '\\' && m_p[1] == 'u') {
                            m_p += 2;
                            unsigned lo = 0;
                            if (!parseHex4(lo))
                                return false;
                            if (lo >= 0xDC00 && lo <= 0xDFFF)
                                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                            else
                                cp = 0xFFFD;                       // lone high → U+FFFD
                        } else {
                            cp = 0xFFFD;
                        }
                    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                        cp = 0xFFFD;                               // lone low → U+FFFD
                    }
                    appendUtf8(s, cp);
                    break;
                }
                default:
                    return fail("unknown escape");
                }
                continue;
            }
            if (c < 0x20)
                return fail("raw control character in string");
            s.push_back(char(c));
            ++m_p;
        }
        return fail("unterminated string");
    }

    bool parseNumber(Value &out)
    {
        const char *start = m_p;
        if (m_p < m_end && *m_p == '-')
            ++m_p;
        bool integral = true;
        while (m_p < m_end) {
            const char c = *m_p;
            if (c >= '0' && c <= '9') {
                ++m_p;
            } else if (c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-') {
                integral = false;
                ++m_p;
            } else {
                break;
            }
        }
        if (m_p == start)
            return fail("invalid number");
        const std::string text(start, m_p);
        if (integral) {
            try {
                out = Value(static_cast<std::int64_t>(std::stoll(text)));
                return true;
            } catch (...) {
                // int64 overflow → fall through to double
            }
        }
        try {
            out = Value(std::stod(text));
        } catch (...) {
            return fail("number out of range");
        }
        return true;
    }

    bool parseObject(Value &out)
    {
        ++m_p;   // {
        Value::Object obj;
        skipWs();
        if (m_p < m_end && *m_p == '}') {
            ++m_p;
            out = Value(std::move(obj));
            return true;
        }
        while (true) {
            skipWs();
            if (m_p >= m_end || *m_p != '"')
                return fail("expected object key");
            std::string key;
            if (!parseString(key))
                return false;
            skipWs();
            if (m_p >= m_end || *m_p != ':')
                return fail("expected ':' after object key");
            ++m_p;
            Value v;
            if (!parseValue(v))
                return false;
            obj.emplace_back(std::move(key), std::move(v));
            skipWs();
            if (m_p < m_end && *m_p == ',') {
                ++m_p;
                continue;
            }
            if (m_p < m_end && *m_p == '}') {
                ++m_p;
                out = Value(std::move(obj));
                return true;
            }
            return fail("expected ',' or '}' in object");
        }
    }

    bool parseArray(Value &out)
    {
        ++m_p;   // [
        Value::Array arr;
        skipWs();
        if (m_p < m_end && *m_p == ']') {
            ++m_p;
            out = Value(std::move(arr));
            return true;
        }
        while (true) {
            Value v;
            if (!parseValue(v))
                return false;
            arr.push_back(std::move(v));
            skipWs();
            if (m_p < m_end && *m_p == ',') {
                ++m_p;
                continue;
            }
            if (m_p < m_end && *m_p == ']') {
                ++m_p;
                out = Value(std::move(arr));
                return true;
            }
            return fail("expected ',' or ']' in array");
        }
    }
};

} // namespace

bool parseJson(const std::string &text, Value &out, std::string *error)
{
    out = Value();
    if (error)
        error->clear();
    Parser p(text.data(), text.data() + text.size(), error);
    return p.run(out);
}

bool readFileUtf8(const std::string &path, std::string &out)
{
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open())
        return false;
    std::ostringstream os;
    os << f.rdbuf();
    out = os.str();
    return true;
}

} // namespace ccordis
