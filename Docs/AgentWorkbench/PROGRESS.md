# Agent Workbench 实施进度

更新：2026-09-29。本次将 `BSHarnessTools` 的只读工具接入模型自主工具调用循环；以 `SPEC.md` 为功能和 UI 验收依据。

## 环境发现（P0）

| 项目 | 实际结果 |
|---|---|
| 工程 | `F:\UE5Project\UEHarness\UEHarness.uproject`；`EngineAssociation=UEAS58` |
| UE 版本 | 本地 `Build.version` 为 5.8.0 |
| 引擎根目录 | `F:\EPIC\Engine58WithPDB\Engine\Windows\Engine`；注册表 UEAS58 指向其上一层 `...\Windows` |
| Editor Target | `Source/UEHarnessEditor.Target.cs`；`TargetType.Editor`、BuildSettings V7、IncludeOrder Unreal5_8 |
| 平台与工具链 | Win64；Visual Studio 14.50、Windows SDK 10.0.22621.0；引擎 `Build.bat` 和 `UnrealEditor-Cmd.exe` 可用 |
| 现有插件 | `BSHarness` 与 `BSHarnessTools` 为 Editor 模块；项目还启用 ModelingToolsEditorMode、StateTree、GameplayStateTree、ModelContextProtocol |
| 现有模型客户端 | 在工程与 BSHarness 源码中未发现可复用的模型客户端 |
| 现有工具系统 | `BSHarnessTools` 有 MCP 工具注册、配置、分派及自动化测试；P1 未接入 |
| 初始工作区状态 | BSHarness Git 仓库已有未跟踪的 `Docs/AgentWorkbench/` 和 `README-AgentWorkbench-Codex.md`；均保留。工程根目录不是 Git 仓库，新增兄弟插件与 `.uproject` 不会出现在 BSHarness 的 `git status` 中 |
| 阻塞 | 构建和命令行自动化无阻塞；未执行图形编辑器内手工验收 |

已核查本地 UE5.8 头文件/示例：`ToolMenus.h` 与 LevelEditor/Blutility 工具栏示例、`SWindow.h` 与窗口创建/关闭示例、`DeveloperSettings.h`、`IContentBrowserSingleton.h`、`AssetData.h`、`HttpModule.h`、`IHttpRequest.h`。P1 仅使用 ToolMenus、Slate、DeveloperSettings；资产与 HTTP 接口留待后续阶段。首次构建发现 `UToolMenus::IsAvailable()` 在本地版本不存在，已改用本地可用的 `UnregisterOwner`。

## 阶段状态

| 阶段 | 代码/文档实施 | 编译 | 自动化 | 编辑器内验收 |
|---|---|---|---|---|
| P0 工程勘察 | 已完成 | 不适用 | 不适用 | 不适用 |
| P1 插件/窗口/布局/设置 | 已完成代码；已修复工具栏注册路径；新窗口首次发送前不进入历史 | UEHarnessEditor Win64 Development 通过 | 4/4 通过 | 原始工具栏未显示；修复后待图形编辑器复验 |
| P2 候选资产/请求快照 | 已完成代码；现支持 /Game/ 蓝图及非蓝图资产，候选行仅 Object Path + X | UEHarnessEditor Win64 Development 通过 | P2 6/6、P1 回归 4/4 通过 | 未验证 |
| P3 执行器/隔离 | 已实现单 Session 异步 HTTP、取消、请求及 Run 超时、归属校验和模型→工具→模型循环；全局并发限额与排队未实现 | UEHarnessEditor Win64 Development 通过 | AgentWorkbench 17/17、DeepSeek 真实工具循环 1/1 通过 | 未验证 |
| P4 历史持久化 | 完成候选、消息、模型、草稿及 Run 快照的保存/恢复和分叉；新增工具调用 ID、参数、结果及历史兼容 | UEHarnessEditor Win64 Development 通过 | AgentWorkbench 历史回归通过 | 未验证 |
| P5 真实 API/蓝图只读工具 | 已为六种 Provider 编码/解析非流式工具调用；桥接七个只读 `BSHarnessTools` 工具并限制资产范围；蓝图图结构读取未实现 | UEHarnessEditor Win64 Development 通过 | 协议测试通过；DeepSeek 真实工具循环 1/1 通过；其他 Provider 未做真实请求 | 未验证 |
| P6 终验/交付 | 未开始 | 未验证 | 未验证 | 未验证 |

## 早期阶段文件计划与实施记录（以下 Mock 描述为历史状态）

