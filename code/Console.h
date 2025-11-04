#pragma once
#include <string>

namespace Core::Console {

// 创建并附加一个 UTF-8 控制台（可重复调用，内部会判断）
void InitUTF8();

// 可选：分离并释放控制台（退出时调用）
void Shutdown();

// 便捷输出（utf8 窄字符串）
void PrintUTF8(const std::string& s);

// 便捷输出（宽字符串）
void PrintW(const std::wstring& s);

}
