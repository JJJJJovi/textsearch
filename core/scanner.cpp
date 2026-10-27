// core/scanner.cpp — 扫描引擎实现（即时模式 T4a）
// 文件窗口映射经 core/port/port_mmap（C1），本文件无平台头依赖。
#include "core/scanner.h"
#include "core/threadpool.h"
#include "core/filesystem.h"
#include "core/encoding.h"
#include "core/text_detect.h"
#include "core/matcher.h"
#include "core/util.h"
#include "core/logger.h"
#include "core/indexer.h"  // T4b 索引路由
#include "core/port/port_mmap.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
#include <mutex>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

// ---- 常量 ----

// MMF 窗口大小（附录 F.2.6：64MB 滑动映射）
static constexpr size_t MMF_WINDOW_SIZE = 64ULL * 1024 * 1024;

// 单行长度 cap（附录 F.2.6：超长行 >16MB 按窗口切片）
// S5.3：改为可注入变量（默认 16MB 不变），便于用小常量做切片去重测试。
// 非线程安全：仅允许在无扫描运行时（测试 setup）调用 setter。
static size_t g_max_line_bytes = 16ULL * 1024 * 1024;

void set_max_line_bytes(size_t n) { g_max_line_bytes = n > 0 ? n : 1; }
size_t get_max_line_bytes() { return g_max_line_bytes; }

// 结果批次大小（附录 F.2.9：每 100 条或每文件一批）
static constexpr size_t BATCH_SIZE = 100;

// 二进制探测缓冲区大小
static constexpr size_t HEAD_BUF_SIZE = 8192;

// B3 告警里最多列举几种「不在索引内」的扩展名（避免刷屏）
static constexpr size_t MAX_EXT_HINT_LIST = 3;

// ---- 文件内辅助 ----

// 扩展名小写化（与 filesystem.cpp 的 ext_to_lower 保持同一比较口径）。
// 索引里存的是建库时的集合，用户本次勾选的是另一份集合，两边都可能大小写不一，
// 比较前统一小写，否则 .TXT 会被误判为「不在索引内」。
static std::wstring lower_ext(std::wstring e) {
    for (auto& c : e) {
        if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
    }
    return e;
}

// B3：勾选了索引中不存在的扩展名 → 静默少结果，用户会误判为「没有匹配」。
// 索引在建库时把扩展名集合写进了 meta.ext_set，此处与本次勾选集合比对，
// 把「勾选了但索引里没有」的类型明确报出来。
// 触发条件刻意保守：任一集合为空（无法判定）时直接不告警，避免误报。
static void warn_unindexed_extensions(const Indexer& idx,
                                      const std::set<std::wstring>& ext_set,
                                      ScanCallbacks& callbacks) {
    if (!callbacks.on_error) return;
    if (ext_set.empty()) return;   // 空集 = 不过滤，无从判定

    std::set<std::wstring> built = idx.indexed_ext_set();
    if (built.empty()) return;     // 老索引无 meta.ext_set → 无法判定，不打扰

    std::set<std::wstring> built_lower;
    for (const auto& e : built) built_lower.insert(lower_ext(e));

    std::vector<std::wstring> missing;
    for (const auto& e : ext_set) {
        if (built_lower.count(lower_ext(e)) == 0) missing.push_back(e);
    }
    if (missing.empty()) return;

    std::wstring msg = L"警告: 勾选的 " + std::to_wstring(ext_set.size()) +
                       L" 种类型中有 " + std::to_wstring(missing.size()) +
                       L" 种不在索引内（";
    for (size_t i = 0; i < missing.size(); ++i) {
        if (i >= MAX_EXT_HINT_LIST) {
            msg += L" 等";
            break;
        }
        if (i > 0) msg += L"、";
        msg += missing[i];
    }
    msg += L"），这些文件搜不到；如需包含请重建索引";

    logger::warn(msg);
    callbacks.on_error(msg);
}

// ---- Scanner 构造/析构 ----

Scanner::Scanner()
    : running_(false), cancelled_(false) {}

Scanner::~Scanner() {
    cancel();
}

bool Scanner::is_running() const {
    return running_.load(std::memory_order_relaxed);
}