- 当前工作树将 `AgentWorkbenchEditor` 作为 `BSHarness.uplugin` 内的 Editor 模块，源码位于 `Plugins/BSHarness/Source/AgentWorkbenchEditor/`。此前独立的 `Plugins/AgentWorkbench/` 路径已不存在；这与 `SPEC.md` 中独立插件路径不同，属于待确认的结构差异，本次工具栏修复未改动该布局。
- `AgentWorkbenchEditorModule.cpp`：Level Editor 工具栏入口与菜单生命周期。
- `AgentWorkbenchSession.h/.cpp`：每个新窗口独立 SessionId、消息、草稿、模型选项、候选容器、Runner 占位状态；历史会话窗口复用；Mock 同步响应和结构化事件。
- `AgentWorkbenchWidgets.h/.cpp`：独立非模态 SWindow；左历史，中间模型→消息列表→独立候选资产空态→固定输入区，右执行事件列表，底部状态栏。
- `AgentWorkbenchSettings.h/.cpp`：Project Settings 插件设置；仅保存凭据环境变量名称、Mock、模型列表、三个超时及执行限制。
- `AgentWorkbenchTests.cpp`：Session 隔离、设置默认值、三窗口和历史激活自动化测试。
- 当前 `.uproject` 启用 BSHarness 且仅允许 Editor Target；`BSHarness.uplugin` 注册 `AgentWorkbenchEditor` 模块。
- `BSHarness/AGENTS.md`：保留原文并添加 AgentWorkbench 专项约定；本进度文件记录实际结果。

## P2 实施范围

- `AgentAssetContextService.h/.cpp`：只在目标窗口点击按钮时读取主 Content Browser 选中资产（本地 UE5.8 `GetSelectedAssets` 实现确认使用 `PrimaryContentBrowser`）；支持 /Game/ 下有效资产路径和类元数据，跳过无效路径、重定向器和无法确认类型，不调用 `GetAsset`/`TryLoad`。追加、规范化对象路径去重、上限与原因计数均按 Session 处理。
- 候选 Widget：保留原三栏及中栏顺序；添加、每行完整 Object Path 与 X 移除、清空、折叠/展开、空态与反馈均可操作，展开高度 200 Slate 单位、列表内部滚动，折叠高度 46；输入框和发送按钮固定在其下方。X 与清空仅改变引用数组。
- Session 与发送：候选 DTO 保存路径、包名、资产类、状态与加入时间。发送前验证当前候选，注册表扫描和已知失效项阻止发送并保留草稿。模型、输入和当前候选复制到本 Run 的快照；Mock 执行区用 JSON 库显示该快照的结构化请求预览，历史用户消息保留当轮候选值副本。
- 只读工具：`list_context_assets` 只返回快照；`get_asset_metadata` 只接受本 Run 已授权的完整对象路径，返回 Asset Registry 中的真实元数据、读取时间及可识别的内存包脏状态。未知工具、额外参数、越界路径和失效资产返回结构化错误；不读取蓝图图表或修改资产。
- `AgentAssetContextTests.cpp`：覆盖蓝图/纹理添加、重定向器过滤、追加去重、旧勾选字段兼容、空选择及结果计数、上限、无效路径、跨 Session 隔离、快照值语义、扫描/失效阻断、工具授权，以及当前工程真实蓝图和非蓝图资产的 Asset Registry 集成查询。

## 本次候选历史补全

- `AgentWorkbenchHistory.h/.cpp`：每个 Session 独立 JSON DTO，写入项目 `Saved/AgentWorkbench/Sessions`，先写临时文件再替换；保存候选 ID、路径、名称、类、勾选和状态，以及消息、草稿、模型、事件和逐 Run 输入值快照。不持久化 `FAssetData`、UObject、运行对象或凭据。逐文件恢复，损坏文件单独报告；没有中心索引依赖。
- `AgentWorkbenchSession.h/.cpp`：启动加载历史；Session 编辑、关闭和退出保存；分叉以深拷贝得到新 SessionId，保留旧 Run 来源信息，不复制活动 Runner。恢复未完成的 Run 为 `Interrupted`，不重放请求。恢复候选时只查 Asset Registry 元数据，扫描中保持 `Unknown`，已知无效保留路径并标为 `Missing`。
- `AgentWorkbenchWidgets.h/.cpp`：历史行右键可删除或新建分叉；候选区折叠/展开；候选移除、清空、模型和草稿变动保存到所属 Session。折叠状态仅是窗口 UI 状态，不随 Session 持久化。
- `AgentWorkbenchHistoryTests.cpp`：覆盖历史往返、失效路径保留、勾选/模型/草稿/Run 快照恢复、坏文件隔离、未完成 Run 中断、分叉候选独立和分叉 Run 历史重载。P2 测试继续覆盖多选过滤、重复、A/B 隔离、快照、路径进入 Mock 模型输入。

## 执行记录

