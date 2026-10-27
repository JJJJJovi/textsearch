// core/text_detect.h — 文本/二进制判定接口
#pragma once

#include <filesystem>
#include <vector>

// 判定文件头部字节是否为文本
// 附录 F.2.1 关键修改：NUL 探测前先做编码预检测，
// 已识别为 UTF-16 的不进 NUL 判定（避免 UTF-16 文件被 0x00 误判为二进制）
bool is_textual(const std::vector<unsigned char>& head);

// 便捷函数：读取文件前 8192 字节后判定
bool is_textual_file(const std::filesystem::path& path);
