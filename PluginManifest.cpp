#include "PluginManifest.h"
#include "Json.h"

namespace ccordis {

namespace {

void readStringArray(const Value &doc, const char *key,
                     std::vector<std::string> &out)
{
    // 先按值存住 at() 的返回（§4.1 警示: asArray() 不得指向临时对象）
    const Value arr = doc.at(key);
    if (const Value::Array *a = arr.asArray()) {
        for (const Value &v : *a) {
            const std::string s = v.toString();
            if (!s.empty())
                out.push_back(s);
        }
    }
}

} // namespace

PluginManifest parseManifestJson(const std::string &jsonText, std::string *error)
{
    PluginManifest m;
    if (error)
        error->clear();

    Value root;
    std::string err;
    if (!parseJson(jsonText, root, &err)) {
        if (error)
            *error = "JSON parse failed: " + err;
        return m;
    }
    if (!root.isObject()) {
        if (error)
            *error = "manifest root must be a JSON object";
        return m;
    }

    m.name = root.at("name").toString();
    m.label = root.at("label").toString();
    m.version = root.at("version").toString("0.0.0");
    m.entry = root.at("entry").toString();

    // "abi" 缺省 → 当前内核 ABI（展示字段；真正判定用 .so 内嵌值, code wins）
    if (root.has("abi"))
        m.abiVersion = static_cast<std::uint32_t>(root.at("abi").toInt(0));
    else
        m.abiVersion = CCORDIS_ABI_VERSION;

    readStringArray(root, "requires", m.required);
    readStringArray(root, "provides", m.provides);
    if (root.at("options").isObject())
        m.options = root.at("options");

    // 结构完整性：缺 name/entry 给出明确原因（isValid()==false 但调用方
    // 能在日志里看到为什么 —— 拒绝静默，契约 M8）
    if (m.name.empty() || m.entry.empty()) {
        if (error)
            *error = "manifest missing required fields:"
                     + std::string(m.name.empty() ? " name" : "")
                     + std::string(m.entry.empty() ? " entry" : "");
        return m;
    }

    // entry 路径安全：禁止绝对路径与 ".." 上溯（防 manifest 把加载点指到
    // 插件目录之外）。允许子目录形式 "sub/libfoo.so"。
    if (m.entry[0] == '/' || m.entry[0] == '\\'
        || m.entry.find("..") != std::string::npos
        || m.entry.find(':') == 1 /*win 盘符*/) {
        if (error)
            *error = "entry must be a relative path inside the plugin dir: "
                     + m.entry;
        m.entry.clear();    // 使 isValid() == false
    }
    return m;
}

} // namespace ccordis