1. 勘察命令：`git status --short`、`rg --files`、读取 `.uproject`/Target/插件代码、读取本地引擎 `Build.version` 和头文件。结果见上表。
2. 构建命令：`& 'F:\EPIC\Engine58WithPDB\Engine\Windows\Engine\Build\BatchFiles\Build.bat' UEHarnessEditor Win64 Development '-Project=F:\UE5Project\UEHarness\UEHarness.uproject' -WaitMutex -NoHotReload`。初次因本地 `UToolMenus::IsAvailable` 不存在失败；修正后最终退出码 0，`Result: Succeeded`。日志：`C:\Users\NiubilityPC\AppData\Local\UnrealBuildTool\Log.txt`。
3. 测试命令：`& 'F:\EPIC\Engine58WithPDB\Engine\Windows\Engine\Binaries\Win64\UnrealEditor-Cmd.exe' 'F:\UE5Project\UEHarness\UEHarness.uproject' '-ExecCmds=Automation RunTests AgentWorkbench.P1' '-TestExit=Automation Test Queue Empty' '-ReportExportPath=F:\UE5Project\UEHarness\Saved\Automation\AgentWorkbenchP1' '-abslog=F:\UE5Project\UEHarness\Saved\Logs\AgentWorkbenchP1Tests.log' -unattended -nullrhi -nosound -nop4 -nosplash`。最终退出码 0；SessionIsolation、SettingsDefaults、MultiWindow 共 3/3 成功。报告：`F:\UE5Project\UEHarness\Saved\Automation\AgentWorkbenchP1\index.json`。
4. 变更检查：检查 BSHarness 仓库状态及工程级新增文件；新增插件和 `.uproject` 位于该 Git 仓库外。未修改引擎源码、无关模块或手工改写生成目录。
5. 2026-09-28 工具栏修复：编辑器日志确认 `AgentWorkbenchEditor` 已从 `Plugins/BSHarness/Binaries/Win64` 加载。UE5.8 的 `LevelEditorToolBar.cpp` 只生成 `LevelEditor.LevelEditorToolBar.User` 等子菜单，不生成此前扩展的父菜单。已将入口改到 `.User`，并使用引擎示例中的 `CalloutToolbar` 样式。重新执行上述 Editor Target 构建，退出码 0。重新执行 `AgentWorkbench.P1` 自动化（报告目录 `Saved/Automation/AgentWorkbenchToolbarFix`），退出码 0、4/4 通过；新增 `ToolbarRegistration` 断言目标子工具栏和按钮条目均已注册。
6. 2026-09-28 P2：重新读取当前规则、SPEC、TASKS、PROGRESS、现有源码与 Git 差异。核查本地 `ContentBrowserSingleton.cpp`、`IContentBrowserSingleton.h`、`AssetData.h`、`IAssetRegistry.h`、`SoftObjectPath.h`、`PackageName.h`、`UObjectGlobals.h` 的实际 API。构建命令同第 2 项；最终退出码 0。首次 P2 自动化为 4/5，其中无点号路径被 UE 基础校验接受；已补充完整 `Package.Asset` 检查并重测。
7. P2 最终自动化命令：`& 'F:\EPIC\Engine58WithPDB\Engine\Windows\Engine\Binaries\Win64\UnrealEditor-Cmd.exe' 'F:\UE5Project\UEHarness\UEHarness.uproject' '-ExecCmds=Automation RunTests AgentWorkbench' '-TestExit=Automation Test Queue Empty' '-ReportExportPath=F:\UE5Project\UEHarness\Saved\Automation\AgentWorkbenchP2Final' '-abslog=F:\UE5Project\UEHarness\Saved\Logs\AgentWorkbenchP2Final.log' -unattended -nullrhi -nosound -nop4 -nosplash`。退出码 0，报告 `Saved/Automation/AgentWorkbenchP2Final/index.json` 显示 10/10 成功（P1 4 项、P2 6 项）。真实项目蓝图的注册表读取与元数据查询通过；测试没有驱动图形 Content Browser 的人工选择。
8. 本次重新读取规则、SPEC、TASKS、ACCEPTANCE、PROGRESS，检查 `git status --short`、已暂存/未暂存差异和源码；保留原有改动。核查本地 UE5.8 `ContentBrowserSingleton.cpp`：`GetSelectedAssets` 从 `PrimaryContentBrowser` 取选择；核查 `SBox.h` 的动态高度属性、`FileManager.h` 的替换/文件遍历接口、`JsonObject.h` 的安全读取接口和 Asset Registry 扫描状态接口。实际多浏览器图形行为未测试。
9. 本次构建：第 2 项命令，最终退出码 0、`Result: Succeeded`。自动化：第 7 项命令，最终报告路径改为 `Saved/Automation/AgentWorkbenchCandidatesHistoryVerified`，日志改为 `Saved/Logs/AgentWorkbenchCandidatesHistoryVerified.log`；退出码 0，报告 `succeeded=12, failed=0, notRun=0`。其中 P1 4、P2 6、History 2。执行 `git diff --check` 与 `git diff --cached --check` 无空白错误（只有工作区行尾转换提示）。

## 本次历史栏交互调整

