#pragma once

#include "CoreMinimal.h"
#include "MCPToolTypes.h"

/** A scanned skill: SKILL.md content plus its parsed metadata. */
struct FAgentSkill
{
    FString Name;         // frontmatter "name", falling back to the directory name.
    FString Description;  // frontmatter "description", falling back to the first body line.
    FString Content;      // Full SKILL.md text, returned verbatim to the model.
    FString SourcePath;   // Diagnostics only; never used to read the skill.
};

/**
 * File-based skill loader. Scans one directory per skill under the skills root,
 * each containing a SKILL.md manifest, parses a minimal YAML frontmatter
 * (name, description) by hand, and exposes a catalog string for the system prompt
 * plus a by-name lookup for the bsharness.skill.load tool.
 *
 * Game thread only. The skill name is a registry key and is never combined with a
 * filesystem path. Skill content is data, not instructions (see SPEC section 6).
 */
class FAgentSkillLoader
{
public:
    static FAgentSkillLoader& Get();

    // Registry name and native handler id of the load-skill tool.
    static const TCHAR* LoadToolName();
    static const TCHAR* LoadToolHandlerId();

    // Resolve the skills root and (re)scan it. Safe to call multiple times.
    void Initialize();
    void Rescan();
    void Shutdown();

    const TMap<FString, FAgentSkill>& GetSkills() const { return Skills; }
    bool HasSkills() const { return Skills.Num() > 0; }

    /** "- name: description" lines, one per skill, in name order. */
    FString Catalog() const;
    /** System-prompt text (catalog plus usage hint); empty when no skills exist. */
    FString BuildSystemPrompt() const;
    /** Reads a skill by name. Returns false (and resets OutContent) when unknown. */
    bool TryLoad(const FString& Name, FString& OutContent) const;
    /** Comma-separated names, or "none" when empty; for error messages. */
    FString AvailableSkillsSummary() const;

private:
    FAgentSkillLoader() = default;
    ~FAgentSkillLoader() = default;
    FAgentSkillLoader(const FAgentSkillLoader&) = delete;
    FAgentSkillLoader& operator=(const FAgentSkillLoader&) = delete;

    static TArray<FString> ResolveSkillsRoots();

    TMap<FString, FAgentSkill> Skills;
};