void Scanner::cancel() {
    cancelled_.store(true, std::memory_order_relaxed);
    // 持锁访问 pool_：cancel 可能在 worker 线程创建/reset pool_ 的同一瞬间被调用
    std::lock_guard<std::mutex> lk(pool_mutex_);
    if (pool_) {
        pool_->cancel();
    }
}

// ---- 批次刷新 ----

void Scanner::flush_batch(std::vector<MatchResult>& batch,
                           std::mutex& batch_mutex,
                           const ScanCallbacks& callbacks) {
    std::vector<MatchResult> to_send;
    {
        std::lock_guard<std::mutex> lock(batch_mutex);
        if (batch.empty()) return;
        to_send.swap(batch);
    }
    if (callbacks.on_results) {
        callbacks.on_results(std::move(to_send));
    }
}

// ---- 小文件全量匹配 ----

std::vector<MatchResult>
Scanner::scan_small_file(const std::filesystem::path& path,
                          const SearchOptions& opts) {
    // read_text 内部已做编码识别 + 解码（R6：二进制模式读）
    std::wstring text = read_text(path);
    if (text.empty()) return {};

    // 构建 MatchFlags
    int flags = MatchFlags::Substring;
    if (opts.use_regex) flags |= MatchFlags::Regex;
    if (opts.ignore_case) flags |= MatchFlags::IgnoreCase;
    if (opts.whole_word) flags |= MatchFlags::WholeWord;

    // max_results 传给 matcher，由 matcher 达到上限即停止
    // 注意：这里传的是单文件上限，全局上限在 process_file 中再裁剪
    // S4：正则模式透传单文件超时（matcher 内逐行检查，超时返回已得结果）
    double timeout = (opts.use_regex && opts.search_timeout_sec > 0)
                         ? static_cast<double>(opts.search_timeout_sec) : 0.0;
    return search_in_text(text, opts.pattern, flags, path, opts.max_results,
                          timeout);
}

// ---- 大文件内存映射流式匹配（F.2.6）----

// 字节级查找最后一个 0x0A（\n），返回其位置；找不到返回 npos
// 0x0A 在 UTF-8 和 GBK 中都不是多字节字符的一部分，可安全按字节切分（R6/R15）
static size_t find_last_newline(const char* data, size_t len) {
    for (size_t i = len; i > 0; --i) {
        if (data[i - 1] == '\n') return i - 1;
    }
    return std::string::npos;
}

