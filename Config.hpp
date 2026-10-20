#pragma once
#include <cstddef>

namespace GModConfig {
    inline constexpr const char* EngineVersions[] = {"VEngineClient015"};
    inline constexpr const char* ClientVersions[] = {"VClient017"};
    inline constexpr const char* EntityVersions[] = {"VClientEntityList003"};
    inline constexpr const char* ModelVersions[] = {"VEngineModel016"};
    inline constexpr const char* MaterialVersions[] = {"VMaterialSystem080"};

    namespace VTable {
        inline constexpr std::size_t GetAllClasses = 8;
        inline constexpr std::size_t GetPlayerInfo = 8;
        inline constexpr std::size_t GetLocalPlayer = 12;
        inline constexpr std::size_t GetMaxClients = 21;
        inline constexpr std::size_t IsInGame = 26;
        inline constexpr std::size_t WorldToScreenMatrix = 36;
        inline constexpr std::size_t GetClientEntity = 3;
        inline constexpr std::size_t ForcedMaterialOverride = 1;
        inline constexpr std::size_t DrawModelExecute = 19;
        inline constexpr std::size_t FindMaterial = 84;
        inline constexpr std::size_t ColorModulate = 28;
        inline constexpr std::size_t SetMaterialVarFlag = 29;
        inline constexpr std::size_t Present = 17;
        inline constexpr std::size_t Reset = 16;
        inline constexpr std::size_t PresentEx = 121;
        inline constexpr std::size_t ResetEx = 132;
    }
    inline constexpr int MenuKey = 0x7A; // F11
    inline constexpr int EspKey = 0x70; // F1
    inline constexpr int ChamsKey = 0x71; // F2
    inline constexpr int EnemyKey = 0x72; // F3
    inline constexpr int MaterialIgnoreZ = 1 << 15;
    inline constexpr float FallbackPlayerHeight = 72.0f;
}
