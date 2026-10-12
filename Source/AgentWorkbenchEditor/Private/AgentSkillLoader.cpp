#include "AgentSkillLoader.h"

#include "AgentWorkbenchSettings.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFile.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#endif

namespace
{
// Remove a leading UTF-8 BOM before parsing.
FString StripBom(const FString& Text)
{
    FString Result = Text;
    if (Result.StartsWith(TEXT("\xFEFF"), ESearchCase::CaseSensitive))
    {
        Result.RightChopInline(1);
    }
    return Result;
}

// Python "value.lstrip('# ')": strip any leading '#' or space characters.
FString LStripHashSpace(FString Value)
{
    while (!Value.IsEmpty() && (Value[0] == TEXT('#') || Value[0] == TEXT(' ')))
    {
        Value.RightChopInline(1);
    }
    return Value;
}

// Python " ".join(value.split()): collapse all whitespace runs to single spaces.
FString CollapseWhitespace(const FString& Value)
{
    TArray<FString> Tokens;
    Value.ParseIntoArrayWS(Tokens);
    return FString::Join(Tokens, TEXT(" "));
}

FString NormalizeDescription(const FString& Raw)
{
    return CollapseWhitespace(LStripHashSpace(Raw.TrimStartAndEnd()));
}

// Splits a leading "---" ... "---" frontmatter block from the body. Returns false
// when there is no valid frontmatter block at the top of the file. Only the "name"
// and "description" keys are read; other keys (e.g. allow_implicit_invocation) are
// ignored, matching the skill format documented in s07.
bool ParseFrontmatter(const FString& Content, FString& OutName, FString& OutDescription,
    FString& OutBody)
{
    OutName.Empty();
    OutDescription.Empty();
    const FString Text = StripBom(Content);
    // Without a frontmatter block the whole file is the body, so the fallback
    // description becomes the file's first line (matching the s07 reference).
    OutBody = Text;

    TArray<FString> Lines;
    Text.ParseIntoArrayLines(Lines);
    if (Lines.IsEmpty() || Lines[0].TrimStartAndEnd() != TEXT("---"))
    {
        return false;
    }
    int32 Close = INDEX_NONE;
    for (int32 Index = 1; Index < Lines.Num(); ++Index)
    {
        if (Lines[Index].TrimStartAndEnd() == TEXT("---"))
        {
            Close = Index;
            break;
        }
    }
    if (Close == INDEX_NONE)
    {
        return false;
    }
    for (int32 Index = 1; Index < Close; ++Index)
    {
        const FString& Line = Lines[Index];
        int32 Colon = INDEX_NONE;
        if (!Line.FindChar(TEXT(':'), Colon)) { continue; }
        const FString Key = Line.Left(Colon).TrimStartAndEnd();
        const FString Value = Line.Mid(Colon + 1).TrimStartAndEnd();
        if (Key == TEXT("name")) { OutName = Value; }
        else if (Key == TEXT("description")) { OutDescription = Value; }
    }
    TArray<FString> BodyLines;
    for (int32 Index = Close + 1; Index < Lines.Num(); ++Index)
    {
        BodyLines.Add(Lines[Index]);
    }
    OutBody = FString::Join(BodyLines, TEXT("\n"));
    return true;
}

FString FirstBodyLine(const FString& Body)
{
    TArray<FString> Lines;
    Body.ParseIntoArrayLines(Lines);
    return Lines.IsEmpty() ? FString() : Lines[0];
}

bool IsUnderDirectory(const FString& Path, const FString& Directory)
{
    FString NormalizedPath = FPaths::ConvertRelativePathToFull(Path);
    FString NormalizedDirectory = FPaths::ConvertRelativePathToFull(Directory);
    NormalizedPath.ReplaceInline(TEXT("\\"), TEXT("/"));
    NormalizedDirectory.ReplaceInline(TEXT("\\"), TEXT("/"));
    if (!NormalizedDirectory.EndsWith(TEXT("/")))
    {
        NormalizedDirectory.AppendChar(TEXT('/'));
    }
    return NormalizedPath.StartsWith(NormalizedDirectory);
}
}

