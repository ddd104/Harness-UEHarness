#include "AgentHookRegistry.h"
#include "AgentSkillDirectoryCustomization.h"
#include "AgentSkillLoader.h"
#include "AgentMemoryStore.h"
#include "AgentWorkbenchSession.h"
#include "AgentWorkbenchSettings.h"
#include "AgentToolWorkflow.h"
#include "Framework/Commands/UIAction.h"
#include "Modules/ModuleManager.h"
#include "PropertyEditorModule.h"
#include "Styling/AppStyle.h"
#include "ToolMenu.h"
#include "ToolMenus.h"

#define LOCTEXT_NAMESPACE "AgentWorkbenchEditorModule"

class FAgentWorkbenchEditorModule : public IModuleInterface
{
public:
    virtual void StartupModule() override
    {
        RegisterAgentWorkbenchToolHooks();
        SessionManager = MakeShared<FAgentSessionManager>();
        FAgentSkillLoader::Get().Initialize();
        // Skill directories keep their editor folder picker (project-relative storage).
        FPropertyEditorModule& PropertyEditor =
            FModuleManager::LoadModuleChecked<FPropertyEditorModule>(TEXT("PropertyEditor"));
        PropertyEditor.RegisterCustomPropertyTypeLayout(
            FAgentSkillDirectory::StaticStruct()->GetFName(),
            FOnGetPropertyTypeCustomizationInstance::CreateStatic(
                &FAgentSkillDirectoryCustomization::MakeInstance));
        MenuStartupHandle = UToolMenus::RegisterStartupCallback(
            FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FAgentWorkbenchEditorModule::RegisterMenus));
    }

    virtual void ShutdownModule() override
    {
        UToolMenus::UnRegisterStartupCallback(MenuStartupHandle);
        UToolMenus::UnregisterOwner(this);
        if (SessionManager) { SessionManager->Shutdown(); SessionManager.Reset(); }
        UnregisterAgentWorkbenchToolHooks();
        if (FModuleManager::Get().IsModuleLoaded(TEXT("PropertyEditor")))
        {
            FPropertyEditorModule& PropertyEditor =
                FModuleManager::GetModuleChecked<FPropertyEditorModule>(TEXT("PropertyEditor"));
            PropertyEditor.UnregisterCustomPropertyTypeLayout(
                FAgentSkillDirectory::StaticStruct()->GetFName());
        }
        // Invalidate pending asynchronous hook continuations before the module unloads.
        if (IsInGameThread()) { FAgentHookRegistry::Get().Shutdown(); }
        FAgentSkillLoader::Get().Shutdown();
        FAgentMemoryStore::Get().Shutdown();
    }

private:
    void RegisterMenus()
    {
        FToolMenuOwnerScoped Owner(this);
        UToolMenu* Menu = UToolMenus::Get()->ExtendMenu(TEXT("LevelEditor.LevelEditorToolBar.User"));
        FToolMenuSection& Section = Menu->FindOrAddSection(TEXT("AgentWorkbench"));
        FToolMenuEntry Entry = FToolMenuEntry::InitToolBarButton(
            TEXT("AgentWorkbenchNewConversation"),
            FUIAction(FExecuteAction::CreateRaw(this, &FAgentWorkbenchEditorModule::CreateConversation)),
            LOCTEXT("NewConversation", "Agent: 新建对话"),
            LOCTEXT("NewConversationTip", "创建一个独立的 Agent Workbench 会话窗口"),
            FSlateIcon(FAppStyle::GetAppStyleSetName(), TEXT("Icons.Plus")));
        Entry.StyleNameOverride = TEXT("CalloutToolbar");
        Section.AddEntry(Entry);
    }

    void CreateConversation()
    {
        if (SessionManager) { SessionManager->CreateSession(); }
    }

    FDelegateHandle MenuStartupHandle;
    TSharedPtr<FAgentSessionManager> SessionManager;
};

IMPLEMENT_MODULE(FAgentWorkbenchEditorModule, AgentWorkbenchEditor)

#undef LOCTEXT_NAMESPACE
