#pragma once

#include "Steam/NetPacket.h"

#include <string_view>

namespace SteamMetadata {

// Call only after loading a nonempty signature table for this steamclient SHA.
// This is a compatibility policy, not detection of an arbitrary future ABI:
// frozen legacy-baseline hashes use old offsets; other 64-bit builds use +16/+24.
// No TOML extensions or per-new-build exceptions are required.
inline NetPacketLayout SelectNetPacketLayout(std::string_view sha256) {
    // The extended layout is verified for 64-bit clients only. Keep x86 unchanged.
    if constexpr (sizeof(void*) != 8) return NetPacketLayout::Legacy;

    // Frozen legacy baseline; provenance and limits are in docs/PORTING_macOS.md.
    static constexpr std::string_view legacyHashes[] = {
        // Windows x64 (44).
        "0bf4de9af49894f0714dc52c4f79f61cd9ca8af92cdf212278693472b4a88f5f",
        "0e68d65dc938eda6104ba52b656ee525da624e7b5c9f09c3ba4131c27dc6f7ec",
        "1429b2a8c3477e630011b4b5d0f81dce416c3b874d638f5ef700fb0ab2553c4a",
        "1c00385f36c7c92c29d93e0396265cf61c23bd68cc96f1061f8e827f92e610bc",
        "1c61ae628cc4a9a367524d54445b21904212a68df1f1dd83a10cf33ab74aae19",
        "2582482f5387210c1f0de4b2ce7c7e2c463a33971e69aa602cc20bb3b9f7470a",
        "276a6a1d76c5b9954a9841779a97736cc60f30fb62958e29cd22f365e95824b8",
        "2a5c1b50bbc052bc725b488ca299902ab934fe26f710135a0207d6e8958c5542",
        "2fc31e60af8c176df62766ac3a84a458731b4ae0721e3b7a82ca1e328934e90e",
        "316c8327062485d08ac3e682e89d4c8fe54388d79fd1fe7902a8290959f1304d",
        "324284481993b6a0cd2566b68c8d57dafea71d712265a73d96c5f80346d30ce3",
        "37a353513e7c220ec5df4425cdc0cd62947bcab1c2207800bcb7b27963f731bb",
        "38b75e0d99ce8a42165bf34c6dbb6efbe17071b17be2416d11c80538c50bd3ce",
        "3905f113844f58e8c908979c97073bf16edd14e6af35d4705090e6b13c84154d",
        "3eda3b0a618c07037c4ef7496c6d69769e90519baf4c1c08daf79786448273e9",
        "3f864358fcf50e49e0a8c6bb8e1bf175e381f5628e4cd4997a59ca5e3976afe5",
        "44d99a1d170fbc33cca72b7e1edc5200f551687c54a5acacd8e1f6093476a520",
        "4c58e110be8e1181413bbda456c6a48a5c1d5ff8b3da70376f9a6272221b690b",
        "4e20ea4d442d1ac1cacb95bdf3ea7e2b12212dea99f970e66d71018b5c2ffa4c",
        "52ff7a513f8d5913a185ac8820eadb17d6e8e4144cd8fff36cc5bae6fa9d3fee",
        "616c6b8ab1960856adfbf938217607651bb18ce7f48bbd92ac772d31e9bbb35c",
        "61dd80e84a1c5ddab034f5253436835dead86b194e214bd99a6bd644a07dde7d",
        "62f3614624ed166d39f29b554aca7ba1256d1c0059d3a6a2bf753688a680173e",
        "6fb52af47edc3d5aab466ddb8d5a229d2bbfd14af5d7e36f1374db8876b22307",
        "78ee7a4b7b34ffdac47b26b9853e5ba35720d39e0ef83b4cfa17602e433d52e5",
        "7d40f9ea6675193a7fa114f86e6dd57b753405be7c68915179e9a8b2b9786f57",
        "7f0518fc3dec6e7e8e40d6fa7c2d88d59763329a765b941dae965144da26aabb",
        "7f1e4d10f5cb0087f50c3f67a8e1adead62c913166cb3cf9105b8a64098ec3e7",
        "849aeca3899f5a9431f143257e659706e22f1d51cae547bf80b5b9bbd4aec0a0",
        "86112382982fa855086f566b2fb8343290e798849029337dcfeecba0d5051b5e",
        "873b9b6f6d98453b1ec1658f02ef6ed1376f95db7e9d5229571cf1ca4a34ad7b",
        "8e7a322b26988b1f5b2996b5e66a9f3ed228e189a9cc3b394e30d1fbd109e989",
        "975a8b91400c076f2d9669ec0cabd5e301274e97f6272778783762336b535af7",
        "9cb6df39646878f9ea02279e39536f35746664a09096fc5dc7fbb12aff0c35ee",
        "9f663cf7aae950ea46265906edf17c5484b952fdc8d16afa628ae696e7b84831",
        "a0f1ce373e2c4b801ca8bada38433a6ca016acf814b58a2647a465f264460343",
        "a58f3cac3cb8fca5dde787af8bad99bb007a204fe7311bd734b7ca6dc0e79952",
        "aa44418f43ed50e0229102fa7c349fb0f35b63070c56ea285ca8dd07b2b16009",
        "afd63785ca7399e3e79b01005535c41624508f3ad1c37676e55978223218763b",
        "caba4826aa3501039d095aee1843a6bfb270fb43a3ab4455b2d6733223579fee",
        "ec96d11e170da4da509a724a50a0a8407577502c9e2fa018e0f06f0ca294ffdc",
        "f310e658367e4662859a8c6246d5b041ff17459546a765aade0539f4a759acc4",
        "f7738d6d6af3ecffdae145d87be788f17fff6dbf36428dbe661f7dd35f3ad722",
        "fb916a146e36e109ebb49241f56f9cb99fabecf4f5caa7e2e73024d1038deee7",
        // macOS arm64 (5).
        "0ddae658774104ebc0dea0d988c1862f03e488623d8f464dc556d76108b2be6c",
        "33901ffca63c5620f82f8b01ad2c52a645e87d6e1b2837a1993d2b97d89a0957",
        "42776e12adaa3114dac4b64fc4fff182cb03d44b2e94d29a48f103ffafc79428",
        "cb790257a7685f802adf0e624da1448363bcf310a727d3ffb7418ac14983b061",
        "d5b95272cf6e90c6ab86c4e3f3535c042b8c9e6fd10f1be7068711074e339699",
    };
    for (const auto legacyHash : legacyHashes) {
        if (sha256 == legacyHash) return NetPacketLayout::Legacy;
    }
    return NetPacketLayout::Extended;
}

} // namespace SteamMetadata
