#include "DazScenePaths.h"

#include "Misc/Paths.h"
#include "Misc/SecureHash.h"

FString FDazScenePaths::SanitizeSegment(const FString& Segment)
{
	FString Result = SanitizeChars(Segment);
	if (Result.IsEmpty() && !Segment.TrimStartAndEnd().IsEmpty())
	{
		return TEXT("n") + ShortHash(Segment);
	}
	return Result.IsEmpty() ? FString(TEXT("_")) : Result;
}

FString FDazScenePaths::ValidAssetName(const FString& Name)
{
	FString Result = Name;
	bool bAnyValid = false;
	for (TCHAR& Char : Result)
	{
		const bool bValid = (Char >= 'A' && Char <= 'Z') || (Char >= 'a' && Char <= 'z') || (Char >= '0' && Char <= '9') || Char == '-' || Char == '_';
		if (!bValid)
		{
			Char = TEXT('_');
		}
		bAnyValid |= bValid && Char != TEXT('_');
	}
	if (!bAnyValid && !Name.TrimStartAndEnd().IsEmpty())
	{
		return TEXT("n") + ShortHash(Name);
	}
	return Result.IsEmpty() ? FString(TEXT("_")) : Result;
}

FString FDazScenePaths::SanitizeChars(const FString& Segment)
{
	FString Result;
	Result.Reserve(Segment.Len());
	bool bLastWasUnderscore = false;
	for (const TCHAR Char : Segment)
	{
		const bool bValid = (Char >= 'A' && Char <= 'Z') || (Char >= 'a' && Char <= 'z') || (Char >= '0' && Char <= '9') || Char == '-' || Char == '_';
		const TCHAR Out = bValid ? Char : TEXT('_');
		if (Out == TEXT('_'))
		{
			if (bLastWasUnderscore)
			{
				continue;
			}
			bLastWasUnderscore = true;
		}
		else
		{
			bLastWasUnderscore = false;
		}
		Result.AppendChar(Out);
	}
	while (Result.StartsWith(TEXT("_")))
	{
		Result.RightChopInline(1);
	}
	while (Result.EndsWith(TEXT("_")))
	{
		Result.LeftChopInline(1);
	}
	return Result;
}

FString FDazScenePaths::MakeFolder(const FString& Root, const FString& RelativeFolder, int32 NameBudget, int32 MaxObjectPathLength)
{
	// The folder part of MakeAssetPath for a stand-in file of the budgeted length
	FString Folder;
	FString Name;
	MakeAssetPath(Root, RelativeFolder / FString::ChrN(FMath::Max(1, NameBudget), TEXT('x')) + TEXT(".x"), MaxObjectPathLength, Folder, Name);
	return Folder;
}

FString FDazScenePaths::ObjectPath(const FString& PackageFolder, const FString& AssetName)
{
	return PackageFolder / AssetName + TEXT(".") + AssetName;
}

FString FDazScenePaths::ShortHash(const FString& Path)
{
	return FMD5::HashAnsiString(*NormalizeKey(Path)).Left(8);
}

FString FDazScenePaths::NormalizeKey(const FString& Path)
{
	FString Key = Path.Replace(TEXT("\\"), TEXT("/")).ToLower();
	while (Key.StartsWith(TEXT("/")))
	{
		Key.RightChopInline(1);
	}
	return Key;
}

void FDazScenePaths::MakeAssetPath(const FString& Root, const FString& RuntimePath, int32 MaxObjectPathLength,
	FString& OutPackageFolder, FString& OutAssetName)
{
	TArray<FString> Parts;
	RuntimePath.Replace(TEXT("\\"), TEXT("/")).ParseIntoArray(Parts, TEXT("/"), true);

	FString FileName = Parts.Num() > 0 ? Parts.Pop() : FString(TEXT("_"));
	OutAssetName = SanitizeSegment(FPaths::GetBaseFilename(FileName));

	TArray<FString> Folders;
	for (const FString& Part : Parts)
	{
		Folders.Add(SanitizeSegment(Part));
	}

	const FString CleanRoot = Root.EndsWith(TEXT("/")) ? Root.LeftChop(1) : Root;
	auto Build = [&]()
	{
		FString Folder = CleanRoot;
		for (const FString& Part : Folders)
		{
			Folder /= Part;
		}
		return Folder;
	};

	OutPackageFolder = Build();
	// What Windows sees: <Project>/Content/<package path>.uasset
	int32 Length = (OutPackageFolder / OutAssetName).Len();
	if (Length <= MaxObjectPathLength)
	{
		return;
	}

	// Too long for Windows: the hash keeps the shortened path unique and stable
	const FString Hash = ShortHash(RuntimePath);
	const int32 MinSegment = 8;
	OutAssetName = OutAssetName.Left(FMath::Max(MinSegment, 40)) + TEXT("_") + Hash;
	Length = (Build() / OutAssetName).Len();

	// Shorten the longest folder first, never below MinSegment characters
	while (Length > MaxObjectPathLength)
	{
		int32 Longest = INDEX_NONE;
		for (int32 Index = 0; Index < Folders.Num(); Index++)
		{
			if (Folders[Index].Len() > MinSegment && (Longest == INDEX_NONE || Folders[Index].Len() > Folders[Longest].Len()))
			{
				Longest = Index;
			}
		}
		if (Longest == INDEX_NONE)
		{
			break;
		}
		const int32 Excess = Length - MaxObjectPathLength;
		const int32 NewLength = FMath::Max(MinSegment, Folders[Longest].Len() - Excess);
		Folders[Longest] = Folders[Longest].Left(NewLength);
		Length = (Build() / OutAssetName).Len();
	}

	// Still too long (very deep folders): shorten the name, keeping the hash
	if (Length > MaxObjectPathLength)
	{
		const int32 Excess = Length - MaxObjectPathLength;
		const FString Stem = OutAssetName.LeftChop(Hash.Len() + 1);
		OutAssetName = Stem.Left(FMath::Max(1, Stem.Len() - Excess)) + TEXT("_") + Hash;
	}
	OutPackageFolder = Build();
}
