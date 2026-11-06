# Payload Dumper

原生 Win32 C 图形界面，不需要 Python、Tk、.NET 或额外安装运行库。
支持 Windows 7 SP1 及以上；已在当前 Windows 电脑验证，旧系统尚未实机测试。

## 运行

双击项目根目录的 `PayloadDumperGUI.exe`（64 位），或选择对应便携包：

```text
dist/win-x64/                 dist/win-x86/
  PayloadDumperGUI.exe          PayloadDumperGUI.exe
  bin/                         bin/
    payload_dumper.exe           payload_dumper.exe
    arbextract.exe               arbextract.exe
    updater.exe                  updater.exe
  licenses/                    licenses/
```

32 位系统使用 x86 包；64 位系统优先使用 x64 包，适合大型 ROM。
每个包中的主程序和两个工具均为同一架构。移动时请携带完整目录。
工具路径始终相对于主程序定位，不依赖当前工作目录或 PATH。

下载：[GitHub Releases](https://github.com/daxiaamu/PayloadDumper_GUI/releases/latest)。

- 浏览、拖入本地 `payload.bin` / OTA ZIP，或粘贴 HTTP/HTTPS 链接。
- 输出目录留空时，在主程序目录创建 `output_文件名`；链接使用 `output`。
- 分区留空提取全部，或填写 `boot,init_boot,vendor_boot` 等逗号分隔名称。
- 线程数留空自动选择，也可指定 1–256。
- 「列出分区」只读取分区列表。
- 「元数据 / 内核版本」依次运行两个查询；某些包不包含 ZIP 元数据或内核，相关查询会报错并保留日志。
- 「检查 ARB」通过本项目的 dumper 提取 `xbl_config.img`，再调用 koaaN 的 arbextract；不使用 dumper 的内置 `--arb` 替代。
- ARB 临时文件使用系统临时目录中的唯一任务目录，结束后清理。
- 后台运行期间可查看、复制或清空日志；「取消任务」会结束子进程。
  日志不回显执行命令；进度条仅在任务运行时显示，完成、失败或取消后隐藏。
  关闭窗口也会先取消任务。已写入的部分解包镜像会保留，不能视为完整镜像。
- 可把文件拖到主程序图标上，或通过 `PayloadDumperGUI.exe "D:\ROM\ota.zip"` 预填输入。

HTTP 链接需要服务器支持 Range；增量包及格式支持范围以 payload_dumper_C 为准。
重新解包到已有目录时，工具可能覆盖同名镜像，请选择合适的输出目录。

## 程序 OTA 更新

这里的 OTA 是本工具自身的在线更新，与 Android 固件解包分开处理。

- 启动后延迟检查一次；底部「检查更新」可手动检查，两者共用一个进行中的会话。
- 自动检查失败或已是最新版时保持安静。手动检查始终反馈结果。
- 更新弹窗展示版本、本地时区的发布时间与可滚动日志，提供「跳过此版本」「稍后」「下载并更新」。
- 「稍后」会在检查入口显示红点；跳过仅影响自动提示，手动检查仍可查看。
- 解包任务结束后才展示待处理更新，避免替换正在使用的工具。
- 按程序自身架构下载对应的完整便携 ZIP；校验 SHA-256 后由独立 C 更新器再次校验、解压并核对 PE 架构、主程序版本及产品名。
- 更新器等待主程序退出，只替换固定的程序/工具/许可证文件；不会删除 ROM、输出镜像或其他用户文件。
- 旧文件先备份，替换失败时回滚，再启动原程序。程序目录需要可写；不请求管理员权限。

当前发行渠道为 stable，beta Release 不会更新 stable 清单。版本比较使用资源版本对应的
`major * 1000000 + minor * 1000 + patch`，不是字符串排序。

元数据使用 GitHub Contents API 作为唯一权威源，配置另外六个 HTTPS 来源；
清单以内容哈希命名并保持不可变。客户端校验清单哈希、策略 revision、渠道及回滚边界；
清单正文也优先通过 Contents API 读取，Raw/CDN 作为回退；网络请求遵循系统代理。
同 revision 的哈希冲突会被拒绝。权威源不可用时至少要求两个镜像主机一致。
多个 jsDelivr 域名共用供应商，不能视为独立供应商保证。

下载镜像由发布流程实测筛选，要求至少五个不同镜像主机完整返回相同 ZIP，另附 GitHub 官方兜底。
未达到要求时不切换 OTA 指针，保留上一版清单。

更新配置保存在当前用户注册表 `HKCU\Software\Daxiaamu\PayloadDumperGUI`。
强制更新边界独立保存在 `updates/policy.json`，初始值为 0（普通更新）；发布脚本不会自动改变它。
若后续显式配置强制边界，已命中的旧客户端只能更新或退出，不能通过跳过或断网绕过已接受的边界。

## 构建

```powershell
.\build.ps1
# 仅构建一个架构
.\build.ps1 -Architectures x64
# 指定现有的本地 Payload_Dumper_C 项目
.\build.ps1 -PayloadProject D:\myproject\Payload_Dumper_C
# 自定义 MinGW-w64 工具链路径
.\build.ps1 -Compiler64 C:\mingw64\bin\gcc.exe -Compiler32 C:\mingw32\bin\gcc.exe
```

需要 MinGW-w64 GCC 和同目录的 windres；默认路径为本机工具链。
构建为静态链接 EXE。主程序使用 C11 与 Windows 系统 API，警告视为错误。
构建脚本从附带的本地项目源码快照重新编译 dumper，而非复制旧版工具。
快照含该项目的 C 源码、头文件、静态解码库及其重建脚本，可离线构建。

`tools/draw_icon.py` 可重新绘制自制图标（需要 Pillow），正常构建直接使用
`assets/app.ico`，不需要 Python。矢量原稿为 `assets/app.svg`。

运行 `./tests/run.ps1` 可执行两个架构的离线 CLI、HTTP Range 和程序内界面集成测试；
详见 `VALIDATION.md`。运行 `./package.ps1` 可生成 x64 / x86 便携 ZIP、源码 ZIP 和 SHA256 校验文件。

测试 OTA 前需先运行 `./package.ps1`，因为集成测试读取完整发行 ZIP。

## 发布

1. 修改 `src/version.h` 的版本和版本号，以及 `updates/policy.json` 的 `policyRevision`，保留已有强制边界。
2. 运行 `build.ps1`、`package.ps1`、`tests/run.ps1`，验证两个架构。
3. 推送源码并将对应 `v版本号` 标签指向该提交；在该仓库创建稳定 Release，上传两个便携 ZIP、源码 ZIP 和 SHA256SUMS。
4. `.github/workflows/ota.yml` 在 Release 发布后读取实际 ZIP 中的 EXE 版本、校验所有镜像，生成不可变 manifest 与 stable 指针，在一个 Git 提交中发布。
   也可通过 workflow_dispatch 传入已发布 tag 重试。它不接收 APK，也不为 beta Release 修改 stable 渠道。

可在已登录 `gh` 的本机运行 `python tools/publish_update.py --tag v1.0.2` 生成相同清单；
仅在所有验证通过后写文件，仍须将 `updates/` 提交到 main。

## 来源

- payload_dumper：本地 `D:\myproject\Payload_Dumper_C`，GPL-3.0；详见
  `third_party/payload_dumper_C/ORIGIN.md` 和该项目的 LICENSE、README。
- arbextract：[koaaN/arbextract](https://github.com/koaaN/arbextract)，MIT；固定提交
  `320efd96ded606b6bbf9c0c9d04ceb35caba32a6`。上游解析源码原样保留，
  增加 Windows Unicode 命令行/文件路径适配，以支持中文路径。
- 主程序图标：本项目自行绘制的分层镜像与提取箭头。
- 更新器 ZIP 解压：miniz tinfl，MIT；JSON 分词：jsmn，MIT。源码及许可证位于 `third_party/`。

Windows 更新适配使用 [android-app-update skill](https://github.com/daxiaamu/update-by-github-skill)
的会话、版本策略、多源校验与发布要求；将 APK 安装替换为按架构校验的便携包替换/回滚。
此版本尚未配置 Authenticode 发布者签名；PE 产品名/版本校验不等于证书签名校验。
