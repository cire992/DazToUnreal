#include "DazToUnrealMorphs.h"
#include "DazToUnreal.h"
#include "Serialization/JsonReader.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimBlueprint.h"
#include "Animation/AnimBlueprintGeneratedClass.h"
#include "EdGraphSchema_K2_Actions.h"
#include "AnimGraphNode_LinkedInputPose.h"
#include "AnimGraphNode_Root.h"
#if ENGINE_MAJOR_VERSION > 4
#include "AnimGraphNode_ControlRig.h"
#endif
#include "Kismet2/BlueprintEditorUtils.h"
#include "Animation/MorphTarget.h"
#include "LODUtilities.h"
#include "Math/DualQuat.h"

UAnimBlueprint* FDazToUnrealMorphs::CreateJointControlAnimation(TSharedPtr<FJsonObject> JsonObject, FString Folder, FString CharacterName, USkeleton* Skeleton, USkeletalMesh* Mesh)
{
	// Only only create the JCM Anim if the JointLinks object exists
	const TArray<TSharedPtr<FJsonValue>>* JointLinkList;// = JsonObject->GetArrayField(TEXT("JointLinks"));
	if (JsonObject->TryGetArrayField(TEXT("JointLinks"), JointLinkList))
	{
		const FString AssetName = CharacterName + TEXT("_JCMAnim");
		const FString PackageName = FPackageName::ObjectPathToPackageName(Folder) / AssetName;
		const FString ObjectPath = PackageName + TEXT(".") + AssetName;

		UAnimBlueprint* AnimBlueprint = nullptr;

		// On a re-import the JCM Anim Blueprint already exists (the skeletal mesh's post-process ABP points at it).
		// FKismetEditorUtilities::CreateBlueprint asserts when a Blueprint of that name exists, so reuse the
		// existing one in place: clear its AnimGraph and rebuild it. References to it stay valid.
		if (UObject* ExistingObject = StaticLoadObject(UObject::StaticClass(), nullptr, *ObjectPath, nullptr, LOAD_NoWarn | LOAD_Quiet))
		{
			AnimBlueprint = Cast<UAnimBlueprint>(ExistingObject);
			if (AnimBlueprint == nullptr)
			{
				UE_LOG(LogDazToUnreal, Error, TEXT("DazToUnreal: %s already exists and isn't an Anim Blueprint (%s). Skipping the JCM Anim Blueprint; nothing was replaced."), *ObjectPath, *ExistingObject->GetClass()->GetName());
				return nullptr;
			}
			UE_LOG(LogDazToUnreal, Log, TEXT("DazToUnreal: Reusing existing JCM Anim Blueprint %s"), *ObjectPath);
			ResetBlueprint(AnimBlueprint, Skeleton);
		}
		else
		{
			// Create the new blueprint next to the SkeletalMesh and get the AnimInstance from it.
#if ENGINE_MAJOR_VERSION == 4 && ENGINE_MINOR_VERSION < 26
			UPackage* NewPackage = CreatePackage(nullptr, *PackageName);
#else
			UPackage* NewPackage = CreatePackage(*PackageName);
#endif
			AnimBlueprint = CreateBlueprint(NewPackage, FName(AssetName), Skeleton);
		}


#if 0
#if ENGINE_MAJOR_VERSION == 4 && ENGINE_MINOR_VERSION <= 26
		FLODUtilities::RegenerateLOD(Mesh, -1, true, true);
#else
		FLODUtilities::RegenerateLOD(Mesh, nullptr, -1, true, true);
#endif
#endif


		// Recompile the AnimBlueprint and return the updated AnimInstance
		FBlueprintEditorUtils::MarkBlueprintAsModified(AnimBlueprint);
		FKismetEditorUtilities::CompileBlueprint(AnimBlueprint);

		return AnimBlueprint;
	}
	return nullptr;
}

// This is a stripped down version of UAnimBlueprintFactory::FactoryCreateNew
UAnimBlueprint* FDazToUnrealMorphs::CreateBlueprint(UObject* InParent, FName Name, USkeleton* Skeleton)
{
	// Create the Blueprint
	UClass* ClassToUse = UAnimInstance::StaticClass();
	UAnimBlueprint* NewBP = CastChecked<UAnimBlueprint>(FKismetEditorUtilities::CreateBlueprint(ClassToUse, InParent, Name, BPTYPE_Normal, UAnimBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass(), NAME_None));

	SetupJointControlGraph(NewBP, Skeleton);
	return NewBP;
}