const TCHAR* FAgentSkillLoader::LoadToolName() { return TEXT("bsharness.skill.load"); }
const TCHAR* FAgentSkillLoader::LoadToolHandlerId() { return TEXT("skill.load"); }

FAgentSkillLoader& FAgentSkillLoader::Get()
{
    static FAgentSkillLoader Loader;
    return Loader;
}

TArray<FString> FAgentSkillLoader::ResolveSkillsRoots()
{
    TArray<FString> Roots;
    const UAgentWorkbenchSettings* Settings = GetDefault<UAgentWorkbenchSettings>();
    if (Settings)
    {
        for (const FAgentSkillDirectory& Entry : Settings->SkillDirectories)
        {
            FString Root = Entry.Path.TrimStartAndEnd();
            if (Root.IsEmpty()) { continue; }
            if (FPaths::IsRelative(Root))
            {
                Root = FPaths::Combine(FPaths::ProjectDir(), Root);
            }
            Roots.AddUnique(FPaths::ConvertRelativePathToFull(Root));
        }
    }
    if (Roots.IsEmpty())
    {
        Roots.Add(FPaths::ConvertRelativePathToFull(
            FPaths::Combine(FPaths::ProjectDir(), TEXT("Skills"))));
    }
    return Roots;
}

void FAgentSkillLoader::Initialize()
{
    Rescan();
}

void FAgentSkillLoader::Shutdown()
{
    Skills.Empty();
}

void FAgentSkillLoader::Rescan()
{
    check(IsInGameThread());
    Skills.Empty();

    class FSkillScanVisitor : public IPlatformFile::FDirectoryVisitor
    {
    public:
        FSkillScanVisitor(TMap<FString, FAgentSkill>& InSkills, const FString& InRoot)
            : Skills(InSkills), Root(InRoot) {}

        virtual bool Visit(const TCHAR* FilenameOrDirectory, bool bIsDirectory) override
        {
            if (!bIsDirectory) { return true; }
            const FString SkillDirectory(FilenameOrDirectory);
            const FString Manifest = FPaths::Combine(SkillDirectory, TEXT("SKILL.md"));
            if (!FPaths::FileExists(Manifest) || !IsUnderDirectory(Manifest, Root))
            {
                return true;
            }
            FString Content;
            if (!FFileHelper::LoadFileToString(Content, *Manifest))
            {
                UE_LOG(LogTemp, Warning, TEXT("AgentSkillLoader: could not read %s"), *Manifest);
                return true;
            }
            FString Name, Description, Body;
            ParseFrontmatter(Content, Name, Description, Body);
            if (Name.IsEmpty())
            {
                Name = FPaths::GetPathLeaf(SkillDirectory);
            }
            if (Description.IsEmpty())
            {
                Description = FirstBodyLine(Body);
            }
            FAgentSkill Skill;
            Skill.Name = Name;
            Skill.Description = NormalizeDescription(Description);
            Skill.Content = MoveTemp(Content);
            Skill.SourcePath = Manifest;
            // Roots are configured in priority order; the first one that provides a
            // name wins, so a later duplicate does not silently replace it.
            if (Skills.Contains(Skill.Name))
            {
                UE_LOG(LogTemp, Display, TEXT("AgentSkillLoader: duplicate skill '%s' at %s ignored"),
                    *Skill.Name, *Manifest);
                return true;
            }
            Skills.Add(Skill.Name, MoveTemp(Skill));
            return true;
        }

    private:
        TMap<FString, FAgentSkill>& Skills;
        FString Root;
    };

    for (const FString& Root : ResolveSkillsRoots())
    {
        if (Root.IsEmpty() || !IFileManager::Get().DirectoryExists(*Root))
        {
            continue;
        }
        FSkillScanVisitor Visitor(Skills, Root);
        IFileManager::Get().IterateDirectory(*Root, Visitor);
    }
}

