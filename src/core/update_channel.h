#pragma once

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

namespace komira {

/// GitHub 更新检查的「通道」表：直连 / 内置加速代理 / 用户自定义加速代理。
///
/// 纯 C++（不依赖 Qt），因此可以在 core_engine_test 里直接单测 —— 通道的取舍逻辑
/// （谁先谁后、自定义前缀怎么归一化、坏输入怎么办）全是纯函数，不该藏在网络层里
/// 靠打真实请求才验得出来。Qt 侧只负责把这些字符串变成 QString 与请求。
namespace update_channel {

/// 行为通道 id（不是具体代理）。
inline constexpr const char* kAuto = "auto";     ///< 直连优先，直连不通再依次走内置代理
inline constexpr const char* kDirect = "direct"; ///< 只直连，不走任何代理
inline constexpr const char* kCustom = "custom"; ///< 用户自定义前缀优先，其余按内置代理兜底

struct Proxy {
    const char* id;
    const char* label;
    const char* prefix;
};

/// 内置加速代理。顺序即兜底顺序：`gh-proxy.org` 是主入口，v4/v6/cdn 是同一家族的
/// 不同线路（IPv4 / IPv6 / CDN），`axisnow` 是独立节点。五个都实测可用（2026-10）：
/// `https://<host>/https://api.github.com/…` 会原样返回 API JSON。
///
/// `label` 一律**以真实主机名开头**：设置页下拉里直接展示它，用户要能一眼看出
/// 「这一条到底走哪个域名」—— 写成「gh-proxy.org（IPv4）」会让五条里的三条看起来
/// 是同一条，选错了也发现不了。
inline const std::vector<Proxy>& builtinProxies() {
    static const std::vector<Proxy> kProxies = {
        {"gh-proxy", "gh-proxy.org", "https://gh-proxy.org"},
        {"gh-proxy-v4", "v4.gh-proxy.org（IPv4）", "https://v4.gh-proxy.org"},
        {"gh-proxy-v6", "v6.gh-proxy.org（IPv6）", "https://v6.gh-proxy.org"},
        {"gh-proxy-cdn", "cdn.gh-proxy.org（CDN）", "https://cdn.gh-proxy.org"},
        {"gh-proxy-axisnow", "axisnow.gh-proxy.org", "https://axisnow.gh-proxy.org"},
    };
    return kProxies;
}

inline const Proxy* findProxy(const std::string& id) {
    for (const Proxy& proxy : builtinProxies()) {
        if (id == proxy.id) return &proxy;
    }
    return nullptr;
}

/// 是否是设置页认得的通道 id（`auto` / `direct` / `custom` / 任一内置代理）。
inline bool isKnownChannel(const std::string& id) {
    return id == kAuto || id == kDirect || id == kCustom || findProxy(id) != nullptr;
}

inline std::string trim(const std::string& raw) {
    std::size_t begin = 0;
    std::size_t end = raw.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(raw[begin]))) ++begin;
    while (end > begin && std::isspace(static_cast<unsigned char>(raw[end - 1]))) --end;
    return raw.substr(begin, end - begin);
}

inline bool hasScheme(const std::string& value) {
    const std::size_t scheme = value.find("://");
    return scheme != std::string::npos && scheme > 0;
}

