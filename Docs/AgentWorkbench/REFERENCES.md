# 官方参考

核查日期：2026-09-28。
这些网页可能默认展示较新的 UE 文档。用户项目的 UE 小版本尚未知；实现必须以本地目标版本头文件和引擎示例为准。
本规格的 UI、类拆分、阶段划分及验收策略是本任务的工程设计，不是声称官方要求采用这一套架构。

## Codex

[C1] OpenAI：Custom instructions with AGENTS.md。说明项目规则文件的发现与分层；现有会话中新建文件不应假定自动重新加载。
https://developers.openai.com/codex/guides/agents-md/

[C2] OpenAI：Prompting。包含按具体文件和范围实施、复现问题、运行相关验证和报告结果的工作流。
https://developers.openai.com/codex/prompting/

## Unreal Engine

[E1] IContentBrowserSingleton。GetSelectedAssets 返回主 Content Browser 选中资产；SyncBrowserToAssets 提供资产导航能力。
https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Editor/ContentBrowser/IContentBrowserSingleton

[E2] FAssetData。轻量资产信息、GetSoftObjectPath、IsInstanceOf 等；页面将其描述为临时结构，不作为本插件持久历史的数据格式。
https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/CoreUObject/FAssetData

[E3] FSoftObjectPath。软对象引用及 /package/path.assetname 格式。
https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/CoreUObject/FSoftObjectPath

[E4] Asset Registry。通过资产元数据查询而不必为基础识别加载完整资产。
https://dev.epicgames.com/documentation/en-us/unreal-engine/asset-registry-in-unreal-engine

[E5] UBlueprint。蓝图资产类型、GeneratedClass、图与变量等对象信息。
https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Engine/UBlueprint

[E6] IHttpRequest。请求级取消、超时和回调相关接口。
https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/HTTP/IHttpRequest

[E7] UDeveloperSettings。项目设置对象基类。
https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/DeveloperSettings/UDeveloperSettings

[E8] FGenericPlatformMisc / GetEnvironmentVariable。读取当前进程可见的环境变量。
https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Core/FGenericPlatformMisc

## 模型工具协议

[M1] OpenAI：Function calling。工具调用包含模型输出调用、应用执行工具、返回工具结果、继续模型生成的流程。真实 Provider 需要按实际服务协议适配，不应把 UI 事件直接当成服务端协议。
https://developers.openai.com/api/docs/guides/function-calling