// 对单行文本做匹配（复用 search_in_text），修正行号
// timeout_sec：S4 单文件正则超时的「剩余预算」（秒）。
// 0 = 不限；<0 = 文件级 deadline 已过 → 直接跳过本行。
static void match_single_line(
    const std::wstring& line, int actual_line,
    const std::filesystem::path& path,
    const SearchOptions& opts, int flags,
    std::vector<MatchResult>& results,
    const std::atomic<bool>& cancelled_flag,
    double timeout_sec = 0.0) {

    if (cancelled_flag.load(std::memory_order_relaxed)) return;
    if (results.size() >= opts.max_results) return;
    if (timeout_sec < 0.0) return;   // 单文件正则超时已到（S4）

    // 超长行切片可能耗时超过剩余预算：切片循环内也用同一截止点
    bool slice_deadline_on = timeout_sec > 0.0;
    std::chrono::steady_clock::time_point slice_deadline;
    if (slice_deadline_on)
        slice_deadline = std::chrono::steady_clock::now() +
            std::chrono::microseconds(static_cast<long long>(timeout_sec * 1e6));

    // 附录 F.2.6：单行 cap，超长行（>16MB 字符）按窗口切片
    if (line.size() > g_max_line_bytes) {
        size_t pattern_chars = opts.pattern.size();
        size_t slice_size = g_max_line_bytes;
        size_t slice_overlap = (pattern_chars > 0) ? (pattern_chars - 1) : 0;
        if (slice_overlap >= slice_size) slice_overlap = 0;

        // S5.3：重叠切片去重——切片按 overlap 前进，同一起始位置可能在相邻
        // 两片各命中一次（短匹配 ≤ overlap 时）。只报比上一片更新的位置。
        long long last_reported_col = 0;   // 1-indexed，0 = 尚未报过

        for (size_t s = 0; s < line.size(); s += (slice_size - slice_overlap)) {
            if (cancelled_flag.load(std::memory_order_relaxed)) return;
            if (slice_deadline_on &&
                std::chrono::steady_clock::now() >= slice_deadline)
                return;   // S4：正则超时，返回已得部分结果
            std::wstring slice = line.substr(s, slice_size);

            std::vector<MatchResult> slice_hits;
            if (opts.use_regex) {
                try {
                    auto re_flags = std::regex_constants::ECMAScript;
                    if (opts.ignore_case) re_flags |= std::regex_constants::icase;
                    std::wregex re(opts.pattern, re_flags);
                    std::wsmatch m;
                    auto it = slice.cbegin();
                    while (it != slice.cend() &&
                           std::regex_search(it, slice.cend(), m, re)) {
                        size_t pos = static_cast<size_t>(m[0].first - slice.cbegin());
                        int col = static_cast<int>(s + pos) + 1;
                        slice_hits.push_back({actual_line, col, line, path});
                        it = m[0].second;
                        if (m[0].first == m[0].second) ++it;
                    }
                } catch (...) {
                    // 正则编译失败等 → 跳过该切片
                }
            } else {
                std::wstring pat = opts.ignore_case ? to_lower_wstring(opts.pattern) : opts.pattern;
                const std::wstring& search_line = opts.ignore_case ? to_lower_wstring(slice) : slice;
                size_t pos = 0;
                while (pos < search_line.size()) {
                    pos = search_line.find(pat, pos);
                    if (pos == std::wstring::npos) break;
                    int col = static_cast<int>(s + pos) + 1;
                    bool ok = true;
                    if (opts.whole_word) {
                        if (s + pos > 0 && is_word_char(line[s + pos - 1])) ok = false;
                        size_t after = s + pos + pat.size();
                        if (after < line.size() && is_word_char(line[after])) ok = false;
                    }
                    if (ok) slice_hits.push_back({actual_line, col, line, path});
                    pos += pat.size();
                    if (pat.empty()) break;
                }
            }
            for (auto& h : slice_hits) {
                if (static_cast<long long>(h.col) <= last_reported_col)
                    continue;   // S5.3：上一片已报过该位置
                last_reported_col = h.col;
                results.push_back(std::move(h));
                if (results.size() >= opts.max_results) return;
            }
            if (slice_overlap == 0) break;
        }
    } else {
        // 普通行：直接用 search_in_text 对该单行匹配
        std::wstring single = line + L"\n";
        auto hits = search_in_text(single, opts.pattern, flags, path,
                                   opts.max_results - results.size(),
                                   timeout_sec);
        for (auto& h : hits) {
            h.line_no = actual_line;
            h.line_text = line;
            results.push_back(std::move(h));
            if (results.size() >= opts.max_results) return;
        }
    }
}

