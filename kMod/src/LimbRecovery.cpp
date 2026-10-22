#include "pch.h"
#include "../include/LimbRecovery.h"
#include "../include/Logger.h"
#include <mutex>
#include <map>
#include <array>

namespace {
	std::mutex g_mutex;
	// Трекинг утраты по указателю персонажа: индекс = RobotLimbs::Limb.
	// Флаг нужен только для логов переходов; само восстановление stateless:
	// культя с HP>=1 чинится независимо от того, видели ли мы момент утраты
	// (иначе конечность, потерянная до загрузки DLL/сейва, не чинится никогда).
	std::map<Character*, std::array<bool, 4>> g_lost;
	// Последний ретрай восстановления: не чаще раза в 30 c на конечность.
	std::map<std::pair<Character*, int>, ULONGLONG> g_lastRetry;
	// Последний отчёт о культе (прогресс HP): не чаще раза в 10 c.
	std::map<std::pair<Character*, int>, ULONGLONG> g_lastReport;

	std::string Fmt(float v)
	{
		char b[32] = {};
		sprintf_s(b, "%.1f", v);
		return std::string(b);
	}

	const char* LimbName(int limb)
	{
		switch (limb) {
		case RobotLimbs::LEFT_ARM: return "left arm";
		case RobotLimbs::RIGHT_ARM: return "right arm";
		case RobotLimbs::LEFT_LEG: return "left leg";
		case RobotLimbs::RIGHT_LEG: return "right leg";
		default: return "unknown";
		}
	}

	// --- POD-only сэмплирование под SEH (без объектов C++ внутри) ---

