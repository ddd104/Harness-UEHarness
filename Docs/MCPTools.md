# MCP 工具模块

`BSHarnessTools` 是 UE5.8 编辑器模块，将 UE 能力定义为 Model Context Protocol（MCP）工具。模块启动时读取插件的 `Config/MCPTools.json`，默认注册下面的 9 个工具。MCP 定义工具发现与调用协议，工具的具体实现由本插件提供。

新增已有能力的工具别名、修改说明和参数默认值、启用或禁用工具，推荐直接修改 JSON，详见 [配置注册工具](ToolConfiguration.md)。通过 `BSHarness.MCP.Reload` 重载，无需重新编译。后面的 C++ 示例保留给新增底层能力或其他模块集成时使用。

## 已注册的常用工具

| 工具名 | 用途 | 常用参数 | 执行方式 |
| --- | --- | --- | --- |
| `bsharness.editor_info` | 当前 UE 版本 | 无 | 同步 |
| `bsharness.project_info` | 项目名称、工程文件及源码根目录 | 无 | 同步 |
| `bsharness.assets.search` | 搜索资产，不加载资产对象 | `path`、`class_path`、`name_contains`、`recursive`、`offset`、`limit` | 同步，编辑器主线程 |
| `bsharness.assets.get` | 查询资产名称、类型、包名及包依赖 | `object_path`，必填 | 同步，编辑器主线程 |
| `bsharness.blueprint.variables` | 读取蓝图声明的成员变量、类型及可获得的默认值 | `object_path`（必填）、`include_inherited`、`offset`、`limit` | 同步，编辑器主线程 |
| `bsharness.assets.open` | 加载并打开资产编辑器；已打开则切换到该窗口 | `object_path`，必填 | 同步，编辑器主线程 |
| `bsharness.actors.list` | 查询当前编辑器场景已加载的 Actor、位置和选中状态 | `selected_only`、`name_contains`、`offset`、`limit` | 同步，编辑器主线程 |
| `bsharness.source.list` | 列出源码文本文件 | `root`、`path`、`recursive`、`offset`、`limit` | 异步，后台文件查询 |
| `bsharness.source.read` | 分行读取源码 | `root`、`path`（必填）、`start_line`、`max_lines` | 异步，后台文件读取 |

参数名区分大小写，不允许未知参数或隐式类型转换。`tools/list` 返回每个工具的参数 Schema。

## Agent Workbench 中的 UE MCP 工具

Agent Workbench 除了读取上面的 `BSHarnessTools` 注册表，还在编辑器运行时发现 UE5.8 `ToolsetRegistry` 和独立注册的 MCP 工具。只会发现**已加载并启用**的工具；安装在引擎目录中但没有启用的工具集不会自动加载。每次发送会冻结当时可用的工具和处理器身份；运行中卸载或重新注册的工具需要在新一轮对话中重新发现。

当前工程已在 `UEHarness.uproject` 为 Editor 目标启用 `AllToolsets`，并单独启用它未覆盖的 `ChaosClothAssetToolset`、`LiveCodingToolset`、`MetaHumanGenerator`、`MVVMToolset`、`SequencerAnimMixerToolset`，共覆盖本机安装的 26 个工具集插件。独立 DebugGame 编辑器的运行时目录发现 876 个可调用 UE MCP 工具，包括 `BlueprintTools.list_variables`；具体数量会随插件加载、设置和 UE 版本变化。正在运行的编辑器需要重新启动才能载入新插件和桥接代码。

模型可使用 `ue_mcp.catalog.search` 搜索本轮冻结的工具目录，使用 `ue_mcp.catalog.describe` 读取某个工具的参数 Schema，再通过 `ue_mcp.call_tool` 传入目录返回的 `registry_name` 与 `arguments` 调用。模型请求中只发送这三个目录与调用入口，不一次性附带全部 UE 工具 Schema。完整目录保留在当前 Run 快照的内存中；历史记录保存实际工具调用与结果，不保存完整工具名单或处理器映射。目录查询不会调用引擎工具；每一次实际 UE MCP 工具调用都要在编辑器窗口中核对工具全名与参数并明确允许。无人值守运行时默认拒绝。审批只适用于本次调用；工具身份或 Schema 变化后不会沿用旧批准。

