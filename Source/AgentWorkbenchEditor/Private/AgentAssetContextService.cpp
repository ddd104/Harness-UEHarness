#include "AgentAssetContextService.h"
#include "AgentWorkbenchSettings.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "ContentBrowserModule.h"
#include "IContentBrowserSingleton.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

namespace
{
TSharedRef<FJsonObject> MakeToolError(const FString& Code, const FString& Message)
{
    TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("ok"), false);
    Result->SetStringField(TEXT("error_code"), Code);
    Result->SetStringField(TEXT("message"), Message);
    return Result;
}
TSharedRef<FJsonObject> CandidateJson(const FAgentCandidate& Candidate)
{
    TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("asset_id"), Candidate.CandidateId.ToString(EGuidFormats::Digits));
    Result->SetStringField(TEXT("name"), Candidate.DisplayName);
    Result->SetStringField(TEXT("object_path"), Candidate.ObjectPath);
    Result->SetStringField(TEXT("package_name"), Candidate.PackageName);
    Result->SetStringField(TEXT("asset_class_path"), Candidate.AssetClassPath);
    return Result;
}

TArray<TSharedPtr<FJsonValue>> AssetArray(const TArray<FAgentCandidate>& Candidates)
{
    TArray<TSharedPtr<FJsonValue>> Values;
    for (const FAgentCandidate& Candidate : Candidates)
    {
        Values.Add(MakeShared<FJsonValueObject>(CandidateJson(Candidate)));
    }
    return Values;
}
}

bool FAgentAssetContextService::NormalizeObjectPath(const FString& Input, FString& OutPath)
{
    OutPath.Empty();
    const FString Trimmed = Input.TrimStartAndEnd();
    int32 LastSlash = INDEX_NONE;
    int32 LastDot = INDEX_NONE;
    Trimmed.FindLastChar(TEXT('/'), LastSlash);
    Trimmed.FindLastChar(TEXT('.'), LastDot);
    // UE accepts some package-only strings as object paths; candidates require Package.Asset.
    if (LastDot <= LastSlash + 1 || LastDot >= Trimmed.Len() - 1) { return false; }
    if (!Trimmed.StartsWith(TEXT("/Game/")) || !FPackageName::IsValidObjectPath(Trimmed)) { return false; }
    const FSoftObjectPath Path(Trimmed);
    if (!Path.IsValid() || !Path.IsAsset() || !Path.GetLongPackageName().StartsWith(TEXT("/Game/"))) { return false; }
    OutPath = Path.ToString();
    return true;
}

FAgentAddCandidatesResult FAgentAssetContextService::AddSelectedAssets(FAgentSession& Session, int32 MaxCandidates)
{
    check(IsInGameThread());
    TArray<FAssetData> Selected;
    FModuleManager::LoadModuleChecked<FContentBrowserModule>(TEXT("ContentBrowser")).Get().GetSelectedAssets(Selected);
    return AddAssets(Session, Selected, MaxCandidates);
}

FAgentAddCandidatesResult FAgentAssetContextService::AddAssets(FAgentSession& Session,
    const TArray<FAssetData>& Assets, int32 MaxCandidates)
{
    check(IsInGameThread());
    FAgentAddCandidatesResult Result;
    if (Assets.IsEmpty())
    {
        Result.Message = TEXT("新增 0，重复 0，跳过 0。请先在主内容浏览器选择 /Game/ 资产。文件夹和 World Outliner 选择不计入。");
        return Result;
    }
    const int32 Limit = FMath::Max(1, MaxCandidates);
    for (const FAssetData& Asset : Assets)
    {
        FString Path;
        if (!Asset.IsValid() || !NormalizeObjectPath(Asset.GetSoftObjectPath().ToString(), Path))
        {
            ++Result.Skipped;
            ++Result.InvalidPath;
            continue;
        }
        if (Asset.AssetClassPath.IsNull())
        {
            ++Result.Skipped;
            ++Result.UnknownType;
            continue;
        }
        if (Asset.IsRedirector())
        {
            ++Result.Skipped;
            ++Result.UnsupportedType;
            continue;
        }
        if (Session.Candidates.ContainsByPredicate([&Path](const TSharedPtr<FAgentCandidate>& Existing)
            { return Existing && Existing->ObjectPath.Equals(Path, ESearchCase::IgnoreCase); }))
        {
            ++Result.Duplicates;
            continue;
        }
        if (Session.Candidates.Num() >= Limit)
        {
            ++Result.OverLimit;
            continue;
        }
        TSharedPtr<FAgentCandidate> Candidate = MakeShared<FAgentCandidate>();
        Candidate->DisplayName = Asset.AssetName.ToString();
        Candidate->ObjectPath = Path;
        Candidate->PackageName = Asset.PackageName.ToString();
        Candidate->AssetClassPath = Asset.AssetClassPath.ToString();
        Candidate->ValidationStatus = EAgentCandidateValidation::Valid;
        Session.Candidates.Add(Candidate);
        ++Result.Added;
    }
    if (Result.Added > 0) { Session.Touch(); }
    Result.Message = FString::Printf(TEXT("新增 %d，重复 %d，跳过 %d，超出上限 %d（最多 %d 项）。"),
        Result.Added, Result.Duplicates, Result.Skipped, Result.OverLimit, Limit);
    if (Result.Skipped > 0)
    {
        Result.Message += FString::Printf(TEXT("跳过原因：无效或越界路径 %d、重定向器 %d、无法确认类型 %d。不会加载资产以猜测类型。"),
            Result.InvalidPath, Result.UnsupportedType, Result.UnknownType);
    }
    return Result;
}

