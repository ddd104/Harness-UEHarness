#include "AgentWorkbenchSession.h"
#include "Framework/Commands/UIAction.h"
#include "Modules/ModuleManager.h"
#include "Styling/AppStyle.h"
#include "ToolMenu.h"
#include "ToolMenus.h"

#define LOCTEXT_NAMESPACE "AgentWorkbenchEditorModule"

class FAgentWorkbenchEditorModule : public IModuleInterface
{
public:
    virtual void StartupModule() override
    {
        SessionManager = MakeShared<FAgentSessionManager>();
        MenuStartupHandle = UToolMenus::RegisterStartupCallback(
            FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FAgentWorkbenchEditorModule::RegisterMenus));
    }

    virtual void ShutdownModule() override
    {
        UToolMenus::UnRegisterStartupCallback(MenuStartupHandle);
        UToolMenus::UnregisterOwner(this);
        if (SessionManager) { SessionManager->Shutdown(); SessionManager.Reset(); }
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
