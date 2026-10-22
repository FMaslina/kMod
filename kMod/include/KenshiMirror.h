#pragma once
// Зеркала игровых структур Kenshi 1.0.65 для работы без тяжёлых заголовков
// SDK (Ogre/MyGUI 2010-х годов не компилируются современным MSVC).
//
// Правила:
// - имена классов/методов/вложенности — ТОЧНО как в KenshiLib, иначе
//   компоновщик не найдёт стаб в KenshiLib.lib (это проверка на этапе
//   сборки: несовпадение сигнатуры = ошибка линковки, а не падение игры);
// - смещения полей сверены с заголовками SDK (указаны источники);
// - не передавать std::string через границу DLL (ABI VS2010 vs VS2022);
// - только POD через границу: указатели, float, int, enum, bool.
#include <cstddef>
#include <cstdint>
#include <kenshi/Enums.h> // лёгкий: только enum, itemType/StatsEnumerated

class Item; // неполного типа достаточно для совпадения манглинга
class Damages; // непрозрачный: только передача по ссылке дальше в движок
class RootObject; // базовый объект движка (только указатели, без чтения полей)

namespace Ogre { class Vector3 { public: float x; float y; float z; }; }

// MedicalSystem.h:15-20
enum LimbState
{
    LIMB_ORIGINAL = 0x0,
    LIMB_STUMP = 0x1,
    LIMB_REPLACED = 0x2,
    LIMB_CRUSHED = 0x3
};

class RobotLimbs
{
public:
    // MedicalSystem.h:47-54
    enum Limb
    {
        LEFT_ARM,
        RIGHT_ARM,
        LEFT_LEG,
        RIGHT_LEG,
        NULL_LIMB
    };

    // MedicalSystem.h:69-72
    void* character; // 0x0
    void* inventory; // 0x8
    LimbState states[0x4]; // 0x10
    void* items[0x4]; // 0x20

    LimbState getState(Limb limb) const; // RVA 0xCD410
    void setLimb(Limb limb, LimbState state, Item* item); // RVA 0xCFD90
};

class GameData
{
public:
    // GameData.h:88,91 — id 0x1C, type 0x50
    char _pad0[0x1C];
    int id; // 0x1C
    char _pad1[0x50 - 0x1C - sizeof(int)];
    itemType type; // 0x50
};

class RaceData
{
public:
    // RaceData.h:31 — data 0x40
    char _pad0[0x40];
    GameData* data; // 0x40
};

class Character;
class MedicalSystem;

class CharStats
{
public:
    // CharStats.h:27-28,99 — medical 0x8, me 0x10, _toughness 0x90
    char _pad0[0x8];
    void* medical; // 0x8
    Character* me; // 0x10
    char _pad1[0x90 - 0x18];
    float _toughness; // 0x90

    void xpStat_timeBased(StatsEnumerated st); // RVA 0x8C5AA0
    void xpStat_eventBased(StatsEnumerated st, float amount); // RVA 0x8C5B60
};

class MedicalSystem
{
public:
    class HealthPartStatus
    {
    public:
        // MedicalSystem.h:135-140
        enum PartType
        {
            PART_TORSO,
            PART_LEG,
            PART_ARM,
            PART_HEAD
        };

        // MedicalSystem.h:131,142-175
        void* data; // 0x0
        PartType whatAmI; // 0x8
        MedicalSystem* medical; // 0x10
        Character* me; // 0x18
        int side; // 0x20 (LeftRight)
        void* robotLimb; // 0x28
        // 0x30..0x40: selfHealing/collapses/fatal/KOMult/hitChance... — пропуск
        char _pad1[0x40 - 0x30];
        float flesh; // 0x40
        float fleshStun; // 0x44
        float bandaging; // 0x48: прогресс бинта (первая помощь движка)
        float juryRigging; // 0x4C: прогресс ремонта робочасти
        float wearDamage; // 0x50: износ робочасти
        float _maxHealth; // 0x54
        float age; // 0x58
        float HPMult; // 0x5C: множитель максимального HP (раса/возраст)
        float derivedFleshHealthPercent; // 0x60: «процент HP», который показывает движок