FAgentAssetLookupResult FAgentAssetContextService::LookupAsset(const FSoftObjectPath& Path)
{
    check(IsInGameThread());
    FAgentAssetLookupResult Result;
    IAssetRegistry& Registry = IAssetRegistry::GetChecked();
    if (Registry.IsLoadingAssets()) { return Result; }
    Result.Data = Registry.GetAssetByObjectPath(Path);
    if (!Result.Data.IsValid()) { Result.Status = EAgentCandidateValidation::Missing; }
    else if (Result.Data.IsRedirector() || Result.Data.AssetClassPath.IsNull()) { Result.Status = EAgentCandidateValidation::Unsupported; }
    else { Result.Status = EAgentCandidateValidation::Valid; }
    return Result;
}

bool FAgentAssetContextService::BuildSnapshot(FAgentSession& Session, const FGuid& RunId,
    TFunctionRef<FAgentAssetLookupResult(const FSoftObjectPath&)> Lookup,
    FAgentRunInputSnapshot& OutSnapshot, FString& OutError)
{
    check(IsInGameThread());
    OutError.Empty();
    FAgentRunInputSnapshot Snapshot;
    Snapshot.SessionId = Session.SessionId;
    Snapshot.RunId = RunId;
    Snapshot.UserInput = Session.DraftText.TrimStartAndEnd();
    Snapshot.ModelOptions = Session.ModelOptions;
    if (Snapshot.UserInput.IsEmpty()) { OutError = TEXT("请输入消息。"); return false; }
    const int32 MaxAssets = FMath::Max(1, GetDefault<UAgentWorkbenchSettings>()->MaxCandidateAssets);
    for (const TSharedPtr<FAgentCandidate>& Candidate : Session.Candidates)
    {
        if (!Candidate) { continue; }
        FString Normalized;
        if (!NormalizeObjectPath(Candidate->ObjectPath, Normalized)
            || !Normalized.Equals(Candidate->ObjectPath, ESearchCase::CaseSensitive))
        {
            OutError = TEXT("候选资产路径无效或不在 /Game/ 下；请移除并重新选择。");
            return false;
        }
        if (Snapshot.IncludedAssets.Num() >= MaxAssets)
        {
            OutError = TEXT("本轮启用的候选资产超过项目设置上限。");
            return false;
        }
        const FAgentAssetLookupResult Found = Lookup(FSoftObjectPath(Normalized));
        Candidate->ValidationStatus = Found.Status;
        if (Found.Status != EAgentCandidateValidation::Valid
            || !Found.Data.IsValid()
            || Found.Data.GetSoftObjectPath().ToString() != Normalized
            || Found.Data.IsRedirector() || Found.Data.AssetClassPath.IsNull())
        {
            OutError = Found.Status == EAgentCandidateValidation::Unknown
                ? TEXT("资产注册表仍在扫描，无法确认候选资产；请稍后重试。")
                : FString::Printf(TEXT("候选资产已失效或不再可用：%s。请移除并重新选择。"), *Normalized);
            return false;
        }
        FAgentCandidate FrozenCandidate = *Candidate;
        FrozenCandidate.bIncluded = true;
        FrozenCandidate.DisplayName = Found.Data.AssetName.ToString();
        FrozenCandidate.PackageName = Found.Data.PackageName.ToString();
        FrozenCandidate.AssetClassPath = Found.Data.AssetClassPath.ToString();
        Snapshot.IncludedAssets.Add(MoveTemp(FrozenCandidate));
    }
    OutSnapshot = MoveTemp(Snapshot);
    return true;
}

