#pragma once

#include "CoreMinimal.h"
#include "MCPToolTypes.h"

/** Extension points are editor-only and are invoked on the Game Thread. */
enum class EBSHookPoint : uint8
{
    PromptSubmitting,
    PromptSubmitted,
    ToolBefore,
    ToolApproval,
    ToolReady,
    ToolAfter,
    RunExited
};

struct BSHARNESSHOOKS_API FBSHookHandle
{
    EBSHookPoint Point = EBSHookPoint::PromptSubmitting;
    uint64 Id = 0;

    bool IsValid() const { return Id != 0; }
};

/** Only AdditionalModelContext may be changed by a PromptSubmitting handler. */
struct BSHARNESSHOOKS_API FBSHookPromptContext
{
    FGuid SessionId;
    FGuid RunId;
    FString UserPrompt;
    FString ModelId;
    TArray<FString> CandidatePaths;
    FString AdditionalModelContext;
};

enum class EBSHookApprovalMode : uint8 { Ask, Smart, Unrestricted };

/** Tool identity and arguments are resolved and frozen before any tool hook runs. */
struct BSHARNESSHOOKS_API FBSHookToolContext
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
    EBSHookApprovalMode ApprovalMode = EBSHookApprovalMode::Ask;
};

enum class EBSHookToolDecision : uint8
{
    Continue,
    Deny,
    Handled
};

struct BSHARNESSHOOKS_API FBSHookToolBeforeResult
{
    EBSHookToolDecision Decision = EBSHookToolDecision::Continue;
    FString Reason;
    FMCPToolResult Result;
    // A handler that opened an editor can request an engine frame before dispatch.
    bool bNeedsVisibleEditorFrame = false;
};

struct BSHARNESSHOOKS_API FBSHookToolApprovalResult
{
    bool bApproved = false;
    FString Reason;
};

enum class EBSHookToolOutcomeKind : uint8
{
    Dispatched,
    Denied,
    Handled,
    Invalid,
    Cancelled,
    TimedOut
};

struct BSHARNESSHOOKS_API FBSHookToolOutcome
{
    EBSHookToolOutcomeKind Kind = EBSHookToolOutcomeKind::Invalid;
    bool bIsError = false;
    // Serialized copy of the final MCP result. Hooks cannot mutate the result
    // subsequently sent to the model through shared JSON pointers.
    FString ResultJson;
    bool bApproved = false;
    bool bWasDispatched = false;
};

enum class EBSHookRunExitReason : uint8
{
    Completed,
    Failed,
    Cancelled,
    TimedOut,
    Closed
};

struct BSHARNESSHOOKS_API FBSHookRunExitContext
{
    FGuid SessionId;
    FGuid RunId;
    EBSHookRunExitReason Reason = EBSHookRunExitReason::Completed;
    FString Detail;
};

using FBSHookPromptSubmittingHandler = TFunction<bool(FBSHookPromptContext&, FString& /* OutReason */)>;
using FBSHookPromptSubmittedHandler = TFunction<void(const FBSHookPromptContext&)>;
// Continuations may be called from any thread; the registry delivers only the first
// result on the Game Thread. A handler may keep its continuation for later work.
using FBSHookToolBeforeContinuation = TFunction<void(FBSHookToolBeforeResult)>;
using FBSHookToolBeforeHandler = TFunction<void(const FBSHookToolContext&, FBSHookToolBeforeContinuation)>;
using FBSHookToolBeforeCompletion = TFunction<void(FBSHookToolBeforeResult)>;
using FBSHookToolApprovalContinuation = TFunction<void(FBSHookToolApprovalResult)>;
using FBSHookToolApprovalHandler = TFunction<void(const FBSHookToolContext&, FBSHookToolApprovalContinuation)>;
using FBSHookToolApprovalCompletion = TFunction<void(FBSHookToolApprovalResult)>;
// Runs after approval and before dispatch, so editor-side setup only happens for
// approved calls. Return true when the setup needs a visible engine frame.
using FBSHookToolReadyHandler = TFunction<bool(const FBSHookToolContext&)>;
using FBSHookToolAfterHandler = TFunction<void(const FBSHookToolContext&, const FBSHookToolOutcome&)>;
using FBSHookRunExitedHandler = TFunction<void(const FBSHookRunExitContext&)>;

/**
 * Process-wide hook registry. Registration, unregistration, and emission must happen
 * on the Game Thread. Lower priorities run first; equal priorities retain their
 * registration order. Names must be unique within a hook point. ToolApproval has
 * one owner; a missing approval handler rejects the request.
 */
class BSHARNESSHOOKS_API FBSHarnessHookRegistry
{
public:
    static FBSHarnessHookRegistry& Get();

    FBSHookHandle RegisterPromptSubmitting(FName Name, int32 Priority, FBSHookPromptSubmittingHandler Handler);
    FBSHookHandle RegisterPromptSubmitted(FName Name, int32 Priority, FBSHookPromptSubmittedHandler Handler);
    FBSHookHandle RegisterToolBefore(FName Name, int32 Priority, FBSHookToolBeforeHandler Handler);
    FBSHookHandle RegisterToolApproval(FName Name, int32 Priority, FBSHookToolApprovalHandler Handler);
    FBSHookHandle RegisterToolReady(FName Name, int32 Priority, FBSHookToolReadyHandler Handler);
    FBSHookHandle RegisterToolAfter(FName Name, int32 Priority, FBSHookToolAfterHandler Handler);
    FBSHookHandle RegisterRunExited(FName Name, int32 Priority, FBSHookRunExitedHandler Handler);
    bool Unregister(FBSHookHandle Handle);
    bool IsRegistered(FBSHookHandle Handle) const;

    bool EmitPromptSubmitting(FBSHookPromptContext& Context, FString& OutReason);
    void EmitPromptSubmitted(const FBSHookPromptContext& Context);
    void RunToolBefore(const FBSHookToolContext& Context, FBSHookToolBeforeCompletion Complete);
    void RequestToolApproval(const FBSHookToolContext& Context, FBSHookToolApprovalCompletion Complete);
    /** Drops pending pre-execution and approval completions for this invocation. */
    void CancelToolInvocation(FGuid InvocationId);
    bool EmitToolReady(const FBSHookToolContext& Context);
    void EmitToolAfter(const FBSHookToolContext& Context, const FBSHookToolOutcome& Outcome);
    void EmitRunExited(const FBSHookRunExitContext& Context);

    /** For shutdown only; invalidates pending async continuations. */
    void Shutdown();

private:
    FBSHarnessHookRegistry();
    ~FBSHarnessHookRegistry();
    FBSHarnessHookRegistry(const FBSHarnessHookRegistry&) = delete;
    FBSHarnessHookRegistry& operator=(const FBSHarnessHookRegistry&) = delete;

    struct FImpl;
    TUniquePtr<FImpl> Impl;
};
