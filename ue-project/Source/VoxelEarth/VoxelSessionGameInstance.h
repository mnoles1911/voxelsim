#pragma once
#include "CoreMinimal.h"
#include "Engine/GameInstance.h"
#include "VoxelSessionGameInstance.generated.h"

// Each client receives a distinct out-of-band transport invite. Only its
// non-secret identifier appears in the connection URL; keys never do.
UCLASS()
class VOXELEARTH_API UVoxelSessionGameInstance : public UGameInstance
{
    GENERATED_BODY()
public:
    virtual void ReceivedNetworkEncryptionToken(const FString& Token,const FOnEncryptionKeyResponse& Delegate) override;
    virtual void ReceivedNetworkEncryptionAck(const FOnEncryptionKeyResponse& Delegate) override;
    virtual EEncryptionFailureAction ReceivedNetworkEncryptionFailure(UNetConnection* Connection) override;
};
