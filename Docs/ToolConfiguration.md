# 使用配置注册工具

工具配置位于插件的 `Config/MCPTools.json`。本次升级需要编译并重启编辑器一次；之后修改工具配置只需执行重载命令，不需要修改 C++、重新编译或重启编辑器。

## 新增一个工具

文件采用以下结构，`tools` 是完整的配置工具清单：

```json
{
  "version": 1,
  "tools": [
    { "name": "bsharness.project_info", "handler": "project_info" }
  ]
}
```

实际文件已包含 8 个默认工具。要新增工具，请把新对象追加到现有 `tools` 数组，不要用上面的最小示例覆盖整个文件。删除条目或设置 `enabled: false` 会在重载后移除对应工具。

例如，追加一个“搜索材质”工具：

```json
{
  "name": "project.search_materials",
  "handler": "assets.search",
  "description": "搜索项目材质，默认返回 20 个，最多返回 50 个。",
  "enabled": true,
  "parameters": {
    "path": { "default": "/Game" },
    "class_path": {
      "default": "/Script/Engine.Material",
      "enum": ["/Script/Engine.Material"]
    },
    "limit": { "default": 20, "maximum": 50 }
  }
}
```

该工具复用 `assets.search` 的实现和参数类型，配置中的默认值会实际注入调用参数。只允许材质类型的限制由单值 `enum` 实现；仅设置 `default` 时，调用者仍然可以显式传入其他值。

## 重载和测试

保存文件后，在编辑器 **Output Log → Cmd** 输入框逐条执行：

```text
BSHarness.MCP.Reload
BSHarness.MCP.List
BSHarness.MCP.Call project.search_materials {}
BSHarness.MCP.Call project.search_materials {"name_contains":"Floor","limit":10}
```

`Reload` 应显示加载成功、工具数量和配置路径；`List` 中应出现新工具。调用完成后，日志中的 `MCP response:` 返回 `result.isError=false`，数据位于 `structuredContent`。搜索结果中每个资产的 `classPath` 应为 `/Script/Engine.Material`；项目没有材质时返回成功的空列表。`isScanning=true` 表示资产注册表仍在扫描，稍后可再次查询。

验证参数限制：

```text
BSHarness.MCP.Call project.search_materials {"limit":51}
```

应返回 JSON-RPC `error.code=-32602`。这些控制台命令测试的是本地工具层，外部 MCP 客户端连接仍需传输与初始化流程。

## 可配置的字段

| 字段 | 含义 |
| --- | --- |
| `name` | 对外工具名，必填，区分大小写，不可重复 |
| `handler` | 已实现的处理器 ID，必填，区分大小写 |
| `description` | 对 Agent 展示的说明；省略则使用处理器说明 |
| `enabled` | 默认 `true`；设为 `false` 后不注册 |
| `parameters` | 对已有参数的说明、默认值和约束进行覆盖 |

每个参数可配置 `description` 和 `default`。整数参数还可配置 `minimum`、`maximum`；字符串参数可配置 `enum`。参数类型、必填信息及未覆盖的约束继承自处理器，不需要重复写整份 `inputSchema`。

约束只能收窄：例如处理器允许 `limit` 为 1–500，配置可设为 1–50，不能扩成 1–1000。处理器已经定义的枚举也只能取其子集。收窄范围或枚举时，要确保继承的默认值仍然有效，必要时同时修改 `default`。

默认值会反映在 `tools/list` 的 Schema 中，并在执行前补入缺失参数。调用者显式提供的值优先。为原本必填的参数提供合法默认值后，调用者可以省略该参数。默认值仍受参数类型和约束校验；不会把字符串数字等错误类型隐式转换。

多个工具可以引用同一处理器，使用不同的名称、说明和参数配置。同步或异步执行方式由处理器决定，配置不会把编辑器 API 移到后台线程。

## 查询可用处理器

```text
BSHarness.MCP.Handlers
```

命令输出配置文件位置、处理器 ID 和完整参数 Schema。当前可用 ID：

| ID | 能力 |
| --- | --- |
| `editor_info` | 查询 UE 版本 |
| `project_info` | 查询项目信息 |
| `assets.search` | 搜索资产 |
| `assets.get` | 查询资产详情与依赖 |
| `assets.open` | 加载并打开资产编辑器，已打开时切换到已有窗口 |
| `actors.list` | 查询编辑器场景 Actor |
| `source.list` | 异步列出源码文件 |
| `source.read` | 异步读取源码 |

配置负责把已有能力映射为工具。新增尚未实现的 UE 操作仍需要新增处理器；`handler` 不是可以任意调用的 C++ 函数名，也不是远程 MCP Server 地址。

## 重载行为

- 先验证完整文件，再一次性更新配置所管理的映射。语法错误、未知字段、未知处理器、重复工具名、未知参数或非法约束都使重载失败，错误信息标明条目，原有工具保持可用。禁用条目同样需要合法配置。
- 已发起的异步调用保留原来的参数并继续完成；之后的新调用使用新配置。删除或禁用工具不取消已有调用。
- 其他模块通过 C++ 另外注册的工具保持不变；若配置与这些工具重名，重载失败。
- 首次启动时如果文件缺失或错误，会记录错误且不注册配置工具。修复文件后执行 `Reload` 即可。
- 修改文件不会自动触发重载。文件必须是标准 JSON，不支持注释或尾随逗号，大小最多 1 MiB，条目最多 1000 个。

仅运行配置相关的自动化测试：

```text
Automation RunTests BSHarness.MCP.Configuration
```

这 3 项测试使用独立注册表，验证默认值、参数限制、重命名与禁用、错误配置回退、不相关工具的保留，以及重载期间的异步调用；不会修改你的配置文件。完整测试流程见 [MCPTools.md](MCPTools.md)。
