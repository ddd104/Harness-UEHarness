#include "Modules/ModuleManager.h"
#include "BSHarnessHookRegistry.h"

class FBSHarnessHooksModule final : public IModuleInterface
{
public:
    virtual void ShutdownModule() override
    {
        if (IsInGameThread())
        {
            FBSHarnessHookRegistry::Get().Shutdown();
        }
    }
};

IMPLEMENT_MODULE(FBSHarnessHooksModule, BSHarnessHooks)
