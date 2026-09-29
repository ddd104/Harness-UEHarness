#include "AgentToolSchemaValidator.h"

#include "Dom/JsonValue.h"
#include "Internationalization/Regex.h"

namespace
{
constexpr int32 MaxDepth = 32;
constexpr int32 MaxJsonNodes = 8192;
constexpr int32 MaxMatchSteps = 32768;
constexpr int32 MaxEqualitySteps = 200000;
constexpr int32 MaxStringChars = 1024 * 1024;
constexpr int32 MaxPatternChars = 256;
constexpr int32 MaxPatternInputChars = 4096;

struct FValidationState
{
    int32 JsonNodes = 0;
    int32 MatchSteps = 0;
    int32 EqualitySteps = 0;
    FString Error;

    bool Fail(const FString& Path, const FString& Reason)
    {
        if (Error.IsEmpty())
        {
            Error = Path + TEXT(": ") + Reason;
        }
        return false;
    }
};

FString MemberPath(const FString& Parent, const FString& Member)
{
    return Parent + TEXT(".") + Member;
}

int32 CodePointLength(const FString& Value);

bool ScanJson(const TSharedPtr<FJsonValue>& Value, const FString& Path, int32 Depth, FValidationState& State)
{
    if (!Value.IsValid() || Value->Type == EJson::None)
    {
        return State.Fail(Path, TEXT("invalid JSON value"));
    }
    if (Depth > MaxDepth || ++State.JsonNodes > MaxJsonNodes)
    {
        return State.Fail(Path, TEXT("JSON depth or size limit exceeded"));
    }
    switch (Value->Type)
    {
    case EJson::Number:
        return FMath::IsFinite(Value->AsNumber()) || State.Fail(Path, TEXT("number must be finite"));
    case EJson::String:
        return (Value->AsString().Len() <= MaxStringChars && CodePointLength(Value->AsString()) >= 0)
            || State.Fail(Path, TEXT("string is too long or contains invalid UTF-16"));
    case EJson::Array:
    {
        const auto& Array = Value->AsArray();
        for (int32 Index = 0; Index < Array.Num(); ++Index)
        {
            if (!ScanJson(Array[Index], FString::Printf(TEXT("%s[%d]"), *Path, Index), Depth + 1, State))
            {
                return false;
            }
        }
        return true;
    }
    case EJson::Object:
    {
        const TSharedPtr<FJsonObject>& Object = Value->AsObject();
        if (!Object.IsValid()) { return State.Fail(Path, TEXT("invalid JSON object")); }
        for (const auto& Pair : Object->Values)
        {
            const FString Key(Pair.Key);
            if (Key.Len() > MaxStringChars || CodePointLength(Key) < 0
                || !ScanJson(Pair.Value, MemberPath(Path, Key), Depth + 1, State))
            {
                return State.Fail(Path, TEXT("object key is too long or contains an invalid value"));
            }
        }
        return true;
    }
    case EJson::Boolean:
    case EJson::Null:
        return true;
    default:
        return State.Fail(Path, TEXT("unsupported JSON value"));
    }
}

bool IsKnownType(const FString& Type)
{
    return Type == TEXT("object") || Type == TEXT("array") || Type == TEXT("string")
        || Type == TEXT("number") || Type == TEXT("integer") || Type == TEXT("boolean")
        || Type == TEXT("null");
}

bool IsNonNegativeInteger(const TSharedPtr<FJsonValue>& Value)
{
    return Value.IsValid() && Value->Type == EJson::Number
        && Value->AsNumber() >= 0.0 && Value->AsNumber() <= static_cast<double>(MAX_int32)
        && FMath::FloorToDouble(Value->AsNumber()) == Value->AsNumber();
}

bool CheckSchema(const TSharedPtr<FJsonValue>& Schema, const FString& Path, int32 Depth, FValidationState& State);

bool CheckSchemaObject(const TSharedPtr<FJsonObject>& Rule, const FString& Path, int32 Depth, FValidationState& State)
{
    if (Depth > MaxDepth) { return State.Fail(Path, TEXT("schema depth limit exceeded")); }
    for (const auto& Pair : Rule->Values)
    {
        const FString Key(Pair.Key);
        const FString FieldPath = MemberPath(Path, Key);
        const TSharedPtr<FJsonValue>& Field = Pair.Value;
        if (!Field.IsValid()) { return State.Fail(FieldPath, TEXT("invalid schema field")); }

        if (Key == TEXT("type"))
        {
            if (Field->Type == EJson::String)
            {
                if (!IsKnownType(Field->AsString())) { return State.Fail(FieldPath, TEXT("unknown JSON type")); }
            }
            else if (Field->Type == EJson::Array)
            {
                TSet<FString> Seen;
                for (const TSharedPtr<FJsonValue>& Entry : Field->AsArray())
                {
                    if (!Entry.IsValid() || Entry->Type != EJson::String || !IsKnownType(Entry->AsString())
                        || Seen.Contains(Entry->AsString()))
                    {
                        return State.Fail(FieldPath, TEXT("type must contain distinct known type names"));
                    }
                    Seen.Add(Entry->AsString());
                }
                if (Seen.IsEmpty()) { return State.Fail(FieldPath, TEXT("type list must not be empty")); }
            }
            else { return State.Fail(FieldPath, TEXT("type must be a string or string array")); }
        }
        else if (Key == TEXT("required"))
        {
            if (Field->Type != EJson::Array) { return State.Fail(FieldPath, TEXT("required must be an array")); }
            TSet<FString> Seen;
            for (const TSharedPtr<FJsonValue>& Entry : Field->AsArray())
            {
                if (!Entry.IsValid() || Entry->Type != EJson::String || Seen.Contains(Entry->AsString()))
                {
                    return State.Fail(FieldPath, TEXT("required must contain distinct strings"));
                }
                Seen.Add(Entry->AsString());
            }
        }
        else if (Key == TEXT("properties"))
        {
            if (Field->Type != EJson::Object || !Field->AsObject().IsValid())
            {
                return State.Fail(FieldPath, TEXT("properties must be an object"));
            }
            for (const auto& Property : Field->AsObject()->Values)
            {
                if (!CheckSchema(Property.Value, MemberPath(FieldPath, FString(Property.Key)), Depth + 1, State))
                {
                    return false;
                }
            }
        }
        else if (Key == TEXT("items") || Key == TEXT("additionalProperties"))
        {
            if (!CheckSchema(Field, FieldPath, Depth + 1, State)) { return false; }
        }
        else if (Key == TEXT("oneOf") || Key == TEXT("anyOf") || Key == TEXT("allOf"))
        {
            if (Field->Type != EJson::Array || Field->AsArray().IsEmpty())
            {
                return State.Fail(FieldPath, TEXT("schema alternatives must be a nonempty array"));
            }
            for (int32 Index = 0; Index < Field->AsArray().Num(); ++Index)
            {
                if (!CheckSchema(Field->AsArray()[Index],
                    FString::Printf(TEXT("%s[%d]"), *FieldPath, Index), Depth + 1, State))
                {
                    return false;
                }
            }
        }
        else if (Key == TEXT("enum"))
        {
            if (Field->Type != EJson::Array || Field->AsArray().IsEmpty())
            {
                return State.Fail(FieldPath, TEXT("enum must be a nonempty array"));
            }
        }
        else if (Key == TEXT("uniqueItems"))
        {
            if (Field->Type != EJson::Boolean)
            {
                return State.Fail(FieldPath, TEXT("uniqueItems must be a boolean"));
            }
        }
        else if (Key == TEXT("const") || Key == TEXT("default"))
        {
            // These accept any valid JSON value. The scan above checks their structure.
        }
        else if (Key == TEXT("minimum") || Key == TEXT("maximum")
            || Key == TEXT("exclusiveMinimum") || Key == TEXT("exclusiveMaximum")
            || Key == TEXT("multipleOf"))
        {
            if (Field->Type != EJson::Number || !FMath::IsFinite(Field->AsNumber())
                || (Key == TEXT("multipleOf") && Field->AsNumber() <= 0.0))
            {
                return State.Fail(FieldPath, TEXT("numeric bound must be finite; multipleOf must be positive"));
            }
        }
        else if (Key == TEXT("minLength") || Key == TEXT("maxLength")
            || Key == TEXT("minItems") || Key == TEXT("maxItems")
            || Key == TEXT("minProperties") || Key == TEXT("maxProperties"))
        {
            if (!IsNonNegativeInteger(Field))
            {
                return State.Fail(FieldPath, TEXT("size bound must be a nonnegative integer"));
            }
        }
        else if (Key == TEXT("pattern"))
        {
            if (Field->Type != EJson::String || Field->AsString().Len() > MaxPatternChars)
            {
                return State.Fail(FieldPath, TEXT("pattern must be a string of at most 256 characters"));
            }
            // FRegexPattern has no validity accessor. A valid pattern wrapped with an
            // unconditional second alternative must match the probe string.
            const FRegexPattern ProbePattern(FString(TEXT("(?:")) + Field->AsString() + TEXT(")|x"));
            FRegexMatcher Probe(ProbePattern, TEXT("x"));
            if (!Probe.FindNext())
            {
                return State.Fail(FieldPath, TEXT("invalid regular expression"));
            }
        }
        else if (Key == TEXT("title") || Key == TEXT("description") || Key == TEXT("$comment")
            || Key == TEXT("$id") || Key == TEXT("$schema"))
        {
            if (Field->Type != EJson::String) { return State.Fail(FieldPath, TEXT("annotation must be a string")); }
        }
        else if (Key == TEXT("deprecated") || Key == TEXT("readOnly") || Key == TEXT("writeOnly"))
        {
            if (Field->Type != EJson::Boolean) { return State.Fail(FieldPath, TEXT("annotation must be a boolean")); }
        }
        else if (Key == TEXT("examples"))
        {
            if (Field->Type != EJson::Array) { return State.Fail(FieldPath, TEXT("examples must be an array")); }
        }
        else
        {
            return State.Fail(FieldPath, TEXT("unsupported schema keyword"));
        }
    }

    for (const TCHAR* Kind : {TEXT("Length"), TEXT("Items"), TEXT("Properties")})
    {
        const FString MinName = FString(TEXT("min")) + Kind;
        const FString MaxName = FString(TEXT("max")) + Kind;
        const TSharedPtr<FJsonValue> Min = Rule->TryGetField(MinName);
        const TSharedPtr<FJsonValue> Max = Rule->TryGetField(MaxName);
        if (Min.IsValid() && Max.IsValid() && Min->AsNumber() > Max->AsNumber())
        {
            return State.Fail(Path, MinName + TEXT(" exceeds ") + MaxName);
        }
    }
    return true;
}

bool CheckSchema(const TSharedPtr<FJsonValue>& Schema, const FString& Path, int32 Depth, FValidationState& State)
{
    if (!Schema.IsValid()) { return State.Fail(Path, TEXT("invalid schema")); }
    if (Schema->Type == EJson::Boolean) { return true; }
    if (Schema->Type != EJson::Object || !Schema->AsObject().IsValid())
    {
        return State.Fail(Path, TEXT("schema must be an object or boolean"));
    }
    return CheckSchemaObject(Schema->AsObject(), Path, Depth, State);
}

bool JsonEqual(const TSharedPtr<FJsonValue>& Left, const TSharedPtr<FJsonValue>& Right,
    int32 Depth, FValidationState& State)
{
    if (++State.EqualitySteps > MaxEqualitySteps || Depth > MaxDepth)
    {
        State.Fail(TEXT("$"), TEXT("JSON comparison limit exceeded"));
        return false;
    }
    if (!Left.IsValid() || !Right.IsValid() || Left->Type != Right->Type) { return false; }
    switch (Left->Type)
    {
    case EJson::Null: return true;
    case EJson::String: return Left->AsString() == Right->AsString();
    case EJson::Number: return Left->AsNumber() == Right->AsNumber();
    case EJson::Boolean: return Left->AsBool() == Right->AsBool();
    case EJson::Array:
    {
        const auto& A = Left->AsArray();
        const auto& B = Right->AsArray();
        if (A.Num() != B.Num()) { return false; }
        for (int32 Index = 0; Index < A.Num(); ++Index)
        {
            if (!JsonEqual(A[Index], B[Index], Depth + 1, State)) { return false; }
        }
        return true;
    }
    case EJson::Object:
    {
        const auto& A = Left->AsObject()->Values;
        const auto& B = Right->AsObject()->Values;
        if (A.Num() != B.Num()) { return false; }
        for (const auto& Pair : A)
        {
            const TSharedPtr<FJsonValue>* Other = B.Find(Pair.Key);
            if (!Other || !JsonEqual(Pair.Value, *Other, Depth + 1, State)) { return false; }
        }
        return true;
    }
    default: return false;
    }
}

bool MatchesType(const TSharedPtr<FJsonValue>& Value, const FString& Type)
{
    if (Type == TEXT("integer"))
    {
        return Value->Type == EJson::Number && FMath::FloorToDouble(Value->AsNumber()) == Value->AsNumber();
    }
    if (Type == TEXT("number")) { return Value->Type == EJson::Number; }
    if (Type == TEXT("string")) { return Value->Type == EJson::String; }
    if (Type == TEXT("object")) { return Value->Type == EJson::Object; }
    if (Type == TEXT("array")) { return Value->Type == EJson::Array; }
    if (Type == TEXT("boolean")) { return Value->Type == EJson::Boolean; }
    return Type == TEXT("null") && Value->Type == EJson::Null;
}

int32 CodePointLength(const FString& Value)
{
    int32 Count = 0;
    for (int32 Index = 0; Index < Value.Len(); ++Index)
    {
        const uint32 Char = static_cast<uint32>(Value[Index]);
        if (Char >= 0xD800 && Char <= 0xDBFF)
        {
            if (Index + 1 >= Value.Len()) { return -1; }
            const uint32 Next = static_cast<uint32>(Value[Index + 1]);
            if (Next < 0xDC00 || Next > 0xDFFF) { return -1; }
            ++Index;
        }
        else if (Char >= 0xDC00 && Char <= 0xDFFF) { return -1; }
        ++Count;
    }
    return Count;
}

bool MatchValue(const TSharedPtr<FJsonValue>& Value, const TSharedPtr<FJsonValue>& Schema,
    const FString& Path, int32 Depth, FValidationState& State)
{
    if (++State.MatchSteps > MaxMatchSteps || Depth > MaxDepth)
    {
        return State.Fail(Path, TEXT("validation depth or work limit exceeded"));
    }
    if (Schema->Type == EJson::Boolean)
    {
        return Schema->AsBool() || State.Fail(Path, TEXT("value is forbidden by schema"));
    }
    const TSharedPtr<FJsonObject>& Rule = Schema->AsObject();
    const TSharedPtr<FJsonValue> Type = Rule->TryGetField(TEXT("type"));
    if (Type.IsValid())
    {
        bool bMatches = false;
        if (Type->Type == EJson::String) { bMatches = MatchesType(Value, Type->AsString()); }
        else
        {
            for (const TSharedPtr<FJsonValue>& Allowed : Type->AsArray())
            {
                if (MatchesType(Value, Allowed->AsString())) { bMatches = true; break; }
            }
        }
        if (!bMatches) { return State.Fail(Path, TEXT("value has the wrong JSON type")); }
    }

    bool bMatchesExactConstraint = false;
    if (const TSharedPtr<FJsonValue> Constant = Rule->TryGetField(TEXT("const")); Constant.IsValid())
    {
        if (!JsonEqual(Value, Constant, 0, State))
        {
            return State.Error.IsEmpty() ? State.Fail(Path, TEXT("value does not match const")) : false;
        }
        bMatchesExactConstraint = true;
    }
    if (const TSharedPtr<FJsonValue> Enum = Rule->TryGetField(TEXT("enum")); Enum.IsValid())
    {
        bool bFound = false;
        for (const TSharedPtr<FJsonValue>& Candidate : Enum->AsArray())
        {
            if (JsonEqual(Value, Candidate, 0, State)) { bFound = true; break; }
            if (!State.Error.IsEmpty()) { return false; }
        }
        if (!bFound) { return State.Fail(Path, TEXT("value is not in enum")); }
        bMatchesExactConstraint = true;
    }

    // Keep the work counters across alternatives, while discarding an ordinary
    // mismatch from a branch that the value does not select.
    for (const TCHAR* Keyword : {TEXT("allOf"), TEXT("anyOf"), TEXT("oneOf")})
    {
        const TSharedPtr<FJsonValue> Alternatives = Rule->TryGetField(Keyword);
        if (!Alternatives.IsValid()) { continue; }
        const FString Kind(Keyword);
        int32 Matches = 0;
        for (const TSharedPtr<FJsonValue>& Alternative : Alternatives->AsArray())
        {
            FValidationState Trial = State;
            const bool bMatched = MatchValue(Value, Alternative, Path, Depth + 1, Trial);
            State.MatchSteps = Trial.MatchSteps;
            State.EqualitySteps = Trial.EqualitySteps;
            if (State.MatchSteps > MaxMatchSteps || State.EqualitySteps > MaxEqualitySteps)
            {
                return State.Fail(Path, TEXT("validation work limit exceeded"));
            }
            if (bMatched) { ++Matches; }
            else if (Kind == TEXT("allOf"))
            {
                return State.Fail(Path, TEXT("allOf constraint failed: ") + Trial.Error);
            }
        }
        if ((Kind == TEXT("anyOf") && Matches == 0)
            || (Kind == TEXT("oneOf") && Matches != 1))
        {
            return State.Fail(Path, Kind + TEXT(" constraint failed"));
        }
        bMatchesExactConstraint = true;
    }

    if (Value->Type == EJson::Number)
    {
        const double Number = Value->AsNumber();
        for (const TCHAR* Key : {TEXT("minimum"), TEXT("maximum"), TEXT("exclusiveMinimum"), TEXT("exclusiveMaximum")})
        {
            const TSharedPtr<FJsonValue> Bound = Rule->TryGetField(Key);
            if (!Bound.IsValid()) { continue; }
            const double Limit = Bound->AsNumber();
            const FString Name(Key);
            const bool bValid = Name == TEXT("minimum") ? Number >= Limit
                : Name == TEXT("maximum") ? Number <= Limit
                : Name == TEXT("exclusiveMinimum") ? Number > Limit : Number < Limit;
            if (!bValid) { return State.Fail(Path, Name + TEXT(" constraint failed")); }
        }
        if (const TSharedPtr<FJsonValue> Multiple = Rule->TryGetField(TEXT("multipleOf")); Multiple.IsValid())
        {
            const double Quotient = Number / Multiple->AsNumber();
            const double Rounded = FMath::IsFinite(Quotient) ? FMath::RoundToDouble(Quotient) : 0.0;
            const bool bMultiple = FMath::IsFinite(Quotient)
                && (Rounded == 0.0 ? Number == 0.0
                    : FMath::Abs(Quotient - Rounded) <= 1e-12 * FMath::Abs(Rounded));
            if (!bMultiple)
            {
                return State.Fail(Path, TEXT("multipleOf constraint failed"));
            }
        }
    }
    else if (Value->Type == EJson::String)
    {
        const FString& String = Value->AsString();
        const int32 Length = CodePointLength(String);
        if (const TSharedPtr<FJsonValue> Min = Rule->TryGetField(TEXT("minLength")); Min.IsValid() && Length < Min->AsNumber())
        {
            return State.Fail(Path, TEXT("minLength constraint failed"));
        }
        if (const TSharedPtr<FJsonValue> Max = Rule->TryGetField(TEXT("maxLength")); Max.IsValid() && Length > Max->AsNumber())
        {
            return State.Fail(Path, TEXT("maxLength constraint failed"));
        }
        if (const TSharedPtr<FJsonValue> Pattern = Rule->TryGetField(TEXT("pattern")); Pattern.IsValid())
        {
            if (String.Len() > MaxPatternInputChars)
            {
                return State.Fail(Path, TEXT("pattern input is too long"));
            }
            const FRegexPattern CompiledPattern(Pattern->AsString());
            FRegexMatcher Matcher(CompiledPattern, String);
            if (!Matcher.FindNext()) { return State.Fail(Path, TEXT("pattern constraint failed")); }
        }
    }
    else if (Value->Type == EJson::Array)
    {
        const auto& Array = Value->AsArray();
        if (const TSharedPtr<FJsonValue> Min = Rule->TryGetField(TEXT("minItems")); Min.IsValid() && Array.Num() < Min->AsNumber())
        {
            return State.Fail(Path, TEXT("minItems constraint failed"));
        }
        if (const TSharedPtr<FJsonValue> Max = Rule->TryGetField(TEXT("maxItems")); Max.IsValid() && Array.Num() > Max->AsNumber())
        {
            return State.Fail(Path, TEXT("maxItems constraint failed"));
        }
        if (const TSharedPtr<FJsonValue> Unique = Rule->TryGetField(TEXT("uniqueItems")); Unique.IsValid() && Unique->AsBool())
        {
            for (int32 First = 0; First < Array.Num(); ++First)
            {
                for (int32 Second = First + 1; Second < Array.Num(); ++Second)
                {
                    if (JsonEqual(Array[First], Array[Second], 0, State))
                    {
                        return State.Fail(Path, TEXT("uniqueItems constraint failed"));
                    }
                    if (!State.Error.IsEmpty()) { return false; }
                }
            }
        }
        if (const TSharedPtr<FJsonValue> Items = Rule->TryGetField(TEXT("items")); Items.IsValid())
        {
            for (int32 Index = 0; Index < Array.Num(); ++Index)
            {
                if (!MatchValue(Array[Index], Items, FString::Printf(TEXT("%s[%d]"), *Path, Index), Depth + 1, State))
                {
                    return false;
                }
            }
        }
    }
    else if (Value->Type == EJson::Object)
    {
        const TSharedPtr<FJsonObject>& Object = Value->AsObject();
        if (const TSharedPtr<FJsonValue> Min = Rule->TryGetField(TEXT("minProperties")); Min.IsValid() && Object->Values.Num() < Min->AsNumber())
        {
            return State.Fail(Path, TEXT("minProperties constraint failed"));
        }
        if (const TSharedPtr<FJsonValue> Max = Rule->TryGetField(TEXT("maxProperties")); Max.IsValid() && Object->Values.Num() > Max->AsNumber())
        {
            return State.Fail(Path, TEXT("maxProperties constraint failed"));
        }
        if (const TSharedPtr<FJsonValue> Required = Rule->TryGetField(TEXT("required")); Required.IsValid())
        {
            for (const TSharedPtr<FJsonValue>& Entry : Required->AsArray())
            {
                if (!Object->HasField(Entry->AsString()))
                {
                    return State.Fail(MemberPath(Path, Entry->AsString()), TEXT("required property is missing"));
                }
            }
        }
        const TSharedPtr<FJsonValue> Properties = Rule->TryGetField(TEXT("properties"));
        const TSharedPtr<FJsonValue> Additional = Rule->TryGetField(TEXT("additionalProperties"));
        for (const auto& Pair : Object->Values)
        {
            const FString Key(Pair.Key);
            const FString PropertyPath = MemberPath(Path, Key);
            const TSharedPtr<FJsonValue> PropertyRule = Properties.IsValid() ? Properties->AsObject()->TryGetField(Key) : nullptr;
            if (PropertyRule.IsValid())
            {
                if (!MatchValue(Pair.Value, PropertyRule, PropertyPath, Depth + 1, State)) { return false; }
            }
            else if (Additional.IsValid())
            {
                if (!MatchValue(Pair.Value, Additional, PropertyPath, Depth + 1, State)) { return false; }
            }
            else if (!bMatchesExactConstraint || Properties.IsValid() || Additional.IsValid())
            {
                return State.Fail(PropertyPath, TEXT("additional property is not allowed"));
            }
        }
    }
    return true;
}
}

bool FAgentToolSchemaValidator::Validate(const TSharedPtr<FJsonObject>& Schema,
    const TSharedRef<FJsonObject>& Arguments, FString& OutError)
{
    OutError.Reset();
    if (!Schema.IsValid())
    {
        OutError = TEXT("$: missing tool input schema");
        return false;
    }

    FValidationState State;
    const TSharedPtr<FJsonValue> SchemaValue = MakeShared<FJsonValueObject>(Schema);
    const TSharedPtr<FJsonValue> ArgumentValue = MakeShared<FJsonValueObject>(Arguments);
    if (!ScanJson(SchemaValue, TEXT("$schema"), 0, State)
        || !ScanJson(ArgumentValue, TEXT("$"), 0, State)
        || !CheckSchema(SchemaValue, TEXT("$schema"), 0, State)
        || !MatchValue(ArgumentValue, SchemaValue, TEXT("$"), 0, State))
    {
        OutError = State.Error;
        return false;
    }
    return true;
}