UE `ToolsetRegistry` 的执行接口没有通用取消或回滚能力。调用超时或用户停止后，Workbench 会忽略迟到结果，但已经开始的工具仍可能继续执行。对具有副作用的工具，应查看实际项目状态再决定是否重试。

- 资产搜索默认 `path=/Game`，递归查询；`class_path` 是精确类型，例如 `/Script/Engine.Material`，不包含派生类型。资产名与 Actor 标签的子串匹配不区分大小写。
- 分页默认 `offset=0`、`limit=100`，`limit` 范围为 1–500。返回 `items`、`total`、`hasMore`，有后续页时提供 `nextOffset`。翻页期间如果编辑器数据发生变化，结果可能随之改变。
- 资产查询返回 `isScanning`。为 `true` 时，资产注册表仍在扫描，当前结果可能不完整；扫描完成后再查询。不自动同步等待完整扫描。
- Actor 查询针对编辑器世界，不查询 PIE 世界，也不加载 World Partition 尚未加载的 Actor。
- 源码根目录 `root=plugin`（默认）对应 `Plugins/BSHarness/Source`，`root=project` 对应工程的 `Source`。`path` 为相对根目录的路径，禁止绝对路径、`.`/`..` 路径段、符号链接和目录联接。列表跳过链接。
- 源码读取默认从第 1 行返回最多 200 行，`max_lines` 范围为 1–1000，保留空行。返回 `lines`、`totalLines`、`hasMore`，有后续内容时提供 `nextLine`。单文件上限为 1 MiB。支持 `.h/.hpp/.cpp/.c/.cc/.cs/.inl/.py/.usf/.ush/.json/.md/.txt`。
- 除 `assets.open` 会加载资产并改变编辑器窗口状态（打开关卡会切换场景）外，其余工具查询项目状态。工具不会主动修改或保存资产与代码。读取结果同时放在 `structuredContent` 与 JSON 文本 `content` 中，便于程序处理和人工查看。

## 在编辑器里直接测试

先编译 `UEHarnessEditor`，再打开 `UEHarness.uproject`。若修改了 C++ 或升级模块，需要重启编辑器；只改工具 JSON 时执行 `BSHarness.MCP.Reload` 即可。打开 **Output Log（输出日志）**，在底部的 **Cmd/控制台命令输入框**逐条执行下列命令；不要在 Python 输入模式下执行。

列出全部工具及其参数：

```text
BSHarness.MCP.List
```

查看引擎和工程信息：

```text
BSHarness.MCP.Call bsharness.editor_info
BSHarness.MCP.Call bsharness.project_info
```

搜索 `/Game` 下的前 5 个资产，并查询某个结果的详情：

```text
BSHarness.MCP.Call bsharness.assets.search {"path":"/Game","limit":5}
BSHarness.MCP.Call bsharness.assets.search {"path":"/Game","class_path":"/Script/Engine.Material","limit":5}
```

从搜索结果复制真实的 `objectPath`，替换下面的占位路径：

```text
BSHarness.MCP.Call bsharness.assets.get {"object_path":"/Game/YourFolder/YourAsset.YourAsset"}
```

打开资产时，使用新工具（把示例路径替换成实际的 `objectPath`）：

```text
BSHarness.MCP.Call bsharness.assets.open {"object_path":"/Game/YourFolder/YourAsset.YourAsset"}
```

也支持 `/Game/YourFolder/YourAsset` 这样的包路径，以及内容浏览器“复制引用”的文本，例如：

