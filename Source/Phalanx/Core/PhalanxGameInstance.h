// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Engine/GameInstance.h"
#include "PhalanxGameInstance.generated.h"

/**
 * 
 */
UCLASS()
class PHALANX_API UPhalanxGameInstance : public UGameInstance
{
	GENERATED_BODY()
public:
	void Init() override;
	void Shutdown() override;
};
