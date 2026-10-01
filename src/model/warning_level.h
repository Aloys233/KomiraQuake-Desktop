#pragma once

namespace komira {

/// 预警级别。《NATIVE_PORT_SPEC》 §1.2。
enum class WarningLevel { Normal = 0, Watch = 1, Warning = 2, Critical = 3 };

inline int warningCode(WarningLevel level) { return static_cast<int>(level); }

inline const char* warningTag(WarningLevel level) {
    switch (level) {
    case WarningLevel::Normal: return "NORMAL";
    case WarningLevel::Watch: return "WATCH";
    case WarningLevel::Warning: return "WARNING";
    case WarningLevel::Critical: return "CRITICAL";
    }
    return "NORMAL";
}

inline const char* warningDisplayName(WarningLevel level) {
    switch (level) {
    case WarningLevel::Normal: return "守候正常";
    case WarningLevel::Watch: return "微震关注";
    case WarningLevel::Warning: return "地震预警";
    case WarningLevel::Critical: return "严重预警";
    }
    return "守候正常";
}

inline bool isAlertLevel(WarningLevel level) {
    return level == WarningLevel::Warning || level == WarningLevel::Critical;
}

} // namespace komira
