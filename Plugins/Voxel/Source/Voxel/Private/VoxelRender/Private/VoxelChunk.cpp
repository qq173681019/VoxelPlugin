// Copyright 2017 Phyronnaz

#include "VoxelPrivatePCH.h"
#include "VoxelChunk.h"
#include "ProceduralMeshComponent.h"
#include "Misc/IQueuedWork.h"
#include "AI/Navigation/NavigationSystem.h"
#include "VoxelRender.h"
#include "Engine.h"
#include "Camera/PlayerCameraManager.h"
#include "InstancedStaticMesh.h"
#include "Kismet/KismetMathLibrary.h"
#include "ChunkOctree.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"

DECLARE_CYCLE_STAT(TEXT("VoxelChunk ~ SetProcMeshSection"), STAT_SetProcMeshSection, STATGROUP_Voxel);
DECLARE_CYCLE_STAT(TEXT("VoxelChunk ~ Update"), STAT_Update, STATGROUP_Voxel);

// Sets default values
AVoxelChunk::AVoxelChunk() : Render(nullptr), MeshBuilder(nullptr), Builder(nullptr)
{
	PrimaryActorTick.bCanEverTick = true;

	// Create primary mesh
	PrimaryMesh = CreateDefaultSubobject<UProceduralMeshComponent>(FName("PrimaryMesh"));
	PrimaryMesh->bCastShadowAsTwoSided = true;
	PrimaryMesh->bUseAsyncCooking = true;
	PrimaryMesh->SetCollisionObjectType(ECollisionChannel::ECC_WorldDynamic);
	PrimaryMesh->Mobility = EComponentMobility::Movable;
	RootComponent = PrimaryMesh;

	ChunkHasHigherRes.SetNumZeroed(6);
}

void AVoxelChunk::Init(TWeakPtr<ChunkOctree> NewOctree)
{
	check(NewOctree.IsValid());
	CurrentOctree = NewOctree.Pin();
	Render = CurrentOctree->Render;

	FIntVector NewPosition = CurrentOctree->GetMinimalCornerPosition();

#if WITH_EDITOR
	FString Name = FString::FromInt(NewPosition.X) + ", " + FString::FromInt(NewPosition.Y) + ", " + FString::FromInt(NewPosition.Z);
	this->SetActorLabel(Name);
#endif

	this->SetActorRelativeLocation((FVector)NewPosition);
	this->SetActorRelativeRotation(FRotator::ZeroRotator);
	this->SetActorRelativeScale3D(FVector::OneVector);

	// Needed because octree is only partially builded when Init is called
	Render->AddTransitionCheck(this);
}

bool AVoxelChunk::Update(bool bAsync)
{
	SCOPE_CYCLE_COUNTER(STAT_Update);

	// Update ChunkHasHigherRes
	if (CurrentOctree->Depth != 0)
	{
		for (int i = 0; i < 6; i++)
		{
			TransitionDirection Direction = (TransitionDirection)i;
			TWeakPtr<ChunkOctree> Chunk = CurrentOctree->GetAdjacentChunk(Direction);
			if (Chunk.IsValid())
			{
				ChunkHasHigherRes[i] = Chunk.Pin()->Depth < CurrentOctree->Depth;
			}
			else
			{
				ChunkHasHigherRes[i] = false;
			}
		}
	}

	if (bAsync)
	{
		if (!MeshBuilder)
		{
			CreateBuilder();
			MeshBuilder = new FAsyncTask<FAsyncPolygonizerTask>(Builder, this);
			MeshBuilder->StartBackgroundTask(Render->MeshThreadPool);

			return true;
		}
		else
		{
			return false;
		}
	}
	else
	{
		if (MeshBuilder)
		{
			MeshBuilder->EnsureCompletion();
			delete MeshBuilder;
			MeshBuilder = nullptr;
		}
		if (Builder)
		{
			delete Builder;
			Builder = nullptr;
		}

		CreateBuilder();
		Builder->CreateSection(Section);
		delete Builder;
		Builder = nullptr;

		ApplyNewMesh();

		return true;
	}
}

