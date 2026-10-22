#pragma once
#include "CharacterUtils.h"

// DeathGuard: долив крови, страховка флага смерти, не-раздевание.
// Пол HP (minHP = 1 - MaxHP для головы/груди/живота) реализован, но по
// умолчанию ВЫКЛЮЧЕН (GeniusUseHpFloor = false): урон уходит в минус,
// живость держит только вето на смерть. Живот — тоже PART_TORSO.
// Основной перехват — хук HealthPartStatus::applyDamage (устанавливается
// в GeniusPlugin), ApplyTick — страховочная сетка на случай обходных путей
// (загрузка сохранения, прочий урон).
bool DeathGuard_InstallHook();
void DeathGuard_ApplyTick(Character* character, float dtSeconds);
