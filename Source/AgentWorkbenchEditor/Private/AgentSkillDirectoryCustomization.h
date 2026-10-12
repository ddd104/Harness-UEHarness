#pragma once

#include "CoreMinimal.h"
#include "IPropertyTypeCustomization.h"

class IPropertyHandle;
class SButton;

/**
 * Details-panel customization for FAgentSkillDirectory. It shows an editable path
 * with a folder picker and stores the chosen directory relative to the project root,
 * so project settings stay portable. A folder outside the project keeps its absolute
 * path because it cannot be expressed relative to the project root.
 */
class FAgentSkillDirectoryCustomization : public IPropertyTypeCustomization
{
public:
    static TSharedRef<IPropertyTypeCustomization> MakeInstance();

    virtual void CustomizeHeader(TSharedRef<IPropertyHandle> StructPropertyHandle,
        FDetailWidgetRow& HeaderRow, IPropertyTypeCustomizationUtils& StructCustomizationUtils) override;
    virtual void CustomizeChildren(TSharedRef<IPropertyHandle> StructPropertyHandle,
        IDetailChildrenBuilder& StructBuilder, IPropertyTypeCustomizationUtils& StructCustomizationUtils) override {}

private:
    FReply OnBrowse();

    TSharedPtr<IPropertyHandle> PathProperty;
    TSharedPtr<SButton> BrowseButton;
};