// 大文件内存映射流式匹配
// F.2.6：64MB 窗口滑动映射，按字节级 \n 切行（避免多字节字符跨窗口拆分）
// A-023：MMF 失败回退 read_text
std::vector<MatchResult>
Scanner::scan_big_file(const std::filesystem::path& path,
                        const SearchOptions& opts) {
    std::vector<MatchResult> results;

    // 构建 MatchFlags
    int flags = MatchFlags::Substring;
    if (opts.use_regex) flags |= MatchFlags::Regex;
    if (opts.ignore_case) flags |= MatchFlags::IgnoreCase;
    if (opts.whole_word) flags |= MatchFlags::WholeWord;

    // S4：单文件正则 deadline——逐行扣减剩余预算，过期后剩余行直接跳过
    // （match_single_line 收到负值 = 已过期）。返回已得的部分结果。
    const bool use_deadline = opts.use_regex && opts.search_timeout_sec > 0;
    std::chrono::steady_clock::time_point file_deadline;
    if (use_deadline)
        file_deadline = std::chrono::steady_clock::now() +
                        std::chrono::seconds(opts.search_timeout_sec);
    auto line_timeout_sec = [&]() -> double {
        if (!use_deadline) return 0.0;
        auto now = std::chrono::steady_clock::now();
        if (now >= file_deadline) return -1.0;
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            file_deadline - now).count();
        return ms > 0 ? static_cast<double>(ms) / 1000.0 : 0.001;
    };

    // 先检测编码（读前 8KB）
    std::vector<unsigned char> head = read_head_bytes(path, HEAD_BUF_SIZE);
    Encoding file_enc = detect_encoding(head);

    // UTF-16 文件：字节级 \n 切分不适用，回退 read_text（A-023 思路）
    // UTF-16 大文件较少见（目标库 94.8% GBK），回退可接受
    if (file_enc == Encoding::Utf16Le || file_enc == Encoding::Utf16Be) {
        return scan_small_file(path, opts);
    }

    // A-023：MMF 失败回退普通 read_text（窗口映射经 core/port，Windows=MMF）
    port::SlidingFileMap sfm;
    if (!sfm.open(path)) {
        return scan_small_file(path, opts);
    }

    // 获取文件大小
    uint64_t file_size = sfm.size();

    // 保险：小文件不应到这里
    if (file_size <= opts.bigfile_threshold_mb * 1024 * 1024) {
        return scan_small_file(path, opts);
    }

    // 窗口滑动（F.2.6：64MB 窗口）
    // 关键设计：按字节级 0x0A 切行，仅在 \n 边界处解码，
    // 避免多字节字符在窗口边界被拆分（UTF-8/GBK 的 0x0A 永远是单字节 \n，R6/R15）
    uint64_t offset = 0;
    // 上一窗口末尾未以 \n 结束的原始字节，拼到下一窗口前
    std::string prev_tail;
    // 行号累计（跨窗口连续）
    int line_no_offset = 0;

    while (offset < file_size) {
        if (cancelled_.load(std::memory_order_relaxed)) break;

        size_t map_size = static_cast<size_t>(
            std::min(static_cast<uint64_t>(MMF_WINDOW_SIZE),
                     file_size - offset));

        const unsigned char* window = sfm.map(offset, map_size);
        if (window == nullptr) break;

        // 拼接 prev_tail + 本窗口原始字节
        const char* data = reinterpret_cast<const char*>(window);
        std::string chunk;
        chunk.reserve(prev_tail.size() + map_size);
        chunk.append(prev_tail);
        chunk.append(data, map_size);
        // 下一次 sfm.map()/析构自动释放本窗口

        // 查找最后一个 \n 的字节位置（安全边界）
        size_t last_nl = find_last_newline(chunk.data(), chunk.size());

        if (last_nl == std::string::npos) {
            // 本窗口无 \n → 整个 chunk 是未完成行，保留为 prev_tail
            // 防止 prev_tail 无限增长：超过窗口大小则强制解码处理
            if (chunk.size() >= MMF_WINDOW_SIZE) {
                // 整个窗口无换行 → 解码并作为单行处理
                std::vector<unsigned char> chunk_bytes(chunk.begin(), chunk.end());
                std::wstring wtext = decode_bytes(chunk_bytes, file_enc);
                if (!wtext.empty()) {
                    // 去除行尾 \r
                    if (!wtext.empty() && wtext.back() == L'\r') wtext.pop_back();
                    match_single_line(wtext, line_no_offset + 1, path, opts, flags,
                                      results, cancelled_, line_timeout_sec());
                    ++line_no_offset;
                }
                prev_tail.clear();
            } else {
                prev_tail = std::move(chunk);
            }
            offset += map_size;
            continue;
        }

        // 解码 up to last_nl+1（含 \n）→ 完整的行集合
        size_t decode_len = last_nl + 1;
        std::vector<unsigned char> decode_bytes_vec(
            chunk.begin(), chunk.begin() + decode_len);
        std::wstring wtext = decode_bytes(decode_bytes_vec, file_enc);

        // 按 \n 切行
        std::vector<std::wstring> lines;
        size_t start = 0;
        for (size_t i = 0; i < wtext.size(); ++i) {
            if (wtext[i] == L'\n') {
                std::wstring line = wtext.substr(start, i - start);
                if (!line.empty() && line.back() == L'\r') line.pop_back();
                lines.push_back(std::move(line));
                start = i + 1;
            }
        }

        // 对每个完整行做匹配
        for (size_t li = 0; li < lines.size(); ++li) {
            if (cancelled_.load(std::memory_order_relaxed)) break;
            int actual_line = line_no_offset + static_cast<int>(li) + 1;
            match_single_line(lines[li], actual_line, path, opts, flags,
                              results, cancelled_, line_timeout_sec());
            if (results.size() >= opts.max_results) {
                goto done_bigfile;
            }
        }

        line_no_offset += static_cast<int>(lines.size());

        // 保留 \n 之后的原始字节为下一窗口前缀
        prev_tail = chunk.substr(decode_len);

        offset += map_size;
    }

    // 处理最后剩余的 prev_tail（文件末尾未以 \n 结束的行）
    if (!prev_tail.empty() && !cancelled_.load(std::memory_order_relaxed) &&
        results.size() < opts.max_results) {
        std::vector<unsigned char> tail_bytes(prev_tail.begin(), prev_tail.end());
        std::wstring wtext = decode_bytes(tail_bytes, file_enc);
        if (!wtext.empty()) {
            // 去除行尾 \r
            if (!wtext.empty() && wtext.back() == L'\r') wtext.pop_back();
            match_single_line(wtext, line_no_offset + 1, path, opts, flags,
                              results, cancelled_, line_timeout_sec());
        }
    }