```text
BSHarness.MCP.Call bsharness.assets.open {"object_path":"/Script/Engine.Material'/Game/YourFolder/YourAsset.YourAsset'"}
```

成功时返回 `opened=true`；资产原本已打开时，返回 `alreadyOpen=true` 并切换到已有编辑器。`isLevel=true` 表示打开的是关卡。关卡打开可能出现 UE 自身的保存提示，需在编辑器中处理；若用户取消或资产类型延迟打开而尚未确认窗口，工具会返回失败说明。新处理器首次加入需要重新编译并重启；只有 JSON 名称和默认值调整才能仅用 `Reload`。

列出场景 Actor；第二条只返回当前选中的 Actor：

```text
BSHarness.MCP.Call bsharness.actors.list {"limit":10}
BSHarness.MCP.Call bsharness.actors.list {"selected_only":true}
```

列出并读取本插件的源码（可直接复制）：

```text
BSHarness.MCP.Call bsharness.source.list {"root":"plugin","path":"BSHarnessTools","limit":10}
BSHarness.MCP.Call bsharness.source.read {"root":"plugin","path":"BSHarnessTools/BSHarnessTools.Build.cs","start_line":1,"max_lines":20}
```

读取工程源码时，把 `root` 换成 `project`，例如：

```text
BSHarness.MCP.Call bsharness.source.read {"root":"project","path":"UEHarness/UEHarness.cpp","max_lines":20}
```

完整 JSON-RPC 请求也可以直接测试：

```text
BSHarness.MCP.Request {"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"bsharness.project_info","arguments":{}}}
```

输出日志筛选 `LogBSHarnessTools` 或 `MCP response:`，即可查看带请求 ID 的 JSON 响应。异步文件查询稍后打印最终响应，不阻塞编辑器等待。示例结果结构：

```json
{"jsonrpc":"2.0","id":1,"result":{"content":[{"type":"text","text":"..."}],"isError":false,"structuredContent":{"projectName":"UEHarness","projectFile":"...","projectSource":"...","pluginSource":"..."}}}
```

检查失败处理：

```text
BSHarness.MCP.Call bsharness.assets.search {"limit":0}
BSHarness.MCP.Call bsharness.source.read {"path":"__missing_source__.cpp"}
BSHarness.MCP.Call not_registered
```

第一条和第三条返回 JSON-RPC `error.code=-32602`；第二条返回 `result.isError=true` 和无法打开文件的错误说明。控制台入口复用同一个注册表和分发器，是本地测试入口；它不提供外部 MCP 网络连接。

## 结构与职责

- `FMCPToolDefinition`：工具名称、说明和 `inputSchema`。
- `FMCPToolRegistry`：使用 `TOOL_HANDLERS` 字典保存工具名称到定义及处理函数的映射；提供注册、注销、枚举、调用接口。名称区分大小写，重复注册会失败。
- `FMCPToolResult`：执行完成后的 `content`、可选 `structuredContent` 和 `isError`。
- `FMCPToolDispatcher`：处理已经解析成 JSON 对象的 `tools/list`、`tools/call` 请求，生成 JSON-RPC 2.0 响应，保留请求 ID。
- `FBSHarnessToolsModule`：管理共享注册表的生命周期。
- `FMCPToolConfiguration`：加载 JSON，验证参数覆盖，注入默认值并一次性替换配置管理的工具。

这里实现的是 MCP 工具层。外部 Agent 的连接还需要接入传输层（如 stdio 或 Streamable HTTP），并由服务端处理 `initialize`、协议版本协商、能力声明、会话和通知。当前模块不会启动监听端口或创建完整 MCP Server。工具列表一次返回全部条目，未提供分页或列表变更通知；接入时可声明 `tools: {}`。

## 注册同步工具

使用方在模块构建依赖中添加 `BSHarnessTools`，并在主线程注册：

