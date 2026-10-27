// core/logger.cpp — 轻量日志系统实现（附录 F.2.8）
// - 输出：%LOCALAPPDATA%/TextSearch/textsearch.log
// - 轮转：单文件超过 5MB 时滚动，保留 3 个历史文件（.log / .log.1 / .log.2 / .log.3）
// - 格式：[yyyy-MM-dd HH:mm:ss][thread_id][LEVEL] message
// - 线程安全：mutex 保护写入与轮转
#include "core/logger.h"
#include "core/util.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <cwchar>
#include <cwctype>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace logger {

namespace {

constexpr size_t kMaxFileBytes = 5ULL * 1024 * 1024;  // 5MB
constexpr int kMaxRotatedFiles = 3;                    // 保留 3 个历史文件

std::mutex g_mutex;
Level g_min_level = Level::Info;
std::filesystem::path g_log_path;
bool g_initialized = false;
bool g_enabled = true;        // B2 总开关（默认开）
bool g_redact_paths = true;   // B2 路径脱敏（默认开 → 安全默认）

// 格式化当前时间戳：yyyy-MM-dd HH:mm:ss
std::string format_timestamp() {
    using namespace std::chrono;
    auto now = system_clock::now();
    auto t = system_clock::to_time_t(now);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);          // Windows CRT（MinGW UCRT 可用）
#else
    localtime_r(&t, &tm);          // POSIX
#endif
    std::ostringstream ss;
    ss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
    return ss.str();
}

// 级别名称
const char* level_name(Level lvl) {
    switch (lvl) {
        case Level::Info:  return "INFO";
        case Level::Warn:  return "WARN";
        case Level::Error: return "ERROR";
    }
    return "INFO";
}

// 轮转：把 textsearch.log -> .log.1, .log.1 -> .log.2, ... 删除最旧的 .log.3
void rotate_files(const std::filesystem::path& base) {
    namespace fs = std::filesystem;
    std::error_code ec;
    // 从最旧开始删除，避免覆盖
    fs::path oldest = base;
    oldest += L"." + std::to_wstring(kMaxRotatedFiles);
    fs::remove(oldest, ec);

    for (int i = kMaxRotatedFiles - 1; i >= 1; --i) {
        fs::path src = base;
        src += L"." + std::to_wstring(i);
        fs::path dst = base;
        dst += L"." + std::to_wstring(i + 1);
        if (fs::exists(src, ec)) {
            fs::rename(src, dst, ec);
        }
    }
    // 当前文件 -> .log.1
    if (fs::exists(base, ec)) {
        fs::path first = base;
        first += L".1";
        fs::rename(base, first, ec);
    }
}

// ============================================================
// B2 路径脱敏
// ============================================================
// 日志的诊断价值主要来自「哪个文件出的问题」，但完整路径常含
// %USERPROFILE%（用户名）、客户名、项目代号等不想外泄的信息
// ——提 issue 贴日志就把这些一并交出去了。
// 折中：保留末两级（直接父目录 + 文件名），上层折叠成 ...。
// 既能定位「哪一个文件」，又抹掉「在谁家里、属于哪个项目」。

// 判断字符是否为路径终止符（含中英标点，中文正文常紧贴路径无空格）
bool is_path_delim(wchar_t c) {
    if (c == L'\0') return true;
    if (iswspace(c)) return true;
    return wcschr(L"\"'`<>|,;=()[]{}：，。；、（）【】《》「」", c) != nullptr;
}

bool ieq(const std::wstring& a, const wchar_t* b) {
    if (a.size() != wcslen(b)) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (towlower(a[i]) != towlower(b[i])) return false;
    }
    return true;
}