/// 归一化用户输入的前缀。
///
/// 接受 `gh-proxy.org` / `https://gh-proxy.org/` / `https://example.com/gh/` 三种写法，
/// 统一成「带 scheme、无尾部斜杠」的前缀。**非法输入返回空串**（调用方据此回退到
/// 内置代理），不做「尽力猜」：把 `htps//x` 猜成可用地址只会让用户以为代理生效了。
inline std::string normalizePrefix(const std::string& raw) {
    std::string value = trim(raw);
    if (value.empty()) return std::string();
    // 任何空白都算非法（复制粘贴常带换行，宁可判非法也不用带空格的 URL 去发请求）。
    for (const char c : value) {
        if (std::isspace(static_cast<unsigned char>(c))) return std::string();
    }
    if (!hasScheme(value)) value = "https://" + value;
    const std::size_t schemeEnd = value.find("://");
    std::string scheme = value.substr(0, schemeEnd);
    std::transform(scheme.begin(), scheme.end(), scheme.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    if (scheme != "http" && scheme != "https") return std::string();
    const std::string host = value.substr(schemeEnd + 3);
    if (host.empty()) return std::string();
    // `/` 之后允许带路径（有些镜像挂在子路径上），但 `?` / `#` 会把拼出来的
    // `<前缀>/https://…` 变成查询串，必须拒掉。
    if (host.find('?') != std::string::npos || host.find('#') != std::string::npos) {
        return std::string();
    }
    while (!value.empty() && value.back() == '/') value.pop_back();
    if (value.size() <= scheme.size() + 3) return std::string(); // 只剩 scheme://
    return value;
}

/// 取前缀里的主机名（用于给「自定义代理」起一个稳定、可读的通道名）。
inline std::string prefixHost(const std::string& prefix) {
    const std::size_t schemeEnd = prefix.find("://");
    if (schemeEnd == std::string::npos) return prefix;
    const std::string rest = prefix.substr(schemeEnd + 3);
    const std::size_t slash = rest.find('/');
    return slash == std::string::npos ? rest : rest.substr(0, slash);
}

/// 加速代理的拼接形式就是 `<前缀>/<完整目标 URL>`（实测 gh-proxy 家族如此）。
inline std::string proxiedUrl(const std::string& prefix, const std::string& target) {
    return prefix + "/" + target;
}

struct Candidate {
    std::string url;
    std::string label;      ///< 通道名，成功后展示给用户（「直连」「gh-proxy.org」…）
    bool accelerator = false;
};

/// 按通道生成**有序**候选表。`target` 是完整目标地址（如 GitHub Releases API）。
///
/// - `auto`（默认）：直连优先，失败再依次走五个内置代理。
/// - `direct`：只直连。
/// - 内置代理 id：该代理优先 → 其余内置代理 → 直连兜底。
/// - `custom`：合法自定义前缀优先 → 内置代理 → 直连；前缀非法/为空时跳过它，
///   仍按「内置代理优先、直连兜底」的次序尝试（用户选了自定义却没填好，不该落到
///   「什么都检查不了」；但也不该悄悄变回直连优先 —— 他选自定义通常正因为直连不通）。
///
/// 重复出现的前缀会被去掉（自定义填的正好是某个内置代理时不该请求两遍）。
inline std::vector<Candidate> candidates(const std::string& channel, const std::string& customRaw,
                                        const std::string& target) {
    std::vector<Candidate> out;
    std::vector<std::string> seen;

    const auto addDirect = [&]() {
        if (std::find(seen.begin(), seen.end(), std::string("direct")) != seen.end()) return;
        seen.push_back("direct");
        out.push_back({target, "直连", false});
    };
    const auto addPrefix = [&](const std::string& prefix, const std::string& label) {
        if (prefix.empty()) return;
        if (std::find(seen.begin(), seen.end(), prefix) != seen.end()) return;
        seen.push_back(prefix);
        out.push_back({proxiedUrl(prefix, target), label, true});
    };
    const auto addBuiltins = [&](const std::string& skipId) {
        for (const Proxy& proxy : builtinProxies()) {
            if (skipId == proxy.id) continue;
            addPrefix(proxy.prefix, proxy.label);
        }
    };

    const std::string normalizedChannel = trim(channel);
    const std::string custom = normalizePrefix(customRaw);

    if (normalizedChannel == kDirect) {
        addDirect();
        return out;
    }
    if (const Proxy* chosen = findProxy(normalizedChannel)) {
        addPrefix(chosen->prefix, chosen->label);
        addBuiltins(chosen->id);
        addDirect();
        return out;
    }
    if (normalizedChannel == kCustom) {
        addPrefix(custom, "自定义代理 " + prefixHost(custom));
        addBuiltins(std::string());
        addDirect();
        return out;
    }
    // `auto` 与任何未知 id：直连优先，再走加速。
    addDirect();
    addBuiltins(std::string());
    return out;
}

} // namespace update_channel
} // namespace komira
