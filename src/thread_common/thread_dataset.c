#include "thread_dataset.h"

#include <string.h>
#include "openthread/dataset.h"
#include "openthread/error.h"

#define PRIVATE_CHANNEL  20u
#define PRIVATE_PAN_ID   0x6e79u

/* Deliberately private to this translation unit: never log or expose these. */
static const uint8_t s_network_key[OT_NETWORK_KEY_SIZE] = {
    0x91, 0x2d, 0xa7, 0x44, 0x5e, 0xb8, 0x0c, 0x39,
    0xf6, 0x13, 0x72, 0xcd, 0x28, 0x85, 0xea, 0x50,
};
static const uint8_t s_pskc[OT_PSKC_MAX_SIZE] = {
    0x6f, 0xf4, 0xc8, 0x1a, 0x7b, 0x23, 0x95, 0x50,
    0x3d, 0xae, 0x62, 0x18, 0xb0, 0x47, 0xdc, 0x89,
};
static const uint8_t s_extended_pan_id[OT_EXT_PAN_ID_SIZE] = {
    0x53, 0x68, 0x75, 0x6e, 0x79, 0x61, 0x20, 0x01,
};
static const uint8_t s_mesh_prefix[OT_MESH_LOCAL_PREFIX_SIZE] = {
    0xfd, 0x71, 0x6e, 0x79, 0x61, 0x20, 0x00, 0x01,
};

int thread_dataset_apply(otInstance *instance)
{
    if (!instance) {
        return OT_ERROR_INVALID_ARGS;
    }

    otOperationalDataset dataset;
    memset(&dataset, 0, sizeof(dataset));
    dataset.mActiveTimestamp.mSeconds = 1;
    memcpy(dataset.mNetworkKey.m8, s_network_key, sizeof(s_network_key));
    memcpy(dataset.mPskc.m8, s_pskc, sizeof(s_pskc));
    memcpy(dataset.mNetworkName.m8, "ShunyaPrivate", sizeof("ShunyaPrivate"));
    memcpy(dataset.mExtendedPanId.m8, s_extended_pan_id, sizeof(s_extended_pan_id));
    memcpy(dataset.mMeshLocalPrefix.m8, s_mesh_prefix, sizeof(s_mesh_prefix));
    dataset.mPanId = PRIVATE_PAN_ID;
    dataset.mChannel = PRIVATE_CHANNEL;
    dataset.mChannelMask = OT_CHANNEL_20_MASK;
    dataset.mSecurityPolicy.mRotationTime = 672;
    dataset.mSecurityPolicy.mRoutersEnabled = true;
    dataset.mComponents.mIsActiveTimestampPresent = true;
    dataset.mComponents.mIsNetworkKeyPresent = true;
    dataset.mComponents.mIsPskcPresent = true;
    dataset.mComponents.mIsNetworkNamePresent = true;
    dataset.mComponents.mIsExtendedPanIdPresent = true;
    dataset.mComponents.mIsMeshLocalPrefixPresent = true;
    dataset.mComponents.mIsPanIdPresent = true;
    dataset.mComponents.mIsChannelPresent = true;
    dataset.mComponents.mIsChannelMaskPresent = true;
    dataset.mComponents.mIsSecurityPolicyPresent = true;
    return otDatasetSetActive(instance, &dataset);
}

uint16_t thread_dataset_pan_id(void)
{
    return PRIVATE_PAN_ID;
}

uint8_t thread_dataset_channel(void)
{
    return PRIVATE_CHANNEL;
}