done_bigfile:
    return results;   // sfm 析构自动关闭句柄/释放窗口
}

// ---- 处理单个文件（线程池任务）----

void Scanner::process_file(const std::filesystem::path& path,
                            const SearchOptions& opts,
                            size_t& global_done,
                            size_t& global_skipped,
                            std::vector<MatchResult>& batch,
                            std::mutex& batch_mutex,
                            std::mutex& counter_mutex,
                            const ScanCallbacks& callbacks,
                            size_t total,
                            size_t& total_results,
                            bool& max_reached) {
    // F.2.4：worker 入口 try/catch，异常通过 on_error 回调报告
    try {
        // 检查取消
        if (cancelled_.load(std::memory_order_relaxed)) return;

        // 修改时间过滤（可选）
        if (opts.mtime_filter.has_value()) {
            std::error_code ec;
            auto ftime = std::filesystem::last_write_time(path, ec);
            if (!ec) {
                // 与索引侧统一用确定性转换（避免两次 now() 采样导致的 ±1 秒抖动）
                time_t mtime = static_cast<time_t>(file_mtime_unix(ftime));
                auto [from, to] = opts.mtime_filter.value();
                if (mtime < from || mtime > to) {
                    // 不在时间范围内 → 跳过（不计入 skipped，仅不计入扫描）
                    {
                        std::lock_guard<std::mutex> lock(counter_mutex);
                        ++global_done;
                    }
                    if (callbacks.on_progress) {
                        callbacks.on_progress(global_done, total);
                    }
                    return;
                }
            }
        }

        // 二进制探测（T1：is_textual）
        std::vector<unsigned char> head = read_head_bytes(path, HEAD_BUF_SIZE);
        if (!is_textual(head)) {
            // 二进制文件 → 跳过（F.2.7）
            {
                std::lock_guard<std::mutex> lock(counter_mutex);
                ++global_done;
                ++global_skipped;
            }
            if (callbacks.on_skip) {
                callbacks.on_skip(global_skipped);
            }
            if (callbacks.on_progress) {
                callbacks.on_progress(global_done, total);
            }
            return;
        }

        // 获取文件大小，决定小文件/大文件路径
        std::error_code ec;
        auto fsize = std::filesystem::file_size(path, ec);
        uint64_t file_size = ec ? 0 : static_cast<uint64_t>(fsize);
        uint64_t threshold_bytes = opts.bigfile_threshold_mb * 1024 * 1024;

        std::vector<MatchResult> file_results;
        if (file_size > threshold_bytes && file_size > 0) {
            // 大文件：MMF 流式（F.2.6）
            file_results = scan_big_file(path, opts);
        } else {
            // 小文件：全量读取匹配
            file_results = scan_small_file(path, opts);
        }

        // 累计结果到批次（F.2.9 批次聚合）
        if (!file_results.empty()) {
            std::lock_guard<std::mutex> lock(batch_mutex);
            for (auto& r : file_results) {
                // 添加前检查全局上限（并发 worker 可能在锁外已计算完结果）
                if (total_results >= opts.max_results) {
                    max_reached = true;
                    break;
                }
                batch.push_back(std::move(r));
                ++total_results;
                // 达到全局上限 → 标记并停止
                if (total_results >= opts.max_results) {
                    max_reached = true;
                }
                // 达到批次大小 → 刷新
                if (batch.size() >= BATCH_SIZE) {
                    std::vector<MatchResult> to_send;
                    to_send.swap(batch);
                    if (callbacks.on_results) {
                        callbacks.on_results(std::move(to_send));
                    }
                }
            }
            // 文件处理完毕时也刷新（F.2.9）
            // 注意：max_reached 时不在此处刷新，剩余批次由 scan_realtime
            // 末尾的 flush_batch 统一处理，避免多 worker 重复刷新
            if (!batch.empty() && !max_reached) {
                std::vector<MatchResult> to_send;
                to_send.swap(batch);
                if (callbacks.on_results) {
                    callbacks.on_results(std::move(to_send));
                }
            }
        }

    } catch (const std::exception& e) {
        // F.2.4：异常通过 on_error 回调报告，不中断整体
        std::string what = e.what();
        if (callbacks.on_error) {
            callbacks.on_error(L"扫描文件失败: " + path.wstring() +
                               L" - " + utf8_to_wstring(what));
        }
        // 计入 skipped
        {
            std::lock_guard<std::mutex> lock(counter_mutex);
            ++global_done;
            ++global_skipped;
        }
        if (callbacks.on_skip) {
            callbacks.on_skip(global_skipped);
        }
        if (callbacks.on_progress) {
            callbacks.on_progress(global_done, total);
        }
        return;
    } catch (...) {
        // F.2.4：未知异常
        if (callbacks.on_error) {
            callbacks.on_error(L"扫描文件失败（未知异常）: " + path.wstring());
        }
        {
            std::lock_guard<std::mutex> lock(counter_mutex);
            ++global_done;
            ++global_skipped;
        }
        if (callbacks.on_skip) {
            callbacks.on_skip(global_skipped);
        }
        if (callbacks.on_progress) {
            callbacks.on_progress(global_done, total);
        }
        return;
    }

    // 正常完成计数
    {
        std::lock_guard<std::mutex> lock(counter_mutex);
        ++global_done;
    }
    if (callbacks.on_progress) {
        callbacks.on_progress(global_done, total);
    }
}