TSharedRef<FJsonObject> FAgentReadOnlyTools::Execute(const FAgentRunInputSnapshot& Snapshot,
    const FString& ToolName, const TSharedPtr<FJsonObject>& Arguments,
    TFunctionRef<FAgentAssetLookupResult(const FSoftObjectPath&)> Lookup)
{
    check(IsInGameThread());
    if (ToolName == TEXT("list_context_assets"))
    {
        if (Arguments && Arguments->Values.Num() > 0) { return MakeToolError(TEXT("InvalidArguments"), TEXT("此工具不接受参数。")); }
        TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetBoolField(TEXT("ok"), true);
        Result->SetStringField(TEXT("session_id"), Snapshot.SessionId.ToString(EGuidFormats::Digits));
        Result->SetStringField(TEXT("run_id"), Snapshot.RunId.ToString(EGuidFormats::Digits));
        Result->SetArrayField(TEXT("assets"), AssetArray(Snapshot.IncludedAssets));
        return Result;
    }
    if (ToolName != TEXT("get_asset_metadata")) { return MakeToolError(TEXT("UnknownTool"), TEXT("工具未注册。")); }
    FString Requested;
    if (!Arguments || Arguments->Values.Num() != 1 || !Arguments->TryGetStringField(TEXT("object_path"), Requested))
    {
        return MakeToolError(TEXT("InvalidArguments"), TEXT("需要唯一的 object_path 字符串参数。"));
    }
    FString Normalized;
    if (!FAgentAssetContextService::NormalizeObjectPath(Requested, Normalized) || Requested != Normalized)
    {
        return MakeToolError(TEXT("InvalidPath"), TEXT("对象路径格式无效或不在允许根路径下。"));
    }
    const FAgentCandidate* Authorized = Snapshot.IncludedAssets.FindByPredicate([&Normalized](const FAgentCandidate& Candidate)
        { return Candidate.ObjectPath == Normalized; });
    if (!Authorized) { return MakeToolError(TEXT("UnauthorizedAsset"), TEXT("对象不在本 Run 启用的候选快照中。")); }
    const FAgentAssetLookupResult Found = Lookup(FSoftObjectPath(Normalized));
    if (Found.Status == EAgentCandidateValidation::Unknown) { return MakeToolError(TEXT("RegistryScanning"), TEXT("资产注册表仍在扫描。")); }
    if (Found.Status != EAgentCandidateValidation::Valid || !Found.Data.IsValid()
        || Found.Data.GetSoftObjectPath().ToString() != Normalized
        || Found.Data.IsRedirector() || Found.Data.AssetClassPath.IsNull())
    {
        return MakeToolError(TEXT("AssetMissing"), TEXT("授权资产已失效或发生变化。"));
    }
    TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("ok"), true);
    Result->SetStringField(TEXT("read_at_utc"), FDateTime::UtcNow().ToIso8601());
    Result->SetStringField(TEXT("object_path"), Normalized);
    Result->SetStringField(TEXT("package_name"), Found.Data.PackageName.ToString());
    Result->SetStringField(TEXT("asset_name"), Found.Data.AssetName.ToString());
    Result->SetStringField(TEXT("asset_class_path"), Found.Data.AssetClassPath.ToString());
    Result->SetNumberField(TEXT("tag_count"), Found.Data.TagsAndValues.Num());
    const UPackage* LoadedPackage = FindPackage(nullptr, *Found.Data.PackageName.ToString());
    Result->SetStringField(TEXT("modification_state"), LoadedPackage
        ? (LoadedPackage->IsDirty() ? TEXT("loaded_unsaved_changes") : TEXT("loaded_clean"))
        : TEXT("package_not_loaded_unknown"));
    Result->SetStringField(TEXT("source"), TEXT("AssetRegistry metadata; asset contents not loaded"));
    return Result;
}