        void applyDamage(const Damages& damage); // RVA 0x6442F0
        bool isRobotic(); // RVA 0x6432F0
        RobotLimbs::Limb getRobotLimbEnum() const; // RVA 0x643420
        bool isDead() const; // RVA 0x656DC0: смерть ЧАСТИ (для головы/груди/живота Genius всегда false)
        // RVA 0x64EE40: кадровый пересчёт части (может перезаписывать _maxHealth)
        bool update(float frameTIME, float healMultFlesh, float healMultRobot,
            float degenerationRate, float _age, float robotWear);
    };

    // MedicalSystem.h: vptr 0x0; blood 0x70; limbs 0x80-0x98; robotLimbs 0xC8;
    // me 0xE0; dead 0x164; anatomy 0x190; stats 0x1A8.
    void* _vftable; // 0x0
    char _pad0[0x70 - 0x8];
    float blood; // 0x70
    float extraBloodLossFromBodyparts; // 0x74
    float currentBleedRate; // 0x78
    char _pad1[0x80 - 0x7C];
    HealthPartStatus* leftLeg; // 0x80
    HealthPartStatus* rightLeg; // 0x88
    HealthPartStatus* leftArm; // 0x90
    HealthPartStatus* rightArm; // 0x98
    float knockoutTimer; // 0xA0
    float worstDamage; // 0xA4 (MedicalSystem.h)
    float needsFirstAidScoreTotal_robot; // 0xA8
    float needsFirstAidScoreTotal_fleshy; // 0xAC
    float restedState; // 0xB0: «отдохнул» — в ваниле лечит только он/нок
    float stealthHinderance; // 0xB4
    float partBestArm; // 0xB8
    float partHead; // 0xBC
    float partWorstTorso; // 0xC0
    float dazedOrAlert; // 0xC4
    RobotLimbs* robotLimbs; // 0xC8
    char _pad3[0xE0 - 0xD0];
    Character* me; // 0xE0
    char _pad4[0x160 - 0xE8];
    bool crippled; // 0x160
    bool unconcious; // 0x161
    bool sub50KO; // 0x162
    bool bloodlossTrauma; // 0x163
    bool dead; // 0x164
    char _pad5[0x190 - 0x165];

    // lektor<HealthPartStatus*>: база-аллокатор 0x8 (vptr), count 0x8,
    // maxSize 0xC, stuff 0x10 — относительно начала lektor.
    struct PartList
    {
        void* _allocVft; // 0x0
        uint32_t count; // 0x8
        uint32_t maxSize; // 0xC
        HealthPartStatus** stuff; // 0x10
    };
    PartList anatomy; // 0x190

    void medicalUpdate(float frameTime); // RVA 0x651880
    float getMaxBlood() const; // RVA 0x643920
    void _NV_updateStats(); // RVA 0x644B50: пересчёт статов движком (может затирать _maxHealth)
    void validateHealthValues(); // RVA 0x644E20
    void updateDamageState(); // RVA 0x6460E0 (protected в SDK, на манглинг не влияет)
    void _reassessRagdollPartsAssumingWeJustClearedTheEntireRagdoll(); // RVA 0x6497D0
    // Перегрузка урона верхнего уровня (combat/wounds путь). Protected в SDK.
    void applyDamage(HealthPartStatus* part, const Damages& damage, bool loadingSavestate, bool canSever, const Ogre::Vector3& force); // RVA 0x64EB80
    bool isDead() const; // RVA 0x32DA90: флаг смерти всей медсистемы
    bool isUnconcious() const; // RVA 0x28D8F0 (только чтение, хук не ставим — нокаут легален)
};

