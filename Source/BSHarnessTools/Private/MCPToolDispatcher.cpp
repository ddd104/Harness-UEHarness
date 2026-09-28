#include "MCPToolDispatcher.h"

namespace
{
	TSharedPtr<FJsonObject> MakeResponse(const TSharedPtr<FJsonValue>& Id, const FMCPToolResult& Result)
	{
		TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
		Response->SetStringField(TEXT("jsonrpc"), TEXT("2.0"));
		Response->SetField(TEXT("id"), Id.IsValid() ? Id : MakeShared<FJsonValueNull>());
		if (Result.ProtocolErrorCode.IsSet())
		{
			TSharedRef<FJsonObject> Error = MakeShared<FJsonObject>();
			Error->SetNumberField(TEXT("code"), Result.ProtocolErrorCode.GetValue());
			Error->SetStringField(TEXT("message"), Result.ProtocolErrorMessage);
			Response->SetObjectField(TEXT("error"), Error);
		}
		else
		{
			Response->SetObjectField(TEXT("result"), Result.ToJson());
		}
		return Response;
	}

	TFuture<TSharedPtr<FJsonObject>> ReadyResponse(TSharedPtr<FJsonObject> Response)
	{
		TPromise<TSharedPtr<FJsonObject>> Promise;
		auto Future = Promise.GetFuture();
		Promise.SetValue(MoveTemp(Response));
		return Future;
	}
}

TFuture<TSharedPtr<FJsonObject>> FMCPToolDispatcher::Dispatch(
	FMCPToolRegistry& Registry, const TSharedRef<FJsonObject>& Request, double TimeoutSeconds)
{
	const TSharedPtr<FJsonValue> Id = Request->TryGetField(TEXT("id"));
	FString Version;
	FString Method;
	if (!Request->HasTypedField<EJson::String>(TEXT("jsonrpc"))
		|| !Request->TryGetStringField(TEXT("jsonrpc"), Version) || Version != TEXT("2.0")
		|| !Request->HasTypedField<EJson::String>(TEXT("method")) || !Request->TryGetStringField(TEXT("method"), Method)
		|| (Id.IsValid() && Id->Type != EJson::String && Id->Type != EJson::Number))
	{
		return ReadyResponse(MakeResponse(nullptr, FMCPToolResult::ProtocolError(-32600, TEXT("Invalid MCP JSON-RPC request."))));
	}
	if (!Id.IsValid())
	{
		// tools/call requires a request ID. Never execute a tool sent as a notification.
		return ReadyResponse(nullptr);
	}
	if (!IsInGameThread())
	{
		return ReadyResponse(MakeResponse(Id, FMCPToolResult::ProtocolError(-32603, TEXT("Dispatch MCP requests on the game thread."))));
	}
	const TSharedPtr<FJsonObject>* ParamsPtr = nullptr;
	if (Request->HasField(TEXT("params")) && !Request->TryGetObjectField(TEXT("params"), ParamsPtr))
	{
		return ReadyResponse(MakeResponse(Id, FMCPToolResult::ProtocolError(-32602, TEXT("params must be an object."))));
	}
	TSharedPtr<FJsonObject> Params = ParamsPtr ? *ParamsPtr : MakeShared<FJsonObject>();
	if (Method.Equals(TEXT("tools/list"), ESearchCase::CaseSensitive))
	{
		if (Params->HasField(TEXT("cursor")))
		{
			return ReadyResponse(MakeResponse(Id, FMCPToolResult::ProtocolError(-32602, TEXT("This registry returns all tools in one page; omit cursor."))));
		}
		TArray<TSharedPtr<FJsonValue>> Tools;
		for (const FMCPToolDefinition& Definition : Registry.ListTools())
		{
			Tools.Add(MakeShared<FJsonValueObject>(Definition.ToJson()));
		}
		TSharedRef<FJsonObject> List = MakeShared<FJsonObject>();
		List->SetArrayField(TEXT("tools"), Tools);
		auto Response = MakeResponse(Id, FMCPToolResult::Success(TEXT("")));
		Response->SetObjectField(TEXT("result"), List);
		return ReadyResponse(Response);
	}
	if (Method.Equals(TEXT("tools/call"), ESearchCase::CaseSensitive))
	{
		FString Name;
		const TSharedPtr<FJsonObject>* ArgumentsPtr = nullptr;
		if (!Params->HasTypedField<EJson::String>(TEXT("name")) || !Params->TryGetStringField(TEXT("name"), Name) || Name.IsEmpty()
			|| (Params->HasField(TEXT("arguments")) && !Params->TryGetObjectField(TEXT("arguments"), ArgumentsPtr)))
		{
			return ReadyResponse(MakeResponse(Id, FMCPToolResult::ProtocolError(-32602, TEXT("tools/call requires name and optional object arguments."))));
		}
		FMCPToolArguments Arguments = ArgumentsPtr ? ArgumentsPtr->ToSharedRef() : MakeShared<FJsonObject>();
		// JSON DOM shared pointers use non-thread-safe reference counts. Carry the ID
		// by value because the continuation can run on a worker completing the tool.
		const bool bStringId = Id->Type == EJson::String;
		const FString StringId = bStringId ? Id->AsString() : FString();
		const double NumberId = bStringId ? 0.0 : Id->AsNumber();
		return Registry.CallTool(Name, Arguments, TimeoutSeconds).Next([bStringId, StringId, NumberId](FMCPToolResult Result)
		{
			TSharedPtr<FJsonValue> ResponseId;
			if (bStringId)
			{
				ResponseId = MakeShared<FJsonValueString>(StringId);
			}
			else
			{
				ResponseId = MakeShared<FJsonValueNumber>(NumberId);
			}
			return MakeResponse(ResponseId, Result);
		});
	}
	return ReadyResponse(MakeResponse(Id, FMCPToolResult::ProtocolError(-32601, TEXT("Method not found in the tools dispatcher."))));
}
