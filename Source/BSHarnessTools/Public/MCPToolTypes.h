#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

/** MCP tools/list metadata. InputSchema must be an object JSON Schema. */
struct BSHARNESSTOOLS_API FMCPToolDefinition
{
	FString Name;
	FString Description;
	TSharedPtr<FJsonObject> InputSchema;
	// Internal native handler identity. Kept out of tools/list JSON so callers cannot
	// mistake a configured display name for the handler that actually executes.
	FString NativeHandlerId;

	TSharedRef<FJsonObject> ToJson() const;
};

/** A completed call, never just an acknowledgement that work was queued. */
struct BSHARNESSTOOLS_API FMCPToolResult
{
	bool bIsError = false;
	TArray<TSharedPtr<FJsonValue>> Content;
	TSharedPtr<FJsonObject> StructuredContent;

	// Dispatch/argument errors become JSON-RPC errors; execution failures use isError.
	TOptional<int32> ProtocolErrorCode;
	FString ProtocolErrorMessage;

	static FMCPToolResult Success(const FString& Text, TSharedPtr<FJsonObject> Data = nullptr);
	static FMCPToolResult Failure(const FString& Message);
	static FMCPToolResult ProtocolError(int32 Code, const FString& Message);
	TSharedRef<FJsonObject> ToJson() const;
};

using FMCPToolArguments = TSharedRef<FJsonObject>;
using FMCPToolCompletion = TFunction<void(FMCPToolResult)>;
using FMCPSyncToolHandler = TFunction<FMCPToolResult(const FMCPToolArguments&)>;
// Called on the game thread. May retain Completion and complete on any thread.
using FMCPAsyncToolHandler = TFunction<void(const FMCPToolArguments&, FMCPToolCompletion)>;