- 删除窗口内“新建对话”和历史行内“分叉”按钮；工具栏新建入口保留。左键点击历史行延迟到下一帧，在当前窗口重新构建全部会话内容，不创建窗口。目标已经在另一窗口时交换两个窗口显示的 Session，以维持单 Session 单可写窗口。
- 右键指定历史行使用 UE5.8 `SListView` 选择与 `FMenuBuilder` 垂直菜单，提供“删除”和“新建分叉”。删除前确认；删除只移除历史 JSON、Session 和候选引用，不触碰真实资产。删除当前 Session 时优先在原窗口显示一个尚未打开的历史 Session；没有可用 Session 时关闭该窗口。右键新建分叉在当前窗口切换到新 Session。
- `AgentWorkbenchTests.cpp` 新增 `History.InPlaceNavigationAndDelete`，验证已打开会话交换窗口、草稿归属不变、分叉不新建窗口，以及删除历史文件后的窗口映射。更新 `AGENTS.md`、规则片段、SPEC、TASKS 和 ACCEPTANCE 中与旧“激活原窗口”交互冲突的文字。
- 本轮读取规则和当前 Git 状态；核查本地 `SListView.h`、`STableRow.h` 的右键选行/菜单行为及 `SWindow.h` 的 `SetContent` 签名。构建命令同第 2 项，最终退出码 0。自动化命令同第 7 项，最终报告 `Saved/Automation/AgentWorkbenchHistoryNavigationFinal/index.json`，日志 `Saved/Logs/AgentWorkbenchHistoryNavigationFinal.log`；退出码 0，13/13 通过（P1 4、P2 6、History 3）。

## 本次历史重命名与快捷键

- 历史右键菜单加入“重命名”；选中历史行按 F2 进入行内编辑，按 Delete 复用已有删除确认。左键切换后重新选中当前行并将焦点给历史列表，保证 F2 可继续作用于当前对话。编辑框内的 Delete 由文本框处理，不触发会话删除。
- 重命名只修改 Session 标题，拒绝空白、换行和超过 128 字符的标题；同步保存历史 JSON、刷新所有历史栏和已打开窗口标题，不改变 SessionId、消息或候选资产。单独保存“手工命名”标记，避免用户把标题改为“新对话”后被下一次发送自动覆盖。
- 核查本地 UE5.8 `SListView.h` 的 `OnKeyDownHandler`、`SInlineEditableTextBlock.h/.cpp` 的行内编辑、文本提交与 F2 行为。构建命令同第 2 项，退出码 0、`Result: Succeeded`。自动化命令同第 7 项，最终报告 `Saved/Automation/AgentWorkbenchHistoryRenameFinal/index.json`、日志 `Saved/Logs/AgentWorkbenchHistoryRenameFinal.log`，退出码 0、13/13 通过。新增断言覆盖空标题拒绝、标题裁剪、历史持久化、窗口标题更新和手工命名保护；图形编辑器的实际 F2/Delete/右键操作未验证。

## 本次 Content 资产添加修复与候选行简化

- 用户反馈主内容浏览器选中的资产被判为“非蓝图 1”。源码确认 `AddAssets` 先检查 `IsInstanceOf<UBlueprint>`，而 `BuildSnapshot` 和 `get_asset_metadata` 也有同样的蓝图限制。按本次需求改为支持有有效 Object Path、位于 /Game/ 且有 AssetClassPath 的 Content 资产；只排除重定向器、无效/越界路径和缺少类元数据的项，不调用 `GetAsset`/`TryLoad`。发送与只读元数据工具同步支持非蓝图资产。
- 候选行只显示完整 Object Path 和右侧“X”；X 仅移除当前 Session 的候选引用。移除行内复选框、名称、定位和复制路径按钮。所有当前候选用于下一次发送；旧历史中的 `bIncluded` 字段仍可读取，但不再隐藏可见候选。失效/待验证路径在列表上方提示，长路径在行内折行且 Tooltip 保留完整值。
- `AgentAssetContextTests.cpp` 覆盖蓝图与纹理混选、重定向器过滤、去重、旧禁用标记不隐藏可见候选、移除后真实资产仍在，以及从当前项目 Asset Registry 找到真实非蓝图资产并完成添加→发送快照→只读元数据→Mock 输入路径闭环。
- 实际构建命令同执行记录第 2 项，退出码 0、`Result: Succeeded`。自动化命令同第 7 项，报告 `Saved/Automation/AgentWorkbenchContentAssets/index.json`、日志 `Saved/Logs/AgentWorkbenchContentAssets.log`，退出码 0、13/13 通过（P1 4、P2 6、History 3）。未在图形编辑器里重新点击添加按钮验证用户所选的具体资产。

## 编辑器内验收与剩余问题

- 本次 Provider 设置：`EAgentWorkbenchProvider` 新增 DeepSeek、OpenAI、Anthropic、Google Gemini、OpenRouter、Ollama，并保留 Mock；代码默认值和项目 `Config/DefaultGame.ini` 中 `ProviderType` 改为 DeepSeek，保留用户现有 `DefaultModel=Mock` 与 `AvailableModels`。当前发送仍明确走 Mock 执行器；真实 Provider 的网络适配、凭据使用和协议验证属于 P5。本地 UE5.8 `UENUM`/`UMETA` 头文件与示例已核查；执行 `Build.bat UEHarnessEditor Win64 Development -Project=F:\UE5Project\UEHarness\UEHarness.uproject -WaitMutex -NoHotReload`，退出码 0、`Result: Succeeded`；执行 `UnrealEditor-Cmd.exe` 的 `Automation RunTests AgentWorkbench`，报告 `Saved/Automation/AgentWorkbenchProviders/index.json`，退出码 0、14/14 通过。`git diff --check` 与 `git diff --cached --check` 无空白错误。图形编辑器中的项目设置下拉显示尚未手工验证。