FString FAgentSkillLoader::Catalog() const
{
    TArray<FString> Names;
    Skills.GetKeys(Names);
    Names.Sort();
    TArray<FString> Lines;
    Lines.Reserve(Names.Num());
    for (const FString& Name : Names)
    {
        const FAgentSkill& Skill = Skills.FindRef(Name);
        Lines.Add(FString::Printf(TEXT("- %s: %s"), *Skill.Name, *Skill.Description));
    }
    return FString::Join(Lines, TEXT("\n"));
}

FString FAgentSkillLoader::BuildSystemPrompt() const
{
    const FString CatalogText = Catalog();
    if (CatalogText.IsEmpty())
    {
        return FString();
    }
    return FString::Printf(TEXT("Skills available:\n%s\n\n"
        "When a skill applies, use the skill-loading tool with the skill name to read "
        "its full instructions before acting."), *CatalogText);
}

bool FAgentSkillLoader::TryLoad(const FString& Name, FString& OutContent) const
{
    OutContent.Empty();
    const FAgentSkill* Skill = Skills.Find(Name);
    if (!Skill)
    {
        return false;
    }
    OutContent = Skill->Content;
    return true;
}

FString FAgentSkillLoader::AvailableSkillsSummary() const
{
    TArray<FString> Names;
    Skills.GetKeys(Names);
    Names.Sort();
    return Names.IsEmpty() ? TEXT("none") : FString::Join(Names, TEXT(", "));
}

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
FString SkillTestMakeTempRoot()
{
    const FString Root = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("AgentWorkbench"),
        TEXT("SkillLoaderTests"), FGuid::NewGuid().ToString());
    IFileManager::Get().MakeDirectory(*Root, true);
    return Root;
}

void SkillTestWriteSkill(const FString& Root, const FString& DirName, const FString& Name,
    const FString& Description, const FString& Body)
{
    const FString SkillDir = FPaths::Combine(Root, DirName);
    IFileManager::Get().MakeDirectory(*SkillDir, true);
    const FString Content = FString::Printf(TEXT("---\nname: %s\ndescription: %s\n---\n%s"),
        *Name, *Description, *Body);
    FFileHelper::SaveStringToFile(Content, *FPaths::Combine(SkillDir, TEXT("SKILL.md")));
}

void SkillTestWriteRawSkill(const FString& Root, const FString& DirName, const FString& Content)
{
    const FString SkillDir = FPaths::Combine(Root, DirName);
    IFileManager::Get().MakeDirectory(*SkillDir, true);
    FFileHelper::SaveStringToFile(Content, *FPaths::Combine(SkillDir, TEXT("SKILL.md")));
}

// Replaces the configured skill directories; returns the previous value for restore.
TArray<FAgentSkillDirectory> SkillTestSetDirectories(const TArray<FString>& Paths)
{
    UAgentWorkbenchSettings* Settings = GetMutableDefault<UAgentWorkbenchSettings>();
    const TArray<FAgentSkillDirectory> Previous = Settings->SkillDirectories;
    Settings->SkillDirectories.Reset();
    for (const FString& Path : Paths)
    {
        FAgentSkillDirectory Entry;
        Entry.Path = Path;
        Settings->SkillDirectories.Add(Entry);
    }
    return Previous;
}

void SkillTestRestoreDirectories(const TArray<FAgentSkillDirectory>& Previous)
{
    GetMutableDefault<UAgentWorkbenchSettings>()->SkillDirectories = Previous;
    FAgentSkillLoader::Get().Rescan();
}

