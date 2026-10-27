// core/logger.h — 轻量日志系统（附录 F.2.8）
// 线程安全、支持轮转的极简 logger，零外部依赖。
#pragma once

#include <filesystem>
#include <string>

namespace logger {

// 日志级别
enum class Level {
    Info,
    Warn,
    Error,
};

// 初始化日志系统（确定日志目录，创建目录）。
// 日志文件位于 %LOCALAPPDATA%/TextSearch/textsearch.log。
// 多次调用安全（幂等）。
void init();

// 写一条日志（线程安全）。
// - lvl: 日志级别
// - msg: 日志消息（宽字符，内部转 UTF-8 写入）
void log(Level lvl, const std::wstring& msg);

// 便捷接口
void info(const std::wstring& msg);
void warn(const std::wstring& msg);
void error(const std::wstring& msg);

// 设置最小输出级别（低于此级别的日志被丢弃），默认 Info。
void set_min_level(Level lvl);

// ---- B2 隐私开关 ----

// 总开关：false 时 log() 直接返回，一个字节都不落盘。默认 true。
// 关掉后日志完全停止（排查问题时会失去线索，仅在高度敏感场景下使用）。
void set_enabled(bool on);
bool is_enabled();

// 路径脱敏：true（默认）时写入日志前把消息里的文件/目录路径折叠成
// 「...\末级目录\文件名」，抹掉上层目录（通常含用户名、项目名、客户名）。
// 关键词从不落盘（见 main_window / config 里的 B2 注释），此项只针对路径。
void set_redact_paths(bool on);
bool redact_paths();

// 脱敏单个路径（供调用方在明确知道某个字符串是路径时主动套用）。
// 例：C:\Users\alice\Documents\secret\notes.txt → ...\secret\notes.txt
// 路径过短（≤3 段，如 C:\tmp\a.txt）时原样返回——本来也没什么可隐藏的。
std::wstring redact_path(const std::filesystem::path& p);

// 当前日志文件路径（供 UI / 调试查看）。
std::filesystem::path log_path();

// 覆盖日志文件路径（便携模式 / 单测用）。init() 之后调用同样有效。
void set_log_path(const std::filesystem::path& p);

} // namespace logger