- 本次历史准入调整：工具栏打开的新 Session 保持临时状态；草稿、模型和候选更改不会写历史。首次有效发送在 RunStarted 后写入历史，历史面板只列有 Run 的 Session。空消息或候选校验失败不会出现历史；关闭或切走未发送的临时 Session 会丢弃它。旧版本已保存的空 Session 文件保持原样，但启动时不显示或加载到当前会话列表。已有历史的分叉继续即时进入历史。
- 增加 `History.OnFirstSend` 自动化，以及多窗口中关闭未发送窗口的检查；现有历史导航测试改用已发送的历史会话。首次构建的 C++ 编译阶段完成，但插件 DLL 链接被正在 Rider 调试的 `UnrealEditor.exe` 占用，`Build.bat` 退出码 1（LNK1104）。用户关闭编辑器后原命令重跑成功，退出码 0、`Result: Succeeded`。命令行自动化使用 `Automation RunTests AgentWorkbench`，报告 `Saved/Automation/AgentWorkbenchHistoryFirstSend/index.json`、日志 `Saved/Logs/AgentWorkbenchHistoryFirstSend.log`；退出码 0、14/14 通过（P1 4、P2 6、History 4）。`git diff --check` 与 `git diff --cached --check` 通过。图形编辑器内尚未验证本次交互。

- 用户在图形编辑器中发现原工具栏按钮未显示。已定位并修复菜单路径；修复后的实际显示仍待使用新构建重启编辑器复验。三窗口视觉布局、Content Browser 切换、DPI、中文输入法与 Ctrl+Enter 的实际交互也仍须在编辑器中验证。命令行测试不能替代手工 UI 验收。
- P1 Mock 同步完成，没有真实请求和可持续的运行任务；停止按钮仅在 Runner 处于运行中时启用。异步取消、超时、回调归属属于 P3。
- 本次历史保存/恢复和分叉已通过命令行自动化；未在图形编辑器中重启、浏览历史或分叉验证。当前历史文件位于项目 Saved 中，没有中心索引；保存失败只写 UE 警告日志，尚无窗口级反馈。
- 设置中的超时和限额已配置，但 P1 Mock 不执行异步任务；真正的限额执行属于 P3。凭据值未读取或保存。
- 图形编辑器内验收未执行：本次选中真实资产后点击添加、候选行仅路径与 X、X 移除，之前的 F2/Delete、右键重命名、左键原窗口切换、删除确认/回退和分叉，以及多个 Content Browser 的实际来源、折叠/滚动、历史重启恢复和请求预览观感仍须手工确认。自动化已验证服务逻辑与真实 Asset Registry 查询，但不能据此标记这些 UI 操作为通过。
- 当前 `AgentWorkbenchEditor` 仍作为 BSHarness 内模块，尚非 SPEC 所述的独立 `Plugins/AgentWorkbench` 插件；P2 保留了现有布局，没有在本阶段搬动模块。

## 下一步

先在图形编辑器内复验真实发送、工具步骤、停止和历史切换；后续按 TASKS.md 补齐并发排队和更深入的蓝图内容读取。

## 2026-09-28 真实模型发送修复

- 对话发送现在调用 `FAgentModelClient`，按当前项目设置选择 Provider，并在 Run 开始时冻结用户输入、模型、已有对话和候选资产路径。请求在 UE HTTP 模块异步执行；响应、失败和取消只更新归属 Run 的 Session。旧 Mock 回复不再进入新请求的对话上下文。
- 工具栏连接名称从项目设置读取；发送按钮、消息标签、执行详情和状态栏去除了固定 Mock 文案。旧 Session 的模型若仍为 Mock，打开模型栏时改用当前默认模型。下拉框打开时重读项目设置中的模型列表。
- 项目设置仍仅保存凭据及 Base URL 的环境变量名称；请求时读取环境变量。缺少真实模型或凭据会直接提示错误，不制造回复。执行详情显示真实请求体，历史文件在 `bPersistDetailedPayloads=false` 时不保存详情正文。
- `UEHarnessEditor Win64 Development` 编译成功。`Automation RunTests AgentWorkbench` 报告 `Saved/Automation/AgentWorkbenchFinal/index.json`，14/14 通过。单独运行 `ManualProvider.LiveRequest`，报告 `Saved/Automation/AgentWorkbenchLiveProviderFinal/index.json`，1/1 通过，已通过 UE HTTP 客户端调用当前配置的 DeepSeek `deepseek-flash` 并取得非空回复。另以相同地址、环境变量和模型完成一次短请求，服务端确认模型名有效。`git diff --check` 无空白错误。
- 图形编辑器内手工发送、停止、切换项目设置后的即时观感未验证。当前未实现模型工具调用循环、蓝图结构读取、全局并发排队、重试或流式输出；不能把这些能力标为完成。

## 2026-09-29 模型自主调用 BSHarnessTools

