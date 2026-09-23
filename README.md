# UstbTraffic

北京科技大学校园网任务栏流量监视器。开机可自启，在 Windows 任务栏上双行显示已用流量和实时下载速率，并随系统浅色/深色主题自动调整文字颜色。

上行是已用流量（可切换绝对用量 / 超出额度 / 百分比），下行是由流量源累计用量差分得到的计费下载速率。

## 编译

本机使用 VS 2026 Build Tools。先打开 **x64 Native Tools** 环境，再 CMake（Ninja 可并行编译）：

```bat
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

也可直接运行仓库里的 `build.bat`。默认按 CPU 核数并行；`build.bat -j4` 或 `build.bat /j4` 限定 4 路。

产物：

- `build\UstbTraffic.exe` — 任务栏程序
- `build\UstbTrafficTests.exe` — 解析与防抖单测

## 使用

1. 运行 `UstbTraffic.exe`（可与编译目录分开拷贝，无需其它 DLL）。
2. 默认显示在任务栏**靠右**位置（系统托盘图标左侧）；也可在右键菜单中改为**靠左**（任务栏最左侧）。
3. 右键菜单：
   - **用量显示**：绝对用量 / 超出用量 / 百分比用量
   - **显示位置**：靠右（托盘左侧）/ 靠左（任务栏最左）
   - **选项**：流量源、学号/密码（自服务/混合）、速率源（仅混合）、轮询间隔、免费额度、边距、列间距、字体
   - **开机自启**：写入当前用户 `HKCU\...\Run`
   - **关于**：版本与作者信息
   - **退出**
4. 若本机还开着 TrafficMonitor 的任务栏窗口，两者可能重叠，建议关掉 TM。

配置文件：`%APPDATA%\UstbTraffic\config.ini`。

未登录或请求失败时显示 `状态 / 未登录` 或 `状态 / --`。

选择自服务或混合模式时，程序会用选项中的学号/密码做 HTTPS 登录并维护 Cookie（约每 2 小时刷新）；学号与密码明文保存在上述配置文件中。

## 数据来源

流量源可选：

- `202.204.48.82` / `202.204.48.66`：按固定间隔 GET 门户页，解析 HTML 中的 `flow='…'`（单位 KB）、`NID`、`uid`、`fee`
- `https://zifuwu.ustb.edu.cn`：登录后 GET `/Self/dashboard`，解析「已用」（单位 M→KB）与「账户余额」（元→内部万分之一元）
- **混合（登录页身份+自服务用量）**：学号/姓名来自门户 `202.204.48.82`；已用流量与余额来自自服务；实时下载速率可在选项里选 `202.204.48.82` 或 `202.204.48.66` 的累计流量差分。若门户未登录，任务栏显示「未登录」（即使自服务仍可访问），避免误以为已登录校园网门户

连续两次有效累计值相减得到速率，并做零流量网页 bug、计数器回绕、休眠唤醒、尖峰过滤和 EMA 平滑。

## 许可证

[MIT](LICENSE)
