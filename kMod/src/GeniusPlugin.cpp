#include "pch.h"
#include "../include/Logger.h"
#include "../include/RaceManager.h"
#include "../include/XPManager.h"
#include "../include/HealthManager.h"
#include "../include/RegenSystem.h"
#include "../include/LimbRecovery.h"
#include "../include/DeathGuard.h"
#include "../include/CharacterUtils.h"
#include <core/Functions.h>
#include <Debug.h>
#include <cstdio>
#include <mutex>
#include <set>

namespace {
	bool g_initialized = false;
	void (*g_origMedicalUpdate)(MedicalSystem*, float) = nullptr;
	bool g_tickLoggedOnce = false;
	bool g_geniusSeen = false;
	unsigned long long g_tickCount = 0;
	unsigned long long g_faultCount = 0;
	ULONGLONG g_faultLogTick = 0;
	std::mutex g_diagMutex;
	std::set<Character*> g_diagSeen;

	struct RaceSample
	{
		bool ok;
		uintptr_t race;
		uintptr_t data;
		int id;
		int type;
		uintptr_t stats;
		float toughness;
		bool gojoTag; // во window GameData+0x58..+0x98 нашлась подстрока "gojo"
		char sid[64]; // прочитанный stringID записи (если удалось)
		int tagResult; // GameDataHasGojoTag: 1 да, 0 нет, -1 недоступно
	};