- `FAgentModelClient` 将已授权工具编码为六种 Provider 的非流式工具定义，解析普通回复或工具提议；模型工具名使用无点号别名，并反查本次请求的允许名单。`FAgentSession` 管理模型→工具→模型循环、调用 ID、步骤上限、Run 总超时和迟到回调。`FAgentToolWorkflow` 在工具提议与实际执行之间提供异步 Gate 接口，后续流程可在此插入；停止或超时后的 Gate 不再发起工具调用。
- `FAgentToolBridge` 从 `BSHarnessTools` 注册表读取定义并执行，只开放 `editor_info`、`project_info`、`assets.search`、`assets.get`、`actors.list`、`source.list`、`source.read`。运行时再次核对配置名称及原生处理器身份，防止配置重绑定提升权限；`assets.search` 限于 `/Game`，`assets.get` 限于本 Run 候选快照。`assets.open` 不提供自动调用。
- 消息和历史 JSON v2 保留工具调用、参数、结果、关联 ID、Gemini 思考签名与冻结限额，旧版 JSON 仍可读取。工具步骤在对话区和执行详情显示；单次结果长度超过 16000 字符时提供显式截断信息。
- `UEHarnessEditor Win64 Development` 编译通过；`Automation RunTests AgentWorkbench` 报告 `Saved/Automation/AgentWorkbenchToolCalling2/index.json`，17/17 通过；新增异步 Gate 取消测试单独运行，`Saved/Automation/AgentWorkbenchWorkflowCancel/index.json`，1/1 通过；`Automation RunTests BSHarness.MCP` 报告 `Saved/Automation/BSHarnessMCPToolCalling/index.json`，12/12 通过。真实 DeepSeek `deepseek-flash` 的 `ManualProvider.LiveToolCall` 报告 `Saved/Automation/AgentWorkbenchLiveToolCall/index.json`，1/1 通过，模型提出工具调用、收到工具结果并完成回复；普通 `ManualProvider.LiveRequest` 也 1/1 通过。
- Anthropic、Gemini、OpenAI、OpenRouter、Ollama 的工具协议只完成自动化结构测试，没有真实服务请求证据。图形编辑器内手工 UI 验收未执行；全局并发排队、自动重试、流式输出与蓝图图结构读取仍未实现。
- 最后补充通用 `RunFailed` 执行事件并重新编译成功；历史兼容回归 `Saved/Automation/AgentWorkbenchHistoryToolCalling/index.json` 为 5/5 通过。

## 2026-09-29 对话与执行详情显示修正

- 对话列表只显示用户输入及每个 Run 的最终 Agent 回复；模型工具提议、工具结果与错误仍留在会话结构中供模型续轮和执行详情使用，候选路径继续由候选区展示。兼容旧历史中缺少 Run ID 的 Agent 文本。
- 执行步骤标题只显示描述；无详情的步骤不提供空展开区。展开模型请求、工具参数和结果时解析 JSON 及嵌套 JSON 字符串，将转义的换行、制表符和斜杠按实际字符显示，同时保留纯文本源码路径中的反斜杠；过长内容标明截断。
- `UEHarnessEditor Win64 Development` 编译通过。`Automation RunTests AgentWorkbench.Display` 报告 `Saved/Automation/AgentWorkbenchDisplay/index.json`，2/2 通过。图形编辑器内手工视觉验收未执行。默认 `bPersistDetailedPayloads=false` 时，历史重启后执行事件的详情正文仍不保存，这是现有持久化设置的行为。

## 2026-09-29 MCP 工具注册与蓝图变量读取

- `Config/MCPTools.json` 配置当前 9 个原生 MCP 工具，新增 `bsharness.blueprint.variables`。它在 Game Thread 加载 `/Game` 蓝图，读取蓝图自身及可选父蓝图声明的成员变量、类型和可获得的默认值；标记 CDO/描述符来源、生成类可能过期状态，分页并限制完整 MCP 结果长度。不编译或保存资产。
- Agent Workbench 从配置注册表枚举所有当前支持的原生处理器；模型响应只按本次请求提供的别名表解析。Run 快照冻结配置名与原生处理器身份，并持久化该映射，防止运行中配置换绑。别名使用不超过 64 字符的稳定名称。
- `assets.get` 和 `blueprint.variables` 可查询经验证的 `/Game` 资产路径，无候选时模型也能先搜索再读取。`assets.open` 仅提供给本 Run 已附加的非关卡候选，执行前重新校验资产类型；搜索限定 `/Game`。配置参数约束仍由 MCP 注册表校验。
- 本轮 UE5.8 Editor Target 使用 `-NoLink` 完成最新源码编译，退出码 0、`Result: Succeeded`。标准链接因 Rider 调试中的 Unreal Editor 占用两个插件 DLL 而报 `LNK1104`；本轮新二进制和自动化测试尚未验证，等待编辑器退出后重新链接并运行。

## 2026-09-29 UE5.8 MCP 工具动态桥接