```cpp
#include "BSHarnessToolsModule.h"

auto& Registry = FBSHarnessToolsModule::Get().GetToolRegistry();
FMCPToolDefinition Definition;
Definition.Name = TEXT("project.example");
Definition.Description = TEXT("Example tool returning a completed result.");
Definition.InputSchema = MakeShared<FJsonObject>();
Definition.InputSchema->SetStringField(TEXT("type"), TEXT("object"));
Definition.InputSchema->SetObjectField(TEXT("properties"), MakeShared<FJsonObject>());
Definition.InputSchema->SetBoolField(TEXT("additionalProperties"), false);

FString Error;
bool bRegistered = Registry.RegisterSyncTool(Definition,
    [](const FMCPToolArguments& Arguments)
    {
        if (!Arguments->Values.IsEmpty())
        {
            return FMCPToolResult::ProtocolError(-32602, TEXT("No arguments expected."));
        }
        // Execute the UE operation and check its actual result here.
        return FMCPToolResult::Success(TEXT("Completed."));
    }, Error);
// If registration fails, report Error; the existing mapping is preserved.
```

注册表校验工具名称、回调和 Schema 根类型，保存 Schema 与调用参数的深拷贝。它不实现完整 JSON Schema 校验器；每个处理函数必须检查必填参数、类型和业务约束，并让实际行为符合声明的 `inputSchema`。

## 注册异步工具

异步处理函数接收完成回调，可以保留回调并在实际操作结束后调用。下面示例使用独立定义，替代上面的同步注册：

```cpp
#include "Async/Async.h"

Definition.Name = TEXT("project.background_example");
bool bAsyncRegistered = Registry.RegisterAsyncTool(Definition,
    [](const FMCPToolArguments& Arguments, FMCPToolCompletion Complete)
    {
        if (!Arguments->Values.IsEmpty())
        {
            Complete(FMCPToolResult::ProtocolError(-32602, TEXT("No arguments expected.")));
            return;
        }
        Async(EAsyncExecution::ThreadPool,
            [Complete = MoveTemp(Complete)]() mutable
            {
                // Perform work that does not access UObjects or editor APIs here.
                Complete(FMCPToolResult::Success(TEXT("Background work completed.")));
            });
    }, Error);
```

回调可以从任意线程完成，只有第一次完成有效。后台任务需按值捕获所需数据；不要跨线程并发读写 JSON DOM、UObject 或注册表。需要修改资产或调用编辑器 API 时，通过 `AsyncTask(ENamedThreads::GameThread, ...)` 回到主线程，确认实际操作和必要的保存成功后再返回成功结果。

## 调用与返回值

```cpp
auto Future = Registry.CallTool(
    TEXT("bsharness.editor_info"), MakeShared<FJsonObject>(), 30.0);

Future.Next([](FMCPToolResult Result)
{
    // This runs on the completing thread, not necessarily the game thread.
    // Inspect ProtocolErrorCode and bIsError before using Content/StructuredContent.
});
```

所有调用均返回 `TFuture<FMCPToolResult>`。同步工具在 `CallTool` 返回时已经完成；异步工具在完成回调被调用后才完成。不要在主线程对尚未完成的 Future 调用 `Wait()` 或 `Get()`，否则可能阻塞编辑器并使主线程回调无法执行。后续逻辑优先使用 `Next()`，需要操作编辑器时自行切回主线程。

返回值用于判断执行结果，不能把“任务已提交”当作成功：

| 情况 | 返回方式 |
| --- | --- |
| 操作成功 | `Success(...)`，MCP `result.isError = false` |
| 操作执行失败 | `Failure(...)`，MCP `result.isError = true` |
| 未知工具、非法参数 | `ProtocolError(-32602, ...)`，JSON-RPC `error` |
| 未知方法 | JSON-RPC `error.code = -32601` |
| 超时或注册表关闭 | 失败结果，结束调用等待 |