	// kind: 0 не конечность, 1 рука, 2 нога; limb: RobotLimbs::Limb/-1.
	bool TrySamplePart(MedicalSystem::HealthPartStatus* part, int* kind, int* limb, float* flesh,
		float* maxHp, float* stun)
	{
		__try {
			if (!part || !kind || !limb || !flesh) return false;
			if (part->isRobotic()) return false;
			typedef MedicalSystem::HealthPartStatus HPS;
			if (part->whatAmI == HPS::PART_ARM) *kind = 1;
			else if (part->whatAmI == HPS::PART_LEG) *kind = 2;
			else { *kind = 0; return true; }
			*limb = static_cast<int>(part->getRobotLimbEnum());
			*flesh = part->flesh;
			if (maxHp) *maxHp = part->_maxHealth;
			if (stun) *stun = part->fleshStun;
			return true;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	// HP конечности: движок при отсечении ставит «запредельное» значение
	// (1 - MaxHP, напр. -25). Возвращаем её к GeniusLimbRestoreHp (+1) —
	// иначе игрок видит вернувшуюся конечность с HP -25 и лечит её заново.
	bool TryWriteFlesh(MedicalSystem::HealthPartStatus* part, float value, float* outBefore)
	{
		__try {
			if (!part) return false;
			if (outBefore) *outBefore = part->flesh;
			part->flesh = value;
			if (part->flesh > part->_maxHealth) part->flesh = part->_maxHealth;
			return true;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	// Панель медицины показывает не flesh, а «эффективный» HP: flesh - fleshStun
	// (проверено по двум независимым случаям: 1.0-25.96=-25, 0.8-17.78=-17).
	// Поэтому возвращённой конечности мало поднять flesh — надо снять и стан,
	// иначе игрок видит вернувшуюся конечность с отрицательным HP.
	bool TryWriteStunZero(MedicalSystem::HealthPartStatus* part, float* outBefore)
	{
		__try {
			if (!part) return false;
			if (outBefore) *outBefore = part->fleshStun;
			part->fleshStun = 0.0f;
			return true;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	// Throttle отчётов о культе: не чаще раза в 10 c на пару
	// персонаж+конечность (культя — редкое состояние, спама не будет).
	bool ShouldReport(Character* character, int limb)
	{
		const ULONGLONG now = GetTickCount64();
		auto key = std::make_pair(character, limb);
		auto it = g_lastReport.find(key);
		if (it != g_lastReport.end() && now - it->second < 10000)
			return false;
		g_lastReport[key] = now;
		return true;
	}

	// Состояние конечности в robotLimbs; -1 при недоступности.
	int TryReadLimbState(MedicalSystem* med, int limb)
	{
		__try {
			if (!med || !med->robotLimbs) return -1;
			if (limb < RobotLimbs::LEFT_ARM || limb > RobotLimbs::RIGHT_LEG) return -1;
			return static_cast<int>(med->robotLimbs->getState(static_cast<RobotLimbs::Limb>(limb)));
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return -1;
		}
	}

	// Возврат культи в исходное состояние; false при сбое/протезе.
	// После setLimb вызываем пересчёт состояний движка, иначе игра
	// продолжает считать конечность утраченной (флаги crippled/arm-ok,
	// валидация HP). Возврат: 0 сбой, 1 ок, 2 протез (не трогаем).
	int TryRestoreLimb(MedicalSystem* med, int limb)
	{
		__try {
			if (!med || !med->robotLimbs) return 0;
			if (limb < RobotLimbs::LEFT_ARM || limb > RobotLimbs::RIGHT_LEG) return 0;
			RobotLimbs::Limb l = static_cast<RobotLimbs::Limb>(limb);
			const int before = static_cast<int>(med->robotLimbs->getState(l));
			if (before == LIMB_REPLACED)
				return 2; // протез — не трогаем предметы (ТЗ 7.4)
			med->robotLimbs->setLimb(l, LIMB_ORIGINAL, nullptr);
			const int after = static_cast<int>(med->robotLimbs->getState(l));
			// Пересчёт состояния после возврата конечности.
			med->updateDamageState();
			med->validateHealthValues();
			// Пересборка рэгдолла с нуля: снимает коллапс культи и
			// перепривязывает визуал конечности.
			med->_reassessRagdollPartsAssumingWeJustClearedTheEntireRagdoll();
			return (after == LIMB_ORIGINAL) ? 1 : 0;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return 0;
		}
	}

	MedicalSystem* TryReadMedical(MedicalSystem::HealthPartStatus* part)
	{
		__try {
			if (!part) return nullptr;
			return part->medical;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return nullptr;
		}
	}

	bool ShouldRetry(Character* character, int limb)
	{
		const ULONGLONG now = GetTickCount64();
		auto key = std::make_pair(character, limb);
		auto it = g_lastRetry.find(key);
		if (it != g_lastRetry.end() && now - it->second < 30000)
			return false;
		g_lastRetry[key] = now;
		return true;
	}
}

void LimbRecovery_ApplyTick(Character* character)
{
	if (!IsGeniusCharacter(character)) return;

	MedicalSystem::HealthPartStatus* parts[16];
	const int n = CollectBodyParts(character, parts, 16);

	// Указатель MedicalSystem один на персонажа — читаем напрямую:
	// CollectBodyParts уже вернул только проверенные указатели частей,
	// medical берём из первой части (поле part->medical движка).
	for (int i = 0; i < n; ++i) {
		int kind = 0, limb = -1;
		float flesh = 0.0f, maxHp = 0.0f, stun = 0.0f;
		if (!TrySamplePart(parts[i], &kind, &limb, &flesh, &maxHp, &stun)) continue;
		if (kind == 0) continue;
		if (limb < RobotLimbs::LEFT_ARM || limb > RobotLimbs::RIGHT_LEG) continue;

		MedicalSystem* med = TryReadMedical(parts[i]);
		const int state = TryReadLimbState(med, limb);
		if (state < 0) continue; // robotLimbs недоступен

		std::lock_guard<std::mutex> lk(g_mutex);
		std::array<bool, 4>& lost = g_lost[character];
		const char* name = LimbName(limb);

		// Факт утраты — культя (движок отсекает конечность на «точке
		// невозврата» 1-MaxHP, а не на -100, поэтому флаг ставим по
		// состоянию конечности, а не по HP). Флаг нужен только для логов.
		if (state == LIMB_STUMP || state == LIMB_CRUSHED) {
			if (!lost[limb]) {
				lost[limb] = true;
				Logger::Info("LimbRecovery: " + std::string(name) + " lost (stump), flesh=" +
					Fmt(flesh) + " stun=" + Fmt(stun));
			}
			// Видно, доходит ли HP культи до порога и уходит ли стан
			// (панель показывает flesh - stun, поэтому важен и он).
			if (ShouldReport(character, limb)) {
				Logger::Info("LimbRecovery: " + std::string(name) + " stump flesh=" +
					Fmt(flesh) + "/" + Fmt(maxHp) + " stun=" + Fmt(stun) +
					" shown=" + Fmt(flesh - stun) + " (restore at >= " +
					Fmt(GeniusLimbRestoreHp) + ")");
			}
			// Порог восстановления: HP >= 1. Решение — по «видимому» HP
			// (flesh - fleshStun), который и показывает медпанель, а не по
			// одному flesh: при большом стане flesh уходит выше +1, пока
			// показанный HP ещё отрицательный (напр. flesh=1.0, stun=18 ->
			// показано -17). Раньше решение принималось по flesh, поэтому
			// конечность возвращалась при показанном -17 и «прыгала» на +1,
			// как только мы снимали стан. Решение — по ФАКТУ (культя +
			// заживший показанный HP), а не по флагу утраты: флаг живёт
			// только до перезапуска DLL, а культя из сейва — навсегда.
			const float shown = flesh - stun;
			if (shown >= GeniusLimbRestoreHp && ShouldRetry(character, limb)) {
				const int rc = TryRestoreLimb(med, limb);
				if (rc == 1) {
					// Возвращённая конечность сохраняет свой показанный HP:
					// панель считает HP как flesh - fleshStun, поэтому ставим
					// flesh = показанный HP и снимаем стан. Так как порог уже
					// достигнут, никакого скачка HP не происходит.
					float beforeFlesh = flesh, beforeStun = stun;
					TryWriteFlesh(parts[i], shown, &beforeFlesh);
					TryWriteStunZero(parts[i], &beforeStun);
					Logger::Info("LimbRecovery: " + std::string(name) + " restored, flesh " +
						Fmt(beforeFlesh) + " -> " + Fmt(shown) +
						", stun " + Fmt(beforeStun) + " -> 0.0 (shown=" +
						Fmt(shown) + ", state=original)");
					lost[limb] = false;
				}
			}
		} else if (state == LIMB_ORIGINAL) {
			if (lost[limb]) {
				// Движок вернул конечность сам (сейв/протез) — но HP мог
				// остаться «запредельным». Поднимаем до порога и снимаем
				// стан, чтобы конечность показывала +1, а не минус.
				float beforeFlesh = flesh, beforeStun = stun;
				if (flesh < GeniusLimbRestoreHp)
					TryWriteFlesh(parts[i], GeniusLimbRestoreHp, &beforeFlesh);
				TryWriteStunZero(parts[i], &beforeStun);
				Logger::Info("LimbRecovery: " + std::string(name) + " whole again, flesh " +
					Fmt(beforeFlesh) + " -> " +
					Fmt(beforeFlesh < GeniusLimbRestoreHp ? GeniusLimbRestoreHp : beforeFlesh) +
					", stun " + Fmt(beforeStun) + " -> 0.0");
				lost[limb] = false;
			}
		} else if (state == LIMB_REPLACED) {
			// Протез — не трогаем ни HP, ни предмет (ТЗ 7.4).
			if (lost[limb]) lost[limb] = false;
		}
	}
}