- 核查本机 UE5.8 引擎源码、项目插件配置与编辑器日志：原工程只加载 `ToolsetRegistry.AgentSkillToolset`，包含 4 个工具；引擎另安装 26 个 Toolset 插件，其中 `EditorToolset` 提供 `BlueprintTools.list_variables`。`AllToolsets` 只聚合其中 21 个，不能代表全部已安装工具。按用户指定的已安装插件范围，工程配置启用 `AllToolsets` 和其余 5 个工具集插件，限定 Editor 目标。
- Agent Workbench 在 Run 开始时冻结 `BSHarnessTools`、已加载 UE ToolsetRegistry 和独立 MCP 工具的完整目录、实例身份和 Schema 指纹。模型只接收原生工具与 `ue_mcp.catalog.search`、`ue_mcp.catalog.describe`、`ue_mcp.call_tool` 三个入口；目录分页查询和按需调用覆盖完整冻结目录，避免一次发送大量工具 Schema。运行中重新注册或更改的工具不能沿用旧 Run 的权限。
- 所有实际 UE MCP 工具及 `assets.open` 都在独立的 Slate 模态窗口中显示实际工具名与解码后的参数，逐次明确允许；无人值守默认拒绝。审批窗口不把参数写入 UE 日志。前置 Gate、审批和执行相互独立；审批后再次检查取消、Run 总时限、冻结身份和参数。UE ToolsetRegistry 没有通用取消/回滚接口，超时后的底层工具可能继续运行，不自动重试写入工具。
- 新增递归 JSON Schema 参数校验器；审批前和实际执行前均校验 required、类型、额外字段、枚举、嵌套结构及大小边界等。修复资产工具 JSON 深拷贝时未初始化目标对象的问题，避免 `assets.open` 配置 Schema 或默认 `assets.search` 参数触发空指针。
- 使用 UE5.8 `UEHarnessEditor Win64 Development -NoLink -NoHotReload` 完成源码编译，退出码 0。由于 Development 编辑器正在运行，使用独立 `UEHarnessEditor Win64 DebugGame -NoHotReload` 完整编译和链接，退出码 0。DebugGame 命令行自动化 `Automation RunTests AgentWorkbench` 报告 `Saved/Automation/AgentWorkbenchUnrealMCPBridge2/index.json`：25/25 成功；`Automation RunTests BSHarness.MCP` 报告 `Saved/Automation/BSHarnessMCPUnrealBridge/index.json`：13/13 成功。实际批准后的 `AgentSkillToolset.ListSkills` 只读调用、目录搜索/描述/调用链路及拒绝未批准调用均通过。
- 启用全部 26 个 Toolset 插件后，DebugGame Editor Target 构建退出码 0；`Automation RunTests AgentWorkbench` 报告 `Saved/Automation/AgentWorkbenchAllInstalledToolsets/index.json`：25/25 成功。运行日志记录 57 个已注册工具集；补充的 `UnrealMCPCatalog` 自动化报告 `Saved/Automation/AgentWorkbenchBlueprintCatalog/index.json`：1/1 成功，目录包含 876 个可调用 UE MCP 工具，并断言 `BlueprintTools.list_variables` 已进入目录。图形界面的审批窗口尚未手工验收，当前 Development 编辑器仍占用旧 DLL；需要保存并关闭后再完成 Development 链接和加载验证。

## 2026-09-29 历史工具目录精简与 Schema 回归

- Run 开始时仍在内存中冻结当时可用的工具和处理器身份，用于目录查询、调用前校验与审批后复核；加入 `RunHistory` 的副本不再包含完整工具名单及处理器映射，保存的 Session JSON 也不再写入这两个字段。历史仍保存实际工具调用、参数、结果和关联 ID。旧版含完整目录的历史可以读取，再次保存时会去除这两个字段。
- UE MCP 目录专项发现 876 个可调用工具，其中原有 5 个工具的 Schema 使用 `oneOf` 等组合规则，曾被调用前参数校验器拒绝。已补齐 `oneOf`、`anyOf`、`allOf` 的结构和参数匹配校验；全部 876 个已发现工具的 Schema 通过支持性检查。
- 最新 DebugGame Editor Target 完整编译与链接退出码 0；Development Editor Target 使用 `-NoLink -NoHotReload` 源码编译退出码 0。用 `UnrealEditor-Win64-DebugGame-Cmd.exe` 执行自动化：`AgentWorkbenchCatalogSchemaCompositions` 1/1、`AgentWorkbenchSchemaCompositions` 5/5、`AgentWorkbenchCatalogCompositionsFull` 27/27、`BSHarnessMCPCatalogCompositionsFull` 13/13，均无失败或警告。`git diff --check` 退出码 0。当前图形 Development 编辑器仍运行旧 DLL，尚未完成 Development 链接和界面手工验收。

## 2026-09-29 对话卡片、复制与执行树

