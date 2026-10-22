#pragma once

// Persistence: плагин deliberately не хранит собственного состояния.
// MaxHP выводится из текущего Toughness каждый тик, регенерация работает
// с живыми значениями flesh — после загрузки сохранения всё корректно
// без миграций и без риска бесконечного роста значений.
inline void SaveCharacterState(void*) {}
inline void LoadCharacterState(void*) {}
