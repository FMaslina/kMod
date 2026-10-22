# Сборка и установка Genius для Kenshi 1.0.65

## Сборка

Из корня решения (`kMod.slnx`, Visual Studio 2026) выберите `Release|x64`,
либо в PowerShell из папки решения:

```powershell
& 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe' .\kMod.slnx /m /p:Configuration=Release /p:Platform=x64
```

Ожидаемый артефакт: `x64\Release\kMod.dll` (экспорт только `startPlugin`).
Зависимости подхватываются автоматически:
`third_party\KenshiModding\KenshiLib_v0.5.1\KenshiLib.lib`,
заголовки KenshiLib из `C:\Users\Owner\Desktop\KenshiModding\KenshiLib-RE_Kenshi_mods\Include`
(пути переопределяются переменными `KenshiLibSdkRoot` / `KenshiLibReleaseRoot`).
Boost и тяжёлые заголовки Ogre/MyGUI не используются: доступ к памяти игры
идёт через точные зеркала структур (`kMod\include\KenshiMirror.h`,
смещения сверены с SDK и зафиксированы `static_assert`).

## Установка

1. Steam/GOG Kenshi **1.0.65** с установленными RE_Kenshi и KenshiLib.
2. Создайте `<каталог Kenshi>\mods\Genius`.
3. Скопируйте `x64\Release\kMod.dll` и `kMod\RE_Kenshi.json` в `mods\Genius`.
4. В лаунчере RE_Kenshi включите Genius; в списке модов игры включите `gojo.mod`.
5. Создавайте персонажа расы Gojo (GameData ID 5026007) — к нему применятся
   механики Genius.

## Проверка

- `%TEMP%\Genius_kMod_loaded.txt` — DLL загружена Windows.
- `RE_Kenshi_log.txt` в корне игры — строки `Genius: ...`
  (`all systems online` при успехе).
- `GeniusPlugin.log` (каталог игры → `mods\Genius\` → `%TEMP%`).
