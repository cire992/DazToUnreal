#pragma once

#include "CoreMinimal.h"
#include "DazToUnrealEnums.h"

class FJsonObject;
class UAnimBlueprint;
class UAnimInstance;
class USkeleton;
class USkeletalMesh;

#if ENGINE_MAJOR_VERSION > 4
typedef FVector3f MorphVectorType;
#else
typedef FVector MorphVectorType;
#endif

class FDazToUnrealMorphs
{
public:
	// Called to create the JCM AnimInstance
	static UAnimBlueprint* CreateJointControlAnimation(TSharedPtr<FJsonObject> JsonObject, FString Folder, FString CharacterName, USkeleton* Skeleton, USkeletalMesh* Mesh);

	// Returns whether the DTU file contains data for AutoJCM
	static bool IsAutoJCMImport(TSharedPtr<FJsonObject> JsonObject);

private:

	// Internal function for creating the AnimBlueprint for the AnimInstance
	static UAnimBlueprint* CreateBlueprint(UObject* InParent, FName Name, USkeleton* Skeleton);

	// Re-import: clears an existing JCM AnimBlueprint's AnimGraph and rebuilds it in place
	static void ResetBlueprint(UAnimBlueprint* AnimBlueprint, USkeleton* Skeleton);

	// Sets the skeleton and builds the Input Pose -> Control Rig -> Output Pose AnimGraph
	static void SetupJointControlGraph(UAnimBlueprint* AnimBlueprint, USkeleton* Skeleton);
};