- 对话区只显示用户输入和每轮 Agent 最终回复的规则保持不变；用户卡片靠右，Agent 卡片靠左，使用角色标签、背景色和间距区分。正文使用只读可选中文本，支持选中后复制及卡片上的整条复制按钮。
- 执行区按 Run 组成树：轮次根节点显示用户问题预览和状态，子节点为原有步骤描述，有详情时再展开为详情节点。步骤和解码后的详情均可选择复制，并有复制按钮；不显示 Run ID 或事件序号。新增事件时保留已展开节点；草稿变化且事件未变时不重建树；列表只有在用户原本位于底部时才跟随新内容，“最新”按钮可主动跳转。
- 新增纯数据 `AgentWorkbench.Display.ExecutionTreeModel` 与 `ExecutionTreeStatus` 自动化，覆盖多轮及旧版无 Run ID 事件、步骤顺序、问题预览、空详情、历史中断后开启新一轮时的状态、终止状态优先级和 UTF-16 表情符号截断。复核 UE5.8 滚动 API 后，将“是否在底部”的判断改为比例阈值，并在树重建后的下一次布局请求滚动到目标节点，避免回看时跳底及新步骤不可见。
- 最终 `UEHarnessEditor Win64 DebugGame -NoHotReload` 和 `UEHarnessEditor Win64 Development -NoHotReload` 均完整编译链接通过；DebugGame 命令行 `Automation RunTests AgentWorkbench` 报告 `Saved/Automation/AgentWorkbenchCopyTreeReviewed/index.json`：29/29 通过、无警告。用户关闭图形编辑器后，Development 命令行同套自动化报告 `Saved/Automation/AgentWorkbenchCopyTreeDevelopment/index.json`：29/29 通过、无警告、无失败或未运行项。
- 用新 Development 二进制重启图形编辑器，确认 Agent Workbench 可打开、历史会话可切换、用户与 Agent 卡片左右分列、执行轮次可展开为步骤。通过系统剪贴板核对了用户消息、Agent 回复和执行步骤的整条复制按钮；验收后已关闭编辑器。旧历史按现有 `bPersistDetailedPayloads=false` 设置未保存详情正文，因此本次未手工验证详情节点与选中文字复制。

## 2026-09-29 任务失败原因显示在对话区

- `FailRun` 已保存失败原因并产生 `RunFailed` 执行事件；本次修正对话筛选，使该轮 `Error` 消息随用户问题显示。失败原因使用靠左的“任务失败”卡片和错误色标签，可选择或整条复制；空原因的旧记录显示通用提示。取消和工具中间结果仍不作为失败回复显示，发送前校验错误仍在输入框下方提示。
- 扩展对话筛选和历史往返自动化，验证失败原因在对应问题后显示，以及保存、恢复后仍可见。`UEHarnessEditor Win64 DebugGame -NoHotReload` 完整编译链接成功；`Automation RunTests AgentWorkbench` 报告 `Saved/Automation/AgentWorkbenchFailureConversation/index.json`：29/29 通过、无警告。Development 源码编译通过，但最终 DLL 链接被当前 Rider 调试的 `UnrealEditor.exe` 占用，尚待编辑器退出后重试。`git diff --check` 通过；本次图形界面手工验收未执行。

## 2026-09-29 执行树卡片点击与历史详情

- 隐藏树行自带的展开三角，点击轮次卡片切换步骤，点击步骤卡片切换详情；轮次、步骤保留整条复制按钮，详情正文保留选择和复制。历史事件未保存正文时，按轮次、顺序与工具名从已有消息恢复可可靠关联的工具参数和结果；无法恢复的模型输入明确提示。
- Development Editor Target 完整编译链接成功；`Automation RunTests AgentWorkbench` 报告 `Saved/Automation/AgentWorkbenchClickableTree/index.json`：30/30 成功、无警告。图形界面实际点击与历史详情展开未完成验收。

## 2026-09-29 执行树连续点击、悬停与滚动宽度

- UE5.8 的 `STableRow` 会在第二次快速点击时先自动切换展开状态，随后卡片的鼠标松开回调再切换一次。执行树现已接管双击，保证卡片每次点击只切换一次。轮次及步骤卡片通过鼠标进入、离开事件高亮和恢复；使用固定 16 像素的外置滚动条槽，列表在滚动条出现时保持宽度。
- 最新代码的 DebugGame Editor Target 完整编译链接通过；Development Editor Target 使用 `-NoLink -NoHotReload` 源码编译通过。双击及滚动条改动后执行 `Automation RunTests AgentWorkbench`，报告 `Saved/Automation/AgentWorkbenchExecutionTreeInteraction/index.json`：30/30 成功、0 失败，另有 1 条无关的引擎联网探测超时警告。随后把悬停检测限定到卡片本身，重新完成 DebugGame 链接及 Development 源码编译；该阶段自动化没有重复运行，因为原测试不覆盖鼠标悬停。
- 用户保存并关闭原 Development 编辑器后，最新 Development Editor Target 完整编译链接通过；其命令行自动化报告 `Saved/Automation/AgentWorkbenchExecutionTreeInteractionDevelopment/index.json`：30/30 成功、无警告。使用新 Development 二进制在图形编辑器打开旧历史，实际验证轮次和步骤连续点击两次后回到原展开状态、鼠标悬停高亮且移出恢复、旧记录可展开查看工具调用参数，以及滚动条出现和消失时卡片右边界保持不变。验收后已关闭本次启动的工作台和编辑器。
