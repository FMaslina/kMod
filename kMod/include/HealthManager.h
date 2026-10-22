#pragma once
#include "CharacterUtils.h" // тянет зеркала Kenshi и константы Genius

// HealthManager: MaxHP частей тела Genius = Toughness * 1.5 (баланс).
// Применение идемпотентно (безопасно вызывать каждый тик, состояние
// после сохранения/загрузки восстанавливается само).
// Два контура: тик medicalUpdate + хуки _NV_updateStats /
// HealthPartStatus::update / validateHealthValues (движок может
// пересчитывать _maxHealth из расовых данных поверх наших значений).
// Диагностика: хартбит раз в 10 c (путь вызова src=, ko/dead/кровь,
// flesh/_maxHealth по частям), сообщение о перезаписи _maxHealth движком
// и о сливе flesh движком — по ним виден момент, когда восстановление HP
// ломается. source: "tick" | "stats" | "validate".
bool HealthManager_InstallHook();
void HealthManager_ApplyTick(Character* character, const char* source);