// Re-import path: brings an existing JCM Anim Blueprint back to the state CreateBlueprint leaves a new one in.
void FDazToUnrealMorphs::ResetBlueprint(UAnimBlueprint* AnimBlueprint, USkeleton* Skeleton)
{
	AnimBlueprint->Modify();

	// The JCM ABP is a plain post-process AnimInstance. Only reparent if it no longer derives from one.
	if (AnimBlueprint->ParentClass == nullptr || !AnimBlueprint->ParentClass->IsChildOf(UAnimInstance::StaticClass()))
	{
		AnimBlueprint->ParentClass = UAnimInstance::StaticClass();
	}

#if ENGINE_MAJOR_VERSION > 4
	// Clear the AnimGraph down to its Output Pose (root) node. SetupJointControlGraph adds the rest again.
	TArray<UEdGraph*> Graphs;
	AnimBlueprint->GetAllGraphs(Graphs);
	for (UEdGraph* Graph : Graphs)
	{
		if (Graph->GetFName() != UEdGraphSchema_K2::GN_AnimGraph)
		{
			continue;
		}

		TArray<UEdGraphNode*> NodesToRemove;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (Node == nullptr)
			{
				continue;
			}
			if (Node->IsA<UAnimGraphNode_Root>())
			{
				Node->BreakAllNodeLinks();
			}
			else
			{
				NodesToRemove.Add(Node);
			}
		}
		for (UEdGraphNode* Node : NodesToRemove)
		{
			FBlueprintEditorUtils::RemoveNode(AnimBlueprint, Node, /*bDontRecompile*/ true);
		}
	}
#endif

	SetupJointControlGraph(AnimBlueprint, Skeleton);
}

// Points the ABP at the skeleton and builds Input Pose -> Control Rig -> Output Pose in its AnimGraph.
void FDazToUnrealMorphs::SetupJointControlGraph(UAnimBlueprint* AnimBlueprint, USkeleton* Skeleton)
{
	AnimBlueprint->TargetSkeleton = Skeleton;

	// Because the BP itself didn't have the skeleton set when the initial compile occured, it's not set on the generated classes either
	if (UAnimBlueprintGeneratedClass* TypedNewClass = Cast<UAnimBlueprintGeneratedClass>(AnimBlueprint->GeneratedClass))
	{
		TypedNewClass->TargetSkeleton = Skeleton;
	}
	if (UAnimBlueprintGeneratedClass* TypedNewClass_SKEL = Cast<UAnimBlueprintGeneratedClass>(AnimBlueprint->SkeletonGeneratedClass))
	{
		TypedNewClass_SKEL->TargetSkeleton = Skeleton;
	}

#if ENGINE_MAJOR_VERSION > 4
	// Need to add the Pose Input Node and connect it to the Output Node in the AnimGraph
	TArray<UEdGraph*> Graphs;
	AnimBlueprint->GetAllGraphs(Graphs);
	for (UEdGraph* Graph : Graphs)
	{
		if (Graph->GetFName() == UEdGraphSchema_K2::GN_AnimGraph)
		{
			// The Output Pose (root) node. A new ABP has only that node in its AnimGraph, and a reset one is cleared down to it.
			UEdGraphNode* OutNode = nullptr;
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (Node && Node->IsA<UAnimGraphNode_Root>())
				{
					OutNode = Node;
					break;
				}
			}

			// If the Blueprint isn't open in an Editor Window adding the UAnimGraphNode_LinkedInputPose node will crash.
			GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()->OpenEditorForAsset(AnimBlueprint);

			// Create the Input Node
			UAnimGraphNode_LinkedInputPose* const InputTemplate = NewObject<UAnimGraphNode_LinkedInputPose>();
			UEdGraphNode* const NewNode = FEdGraphSchemaAction_NewNode::SpawnNodeFromTemplate<UAnimGraphNode_LinkedInputPose>(Graph, InputTemplate, FVector2D(0.0f, 0.0f), false);

			// Create the Control Rig Node
			UAnimGraphNode_ControlRig* const ControlRigTemplate = NewObject<UAnimGraphNode_ControlRig>();
			UEdGraphNode* const ControlRigNode = FEdGraphSchemaAction_NewNode::SpawnNodeFromTemplate<UAnimGraphNode_ControlRig>(Graph, ControlRigTemplate, FVector2D(0.0f, 0.0f), false);

			if (NewNode && OutNode && ControlRigNode)
			{
				UEdGraphPin* NextPin = NewNode->FindPin(TEXT("Pose"));

				UEdGraphPin* ControlRigSourcePin = ControlRigNode->FindPin(TEXT("Source"));
				UEdGraphPin* ControlRigNextPin = ControlRigNode->FindPin(TEXT("Pose"));

				UEdGraphPin* OutPin = OutNode->FindPin(TEXT("Result"));

				NextPin->MakeLinkTo(ControlRigSourcePin);
				ControlRigNextPin->MakeLinkTo(OutPin);
			}

			// Don't need the editor open anymore, so close it now.
			// Note: Turns out I can't close it because part of the action happens on a tick
			//GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()->CloseAllEditorsForAsset(AnimBlueprint);
		}
	}
#endif
}

bool FDazToUnrealMorphs::IsAutoJCMImport(TSharedPtr<FJsonObject> JsonObject)
{
	// UE4 doesn't have some of the functions needed for creating the control rig and blueprint automatically.
#if ENGINE_MAJOR_VERSION > 4
	const TArray<TSharedPtr<FJsonValue>>* JointLinkList;
	if (JsonObject->TryGetArrayField(TEXT("JointLinks"), JointLinkList))
	{
		return true;
	}
#endif
	return false;

}