# UstbTraffic

北京科技大学校园网任务栏流量监视器。开机可自启，在 Windows 任务栏上双行显示已用流量和实时下载速率，并随系统浅色/深色主题自动调整文字颜色。

上行是已用流量（可切换绝对用量 / 超出额度 / 百分比），下行是由登录页累计 `flow` 差分得到的计费下载速率。

## 编译

本机使用 VS 2026 Build Tools。先打开 **x64 Native Tools** 环境，再 CMake：

```bat
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
cmake -S . -B build -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

也可直接运行仓库里的 `build.bat`。

产物：

- `build\UstbTraffic.exe` — 任务栏程序
- `build\UstbTrafficTests.exe` — 解析与防抖单测

## 使用

1. 运行 `UstbTraffic.exe`（可与编译目录分开拷贝，无需其它 DLL）。
2. 默认显示在任务栏**靠右**位置（系统托盘图标左侧）；也可在右键菜单中改为**靠左**（任务栏最左侧）。
3. 右键菜单：
   - **用量显示**：绝对用量 / 超出用量 / 百分比用量
   - **显示位置**：靠右（托盘左侧）/ 靠左（任务栏最左）
   - **选项**：登录页地址、轮询间隔、免费额度、边距、列间距
   - **开机自启**：写入当前用户 `HKCU\...\Run`
   - **关于**：版本与作者信息
   - **退出**
4. 若本机还开着 TrafficMonitor 的任务栏窗口，两者可能重叠，建议关掉 TM。

配置文件：`%APPDATA%\UstbTraffic\config.ini`。

未登录或请求失败时显示 `状态 / 未登录` 或 `状态 / --`。程序不会替你登录，也不保存密码。

## 数据来源

默认按固定间隔 GET `http://202.204.48.82/`（可在选项中修改），解析 HTML 中的 `flow='…'`（单位 KB，引号内可能有空格）和 `NID`。连续两次有效累计值相减得到速率，并做零流量网页 bug、计数器回绕、休眠唤醒、尖峰过滤和 EMA 平滑。

## 许可证

[MIT](LICENSE)
