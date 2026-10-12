#pragma once

#include "CoreMinimal.h"
#include "MCPToolTypes.h"

/**
 * Extension points owned by AgentWorkbenchEditor. They are editor-only and are
 * invoked on the Game Thread. The registry used to live in a separate
 * BSHarnessHooks module and is now a component of this module.
 */
enum class EAgentHookPoint : uint8
{
    PromptSubmitting,
    PromptSubmitted,
    ToolBefore,
    ToolApproval,
    ToolReady,
    ToolAfter,
    RunExited
};

struct FAgentHookHandle
{
    EAgentHookPoint Point = EAgentHookPoint::PromptSubmitting;
    uint64 Id = 0;

    bool IsValid() const { return Id != 0; }
};

/** Only AdditionalModelContext may be changed by a PromptSubmitting handler. */
struct FAgentHookPromptContext
{
    FGuid SessionId;
    FGuid RunId;
    FString UserPrompt;
    FString ModelId;
    TArray<FString> CandidatePaths;
    FString AdditionalModelContext;
};

enum class EAgentHookApprovalMode : uint8 { Ask, Smart, Unrestricted };

/** Tool identity and arguments are resolved and frozen before any tool hook runs. */
struct FAgentHookToolContext
{
    FGuid SessionId;
    FGuid RunId;
    FGuid RequestId;
    // Generated locally for each invocation; do not use the model's ToolCallId as a key.
    FGuid InvocationId;
    FString ToolCallId;
    FString RequestedName;
    FString TargetName;
    FString DisplayName;
    FString FrozenHandlerId;
    FString ArgumentsJson;
    bool bRequiresApproval = false;
    // The Workbench maps its Ask/Smart/Unrestricted settings to this value.
    EAgentHookApprovalMode ApprovalMode = EAgentHookApprovalMode::Ask;
};

enum class EAgentHookToolDecision : uint8
{
    Continue,
    Deny,
    Handled
};

struct FAgentHookToolBeforeResult
{
    EAgentHookToolDecision Decision = EAgentHookToolDecision::Continue;
    FString Reason;
    FMCPToolResult Result;
    // A handler that opened an editor can request an engine frame before dispatch.
    bool bNeedsVisibleEditorFrame = false;
};

struct FAgentHookToolApprovalResult
{
    bool bApproved = false;
    FString Reason;
};

enum class EAgentHookToolOutcomeKind : uint8
{
    Dispatched,
    Denied,
    Handled,
    Invalid,
    Cancelled,
    TimedOut
};

struct FAgentHookToolOutcome
{
    EAgentHookToolOutcomeKind Kind = EAgentHookToolOutcomeKind::Invalid;
    bool bIsError = false;
    // Serialized copy of the final MCP result. Hooks cannot mutate the result
    // subsequently sent to the model through shared JSON pointers.
    FString ResultJson;
    bool bApproved = false;
    bool bWasDispatched = false;
};

enum class EAgentHookRunExitReason : uint8
{
    Completed,
    Failed,
    Cancelled,
    TimedOut,
    Closed
};

struct FAgentHookRunExitContext
{
    FGuid SessionId;
    FGuid RunId;
    EAgentHookRunExitReason Reason = EAgentHookRunExitReason::Completed;
    FString Detail;
};

using FAgentHookPromptSubmittingHandler = TFunction<bool(FAgentHookPromptContext&, FString& /* OutReason */)>;
using FAgentHookPromptSubmittedHandler = TFunction<void(const FAgentHookPromptContext&)>;
// Continuations may be called from any thread; the registry delivers only the first
// result on the Game Thread. A handler may keep its continuation for later work.
using FAgentHookToolBeforeContinuation = TFunction<void(FAgentHookToolBeforeResult)>;
using FAgentHookToolBeforeHandler = TFunction<void(const FAgentHookToolContext&, FAgentHookToolBeforeContinuation)>;
using FAgentHookToolBeforeCompletion = TFunction<void(FAgentHookToolBeforeResult)>;
using FAgentHookToolApprovalContinuation = TFunction<void(FAgentHookToolApprovalResult)>;
using FAgentHookToolApprovalHandler = TFunction<void(const FAgentHookToolContext&, FAgentHookToolApprovalContinuation)>;
using FAgentHookToolApprovalCompletion = TFunction<void(FAgentHookToolApprovalResult)>;
// Runs after approval and before dispatch, so editor-side setup only happens for
// approved calls. Return true when the setup needs a visible engine frame.
using FAgentHookToolReadyHandler = TFunction<bool(const FAgentHookToolContext&)>;
using FAgentHookToolAfterHandler = TFunction<void(const FAgentHookToolContext&, const FAgentHookToolOutcome&)>;
using FAgentHookRunExitedHandler = TFunction<void(const FAgentHookRunExitContext&)>;

/**
 * Process-wide Agent Workbench hook registry. Registration, unregistration, and
 * emission must happen on the Game Thread. Lower priorities run first; equal
 * priorities retain their registration order. Names must be unique within a hook
 * point. ToolApproval has one owner; a missing approval handler rejects the request.
 */
class FAgentHookRegistry
{
public:
    static FAgentHookRegistry& Get();

    FAgentHookHandle RegisterPromptSubmitting(FName Name, int32 Priority, FAgentHookPromptSubmittingHandler Handler);
    FAgentHookHandle RegisterPromptSubmitted(FName Name, int32 Priority, FAgentHookPromptSubmittedHandler Handler);
    FAgentHookHandle RegisterToolBefore(FName Name, int32 Priority, FAgentHookToolBeforeHandler Handler);
    FAgentHookHandle RegisterToolApproval(FName Name, int32 Priority, FAgentHookToolApprovalHandler Handler);
    FAgentHookHandle RegisterToolReady(FName Name, int32 Priority, FAgentHookToolReadyHandler Handler);
    FAgentHookHandle RegisterToolAfter(FName Name, int32 Priority, FAgentHookToolAfterHandler Handler);
    FAgentHookHandle RegisterRunExited(FName Name, int32 Priority, FAgentHookRunExitedHandler Handler);
    bool Unregister(FAgentHookHandle Handle);
    bool IsRegistered(FAgentHookHandle Handle) const;

    bool EmitPromptSubmitting(FAgentHookPromptContext& Context, FString& OutReason);
    void EmitPromptSubmitted(const FAgentHookPromptContext& Context);
    void RunToolBefore(const FAgentHookToolContext& Context, FAgentHookToolBeforeCompletion Complete);
    void RequestToolApproval(const FAgentHookToolContext& Context, FAgentHookToolApprovalCompletion Complete);
    /** Drops pending pre-execution and approval completions for this invocation. */
    void CancelToolInvocation(FGuid InvocationId);
    bool EmitToolReady(const FAgentHookToolContext& Context);
    void EmitToolAfter(const FAgentHookToolContext& Context, const FAgentHookToolOutcome& Outcome);
    void EmitRunExited(const FAgentHookRunExitContext& Context);

    /** For shutdown only; invalidates pending async continuations. */
    void Shutdown();

private:
    FAgentHookRegistry();
    ~FAgentHookRegistry();
    FAgentHookRegistry(const FAgentHookRegistry&) = delete;
    FAgentHookRegistry& operator=(const FAgentHookRegistry&) = delete;

    struct FImpl;
    TUniquePtr<FImpl> Impl;
};
