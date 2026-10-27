// cli/main.cpp — textsearch-cli 跨平台命令行入口（C2）
//
// 复用 core/ 全部能力（扫描/索引/增量更新），无 GUI 依赖。
// core 的平台差异已在 core/port/ 收口（C1）——非 Windows 平台接 port_stub
// 即可编译（GBK 编码支持暂为移植层 stub 限制）。
//
// 用法：
//   textsearch-cli search <目录> <关键词> [选项]
//   textsearch-cli build   <目录> [选项]
//   textsearch-cli update  <目录>
//
// search 选项：
//   --regex          关键词按 ECMAScript 正则解释
//   --case           区分大小写（默认忽略）
//   --word           全词匹配
//   --no-recursive   不递归子目录
//   --realtime       强制实时扫描（跳过索引，用于两模式结果比对）
//   --ext=.c,.h      扩展名过滤（逗号分隔；缺省 = 常用扩展名全集）
//   --max=N          结果上限（默认 100000，同 GUI）
//   --threshold=N    大文件流式阈值 MB（默认 50）
//   --json           JSON 输出（默认文本行 path:line:col:\ttext）
// build 选项：
//   --ext=... / --threshold=N  同上
//
// 输出为 UTF-8 字节（管道/重定向友好）；Windows 控制台显示中文请先
// `chcp 65001` 或使用 Windows Terminal。诊断/进度/摘要走 stderr。
// 退出码：0 = 正常执行（含 0 命中），1 = 用法/路径/正则错误。
#include "core/scanner.h"
#include "core/indexer.h"
#include "core/extension_manager.h"
#include "core/matcher.h"
#include "core/util.h"
#include "core/logger.h"

#include <chrono>
#include <cstdio>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#ifdef _WIN32
#include <io.h>
#define TS_ISATTY _isatty
#define TS_FILENO _fileno
#else
#include <unistd.h>
#define TS_ISATTY isatty
#define TS_FILENO fileno
#endif

