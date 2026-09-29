# Agent Workbench 完整实现规格

版本：1.2，包含 /Game/ Content 资产候选区。
状态：需求规格，尚未针对用户工程实现或编译验证。
本文中的 FAgent*/SAgent*/IAgent* 都是建议的自定义类型名，不是 Unreal Engine 现成 API。

## 1. 目标与范围

在现有 UE 工程中创建项目级 Editor-only 插件 `Plugins/AgentWorkbench`，模块名 `AgentWorkbenchEditor`。
工具栏提供“Agent：新建对话”入口。每次点击创建新的非模态独立窗口和新的 Session。
新窗口的 Session 先保持临时状态；只有输入有效消息并成功发送、Run 开始后，才加入历史对话并持久化。空窗口、未发送草稿、发送校验失败均不产生历史条目。
历史列表允许重新打开对话。多个 Session 的上下文、模型、候选资产、运行状态及停止操作不得互相污染。
每个窗口允许选择模型和模型参数。项目设置管理连接、环境变量名称、默认模型、超时和执行限额。

新增目标：用户在 Content Browser 中选择 /Game/ 资产，再到目标 Agent 窗口点击“添加选中资产”，将引用加入该 Session 的候选区，供后续请求携带路径和只读工具使用。

首版不支持任意写工具、后台常驻服务、多人跨机器同步、任意脚本执行或自动编辑蓝图。
当前对话发送使用项目设置所选的真实 Provider；模型请求适配层独立于 UI。旧 Mock 历史仅作历史记录保留。

## 2. 固定 UI 布局

默认窗口客户区为 1400×900 Slate 单位，建议最小 1100×700；使用原生编辑器样式及 DPI 缩放，不用绝对坐标拼接窗口。
横向 SSplitter 初始比例为 0.20 / 0.50 / 0.30，可拖动调整。
窗口为非模态且非永久置顶，用户必须能回到 Content Browser 选择资产。

```text
┌─────────────────────────────────────────────────────────────────────────┐
│ Agent Workbench · 当前对话标题                           [－] [□] [×]    │
├────────────────┬─────────────────────────────────┬──────────────────────┤
│ 历史对话        │ 连接 [默认 ▾] 模型 [名称 ▾] [参数] │ 执行详情             │
│ [搜索历史]      ├─────────────────────────────────┤ 当前步骤描述         │
│                │                                 │                      │
│                │ 对话记录                        │ ▼ 模型请求           │
│ ● 当前对话      │ 用户消息 / Agent 最终回复         │   输入 / 输出 / 耗时 │
│ ○ 另一对话      │                                 │                      │
│                │                                 │ ▼ 工具调用           │
│                ├─────────────────────────────────┤   工具名 / 参数 / 结果│
│                │ 候选资产 · 2 项                  │                      │
│                │ [添加选中资产] [清空候选] [折叠]  │                      │
│                │ /Game/BP/BP_Player.BP_Player [X]│                      │
│                │ /Game/BP/BP_Enemy.BP_Enemy   [X]│                      │
│                ├─────────────────────────────────┤                      │
│                │ 用户输入框                       │ [全部] [工具] [错误] │
│                │ Ctrl+Enter 发送，Enter 换行       │ [导出脱敏日志]       │
│                │                   [停止] [发送] │                      │
├────────────────┴─────────────────────────────────┴──────────────────────┤
│ Session: xxxx · 状态 · 模型 · 本 Run 使用资产数 · 已执行步骤              │
└─────────────────────────────────────────────────────────────────────────┘
```