class Character
{
public:
    bool isDead() const; // RVA 0x620E30: для Genius всегда false
    void declareDead(); // RVA 0x7A4FA0: для Genius не вызывается
    void dropGearOnDeath(bool vampiricDeath); // RVA 0x5CB090: для Genius не вызывается
    // Character.h:561,705-706 — myRace 0x2E0, stats 0x450, medical 0x458
    char _pad0[0x2E0];
    RaceData* myRace; // 0x2E0
    char _pad1[0x450 - 0x2E8];
    CharStats* stats; // 0x450
    MedicalSystem medical; // 0x458 (встроен, не указатель)
};

class GameWorld
{
public:
    bool removeFromDeathParade(Character* who); // RVA 0x795A40
    // Последний рубеж: физическое уничтожение объекта. Для Genius блокируем
    // только НЕпотоковую смерть (justUnloaded==false); выгрузку зон не трогаем.
    bool destroy(RootObject* obj, bool justUnloaded, const char* debugInfo); // RVA 0x798890
};

__declspec(dllimport) GameWorld* ou;

// Проверки раскладки на этапе компиляции.
static_assert(offsetof(GameData, id) == 0x1C, "GameData::id offset");
static_assert(offsetof(GameData, type) == 0x50, "GameData::type offset");
static_assert(offsetof(RaceData, data) == 0x40, "RaceData::data offset");
static_assert(offsetof(CharStats, me) == 0x10, "CharStats::me offset");
static_assert(offsetof(CharStats, _toughness) == 0x90, "CharStats::_toughness offset");
static_assert(offsetof(Character, myRace) == 0x2E0, "Character::myRace offset");
static_assert(offsetof(Character, stats) == 0x450, "Character::stats offset");
static_assert(offsetof(Character, medical) == 0x458, "Character::medical offset");
static_assert(offsetof(MedicalSystem, blood) == 0x70, "MedicalSystem::blood offset");
static_assert(offsetof(MedicalSystem, extraBloodLossFromBodyparts) == 0x74, "MedicalSystem::extraBlood offset");
static_assert(offsetof(MedicalSystem, currentBleedRate) == 0x78, "MedicalSystem::currentBleedRate offset");
static_assert(offsetof(MedicalSystem, leftLeg) == 0x80, "MedicalSystem::leftLeg offset");
static_assert(offsetof(MedicalSystem, robotLimbs) == 0xC8, "MedicalSystem::robotLimbs offset");
static_assert(offsetof(MedicalSystem, me) == 0xE0, "MedicalSystem::me offset");
static_assert(offsetof(MedicalSystem, dead) == 0x164, "MedicalSystem::dead offset");
static_assert(offsetof(MedicalSystem, unconcious) == 0x161, "MedicalSystem::unconcious offset");
static_assert(offsetof(MedicalSystem, knockoutTimer) == 0xA0, "MedicalSystem::knockoutTimer offset");
static_assert(offsetof(MedicalSystem, anatomy) == 0x190, "MedicalSystem::anatomy offset");
static_assert(offsetof(MedicalSystem::HealthPartStatus, whatAmI) == 0x8, "HPS::whatAmI offset");
static_assert(offsetof(MedicalSystem::HealthPartStatus, me) == 0x18, "HPS::me offset");
static_assert(offsetof(MedicalSystem::HealthPartStatus, flesh) == 0x40, "HPS::flesh offset");
static_assert(offsetof(MedicalSystem::HealthPartStatus, _maxHealth) == 0x54, "HPS::_maxHealth offset");
static_assert(offsetof(MedicalSystem::HealthPartStatus, HPMult) == 0x5C, "HPS::HPMult offset");
static_assert(offsetof(MedicalSystem::HealthPartStatus, derivedFleshHealthPercent) == 0x60, "HPS::derivedFleshHealthPercent offset");
static_assert(offsetof(RobotLimbs, states) == 0x10, "RobotLimbs::states offset");