void AVoxelChunk::CheckTransitions()
{
	if (Render->World->bComputeTransitions)
	{
		for (int i = 0; i < 6; i++)
		{
			TransitionDirection Direction = (TransitionDirection)i;
			TWeakPtr<ChunkOctree> Chunk = CurrentOctree->GetAdjacentChunk(Direction);
			if (Chunk.IsValid())
			{
				TSharedPtr<ChunkOctree> ChunkPtr = Chunk.Pin();

				bool bThisHasHigherRes = ChunkPtr->Depth > CurrentOctree->Depth;

				check(ChunkPtr->GetVoxelChunk());
				if (bThisHasHigherRes != ChunkPtr->GetVoxelChunk()->HasChunkHigherRes(InvertTransitionDirection(Direction)))
				{
					Render->UpdateChunk(Chunk, true);
				}
			}
		}
	}
}

void AVoxelChunk::Unload()
{
	DeleteTasks();

	// Needed because octree is only partially updated when Unload is called
	Render->AddTransitionCheck(this);

	GetWorld()->GetTimerManager().SetTimer(DeleteTimer, this, &AVoxelChunk::Delete, Render->World->DeletionDelay, false);
}

void AVoxelChunk::Delete()
{
	// In case delete is called directly
	DeleteTasks();

	// Reset mesh & position & clear lines
	PrimaryMesh->SetProcMeshSection(0, FProcMeshSection());

#if WITH_EDITOR
	SetActorLabel("InactiveChunk");
#endif // WITH_EDITOR

	// Delete foliage
	for (auto FoliageComponent : FoliageComponents)
	{
		FoliageComponent->DestroyComponent();
	}
	FoliageComponents.Empty();


	// Add to pool
	Render->SetChunkAsInactive(this);


	// Reset variables
	Render = nullptr;
	CurrentOctree.Reset();
}

void AVoxelChunk::OnMeshComplete(FProcMeshSection& InSection)
{
	SCOPE_CYCLE_COUNTER(STAT_SetProcMeshSection);

	Section = InSection;

	Render->AddApplyNewMesh(this);
}

void AVoxelChunk::ApplyNewMesh()
{
	// TODO
	//if (CurrentOctree->Depth <= Render->World->MaxGrassDepth)

	if (MeshBuilder)
	{
		MeshBuilder->EnsureCompletion();
		delete MeshBuilder;
		MeshBuilder = nullptr;
	}
	if (Builder)
	{
		delete Builder;
		Builder = nullptr;
	}

	Render->AddFoliageUpdate(this);

	PrimaryMesh->SetProcMeshSection(0, Section);

	UNavigationSystem::UpdateComponentInNavOctree(*PrimaryMesh);
}

void AVoxelChunk::SetMaterial(UMaterialInterface* Material)
{
	PrimaryMesh->SetMaterial(0, Material);
}

bool AVoxelChunk::HasChunkHigherRes(TransitionDirection Direction)
{
	return CurrentOctree->Depth != 0 && ChunkHasHigherRes[Direction];
}

bool AVoxelChunk::UpdateFoliage()
{
	if (FoliageTasks.Num() == 0)
	{
		for (int Index = 0; Index < Render->World->GrassTypes.Num(); Index++)
		{
			auto GrassType = Render->World->GrassTypes[Index];
			for (auto GrassVariety : GrassType->GrassVarieties)
			{
				FAsyncTask<FAsyncFoliageTask>* FoliageTask = new FAsyncTask<FAsyncFoliageTask>(Section, GrassVariety, Index, Render->World->GetVoxelSize(), CurrentOctree->GetMinimalCornerPosition(), 10, this);

				FoliageTask->StartBackgroundTask(Render->FoliageThreadPool);
				FoliageTasks.Add(FoliageTask);
			}
		}
		return true;
	}
	else
	{
		return false;
	}
}


void AVoxelChunk::OnFoliageComplete()
{
	CompletedFoliageTaskCount++;
	if (CompletedFoliageTaskCount == FoliageTasks.Num())
	{
		OnAllFoliageComplete();
	}
}

void AVoxelChunk::OnAllFoliageComplete()
{
	Render->AddApplyNewFoliage(this);
	CompletedFoliageTaskCount = 0;
}