namespace {

// stderr 是否为交互终端（非终端——重定向/管道——时抑制 \r 进度刷新）
bool stderr_is_tty() {
    return TS_ISATTY(TS_FILENO(stderr)) != 0;
}

void print_usage() {
    std::fputs(
        "TextSearch CLI —— 全文搜索（实时扫描 / 倒排索引双模式，CJK 一等公民）\n"
        "\n"
        "用法:\n"
        "  textsearch-cli search <目录> <关键词> [选项]\n"
        "  textsearch-cli build   <目录> [选项]\n"
        "  textsearch-cli update  <目录>\n"
        "\n"
        "search 选项:\n"
        "  --regex          正则模式（ECMAScript）\n"
        "  --case           区分大小写（默认忽略）\n"
        "  --word           全词匹配\n"
        "  --no-recursive   不递归子目录\n"
        "  --realtime       强制实时扫描（跳过索引）\n"
        "  --ext=.c,.h      扩展名过滤（缺省 = 常用扩展名全集）\n"
        "  --max=N          结果上限（默认 100000）\n"
        "  --threshold=N    大文件流式阈值 MB（默认 50）\n"
        "  --json           JSON 输出（默认 path:line:col:\\ttext）\n"
        "build 选项:\n"
        "  --ext=... / --threshold=N  同上\n"
        "\n"
        "输出为 UTF-8。诊断与摘要走 stderr；退出码 0=正常，1=错误。\n",
        stderr);
}

// --ext=a,b,c → {".a",".b",".c"}（小写 + 前导点，与 GUI/索引口径一致）
std::set<std::wstring> parse_ext_list(const std::wstring& val) {
    std::set<std::wstring> out;
    std::wstring cur;
    for (wchar_t c : val) {
        if (c == L',' || c == L';') {
            if (!cur.empty()) out.insert(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) out.insert(cur);
    std::set<std::wstring> norm;
    for (auto e : out) {
        if (!e.empty() && e[0] != L'.') e = L"." + e;
        norm.insert(to_lower_wstring(e));
    }
    return norm;
}

// stdout 写 UTF-8（管道/重定向友好；Windows 控制台显示需 chcp 65001）
void out_utf8(const std::wstring& w) {
    std::string u = wstring_to_utf8(w);
    std::fwrite(u.data(), 1, u.size(), stdout);
}

// stderr 写 UTF-8。⚠️ 不用 fwprintf：%ls 在 C locale 下会把中文转成 '?'，
// 必须走 wstring_to_utf8 后按字节写。
void err_utf8(const std::wstring& w) {
    std::string u = wstring_to_utf8(w);
    std::fwrite(u.data(), 1, u.size(), stderr);
}

std::wstring json_escape(const std::wstring& s) {
    std::wstring out;
    for (wchar_t c : s) {
        switch (c) {
            case L'"':  out += L"\\\""; break;
            case L'\\': out += L"\\\\"; break;
            case L'\b': out += L"\\b";  break;
            case L'\f': out += L"\\f";  break;
            case L'\n': out += L"\\n";  break;
            case L'\r': out += L"\\r";  break;
            case L'\t': out += L"\\t";  break;
            default:
                if (c < 0x20) {
                    wchar_t buf[8];
                    swprintf(buf, 8, L"\\u%04x", static_cast<unsigned>(c));
                    out += buf;
                } else {
                    out.push_back(c);
                }
        }
    }
    return out;
}

int cmd_search(const std::vector<std::wstring>& args) {
    if (args.size() < 2) {
        std::fputs("错误：search 需要 <目录> <关键词>\n", stderr);
        return 1;
    }
    SearchOptions opts;
    opts.root = args[0];
    opts.pattern = args[1];
    bool json = false;
    std::set<std::wstring> ext_set;   // 空 = 未指定 → 常用扩展名全集

    for (size_t i = 2; i < args.size(); ++i) {
        const std::wstring& a = args[i];
        if (a == L"--regex")        opts.use_regex = true;
        else if (a == L"--case")    opts.ignore_case = false;
        else if (a == L"--word")    opts.whole_word = true;
        else if (a == L"--no-recursive") opts.recursive = false;
        else if (a == L"--realtime")     opts.force_realtime = true;
        else if (a == L"--json")         json = true;
        else if (a.rfind(L"--ext=", 0) == 0)
            ext_set = parse_ext_list(a.substr(6));
        else if (a.rfind(L"--max=", 0) == 0)
            opts.max_results = static_cast<size_t>(std::stoul(a.substr(6)));
        else if (a.rfind(L"--threshold=", 0) == 0)
            opts.bigfile_threshold_mb = std::stoul(a.substr(12));
        else {
            err_utf8(L"错误：未知选项 " + a + L"\n");
            return 1;
        }
    }
    if (ext_set.empty()) ext_set = default_whitelist();

    // 前置校验（口径同 GUI）
    std::error_code ec;
    if (!std::filesystem::is_directory(opts.root, ec)) {
        std::fputs("错误：目录不存在或不是目录\n", stderr);
        return 1;
    }
    if (opts.pattern.empty()) {
        std::fputs("错误：关键词为空\n", stderr);
        return 1;
    }
    if (opts.use_regex) {
        RegexCheckResult rc = check_regex(opts.pattern, opts.ignore_case);
        if (!rc.valid) {
            err_utf8(L"错误：正则语法错误：" + rc.message + L"\n");
            return 1;
        }
        if (rc.redos_warning) {
            err_utf8(L"警告：" + rc.message + L"\n");
        }
    }

    // 模式预判（与 core/scanner.cpp 路由同口径；仅用于 stderr 摘要）
    bool use_index = !opts.force_realtime && Indexer::index_exists(opts.root);

    std::vector<MatchResult> results;
    std::mutex mu;
    size_t skipped = 0;
    auto t0 = std::chrono::steady_clock::now();

    Scanner scanner;
    ScanCallbacks cb;
    const bool progress_tty = stderr_is_tty();
    cb.on_progress = [progress_tty](size_t done, size_t total) {
        if (progress_tty) {
            std::fprintf(stderr, "\r%zu / %zu 文件", done, total);
            std::fflush(stderr);
        }
    };
    cb.on_results = [&](std::vector<MatchResult> batch) {
        std::lock_guard<std::mutex> lk(mu);
        for (auto& r : batch) results.push_back(std::move(r));
    };
    cb.on_skip = [&](size_t s) {
        std::lock_guard<std::mutex> lk(mu);
        skipped = s;
    };
    cb.on_error = [](const std::wstring& msg) {
        err_utf8(L"\n" + msg + L"\n");
    };
    scanner.scan_realtime(opts, ext_set, cb);
    if (progress_tty) std::fputs("\r\33[K", stderr);   // 清掉进度行

    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();

    if (json) {
        std::fputs("[", stdout);
        for (size_t i = 0; i < results.size(); ++i) {
            const auto& r = results[i];
            std::fputs(i ? ",\n" : "\n", stdout);
            std::fputs("  {\"file\": \"", stdout);
            out_utf8(json_escape(r.file_path.wstring()));
            std::fputs("\", \"line\": ", stdout);
            std::fprintf(stdout, "%d", r.line_no);
            std::fputs(", \"col\": ", stdout);
            std::fprintf(stdout, "%d", r.col);
            std::fputs(", \"text\": \"", stdout);
            out_utf8(json_escape(r.line_text));
            std::fputs("\"}", stdout);
        }
        std::fputs(results.empty() ? "]\n" : "\n]\n", stdout);
    } else {
        for (const auto& r : results) {
            out_utf8(r.file_path.wstring());
            std::fprintf(stdout, ":%d:%d:\t", r.line_no, r.col);
            out_utf8(r.line_text);
            std::fputc('\n', stdout);
        }
    }

    err_utf8(L"摘要：" + std::to_wstring(results.size()) + L" 条命中，" +
             (use_index ? L"索引模式" : L"实时扫描") + L"，耗时 " +
             std::to_wstring(static_cast<long long>(ms)) + L" ms" +
             (skipped > 0 ? L"（跳过 " + std::to_wstring(skipped) +
                           L" 个文件）" : std::wstring()) + L"\n");
    return 0;
}

int cmd_build(const std::vector<std::wstring>& args) {
    if (args.empty()) {
        std::fputs("错误：build 需要 <目录>\n", stderr);
        return 1;
    }
    std::filesystem::path root = args[0];
    std::set<std::wstring> ext_set;
    uint64_t threshold_mb = 50;

    for (size_t i = 1; i < args.size(); ++i) {
        const std::wstring& a = args[i];
        if (a.rfind(L"--ext=", 0) == 0)
            ext_set = parse_ext_list(a.substr(6));
        else if (a.rfind(L"--threshold=", 0) == 0)
            threshold_mb = std::stoul(a.substr(12));
        else {
            err_utf8(L"错误：未知选项 " + a + L"\n");
            return 1;
        }
    }
    if (ext_set.empty()) ext_set = default_whitelist();

    std::error_code ec;
    if (!std::filesystem::is_directory(root, ec)) {
        std::fputs("错误：目录不存在或不是目录\n", stderr);
        return 1;
    }

    Indexer idx;
    idx.set_bigfile_threshold_mb(threshold_mb);
    auto t0 = std::chrono::steady_clock::now();
    const bool build_progress_tty = stderr_is_tty();
    bool ok = idx.build(root, ext_set, [build_progress_tty](size_t done, size_t total) {
        if (build_progress_tty) {
            std::fprintf(stderr, "\r建索引 %zu / %zu 文件", done, total);
            std::fflush(stderr);
        }
    });
    if (build_progress_tty) std::fputs("\r\33[K", stderr);
    if (!ok) {
        std::fputs("建立索引失败：磁盘不可写/空间不足/权限不足\n", stderr);
        return 1;
    }
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    err_utf8(L"索引已建立：" + (root / L".textsearch").wstring() +
             L"（" + std::to_wstring(ext_set.size()) + L" 种扩展名，耗时 " +
             std::to_wstring(static_cast<long long>(ms)) + L" ms）\n");
    return 0;
}

int cmd_update(const std::vector<std::wstring>& args) {
    if (args.empty()) {
        std::fputs("错误：update 需要 <目录>\n", stderr);
        return 1;
    }
    std::filesystem::path root = args[0];
    if (!Indexer::index_exists(root)) {
        std::fputs("错误：该目录尚未建立索引（先执行 build）\n", stderr);
        return 1;
    }
    Indexer idx;
    if (!idx.open(root)) {
        std::fputs("错误：索引打开失败（可能已损坏，请重建）\n", stderr);
        return 1;
    }
    size_t changed = 0;
    if (!idx.update_incremental(&changed)) {
        std::fputs("增量更新失败\n", stderr);
        return 1;
    }
    err_utf8(L"增量更新完成：" + std::to_wstring(changed) + L" 个变更文件\n");
    return 0;
}

}  // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
#else
int main(int argc, char** argv) {
#endif
    // CLI 默认不写日志文件（GUI 才写 %LOCALAPPDATA%/TextSearch/textsearch.log）；
    // 关键词在 GUI 侧本就永不落盘，此处同样不落盘。
    logger::set_enabled(false);

    std::vector<std::wstring> args;
#ifdef _WIN32
    for (int i = 1; i < argc; ++i) args.push_back(argv[i]);
#else
    for (int i = 1; i < argc; ++i) {
        // POSIX：按本地活跃代码页无脑转宽字符并不严谨；这里以 UTF-8 解释
        // （port_stub 的转换层同口径）。中文参数请确保终端为 UTF-8。
        args.push_back(utf8_to_wstring(argv[i]));
    }
#endif
    if (args.empty()) {
        print_usage();
        return 1;
    }
    const std::wstring& cmd = args[0];
    std::vector<std::wstring> rest(args.begin() + 1, args.end());
    try {
        if (cmd == L"search") return cmd_search(rest);
        if (cmd == L"build")  return cmd_build(rest);
        if (cmd == L"update") return cmd_update(rest);
        if (cmd == L"--help" || cmd == L"-h" || cmd == L"help") {
            print_usage();
            return 0;
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "错误：%s\n", e.what());
        return 1;
    }
    err_utf8(L"错误：未知子命令 " + cmd + L"\n");
    print_usage();
    return 1;
}