	// POD-only сэмплирование под SEH.
	bool SampleRace(Character* c, RaceSample* out)
	{
		__try {
			if (!c || !out) return false;
			out->ok = false;
			out->race = reinterpret_cast<uintptr_t>(c->myRace);
			out->data = 0;
			out->id = -1;
			out->type = -1;
			out->stats = reinterpret_cast<uintptr_t>(c->stats);
			out->toughness = -1.0f;
			out->gojoTag = false;
			out->sid[0] = 0;
			out->tagResult = -1;
			if (c->myRace && c->myRace->data) {
				GameData* d = c->myRace->data;
				out->data = reinterpret_cast<uintptr_t>(d);
				out->id = d->id;
				out->type = static_cast<int>(d->type);
				// Ищем "gojo" в окрестности stringID без предположений
				// о раскладке std::string чужого компилятора.
				const char* base = reinterpret_cast<const char*>(d);
				const char needle[11] = { 'g', 'e', 'n', 'i', 'u', 's', 'r', 'a', 'c', 'e', 0 };
				for (int off = 0x40; off <= 0xA0 && !out->gojoTag; ++off) {
					bool hit = true;
					for (int k = 0; k < 10; ++k) {
						char ch = base[off + k];
						if (ch >= 'A' && ch <= 'Z') ch = (char)(ch + 32);
						if (ch != needle[k]) { hit = false; break; }
					}
					out->gojoTag = hit;
				}
			}
			if (c->stats)
				out->toughness = c->stats->_toughness;
			if (c->myRace && c->myRace->data) {
				GameDataAnyStringId(c->myRace->data, out->sid, sizeof(out->sid));
				out->tagResult = GameDataIsGeniusRecord(c->myRace->data) ? 1 : 0;
			}
			out->ok = true;
			return true;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	MedicalSystem* TryReadMedicalOwner(MedicalSystem* med, Character** outMe)
	{
		__try {
			if (!med || !outMe) return nullptr;
			*outMe = med->me;
			return med;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return nullptr;
		}
	}

	// POD-only: проверяет Genius и возвращает его sid для лога.
	bool TryGeniusSid(Character* c, char* sid, size_t cap)
	{
		__try {
			if (!c || !sid || cap == 0) return false;
			sid[0] = 0;
			if (!IsGeniusCharacter(c)) return false;
			if (!c->myRace || !c->myRace->data) return true; // Genius, но sid не читается
			GameDataStringId(c->myRace->data, sid, cap);
			return true;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	void DiagnoseCharacter(Character* c)
	{
		{
			std::lock_guard<std::mutex> lk(g_diagMutex);
			if (g_diagSeen.size() >= 1024) return;
			if (!g_diagSeen.insert(c).second) return;
		}
		RaceSample s{};
		if (!SampleRace(c, &s)) {
			return;
		}
	}

	// POD-only ядро тика под SEH: здесь нет объектов C++, только вызовы.
	// Возврат: 0 пустая/неполная медсистема, 1 чужой персонаж,
	// 2 Genius обработан, 3 сбой доступа (механики в этом кадре не применены).
	int SafeMedicalTick(MedicalSystem* med, float dt)
	{
		__try {
			if (!med) return 0;
			Character* c = med->me;
			if (!c) return 0;
			if (!IsGeniusCharacter(c)) return 1;
			HealthManager_ApplyTick(c, "tick");
			RegenSystem_ApplyTick(c, dt);
			LimbRecovery_ApplyTick(c);
			DeathGuard_ApplyTick(c, dt);
			return 2;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			// Именно этот путь раньше был «тихим»: реген пропадал, в логе — ничего.
			return 3;
		}
	}

	// Тик медицинской системы персонажа: движок вызывает его сам для
	// каждого загруженного персонажа, отдельный перебор списка не нужен.
	// Сначала штатная обработка (исцеление, состояния), затем механики
	// Genius — наши значения побеждают.
	void MedicalUpdateHook(MedicalSystem* med, float frameTime)
	{
		if (g_origMedicalUpdate)
			g_origMedicalUpdate(med, frameTime);
		if (!med) return;

		float dt = frameTime;
		if (!(dt > 0.0f)) dt = 0.0f;
		if (dt > 1.0f) dt = 1.0f;

		const int rc = SafeMedicalTick(med, dt);
		if (rc == 3) {
			// Сбой доступа к движковым структурам: реген/HP в этом кадре
			// НЕ применились. Пишем не чаще раза в 5 c, но пишем обязательно.
			++g_faultCount;
			const ULONGLONG now = GetTickCount64();
			if (now - g_faultLogTick >= 5000) {
				g_faultLogTick = now;
				Logger::Warn("Genius: medical tick fault (total=" +
					std::to_string(g_faultCount) + ") - regen/HP skipped this frame");
			}
			return;
		}
		if (rc == 0) return; // пустая/неполная медсистема — это норма
		++g_tickCount;
		if (rc == 2 && !g_geniusSeen) {
			g_geniusSeen = true;
			Character* who = nullptr;
			char sid[64] = {};
			if (TryReadMedicalOwner(med, &who) && who &&
				TryGeniusSid(who, sid, sizeof(sid)))
				Logger::Info(std::string("Genius character CONFIRMED active: sid=") +
					(sid[0] ? sid : "?"));
			else
				Logger::Info("Genius character CONFIRMED active (sid unreadable)");
		}
		if (rc == 1) {
			Character* who = nullptr;
			if (TryReadMedicalOwner(med, &who) && who)
				DiagnoseCharacter(who); // чужой: однократно логируем его расу
		}
		if (!g_tickLoggedOnce && g_tickCount >= 600) {
			g_tickLoggedOnce = true;
		}
	}

	bool InstallMedicalTickHook()
	{
		const intptr_t addr = KenshiLib::GetRealAddress(&MedicalSystem::medicalUpdate);
		if (!addr) {
			return false;
		}
		if (KenshiLib::AddHook(addr, (void*)&MedicalUpdateHook, &g_origMedicalUpdate) != KenshiLib::SUCCESS) {
			return false;
		}
		return true;
	}

	void PluginInitImpl()
	{
		// Лог-файл открываем ПЕРВЫМ: без него все Info/Warn/Error уходят
		// в никуда, и GeniusPlugin.log вообще не создаётся.
		Logger::Init();

		// Первое сообщение — в штатный лог RE_Kenshi через char* (без
		// std::string через границу DLL), оно видно всегда.
		DebugLog("Genius: startPlugin entry point reached");

		Logger::Info("Genius plugin starting (KenshiLib, target Kenshi 1.0.65)");
		Logger::Info("Genius build tag: 2026-10-18K9 limb-restore-at-shown-hp");

		InitializeGeniusRaceBinding();

		const bool xp = XPManager_InstallHooks();
		const bool tick = InstallMedicalTickHook();
		const bool dmg = DeathGuard_InstallHook();
		const bool hp = HealthManager_InstallHook();

		// Раньше результат установки хуков нигде не логировался, и провал
		// (например, устаревший RVA в KenshiLib) выглядел как «всё онлайн».
		Logger::Info(std::string("Genius plugin: hook status XP=") + (xp ? "ok" : "FAIL") +
			" medicalTick=" + (tick ? "ok" : "FAIL") +
			" deathGuard=" + (dmg ? "ok" : "FAIL") +
			" healthManager=" + (hp ? "ok" : "FAIL"));
		if (!tick || !hp)
			Logger::Warn("Genius plugin: HP/regen system degraded - see per-hook warnings above");

		char hpMult[32] = {};
		char regen[32] = {};
		sprintf_s(hpMult, "%.3g", GeniusHpPerToughness);
		sprintf_s(regen, "%.3g", GeniusRegenPerSecond * 100.0f);
		Logger::Info(std::string("Genius plugin: all systems online (HP=Toughness*") + hpMult +
			", regen=" + regen + "% MaxHP/s in any state, limb restore, death guard)");
		Logger::Info("Genius: log file at " + Logger::LogFilePath());
	}
}

// Каноническая точка входа RE_Kenshi-плагина (как в официальных примерах).
__declspec(dllexport) void startPlugin()
{
	if (g_initialized) return;
	g_initialized = true;
	PluginInitImpl();
}