左栏：历史搜索、标题、更新时间、状态；仅展示已经开始过 Run 的对话，新建空窗口不出现历史行；不显示窗口内“新建对话”按钮或行内“分叉”按钮。左键点击历史时，当前窗口完整切换到所选 Session，模型、消息、候选、草稿、执行详情和底部状态同时更新，不新建窗口。目标已在另一窗口显示时交换两个窗口的 Session，保证一个 Session 只有一个可写窗口。右键指定历史行显示垂直菜单“重命名 / 删除 / 新建分叉”；选中行按 F2 重命名、按 Delete 删除。重命名仅修改 Session 标题，立即更新窗口标题并保存；空标题无效。删除只移除该 Session 的历史和候选引用，不修改真实资产；新建分叉后在当前窗口切换到分叉。
中栏顺序固定：模型栏 → 对话列表 → 候选资产区 → 用户输入及操作栏。
对话区：用户消息靠右、Agent 最终回复靠左，分别用卡片和角色标签区分；正文可选择复制，也提供整条复制按钮。仍只显示用户输入与每轮 Agent 最终回复。
右栏：按每轮对话组成树，轮次下按时间顺序列出执行步骤，有详情的步骤继续展开为详情节点。轮次标题使用用户问题预览和状态；步骤标题只显示描述，不显示 Run ID 或事件序号。详情展示解码后的模型输入、工具参数、工具结果和错误；步骤与详情文本可选择复制，并提供复制按钮。内部仍保留 Run ID 用于归属校验。新事件刷新时保留已展开节点和用户浏览位置。
候选区默认高约 160，空态紧凑；展开状态建议限制到 240 左右并内部滚动，避免候选数量增多挤掉输入框。
输入框与发送、停止按钮固定在中栏底部，不跟随整段聊天记录滚出视口。
每个候选行只显示完整 Object Path 和右侧 X 删除按钮；长路径可折行，Tooltip 保留完整路径。
使用列表控件处理长对话和执行事件，不用一个无限增长的文本框。
Enter 换行；Ctrl+Enter 才发送，不能把中文输入法提交候选误判为发送。
当前 Session 有活跃 Run 时禁用发送，但允许编辑草稿、下一轮模型参数和候选列表。
运行中修改时显示“更改将在下一次发送时生效”。

建议 Widget 拆分：
- SAgentChatWindow
- SAgentHistoryPanel
- SAgentModelOptionsBar
- SAgentConversationList
- SAgentAssetCandidatePanel
- SAgentAssetCandidateRow
- SAgentExecutionPanel

不要把 UI、HTTP、存储和工具执行全部写在一个 Widget 类中。

## 3. Window / Session / Run 语义

工具栏新建：创建新 SessionId、新 Session、新 Runner、新 SWindow。
首次有效发送开始 Run 后才将该 Session 加入历史并写入 JSON；关闭或切走尚未发送的临时 Session 时丢弃它，不生成历史文件。已有历史的分叉复制其 Run 记录，因此创建后可立即在历史中看到。
打开历史：在当前窗口切换；若目标 Session 已在另一窗口打开，则交换两个窗口所显示的 Session，不并发创建两个可写副本。
分叉历史：复制一个完整消息检查点、模型选项和候选引用为值副本，生成新 SessionId，原 Session 不变。
不复制 HTTP 对象、取消令牌、执行句柄、未完成工具状态和可变服务端会话句柄。
同一 Session 首版只允许一个活跃 Run；不同 Session 可以同时执行。
关闭活跃窗口时提示“停止任务并关闭 / 返回”；不默认改为后台运行。
关闭确认发生在关闭请求阶段；关闭完成事件只负责解除绑定和清理。
没有活跃任务则直接关闭视图；只保存已开始过 Run 的 Session。
项目退出或崩溃后，未完成任务恢复为 Interrupted，不自动重新执行模型或工具请求。

## 4. 候选资产：添加与交互

### 4.1 输入来源

只读取 Content Browser 资产选择，不读取 World Outliner 中选中的 Actor。
点击“添加选中资产”时读取一次选中列表；选择变化不能自动广播到所有 Agent 窗口。
初版来源明确为主 Content Browser 当前选中的资产。多个 Content Browser 的实际主浏览器行为需在目标引擎版本验证，并在 Tooltip 标明来源，不能声称任意浏览器选择都自动合并。
读取链路参考：FContentBrowserModule → IContentBrowserSingleton::GetSelectedAssets → TArray<FAssetData>。[E1]

