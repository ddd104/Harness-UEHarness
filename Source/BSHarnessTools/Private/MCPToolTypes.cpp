#include "MCPToolTypes.h"

TSharedRef<FJsonObject> FMCPToolDefinition::ToJson() const
{
	TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("name"), Name);
	Json->SetStringField(TEXT("description"), Description);
	Json->SetObjectField(TEXT("inputSchema"), InputSchema);
	return Json;
}

FMCPToolResult FMCPToolResult::Success(const FString& Text, TSharedPtr<FJsonObject> Data)
{
	FMCPToolResult Result;
	TSharedRef<FJsonObject> Block = MakeShared<FJsonObject>();
	Block->SetStringField(TEXT("type"), TEXT("text"));
	Block->SetStringField(TEXT("text"), Text);
	Result.Content.Add(MakeShared<FJsonValueObject>(Block));
	Result.StructuredContent = MoveTemp(Data);
	return Result;
}

FMCPToolResult FMCPToolResult::Failure(const FString& Message)
{
	FMCPToolResult Result = Success(Message);
	Result.bIsError = true;
	return Result;
}

FMCPToolResult FMCPToolResult::ProtocolError(int32 Code, const FString& Message)
{
	FMCPToolResult Result = Failure(Message);
	Result.ProtocolErrorCode = Code;
	Result.ProtocolErrorMessage = Message;
	return Result;
}

TSharedRef<FJsonObject> FMCPToolResult::ToJson() const
{
	TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetArrayField(TEXT("content"), Content);
	Json->SetBoolField(TEXT("isError"), bIsError);
	if (StructuredContent.IsValid())
	{
		Json->SetObjectField(TEXT("structuredContent"), StructuredContent);
	}
	return Json;
}