// Project-root-relative form of an absolute path, matching the settings folder picker.
FString SkillTestToProjectRelative(const FString& Absolute)
{
    FString Full = FPaths::ConvertRelativePathToFull(Absolute);
    FString ProjectDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
    Full.ReplaceInline(TEXT("\\"), TEXT("/"));
    ProjectDir.ReplaceInline(TEXT("\\"), TEXT("/"));
    if (!ProjectDir.EndsWith(TEXT("/"))) { ProjectDir.AppendChar(TEXT('/')); }
    return Full.StartsWith(ProjectDir, ESearchCase::IgnoreCase)
        ? Full.RightChop(ProjectDir.Len()) : Full;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAgentSkillLoaderScanTest,
    "AgentWorkbench.SkillLoader.ScanCatalogLoad",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAgentSkillLoaderScanTest::RunTest(const FString& Parameters)
{
    const FString Root = SkillTestMakeTempRoot();
    SkillTestWriteSkill(Root, TEXT("code-review"), TEXT("code-review"),
        TEXT("Perform thorough code reviews."), TEXT("# Code Review\n\nInstructions here."));
    SkillTestWriteSkill(Root, TEXT("pdf"), TEXT("pdf"),
        TEXT("Process PDF files."), TEXT("# PDF\n\nMore here."));
    // Empty frontmatter values fall back to the directory name and the first body line.
    SkillTestWriteSkill(Root, TEXT("no-frontmatter"), TEXT(""), TEXT(""),
        TEXT("Fallback description line.\nSecond line."));
    // No frontmatter block at all: also falls back to directory name + first body line.
    SkillTestWriteRawSkill(Root, TEXT("raw"), TEXT("# Raw Skill\n\nFirst line description.\n"));

    const TArray<FAgentSkillDirectory> Previous = SkillTestSetDirectories({Root});

    FAgentSkillLoader& Loader = FAgentSkillLoader::Get();
    Loader.Rescan();

    TestEqual(TEXT("Four skills scanned"), Loader.GetSkills().Num(), 4);
    TestTrue(TEXT("Catalog lists code-review"),
        Loader.Catalog().Contains(TEXT("- code-review: Perform thorough code reviews.")));
    TestTrue(TEXT("Catalog lists empty-frontmatter fallback description"),
        Loader.Catalog().Contains(TEXT("- no-frontmatter: Fallback description line.")));
    TestTrue(TEXT("Catalog lists no-frontmatter-block fallback"),
        Loader.Catalog().Contains(TEXT("- raw: Raw Skill")));
    TestTrue(TEXT("System prompt includes the catalog"),
        Loader.BuildSystemPrompt().Contains(TEXT("Skills available:")));

    FString Content;
    TestTrue(TEXT("Load returns full content"),
        Loader.TryLoad(TEXT("code-review"), Content) && Content.Contains(TEXT("Instructions here.")));
    TestFalse(TEXT("Unknown skill is rejected"), Loader.TryLoad(TEXT("missing"), Content));
    TestTrue(TEXT("Summary lists known names"),
        Loader.AvailableSkillsSummary().Contains(TEXT("code-review")));

    SkillTestRestoreDirectories(Previous);
    IFileManager::Get().DeleteDirectory(*Root, false, true);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAgentSkillLoaderMultipleRootsTest,
    "AgentWorkbench.SkillLoader.MultipleDirectories",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAgentSkillLoaderMultipleRootsTest::RunTest(const FString& Parameters)
{
    const FString RootA = SkillTestMakeTempRoot();
    const FString RootB = SkillTestMakeTempRoot();
    SkillTestWriteSkill(RootA, TEXT("alpha"), TEXT("alpha"), TEXT("Alpha from A."), TEXT("# Alpha\n\nA."));
    SkillTestWriteSkill(RootB, TEXT("beta"), TEXT("beta"), TEXT("Beta from B."), TEXT("# Beta\n\nB."));
    // The same name in two roots: the earlier configured directory must win.
    SkillTestWriteSkill(RootA, TEXT("shared"), TEXT("shared"), TEXT("Shared from A."), TEXT("# Shared\n\nA body."));
    SkillTestWriteSkill(RootB, TEXT("shared"), TEXT("shared"), TEXT("Shared from B."), TEXT("# Shared\n\nB body."));

    const TArray<FAgentSkillDirectory> Previous = SkillTestSetDirectories({RootA, RootB});

    FAgentSkillLoader& Loader = FAgentSkillLoader::Get();
    Loader.Rescan();

    TestEqual(TEXT("Skills from both directories are merged"), Loader.GetSkills().Num(), 3);
    FString Content;
    TestTrue(TEXT("Skill from the first directory is present"), Loader.TryLoad(TEXT("alpha"), Content));
    TestTrue(TEXT("Skill from the second directory is present"), Loader.TryLoad(TEXT("beta"), Content));
    Content.Empty();
    TestTrue(TEXT("Duplicate name resolves"), Loader.TryLoad(TEXT("shared"), Content));
    TestTrue(TEXT("Earlier configured directory wins a duplicate name"), Content.Contains(TEXT("A body.")));
    TestFalse(TEXT("Later duplicate content is not used"), Content.Contains(TEXT("B body.")));

    SkillTestRestoreDirectories(Previous);
    IFileManager::Get().DeleteDirectory(*RootA, false, true);
    IFileManager::Get().DeleteDirectory(*RootB, false, true);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAgentSkillLoaderProjectRelativeTest,
    "AgentWorkbench.SkillLoader.ProjectRelativePath",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAgentSkillLoaderProjectRelativeTest::RunTest(const FString& Parameters)
{
    // The settings picker stores a project-relative path; the loader must resolve it
    // against the project root rather than the process working directory.
    const FString Root = SkillTestMakeTempRoot();
    SkillTestWriteSkill(Root, TEXT("relative-skill"), TEXT("relative-skill"),
        TEXT("Reached through a project-relative path."), TEXT("# Relative\n\nBody."));

    const FString Relative = SkillTestToProjectRelative(Root);
    TestTrue(TEXT("Temp directory is inside the project"), FPaths::IsRelative(Relative));
    TestTrue(TEXT("Relative path does not start with a drive root"),
        !Relative.StartsWith(TEXT("/")) && !Relative.Contains(TEXT(":")));

    const TArray<FAgentSkillDirectory> Previous = SkillTestSetDirectories({Relative});
    FAgentSkillLoader& Loader = FAgentSkillLoader::Get();
    Loader.Rescan();

    TestEqual(TEXT("Project-relative directory is scanned"), Loader.GetSkills().Num(), 1);
    FString Content;
    TestTrue(TEXT("Project-relative directory resolves to the skill"),
        Loader.TryLoad(TEXT("relative-skill"), Content));

    SkillTestRestoreDirectories(Previous);
    IFileManager::Get().DeleteDirectory(*Root, false, true);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAgentSkillLoaderEmptyCatalogTest,
    "AgentWorkbench.SkillLoader.EmptyCatalog",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAgentSkillLoaderEmptyCatalogTest::RunTest(const FString& Parameters)
{
    const FString EmptyRoot = SkillTestMakeTempRoot();
    const TArray<FAgentSkillDirectory> Previous = SkillTestSetDirectories({EmptyRoot});

    FAgentSkillLoader& Loader = FAgentSkillLoader::Get();
    Loader.Rescan();
    TestEqual(TEXT("No skills"), Loader.GetSkills().Num(), 0);
    TestTrue(TEXT("Empty catalog has empty system prompt"), Loader.BuildSystemPrompt().IsEmpty());
    TestEqual(TEXT("Empty summary is none"), Loader.AvailableSkillsSummary(), FString(TEXT("none")));

    SkillTestRestoreDirectories(Previous);
    IFileManager::Get().DeleteDirectory(*EmptyRoot, false, true);
    return true;
}

#endif