支持 /Game/ 下有有效完整对象路径和 AssetClassPath 的 Content 资产，包括蓝图、纹理、材质等；排除目录、World Outliner Actor、越界路径和未解析重定向器。
按 AssetData 元数据和路径判断，不根据 BP_ 文件名前缀判断，也不因不是蓝图就拒绝。
不支持的资产跳过并说明原因。没有选中资产时显示“请先在主内容浏览器选择 /Game/ 资产”。
不得为识别全部选中对象而逐个强制 GetAsset/TryLoad；无法确认的类型显示 Unknown 并注明。
`FAssetData::IsInstanceOf`、类解析策略及 Asset Registry 类型查询均以本地签名为准。[E2][E4]

### 4.2 添加规则

对已支持的选中资产做路径规范化，并在当前 Session 内按规范化完整对象路径去重。
连续添加是追加，不替换此前候选。已经存在的条目不能覆盖用户状态。
所有显示在候选列表中的条目用于下一次发送；操作后显示新增数、重复数、跳过数。历史数据中的旧勾选字段保留以兼容旧文件，但当前界面不显示复选框。
行右侧“X”只移除当前 Session 的引用；“清空候选”只清空该 Session 的候选列表。
不触发删除、保存、重命名、复制、加载全部图、蓝图编译或模型请求。
拖拽添加资产可作为后续增强，不是首版阻塞项；如实现，必须复用同一校验、去重和 Session 路由函数。

### 4.3 归属与快照

每个 Session 自己保存候选数组，禁止全局 CurrentSelectedAssets 作为对话上下文来源。
每次发送按以下顺序执行：
1. 校验当前候选的引用格式、允许根路径及已知存在状态；注册表扫描未完成不能直接判定 Missing。
2. 已知失效的资产阻止发送并提示移除后重新选择；没有候选仍允许普通聊天。
3. 复制当前候选、模型配置和用户输入到不可变 FAgentRunInputSnapshot。
4. 再开始排队、模型调用和工具执行。
5. 后续工具只能使用此快照，不能重新读取 Content Browser 或 Session 当前可编辑候选列表。

运行中在 A 窗口增删候选只影响 A 的下一次发送；B 不变，A 当前 Run 也不变。
当前候选指下一次发送要附加的引用。移除不会撤回先前请求中已经发送的内容，也不会抹掉历史消息中出现的路径。
快照冻结的是引用和请求配置，不是整个项目资产内容。工具读取发生时必须记录实际读取时间、解析路径及可识别的修改状态。

## 5. 数据模型与资产路径

候选条目建议字段：

| 字段 | 含义 |
|---|---|
| CandidateId | 当前会话内稳定 ID，可用 FGuid |
| DisplayName | 显示名称 |
| ObjectPath | 规范化 FSoftObjectPath，工具主标识与去重依据 |
| PackageName | UE 包名，便于 UI 和工具展示 |
| AssetClassPath | 资产类型路径，例如 /Script/Engine.Blueprint，不是生成类路径 |
| bIncluded | 下一次发送是否附加 |
| ValidationStatus | Unknown / Valid / Missing / Unsupported / Moved 等 |
| AddedAt | 加入当前会话的时间 |

持久化时使用自有 DTO，ObjectPath 等以字符串保存，并有 SchemaVersion。
不要把临时 FAssetData、UObject 指针或 Widget 引用直接序列化为历史。[E2]

路径示例：
```text
PackageName: /Game/Blueprints/BP_Enemy
ObjectPath:  /Game/Blueprints/BP_Enemy.BP_Enemy
AssetClassPath: /Script/Engine.Blueprint
```

通过 FAssetData.GetSoftObjectPath() 等目标版本 API 获取，不用猜路径或从磁盘绝对路径拼接。
FSoftObjectPath 是对象软引用，支持 /package/path.assetname 形式。[E2][E3]
读取蓝图资产时使用蓝图对象路径，不要无条件追加 _C，或用 GeneratedClass 路径替代蓝图资产引用。
GeneratedClass 与 UBlueprint 本体是不同对象；需要类路径时作为单独可选字段获取并注明。[E5]

Session 建议保存 SessionId、Title、CreatedAt/UpdatedAt、Messages、ModelOptions、CandidateAssets、RunSummaries、DraftText。
消息结构必须能表示 role、文本、tool calls、tool results 和关联 ID，不能只保存 UI 渲染字符串。