void AVoxelChunk::ApplyNewFoliage()
{
	for (int i = 0; i < FoliageComponents.Num(); i++)
	{
		FoliageComponents[i]->DestroyComponent();
	}
	FoliageComponents.Empty();

	for (auto FoliageTask : FoliageTasks)
	{
		FoliageTask->EnsureCompletion();
		FAsyncFoliageTask Task = FoliageTask->GetTask();
		if (Task.InstanceBuffer.NumInstances())
		{
			FGrassVariety GrassVariety = Task.GrassVariety;

			int32 FolSeed = FCrc::StrCrc32((GrassVariety.GrassMesh->GetName() + GetName()).GetCharArray().GetData());
			if (FolSeed == 0)
			{
				FolSeed++;
			}

			//Create component
			UHierarchicalInstancedStaticMeshComponent* HierarchicalInstancedStaticMeshComponent = NewObject<UHierarchicalInstancedStaticMeshComponent>(this, NAME_None, RF_Transient);

			HierarchicalInstancedStaticMeshComponent->OnComponentCreated();
			HierarchicalInstancedStaticMeshComponent->RegisterComponent();
			if (HierarchicalInstancedStaticMeshComponent->bWantsInitializeComponent) HierarchicalInstancedStaticMeshComponent->InitializeComponent();

			HierarchicalInstancedStaticMeshComponent->Mobility = EComponentMobility::Movable;
			HierarchicalInstancedStaticMeshComponent->bCastStaticShadow = false;

			HierarchicalInstancedStaticMeshComponent->SetStaticMesh(GrassVariety.GrassMesh);
			HierarchicalInstancedStaticMeshComponent->MinLOD = GrassVariety.MinLOD;
			HierarchicalInstancedStaticMeshComponent->bSelectable = false;
			HierarchicalInstancedStaticMeshComponent->bHasPerInstanceHitProxies = false;
			HierarchicalInstancedStaticMeshComponent->bReceivesDecals = GrassVariety.bReceivesDecals;
			static FName NoCollision(TEXT("NoCollision"));
			HierarchicalInstancedStaticMeshComponent->SetCollisionProfileName(NoCollision);
			HierarchicalInstancedStaticMeshComponent->bDisableCollision = true;
			HierarchicalInstancedStaticMeshComponent->SetCanEverAffectNavigation(false);
			HierarchicalInstancedStaticMeshComponent->InstancingRandomSeed = FolSeed;
			HierarchicalInstancedStaticMeshComponent->LightingChannels = GrassVariety.LightingChannels;

			HierarchicalInstancedStaticMeshComponent->InstanceStartCullDistance = GrassVariety.StartCullDistance;
			HierarchicalInstancedStaticMeshComponent->InstanceEndCullDistance = GrassVariety.EndCullDistance;

			HierarchicalInstancedStaticMeshComponent->bAffectDistanceFieldLighting = false;

			HierarchicalInstancedStaticMeshComponent->AttachToComponent(GetRootComponent(), FAttachmentTransformRules::KeepRelativeTransform);
			FTransform DesiredTransform = GetRootComponent()->GetComponentTransform();
			DesiredTransform.RemoveScaling();
			HierarchicalInstancedStaticMeshComponent->SetWorldTransform(DesiredTransform);

			FoliageComponents.Add(HierarchicalInstancedStaticMeshComponent);

			if (!HierarchicalInstancedStaticMeshComponent->PerInstanceRenderData.IsValid())
			{
				HierarchicalInstancedStaticMeshComponent->InitPerInstanceRenderData(&Task.InstanceBuffer);
			}
			else
			{
				HierarchicalInstancedStaticMeshComponent->PerInstanceRenderData->UpdateFromPreallocatedData(HierarchicalInstancedStaticMeshComponent, Task.InstanceBuffer);
			}

			HierarchicalInstancedStaticMeshComponent->AcceptPrebuiltTree(Task.ClusterTree, Task.OutOcclusionLayerNum);

			//HierarchicalInstancedStaticMeshComponent->RecreateRenderState_Concurrent();
			HierarchicalInstancedStaticMeshComponent->MarkRenderStateDirty();
		}

		delete FoliageTask;
	}
	FoliageTasks.Empty();
}


void AVoxelChunk::DeleteTasks()
{
	if (MeshBuilder)
	{
		MeshBuilder->EnsureCompletion();
		delete MeshBuilder;
		MeshBuilder = nullptr;
	}
	if (Builder)
	{
		delete Builder;
		Builder = nullptr;
	}
	for (auto FoliageTask : FoliageTasks)
	{
		FoliageTask->EnsureCompletion();
		delete FoliageTask;
	}
	FoliageTasks.Empty();
}

void AVoxelChunk::CreateBuilder()
{
	check(!Builder);
	Builder = new VoxelPolygonizer(
		CurrentOctree->Depth,
		Render->World->Data,
		CurrentOctree->GetMinimalCornerPosition(),
		ChunkHasHigherRes,
		CurrentOctree->Depth != 0 && Render->World->bComputeTransitions
	);
}