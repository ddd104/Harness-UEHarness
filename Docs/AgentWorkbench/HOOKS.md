# Agent Workbench Hook 扩展接口

`BSHarnessHooks` 是 Editor-only 模块，公开注册表位于 `Source/BSHarnessHooks/Public/BSHarnessHookRegistry.h`。`AgentWorkbenchEditor` 负责 Run 状态机、模型请求、工具目标解析和最终派发；扩展模块在启动时向注册表登记处理器，在关闭前用返回的 `FBSHookHandle` 注销。`BSHarnessTools` 的工具注册表继续管理工具名称、Schema 和实际处理函数。Hook 仅作用于 Agent Workbench 的执行路径；直接调用 MCP 分派器的入口不经过这些 Hook。

## 触发顺序

1. 有效输入及候选资产形成临时快照后，`PromptSubmitting` 可拒绝提交或追加 `AdditionalModelContext`。用户原文、Session/Run ID、模型和候选路径保持不变；追加内容仅进入本轮冻结的模型对话，最多 16000 字符。成功开始 Run 后发送 `PromptSubmitted`。
2. 每次工具提议先由 Workbench 解析为真实目标，核对本 Run 冻结的处理器身份和参数 Schema。需要审批的调用由独占的 `ToolApproval` 处理器决定；没有可用的内置审批处理器时拒绝。
3. 审批通过后按优先级运行 `ToolBefore`。处理器可继续、拒绝，或为已经完成的操作提供结果；`Handled` 调用不会进入工具桥接层。随后运行 `ToolReady`，供已批准调用准备编辑器视图，并可请求等待可见引擎帧。实际 UE MCP 工具仍由 Workbench 保证跨引擎帧派发。
4. 派发前再次核对取消、期限、真实目标、冻结的处理器身份和批准时的参数。完成或拒绝后发送 `ToolAfter`，其中包含最终结果、是否进入工具桥接层及结果来源；进入桥接层后仍可能被其最终校验拒绝。扩展读取的是值类型上下文和序列化结果，不能通过共享 JSON 指针改写派发参数或工具返回。
5. Run 完成、失败、取消、总超时或正常关闭时发送一次 `RunExited`。历史恢复时将未完成 Run 标为 Interrupted，不重新执行 Hook 或工具。

## 注册和并发约定

- 每个 Hook 点内名称唯一；优先级数值较小的先运行，同优先级保持注册顺序。注册和触发发生在 Game Thread。`ToolBefore` 与 `ToolApproval` 的异步 continuation 可从其他线程调用；注册表只接受首次结果，再交回 Game Thread。
- Hook 上下文带 SessionId、RunId，并在工具阶段带 RequestId、ToolCallId 和本地唯一 InvocationId。扩展的可变状态应按这些 ID 隔离；不要保存可写的 Session、Slate 或 UObject 指针以供后台线程使用。
- 模块关闭时先阻止新工作并结束正在运行的 Session，再注销处理器。动态热卸载需要额外验证，不应假定持有的异步回调能安全跨模块卸载。
- 审批模式来自发送时冻结的 Run 快照。Hook 的批准决定不会替代工具桥接层对身份、Schema、路径、取消和超时的最终校验。

实现与验证状态见 `PROGRESS.md`。