RunInputSnapshot 至少包含 SessionId、RunId、用户消息或消息快照、模型配置快照、IncludedAssets、读取工具权限、创建时间。
包含上述纯数据的快照可以存储；凭据、请求对象、令牌和线程句柄不能进入该结构的序列化部分。

## 6. 模型输入与只读资产工具

发送时把当前候选资产组成结构化上下文，与当前用户消息关联，持久化时保留此轮的引用快照。
不要反复把一个可变“全局候选列表”覆盖到历史请求中。
不要把全部资产图和二进制 .uasset 自动塞进 prompt。先传路径，按模型工具调用按需读取。

示例上下文数据，实际 SessionId/RunId/CandidateId 应使用真实值：
```json
{
  "context_type": "ue_asset_candidates",
  "session_id": "session-guid",
  "run_id": "run-guid",
  "assets": [
    {
      "asset_id": "candidate-guid",
      "name": "BP_Enemy",
      "object_path": "/Game/Blueprints/BP_Enemy.BP_Enemy",
      "package_name": "/Game/Blueprints/BP_Enemy",
      "asset_class_path": "/Script/Engine.Blueprint"
    }
  ]
}
```

使用 JSON 库和 Provider 适配层组装真实请求，不把上面的自定义字段当成所有服务商都原生支持的请求字段。
Provider 可将这段内容作为带标签的用户上下文内容编码进实际协议。
发送前的请求预览或执行区必须能够看到实际送出的脱敏模型输入，以及本轮使用哪些资产。

工具建议：
- list_context_assets()：只返回当前 Run 快照。
- get_asset_metadata(object_path)：按已授权的候选路径返回元数据。
- read_blueprint_asset(object_path, detail_level, cursor)：返回本体的父类、变量、函数、组件、图概要；详细节点与连线按分页请求返回。

前两个工具先完成；蓝图结构读取在真实模型阶段前后完成，但不能用伪造数据冒充实际读取。
对不支持的蓝图子类型返回明确 Unsupported，不编造 Widget Tree、动画图或组件信息。
读取内存中的蓝图时注明读取的是当前编辑器状态，并报告未保存修改的可识别状态；不能自动编译或保存来“修复”读取。
读取 UObject 和构建纯数据快照在 Game Thread；不要在网络线程访问蓝图对象。
大型加载使用目标版本支持的异步加载或受控调度；限制单次工作量，并允许取消后丢弃结果。
工具输出限制最大条目数/长度，有截断时显式返回 truncated 和可用的后续读取方式。

当前 MCP 桥接允许对经过校验的 `/Game` 资产路径执行只读元数据和蓝图变量查询；附加候选会进入本 Run 快照，帮助模型准确选择资产。打开资产编辑器仅允许本 Run 候选快照中的非关卡资产。

UE5.8 MCP 扩展在运行时发现当前已加载的 ToolsetRegistry 工具集及独立 MCP 工具。Run 开始时在内存中冻结完整工具目录和处理器身份；模型先按需搜索目录、读取目标参数 Schema，再请求调用，避免将大型工具集全部塞进每次模型请求。历史只保存实际工具调用与结果，不保存完整工具目录及处理器映射。所有实际 UE MCP 工具调用及 `assets.open` 均在执行前显示完整工具名与参数，并要求用户逐次允许；无人值守默认拒绝。审批后还要重新校验 Run 取消、总超时、工具身份和参数；目录查询本身不执行引擎工具。
仅发送候选名称不等于资产内容已读取，UI 和模型输出必须保持区分。
允许根路径默认为 /Game/。越界路径、未注册工具、额外文件读写或任意 shell 请求应被拒绝并记录。
资产文本、注释和工具返回是数据，不应赋予其高于用户或系统设置的指令优先级。
不要仅依赖模型提示词限制权限；本地工具分派必须校验路径、参数和权限。

## 7. 架构与并发隔离

