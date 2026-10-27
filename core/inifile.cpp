// core/inifile.cpp — 极简 INI 读写实现
#include "core/inifile.h"
#include "core/util.h"

#include <algorithm>
#include <fstream>
#include <sstream>

namespace {

// 去首尾空白
std::wstring trim(const std::wstring& s) {
    size_t b = 0, e = s.size();
    auto isws = [](wchar_t c) {
        return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n' ||
               c == L'\u00A0' || c == L'\uFEFF';
    };
    while (b < e && isws(s[b])) ++b;
    while (e > b && isws(s[e - 1])) --e;
    return s.substr(b, e - b);
}

// 去掉行尾注释（; 或 # 开头，简单处理：不处理引号内的情况，配置文件无需复杂）
std::wstring strip_comment(const std::wstring& line) {
    for (size_t i = 0; i < line.size(); ++i) {
        if (line[i] == L';' || line[i] == L'#') {
            return line.substr(0, i);
        }
    }
    return line;
}

std::wstring to_lower(std::wstring s) {
    for (auto& c : s) {
        if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
    }
    return s;
}

} // namespace

bool IniFile::load(const std::filesystem::path& path) {
    data_.clear();
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) return false;

    std::string bytes((std::istreambuf_iterator<char>(ifs)),
                      std::istreambuf_iterator<char>());
    if (bytes.empty()) return true;

    // 跳过 UTF-8 BOM
    size_t off = 0;
    if (bytes.size() >= 3 &&
        static_cast<unsigned char>(bytes[0]) == 0xEF &&
        static_cast<unsigned char>(bytes[1]) == 0xBB &&
        static_cast<unsigned char>(bytes[2]) == 0xBF) {
        off = 3;
    }
    std::wstring text = utf8_to_wstring(bytes.substr(off));

    std::wstring section;
    std::wistringstream iss(text);
    std::wstring line;
    while (std::getline(iss, line)) {
        // 去 UTF-8 文件在 Windows 下读出的 \r
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        std::wstring s = trim(strip_comment(line));
        if (s.empty()) continue;

        if (s.front() == L'[' && s.back() == L']') {
            section = trim(s.substr(1, s.size() - 2));
            continue;
        }
        size_t eq = s.find(L'=');
        if (eq == std::wstring::npos) continue;   // 无等号 → 忽略该行
        std::wstring key = trim(s.substr(0, eq));
        std::wstring val = trim(s.substr(eq + 1));
        if (key.empty()) continue;
        data_[section][to_lower(key)] = val;
    }
    return true;
}

bool IniFile::save(const std::filesystem::path& path) const {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);

    std::wostringstream oss;
    for (const auto& [section, kv] : data_) {
        if (!section.empty()) {
            oss << L'[' << section << L']' << L'\n';
        }
        for (const auto& [key, val] : kv) {
            oss << key << L" = " << val << L'\n';
        }
        oss << L'\n';
    }
    std::wstring text = oss.str();

    std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
    if (!ofs) return false;
    // UTF-8 BOM（方便记事本双击编辑中文不乱码）
    const char bom[3] = {'\xEF', '\xBB', '\xBF'};
    ofs.write(bom, 3);
    std::string u8 = wstring_to_utf8(text);
    ofs.write(u8.data(), static_cast<std::streamsize>(u8.size()));
    return ofs.good();
}

std::wstring IniFile::get(const std::wstring& section, const std::wstring& key,
                          const std::wstring& def) const {
    auto sit = data_.find(section);
    if (sit == data_.end()) return def;
    auto kit = sit->second.find(to_lower(key));
    if (kit == sit->second.end()) return def;
    return kit->second;
}

int IniFile::get_int(const std::wstring& section, const std::wstring& key,
                     int def) const {
    std::wstring v = get(section, key);
    if (v.empty()) return def;
    try {
        return std::stoi(v);
    } catch (...) {
        return def;
    }
}

bool IniFile::get_bool(const std::wstring& section, const std::wstring& key,
                       bool def) const {
    std::wstring v = to_lower(trim(get(section, key)));
    if (v.empty()) return def;
    if (v == L"1" || v == L"true" || v == L"yes" || v == L"on") return true;
    if (v == L"0" || v == L"false" || v == L"no" || v == L"off") return false;
    return def;
}

std::vector<std::wstring> IniFile::get_list(const std::wstring& section,
                                            const std::wstring& key) const {
    std::vector<std::wstring> out;
    std::wstring raw = get(section, key);
    std::wistringstream iss(raw);
    std::wstring item;
    while (std::getline(iss, item, L',')) {
        std::wstring t = trim(item);
        if (!t.empty()) out.push_back(t);
    }
    return out;
}

void IniFile::set(const std::wstring& section, const std::wstring& key,
                  const std::wstring& val) {
    data_[section][to_lower(key)] = val;
}

void IniFile::set_int(const std::wstring& section, const std::wstring& key, int val) {
    set(section, key, std::to_wstring(val));
}

void IniFile::set_bool(const std::wstring& section, const std::wstring& key, bool val) {
    set(section, key, val ? L"1" : L"0");
}

void IniFile::set_list(const std::wstring& section, const std::wstring& key,
                       const std::vector<std::wstring>& vals) {
    std::wstring joined;
    for (const auto& v : vals) {
        if (!joined.empty()) joined += L", ";
        joined += v;
    }
    set(section, key, joined);
}

void IniFile::erase(const std::wstring& section, const std::wstring& key) {
    auto sit = data_.find(section);
    if (sit == data_.end()) return;
    sit->second.erase(to_lower(key));
    if (sit->second.empty()) data_.erase(sit);
}