// ---- 即时扫描主流程（T4a）----

void Scanner::scan_realtime(const SearchOptions& opts,
                             const std::set<std::wstring>& ext_set,
                             ScanCallbacks& callbacks) {
    // 重置状态
    cancelled_.store(false, std::memory_order_relaxed);
    running_.store(true, std::memory_order_relaxed);

    // F.1.4：空 pattern 不启动扫描
    if (opts.pattern.empty()) {
        if (callbacks.on_error) {
            callbacks.on_error(L"搜索关键词为空，不启动扫描");
        }
        if (callbacks.on_complete) {
            callbacks.on_complete();
        }
        running_.store(false, std::memory_order_relaxed);
        return;
    }

    // F.2.7：无效正则 → 编译前检查，不启动 worker
    if (opts.use_regex) {
        RegexCheckResult rc = check_regex(opts.pattern, opts.ignore_case);
        if (!rc.valid) {
            if (callbacks.on_error) {
                callbacks.on_error(L"正则语法错误: " + rc.message);
            }
            if (callbacks.on_complete) {
                callbacks.on_complete();
            }
            running_.store(false, std::memory_order_relaxed);
            return;
        }
        if (rc.redos_warning && callbacks.on_error) {
            callbacks.on_error(L"警告: " + rc.message);
        }
    }

    // T4b 索引路由：检查是否有可用索引，有则走索引搜索
    if (!opts.force_realtime && Indexer::index_exists(opts.root)) {
        // 复用已打开的 Indexer（按 root + mtime 缓存），避免每次搜索都重新
        // load 整个索引文件到内存（IndexStore::open 的全文件解析开销）。
        auto idx = Indexer::open_cached(opts.root);
        if (idx) {
            // 进度提示
            if (callbacks.on_progress) {
                callbacks.on_progress(0, 1);
            }

            // 搜索前增量更新：目录增删改后索引会陈旧，若不更新会漏报/误报
            //（漏新文件、用旧内容匹配、仍搜到已删文件）。
            // update_incremental 内部有快速预检：无变化时只做轻量对比，
            // 不触发 IndexStore 的全量 load / save，常规搜索几乎无额外开销。
            if (opts.auto_update_index) {
                size_t changed = 0;
                if (idx->update_incremental(&changed) && changed > 0) {
                    logger::info(L"索引已同步 " + std::to_wstring(changed) + L" 个变更文件");
                    // 通过通知通道告知 UI（复用 on_error 通道，on_done 会追加到完成文本）
                    if (callbacks.on_error) {
                        callbacks.on_error(L"索引已同步 " + std::to_wstring(changed) +
                                           L" 个变更文件");
                    }
                }
            }

            // B3：勾选了建库时不存在的扩展名 → 结果偏少却是静默的。
            // 此时索引已打开，读取 meta.ext_set 是零额外成本的一次查表。
            warn_unindexed_extensions(*idx, ext_set, callbacks);

            // 索引搜索（传入当前扩展名集合，过滤掉用户未勾选的类型）
            auto results = idx->search(opts.pattern, opts, ext_set);

            // 用户取消检查
            if (cancelled_.load(std::memory_order_relaxed)) {
                running_.store(false, std::memory_order_relaxed);
                if (callbacks.on_complete) callbacks.on_complete();
                return;
            }

            // 返回结果
            if (!results.empty() && callbacks.on_results) {
                callbacks.on_results(std::move(results));
            }
            if (callbacks.on_progress) {
                callbacks.on_progress(1, 1);
            }
            running_.store(false, std::memory_order_relaxed);
            if (callbacks.on_complete) callbacks.on_complete();
            return;
        }
        // 索引打开失败 → 降级即时模式
        if (callbacks.on_error) {
            callbacks.on_error(L"索引已损坏，已回退即时扫描，建议重建");
        }
    }

    // 构建文件列表
    std::vector<std::filesystem::path> files =
        enumerate_files(opts.root, ext_set, opts.recursive);

    size_t total = files.size();
    if (total == 0) {
        if (callbacks.on_error) {
            callbacks.on_error(L"路径下未找到匹配文件: " + opts.root.wstring());
        }
        if (callbacks.on_complete) {
            callbacks.on_complete();
        }
        running_.store(false, std::memory_order_relaxed);
        return;
    }

    // 初始进度
    if (callbacks.on_progress) {
        callbacks.on_progress(0, total);
    }

    // 创建线程池（并发度 clamp 在 ThreadPool 内部处理）；持锁创建，
    // 与 cancel() 中的 pool_->cancel() 互斥
    {
        std::lock_guard<std::mutex> lk(pool_mutex_);
        pool_ = std::make_unique<ThreadPool>(0);
    }

    // 设置正则取消标志（传给 matcher 的 regex 逐行中断检查）
    regex_cancel_ptr_ = &cancelled_;
    set_regex_cancel_flag(regex_cancel_ptr_);

    // 共享状态
    size_t global_done = 0;
    size_t global_skipped = 0;
    size_t total_results = 0;
    bool max_reached = false;
    std::vector<MatchResult> batch;
    std::mutex batch_mutex;
    std::mutex counter_mutex;

    // 提交所有文件任务
    for (const auto& path : files) {
        // 检查取消 / 最大结果
        if (cancelled_.load(std::memory_order_relaxed) || max_reached) break;

        pool_->enqueue([this, path, &opts, &global_done, &global_skipped,
                        &batch, &batch_mutex, &counter_mutex,
                        &callbacks, total, &total_results, &max_reached]() {
            process_file(path, opts, global_done, global_skipped,
                         batch, batch_mutex, counter_mutex,
                         callbacks, total, total_results, max_reached);
        });
    }

    // 等待所有任务完成
    pool_->wait_all();

    // 清除正则取消标志
    set_regex_cancel_flag(nullptr);
    regex_cancel_ptr_ = nullptr;

    // 刷新剩余批次
    flush_batch(batch, batch_mutex, callbacks);

    // 最终进度
    if (callbacks.on_progress) {
        callbacks.on_progress(global_done, total);
    }

    // 最大结果提示
    if (max_reached && callbacks.on_error) {
        callbacks.on_error(L"结果已达上限 (" +
                           std::to_wstring(opts.max_results) +
                           L")，已停止收集，请缩小搜索范围");
    }

    // 完成回调
    if (callbacks.on_complete) {
        callbacks.on_complete();
    }

    {
        std::lock_guard<std::mutex> lk(pool_mutex_);
        pool_.reset();
    }
    running_.store(false, std::memory_order_relaxed);
}