建议职责：
- FAgentWorkbenchEditorModule：模块生命周期、菜单注册。
- UAgentWorkbenchSettings：项目配置。
- FAgentSessionManager：会话注册、窗口映射、历史恢复与分叉。
- FAgentSession：可持久化会话数据。
- FAgentRunner：当前 Session 的任务状态机、请求、取消、工具调用循环。
- IAgentModelProvider：协议能力、请求构造和响应解析；提供 Mock 实现。
- FAgentAssetContextService：选择读取、元数据提取、路径校验，不能拥有所有 Session 共用的可变候选数组。
- FAgentToolScheduler：只读工具注册和调度。
- FAgentHistoryStore：有序保存与恢复。

每个 Session 独立消息、模型配置、候选、Runner、取消令牌、请求句柄和流式缓冲。
Manager 可以全局共享，但不能保存全局 CurrentSession/CurrentRequest/CurrentModel 来路由各窗口任务。
共享可以是无会话可变状态的服务、工具注册表、只读元数据缓存和全局并发限流。
所有 UI/Session 可变状态统一在 Game Thread 管理，避免仅因共享指针使用 ThreadSafe 就误以为对象内部状态线程安全。

标识至少包括 SessionId、RunId、RequestId、ToolCallId、EventSequence。
每次重试使用新 RequestId。同一 Run 中的每次模型调用都有独立请求标识。
工具结果必须校验所属 Run 和待完成调用 ID；重复、晚到结果只能记录为忽略，不得重复推进模型循环。
取消时先使当前任务/请求失效再取消底层请求。旧回调晚到不更新新 Run，也不触发下一个工具。
HTTP 传输完成不代表 API 成功：必须校验 HTTP 状态、响应体、JSON、工具参数与协议错误。[E6]

建议 Run 状态：Queued、RequestingModel、ExecutingTool、Completed、Failed、Cancelling、Cancelled、TimedOut、Interrupted。
首版无审批/写工具时不要提供虚假的“允许写入”开关。
全局 MaxConcurrentRuns 只限制执行，不能限制新建窗口。排队 Run 也要持有发送时快照。
取消排队 Run 不应影响已运行的其他 Session；队列顺序应可预测。

## 8. 项目设置与窗口模型参数

使用目标版本的 UDeveloperSettings 暴露 Project Settings → Plugins → Agent Workbench。[E7]
项目设置建议字段：
- ApiKeyEnvironmentVariable：默认 UE_AGENT_API_KEY，只存名称。
- BaseUrlEnvironmentVariable：默认 UE_AGENT_BASE_URL，只存名称。
- ProviderType：项目设置可选择 DeepSeek、OpenAI、Anthropic、Google Gemini、OpenRouter、Ollama；默认选择 DeepSeek。当前支持非流式文本及只读工具调用循环；更深入的蓝图读取仍待实现。六种协议已有结构测试，真实工具循环仅在 DeepSeek 验证。
- 模型接口的相对路径或明确的 endpoint 语义，避免重复拼接 /v1。
- DefaultModel、AvailableModels，可配置，不硬编码不存在的模型名。
- RequestTimeoutSeconds：默认 120。
- ToolTimeoutSeconds：默认 60。
- RunTimeoutSeconds：默认 900，包括排队和工具循环，具体计时规则必须实现并写明。
- MaxToolSteps：默认 16。
- MaxConcurrentRuns：默认 4，与窗口数量无关。
- MaxCandidateAssets：默认 64，超出时明确提示而非静默丢失。
- bPersistDetailedPayloads：默认 false，关闭不影响必要的结构化会话恢复。

默认值是设计起点，不是固定服务商要求。
三个超时分别实现：HTTP 请求级、工具执行级、Run 总体级；不能用一个 HTTP timeout 代替全部。[E6]
读取环境变量仅用于运行时连接，不修改当前进程环境、不枚举环境变量、不输出变量值。[E8]
变量缺失时显示变量名称和“未配置”，不发送无效请求。
界面只能显示已配置/未配置，不能显示密钥正文或部分密钥。
不要将密钥、Authorization、含令牌的 URL 参数写入历史、UE_LOG、导出日志或错误文本。
凭据更改的读取时机固定为下一次 Run；不在正在运行的请求中途替换。
编辑器进程的环境与用户后来修改的系统设置不一定同步；不声称每次读取都能自动刷新进程外环境。

