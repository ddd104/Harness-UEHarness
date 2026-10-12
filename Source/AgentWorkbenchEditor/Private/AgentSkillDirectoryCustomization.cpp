#include "AgentSkillDirectoryCustomization.h"

#include "AgentWorkbenchSettings.h"
#include "DesktopPlatformModule.h"
#include "DetailWidgetRow.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/FileManager.h"
#include "IDesktopPlatform.h"
#include "Misc/Paths.h"
#include "PropertyHandle.h"
#include "Styling/AppStyle.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SWindow.h"

#define LOCTEXT_NAMESPACE "AgentSkillDirectoryCustomization"

namespace
{
// Store a picked directory relative to the project root when it lives inside the
// project; otherwise keep the absolute path.
FString ToProjectRelativePath(const FString& Directory)
{
    FString Full = FPaths::ConvertRelativePathToFull(Directory);
    FString ProjectDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
    Full.ReplaceInline(TEXT("\\"), TEXT("/"));
    ProjectDir.ReplaceInline(TEXT("\\"), TEXT("/"));
    if (!ProjectDir.EndsWith(TEXT("/")))
    {
        ProjectDir.AppendChar(TEXT('/'));
    }
    if (Full.StartsWith(ProjectDir, ESearchCase::IgnoreCase))
    {
        FString Relative = Full.RightChop(ProjectDir.Len());
        Relative.RemoveFromEnd(TEXT("/"));
        return Relative.IsEmpty() ? FString(TEXT(".")) : Relative;
    }
    return FPaths::ConvertRelativePathToFull(Directory);
}

// Turn a stored value back into an absolute directory for the dialog's start path.
FString ResolveFromProject(const FString& Stored)
{
    const FString Trimmed = Stored.TrimStartAndEnd();
    if (Trimmed.IsEmpty() || !FPaths::IsRelative(Trimmed))
    {
        return Trimmed;
    }
    return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), Trimmed));
}
}

TSharedRef<IPropertyTypeCustomization> FAgentSkillDirectoryCustomization::MakeInstance()
{
    return MakeShared<FAgentSkillDirectoryCustomization>();
}

void FAgentSkillDirectoryCustomization::CustomizeHeader(TSharedRef<IPropertyHandle> StructPropertyHandle,
    FDetailWidgetRow& HeaderRow, IPropertyTypeCustomizationUtils& StructCustomizationUtils)
{
    PathProperty = StructPropertyHandle->GetChildHandle(
        GET_MEMBER_NAME_CHECKED(FAgentSkillDirectory, Path));
    if (!PathProperty.IsValid()) { return; }

    HeaderRow
        .NameContent()
        [
            StructPropertyHandle->CreatePropertyNameWidget()
        ]
        .ValueContent()
        .MinDesiredWidth(420.0f)
        .MaxDesiredWidth(2000.0f)
        [
            SNew(SHorizontalBox)
            + SHorizontalBox::Slot()
            .FillWidth(1.0f)
            .VAlign(VAlign_Center)
            [
                PathProperty->CreatePropertyValueWidget()
            ]
            + SHorizontalBox::Slot()
            .AutoWidth()
            .Padding(4.0f, 0.0f, 0.0f, 0.0f)
            .VAlign(VAlign_Center)
            [
                SAssignNew(BrowseButton, SButton)
                .ButtonStyle(FAppStyle::Get(), "HoverHintOnly")
                .ToolTipText(LOCTEXT("BrowseTip",
                    "选择一个技能目录；项目内的目录会保存为相对项目根目录的路径"))
                .OnClicked(this, &FAgentSkillDirectoryCustomization::OnBrowse)
                .ContentPadding(2.0f)
                .ForegroundColor(FSlateColor::UseForeground())
                .IsFocusable(false)
                [
                    SNew(SImage)
                    .Image(FAppStyle::GetBrush("PropertyWindow.Button_Ellipsis"))
                    .ColorAndOpacity(FSlateColor::UseForeground())
                ]
            ]
        ];
}

FReply FAgentSkillDirectoryCustomization::OnBrowse()
{
    check(IsInGameThread());
    if (!PathProperty.IsValid()) { return FReply::Handled(); }
    IDesktopPlatform* DesktopPlatform = FDesktopPlatformModule::Get();
    if (!DesktopPlatform) { return FReply::Handled(); }

    FString Current;
    PathProperty->GetValue(Current);
    FString StartDirectory = ResolveFromProject(Current);
    if (StartDirectory.IsEmpty() || !IFileManager::Get().DirectoryExists(*StartDirectory))
    {
        StartDirectory = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
    }

    void* ParentWindowHandle = nullptr;
    if (BrowseButton.IsValid() && FSlateApplication::IsInitialized())
    {
        const TSharedPtr<SWindow> ParentWindow =
            FSlateApplication::Get().FindWidgetWindow(BrowseButton.ToSharedRef());
        if (ParentWindow.IsValid() && ParentWindow->GetNativeWindow().IsValid())
        {
            ParentWindowHandle = ParentWindow->GetNativeWindow()->GetOSWindowHandle();
        }
    }

    FString Chosen;
    if (DesktopPlatform->OpenDirectoryDialog(ParentWindowHandle,
        LOCTEXT("ChooseSkillDirectory", "Choose a skill directory").ToString(),
        StartDirectory, Chosen))
    {
        PathProperty->SetValue(ToProjectRelativePath(Chosen));
    }
    return FReply::Handled();
}

#undef LOCTEXT_NAMESPACE