// 折叠单个路径 token（调用方已确认整串就是路径）
std::wstring fold_path(const std::wstring& p) {
    const bool unc = (p.size() >= 2 && p[0] == L'\\' && p[1] == L'\\');

    std::vector<std::wstring> segs;
    std::wstring cur;
    for (wchar_t c : p) {
        if (c == L'\\' || c == L'/') {
            if (!cur.empty()) { segs.push_back(cur); cur.clear(); }
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) segs.push_back(cur);
    // ≤3 段（如 C:\tmp\a.txt）没有可隐藏的上层信息，原样返回——
    // 脱敏成 ...\tmp\a.txt 反而丢了盘符却什么也没保护。
    if (segs.size() <= 3) return p;

    // 保留末 2 段
    size_t keep = segs.size() - 2;
    // 定向抹掉 C:\Users\<用户名>\...：用户名段必须一起去掉
    if (!unc && segs.size() >= 3 && ieq(segs[1], L"Users") && keep < 3) {
        keep = 3;
    }
    if (keep >= segs.size()) keep = segs.size() - 2;   // 保险（理论上到不了）

    std::wstring out = unc ? L"\\\\..." : L"...";
    for (size_t i = keep; i < segs.size(); ++i) {
        out += L'\\';
        out += segs[i];
    }
    return out;
}

// 扫描整条消息，把识别出的路径片段替换成折叠形式。
// 识别规则（够用即可，脱敏是"降低暴露"而非安全边界）：
//   - 盘符绝对路径 X:\... 或 X:/...
//   - UNC 路径 \\server\share\...
// 相对路径（src\main.cpp）不含盘符/UNC 前缀，识别不到 → 原样保留。
std::wstring scrub_paths(const std::wstring& in) {
    std::wstring out;
    out.reserve(in.size());
    const size_t n = in.size();
    size_t i = 0;
    while (i < n) {
        const bool drive_path =
            i + 2 < n && iswalpha(static_cast<wint_t>(in[i])) &&
            in[i + 1] == L':' && (in[i + 2] == L'\\' || in[i + 2] == L'/');
        const bool unc_path =
            i + 1 < n && in[i] == L'\\' && in[i + 1] == L'\\';
        if (!drive_path && !unc_path) {
            out += in[i++];
            continue;
        }
        size_t j = i;
        while (j < n && !is_path_delim(in[j])) ++j;
        if (j == n && j > i && in[j - 1] == L'\\') --j;   // 末尾的 \ 不算路径内容
        out += fold_path(in.substr(i, j - i));
        i = j;
    }
    return out;
}

} // namespace

void init() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_initialized) return;
    g_log_path = get_app_data_dir() / L"textsearch.log";
    std::error_code ec;
    std::filesystem::create_directories(get_app_data_dir(), ec);
    g_initialized = true;
}

void set_min_level(Level lvl) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_min_level = lvl;
}

void set_enabled(bool on) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_enabled = on;
}

bool is_enabled() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_enabled;
}

void set_redact_paths(bool on) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_redact_paths = on;
}

bool redact_paths() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_redact_paths;
}

void set_log_path(const std::filesystem::path& p) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_log_path = p;
    g_initialized = true;
}

std::wstring redact_path(const std::filesystem::path& p) {
    if (!p.empty()) {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_redact_paths) return p.wstring();
    }
    return fold_path(p.wstring());
}

std::filesystem::path log_path() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_initialized) {
        g_log_path = get_app_data_dir() / L"textsearch.log";
    }
    return g_log_path;
}

void log(Level lvl, const std::wstring& msg) {
    std::filesystem::path path;
    std::wstring body = msg;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_enabled) return;
        if (static_cast<int>(lvl) < static_cast<int>(g_min_level)) return;
        if (!g_initialized) {
            g_log_path = get_app_data_dir() / L"textsearch.log";
            g_initialized = true;
        }
        // B2：落盘前统一脱敏。放在 logger 层而不是各调用点，是为了让
        // 「以后新加的日志」也自动受保护——调用方不需要记得手动套 redact_path。
        if (g_redact_paths) body = scrub_paths(msg);
        path = g_log_path;
    }

    // 组装日志行（UTF-8）
    std::string ts = format_timestamp();
    std::thread::id tid = std::this_thread::get_id();
    std::ostringstream tid_ss;
    tid_ss << tid;
    std::string utf8_msg = wstring_to_utf8(body);
    std::string line = "[" + ts + "][" + tid_ss.str() + "][" +
                       level_name(lvl) + "] " + utf8_msg + "\n";

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        namespace fs = std::filesystem;
        std::error_code ec;
        // 检查是否需要轮转
        if (fs::exists(path, ec)) {
            auto sz = fs::file_size(path, ec);
            if (!ec && sz >= kMaxFileBytes) {
                rotate_files(path);
            }
        }
        // 追加写入
        std::ofstream ofs(path, std::ios::binary | std::ios::app);
        if (ofs) {
            ofs.write(line.data(), static_cast<std::streamsize>(line.size()));
        }
    }
}

void info(const std::wstring& msg) { log(Level::Info, msg); }
void warn(const std::wstring& msg) { log(Level::Warn, msg); }
void error(const std::wstring& msg) { log(Level::Error, msg); }

} // namespace logger