默认超时为 30 秒，可逐次调整为有限正数。超时通过编辑器 Core Ticker 检测，也在完成回调到达时检查。阻塞主线程的同步工具无法被强制中断，超时检查只能在其返回后执行。耗时操作应采用异步处理。

**超时和注册表关闭不会终止底层任务，也不会回滚已发生的修改。** 超时后的完成回调会被忽略。重试有副作用的工具前，应查询实际执行状态；必要时由具体工具实现幂等、进度和取消机制。

注册、注销、列表查询、调用、分发以及注册表析构均在主线程执行。网络线程应先把请求转交主线程。注销只阻止新调用，已经发起的调用可以继续完成；注册表关闭会使未完成调用返回失败。工具提供模块必须在卸载自身代码前停止并等待其后台任务完成；注册表不拥有这些任务。本模块禁用动态重载。

## MCP 请求示例

在主线程调用 `FMCPToolDispatcher::Dispatch(Registry, Request)`，等待返回的 Future 完成后由传输层发送响应：

```json
{"jsonrpc":"2.0","id":1,"method":"tools/list"}
```

```json
{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"bsharness.editor_info","arguments":{}}}
```

分发器不对通知生成响应；以无 ID 通知形式传入的工具调用也不会执行。响应 Future 的空指针表示无需发送消息。传输层负责 JSON 解析错误、批量请求策略以及非工具方法的处理。

## 验证

在编辑器控制台执行：

```text
Automation RunTests BSHarness.MCP
```

或者在 Session Frontend 的 Automation 页面搜索 `BSHarness.MCP`，选择全部测试并运行。测试覆盖注册、同步与异步调用、超时、关闭、协议分发、内置查询、控制台入口、实际打开和复用资产编辑器，以及配置默认值、失败重载回退和重载时的异步调用。内置工具测试以随插件提供的 9 个默认工具配置为基准；若禁用、改名或修改这些工具的默认行为，请先恢复默认配置再运行完整回归测试。配置测试使用独立注册表，不改写磁盘配置。资产测试只创建内存中的临时曲线资产，结束后清理，不写入项目资产文件。

本机 PowerShell 编译命令：

```powershell
& 'F:\EPIC\Engine58WithPDB\Engine\Windows\Engine\Build\BatchFiles\Build.bat' UEHarnessEditor Win64 Development '-Project=F:\UE5Project\UEHarness\UEHarness.uproject' -WaitMutex -NoHotReloadFromIDE
```

本机无界面测试命令：

```powershell
& 'F:\EPIC\Engine58WithPDB\Engine\Windows\Engine\Binaries\Win64\UnrealEditor-Cmd.exe' 'F:\UE5Project\UEHarness\UEHarness.uproject' '-ExecCmds=Automation RunTests BSHarness.MCP' '-TestExit=Automation Test Queue Empty' '-ReportExportPath=F:\UE5Project\UEHarness\Saved\Automation\BSHarnessMCP' '-abslog=F:\UE5Project\UEHarness\Saved\Logs\BSHarnessMCPTests.log' -unattended -nullrhi -nosound -nop4 -nosplash
```

检查 `Saved/Automation/BSHarnessMCP/index.json` 中的 `succeeded`、`failed`、`notRun` 实际计数，并查看 `Saved/Logs/BSHarnessMCPTests.log`。不能只看进程退出码；引擎有时在测试失败后仍返回 0。换机器时替换上述引擎路径与工程路径。


### 编辑器运行时验证新增处理器

当前 Development 编辑器运行时，可以用独立 DebugGame 构建和 `UnrealEditor-Win64-DebugGame-Cmd.exe -debug` 验证，避免替换正在使用的 DLL。本次 `assets.open` 已按此方式测试。正常 Development 编辑器要使用新增处理器，需要先保存工作并关闭编辑器，再执行上面的 Development 编译命令，然后重新打开工程；仅重启而不重新编译 Development 仍会加载旧处理器。
