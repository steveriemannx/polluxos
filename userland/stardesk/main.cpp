// PolluxOS 桌面壳入口 —— 占位实现。
// 现在只弹一个 dui 窗口验证 Wayland 后端链路:
//   合成器(weston/sway) -> libwayland -> dui(NativeWindow_Wayland) -> Skia
// 后续在此接入真正的桌面功能(任务栏、启动器、窗口列表)。
//
// dui 的三种 UI 模式见 dui/docs/ThreeModes.md:
//   XML 运行时解析 / 构建期转码(XML-to-code) / 纯 C++。
// 桌面壳推荐 XML 模式: 布局在 ui/shell.xml 里调，不用重编译。

#include "dui.h"   // TODO: 按 dui 实际公开头文件路径调整 (include/dui/)

int main()
{
    // TODO: 替换为真实 UI 应用代码，示例参照 dui/examples/* 中基于
    // dui 的窗口创建流程(SetAttribute/AddItem 或 XML 加载)。
#if 0
    ui::Window w;
    w.CreateWindow(L"PolluxOS", ui::kUIWndClass);
    w.Show(true);
    // ... 消息循环
#endif
    return 0;
}
