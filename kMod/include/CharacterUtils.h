#pragma once
#include "KenshiMirror.h"

// Параметры расы Genius (ТЗ): отдельная выбираемая раса задаётся записью
// gojo.mod (RACE, GameData ID 5026007) — создать расу средствами RE_Kenshi
// невозможно, в KenshiLib нет API создания GameData-записей.
inline constexpr int GeniusRaceDataId = 5026007;
inline constexpr float GeniusXpMultiplier = 1.5f;
inline constexpr float GeniusHpPerToughness = 1.5f;
inline constexpr float GeniusRegenPerSecond = 0.001f; // 0.1% MaxHP в секунду
// Наш реген снимает и стан-урон (fleshStun, 0x44) с той же скоростью, что и
// flesh. Без этого после заполнения flesh остаётся «полоска» стана, которую
// снимает только движок (ванильно, ~в 7 раз медленнее), и персонаж лежит в
// ноке с полным HP. false = стан лечит только движок (как в ваниле).
inline constexpr bool GeniusHealStun = true;
// Пол HP (1-MaxHP для головы/груди/живота, -100 для конечностей).
// false = пола нет вообще: минус любой в зависимости от урона, смерть при
// этом всё равно заблокирована (вето dead, isDead=false, блок destroy).
inline constexpr bool GeniusUseHpFloor = false;
inline float GeniusMinHp(float maxHp) { return 1.0f - maxHp; }
inline constexpr float GeniusLimbLossHp = -100.0f;
inline constexpr float GeniusLimbRestoreHp = 1.0f;

bool IsGeniusCharacter(Character* character);

// Toughness персонажа (0, если недоступен).
float GetGeniusToughness(Character* character);

// Целевое максимальное HP части тела Genius по ТЗ: Toughness * 1.5
// (значение подобрано для баланса, см. GeniusHpPerToughness).
int GeniusTargetMaxHP(float toughness);

// Проверка stringID записи на принадлежность моду-носителю Genius.
bool GameDataHasGojoTag(GameData* d);

// Копия stringID для диагностики (false если недоступен).
bool GameDataStringId(GameData* d, char* out, size_t cap);

// Свободный stringID любой расы для диагностики (не для опознания).
bool GameDataAnyStringId(GameData* d, char* out, size_t cap);

// Единая проверка записи расы: числовой ID либо суффикс stringID.
// Используется и детектом, и диагностикой — один код, без расхождений.
bool GameDataIsGeniusRecord(GameData* d);

// Собрать уникальные статусы частей тела: записи anatomy + прямые
// указатели конечностей. Возвращает количество (0 при недоступности).
// Проверяет вменяемость счётчиков, каждый указатель — под SEH вызывающей.
int CollectBodyParts(Character* character, MedicalSystem::HealthPartStatus** out, int capacity);

// Проверка вменяемости записи части тела: указатель жив, принадлежит этому
// персонажу, тип и значения в разумных пределах. Защита от висячих
// указателей (напр. после ампутации): в мусор не пишем.
bool SaneBodyPart(Character* character, MedicalSystem::HealthPartStatus* part);
