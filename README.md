# GW2 快捷喊话（激战2 Nexus 插件）

在激战2游戏内显示一个悬浮面板，点一下就把预设句子发到聊天，支持多频道群发、自定义外观。

作者：摸鱼哥

## 功能

- 游戏内悬浮面板，点击句子直接发送
- 右键句子按钮指定频道，可多选：公会1-6 / 地图 / 队伍 / 战队 / 团队 / 说
- 句子条数不限（上限 200），支持鼠标拖动排序
- 排列方式：纵向单排、横向单排、表格（表格可选按行 / 按列填充）
- 外观可调：面板背景透明度、按钮透明度、列表背景透明度、按钮颜色、文字颜色、文字大小、按钮宽高
- 面板位置锁定、隐藏标题栏和底部提示
- 快捷键不预绑定，在 Nexus → 输入绑定 里为每条单独设置；面板开关默认 `CTRL+ALT+M`

## 安装

前提：游戏已安装 [Nexus](https://github.com/RaidcoreGG) 插件管理工具（游戏根目录有 `addonLoader.dll`），否则 GW2 不会加载 `addons` 里的 DLL。

1. 双击 `release/快捷喊话安装-双击运行-需要游戏安装Nexus插件管理工具.bat`
2. 选 `1`（安装 / 更新）
3. 填游戏目录（放 `Gw2-64.exe` 的文件夹，例如 `E:\Game\GuildWars2`）

命令行方式：

```powershell
powershell -ExecutionPolicy Bypass -File release\快捷喊话安装.ps1 -Action 安装 -GameDir "E:\Game\GuildWars2"
powershell -ExecutionPolicy Bypass -File release\快捷喊话安装.ps1 -Action 状态 -GameDir "E:\Game\GuildWars2"
powershell -ExecutionPolicy Bypass -File release\快捷喊话安装.ps1 -Action 回退 -GameDir "E:\Game\GuildWars2"
```

- 安装只放 `addons\QuickChat.dll`，不动已有配置；旧版本会备份到脚本旁的 `backups\`
- 加 `-DryRun` 只预演，不改文件
- 卸载默认保留配置，要一起删加 `-PurgeConfig`

## 使用

进入游戏后右上角出现「快捷喊话」面板，默认 10 条预设句子。设置入口：Nexus → 设置 → 快捷喊话。

改句子时注意：Nexus 的输入框不支持中文输入法，先在浏览器/记事本里复制中文，再点「从剪贴板粘贴」。

配置保存在 `<游戏目录>\addons\QuickChat\QuickChatAddon.ini`。

## 编译

需要 VS 2022（C++ 工作负载）+ Windows SDK 10，以及 Nexus 模板的 ImGui 1.80（仓库内已带）。

```bat
cd source
build.cmd
```

产物：`source\bin\Release\QuickChat.dll`，复制到 `release\addons\` 或直接覆盖游戏里的 `addons\QuickChat.dll`（游戏运行时 Nexus 会热加载）。

`source\test\run_test.cmd` 是离线自检程序：用真实 ImGui 上下文 + 模拟 Nexus API 跑通加载、配置、发送全链路。

## 技术说明

- 国服客户端会拦截 `SendInput` 合成输入，插件改用 Nexus 的 `WndProc_SendToGameOnly` 直接把按键消息投递给游戏窗口
- 国服聊天框按 GBK 双字节解码，逐字节投递 UTF-8 或 `WM_IME_CHAR` 会触发游戏断言直接闪退，所以「文字投递方式」请保持 GBK 字节
- ImGui 版本与 Nexus 保持一致（1.80），避免上下文 ABI 不匹配

## 许可

MIT，见 [LICENSE](LICENSE)。