窗口模型选项至少包含模型名称和最大输出 Token；Temperature、流式等仅在适配器声明支持时启用。
项目默认 → 新建 Session 时复制 → 发送时复制到 Run；窗口修改不能写回全局默认配置。
已有 Run 使用快照；窗口运行中修改参数只对下一轮生效。
首个真实 Provider 可先采用非流式，能力声明为不支持流式，不展示可点击但无效的流式开关。
如实现流式，必须正确处理任意网络分块、UTF-8 边界、多个事件合并、工具参数增量、错误和取消。
不因为多个服务使用 HTTP 就假定请求参数、工具协议和流式事件完全相同。

## 9. 执行区与日志

按 Run 和步骤展示结构化事件，不只拼接文本：
RunStarted、ModelRequestStarted、ModelRequestCompleted、ToolStarted、ToolCompleted、RunCompleted、RunFailed、RunCancelled、RunTimedOut 等。
每条事件包含归属标识、序号、类型、时间、状态、摘要和脱敏详情。
模型输入显示实际编码后请求的脱敏视图；模型输出只显示服务实际返回的内容和工具调用。
不伪造或承诺获得模型隐藏推理。
工具事件包含工具名、参数、实际读取的资产路径、结果、截断标记和错误。
单个工具失败只作用于其所属 Run；采用“返回工具错误让模型处理”还是终止 Run 必须有明确策略，并受步骤/时间上限限制。
UI 刷新批处理，日志有容量上限；不能无限占用内存。

## 10. 历史与路径变化

保存位置建议：
```text
<Project>/Saved/AgentWorkbench/
  index.json
  Sessions/<SessionId>/session.json
  Sessions/<SessionId>/events.jsonl
```

会话包含 SchemaVersion、消息、模型参数、候选资产、用户草稿及 Run 摘要；模型工具消息必须保留完整关联以支持恢复。
详细输入/输出按隐私开关保存；凭据永不保存。
同一 Session 的写入通过队列串行化，共享 index 统一写入。使用临时快照和受控替换，检查失败并保留可恢复版本；不要承诺所有文件系统都具有相同原子替换行为。
index 可从各 Session 快照重建；坏文件应隔离并报告，不能导致所有历史丢失。
只读打开历史，不重新发送网络请求、不自动重放工具。

资产重命名/移动时可用目标引擎的资产事件更新“当前候选列表”，或在再次校验时提示重新选择。
不能声称软路径在所有跨重启/跨重命名情况下都会自动保持有效。
既有 Run 输入快照与历史路径保持原样，不回写修改过去已发送的请求。
如果运行中路径已失效，返回明确 AssetMissing/AssetChanged，而不是静默读取另一个对象。
注册表仍在扫描时状态为 Unknown/Scanning，等待就绪或提示用户，不能把未索引直接当删除。
分叉候选时做值复制；随后两边移除、添加和路径更新互不影响。

## 11. 生命周期与关闭

菜单、编辑器事件、资产事件、定时器、窗口事件在关闭/模块退出时解除绑定。
避免 Session/Runner/Widget/Window 互相强引用形成引用环。
没有有效视图时不回调悬空 Widget。
模块退出先阻止新任务，再取消当前请求与待执行工具，停止定时器/任务队列，完成受控持久化和视图清理。
弱指针防悬空不等于模块代码可以任意卸载；必须确保没有仍会执行插件函数的后台任务。
不要在 Game Thread 等待必须由 Game Thread 回调才能完成的任务，避免关闭死锁。
不承诺未经验证的动态热卸载能力。涉及模块结构或反射类型变更时，按目标工程的可靠构建流程验证。

## 12. 首版成功标准

必须能看到并验证：三次点击出现三个独立窗口；在 A/B 添加不同蓝图；请求预览各自仅携带本轮路径；并发执行不串话；取消 A 不影响 B；A 的迟到响应不污染新任务；历史恢复候选与参数；真实蓝图读取路径正确；移除候选不操作真实资产；日志无凭据。

每个阶段的“代码完成”“编译通过”“自动化通过”“编辑器验收通过”必须分开记录。
没有引擎/网络/密钥/图形环境时继续完成能完成的实现与静态检查，但将相应验证项标为未验证，不捏造结果。
