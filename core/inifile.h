// core/inifile.h — 极简 INI 配置文件读写（自研，零依赖）
//
// 为什么用 INI 而不是 JSON：
// - 性能不是考量（配置文件 KB 级，进程启动读一次）
// - **人手可编辑性**才是：INI 每行 `key = value`、支持 `;` 注释，
//   写错一行只影响一项；JSON 少个引号/逗号整份文件就解析失败。
//   常用扩展名等需要用户自行维护的配置，INI 明显更合适。
//
// 支持：
//   [section]          分区
//   key = value        键值（等号两侧空白忽略）
//   ; 或 #             行注释（也支持行尾注释）
//   逗号分隔列表        get_list / set_list（自动 trim、去空项）
//
// 编码：文件统一 UTF-8 **带 BOM**（方便记事本双击打开编辑中文不乱码），
// 读取时自动跳过 BOM。
#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <vector>

class IniFile {
public:
    // 加载文件。文件不存在返回 false（内容为空，可按默认值写入）。
    bool load(const std::filesystem::path& path);
    // 保存（UTF-8 带 BOM，LF 换行）。父目录不存在时自动创建。
    bool save(const std::filesystem::path& path) const;

    // ---- 读取（键/节缺失时返回默认值）----
    std::wstring get(const std::wstring& section, const std::wstring& key,
                     const std::wstring& def = L"") const;
    int  get_int(const std::wstring& section, const std::wstring& key,
                 int def = 0) const;
    bool get_bool(const std::wstring& section, const std::wstring& key,
                  bool def = false) const;
    // 逗号分隔列表（自动 trim、去空项）
    std::vector<std::wstring> get_list(const std::wstring& section,
                                       const std::wstring& key) const;

    // ---- 写入 ----
    void set(const std::wstring& section, const std::wstring& key,
             const std::wstring& val);
    void set_int(const std::wstring& section, const std::wstring& key, int val);
    void set_bool(const std::wstring& section, const std::wstring& key, bool val);
    void set_list(const std::wstring& section, const std::wstring& key,
                  const std::vector<std::wstring>& vals);

    // 删除一个键（节里再无键时连节一起删）。键不存在时无操作。
    // B2：用于把旧版本已经写进 ini 的 last_pattern（搜索关键词）清除掉。
    void erase(const std::wstring& section, const std::wstring& key);

    bool empty() const { return data_.empty(); }

private:
    // section → (key → value)
    std::map<std::wstring, std::map<std::wstring, std::wstring>> data_;
};
