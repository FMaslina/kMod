#include "pch.h"
#include "../include/CharacterUtils.h"
#include "../include/Logger.h"

bool IsGeniusCharacter(Character* character)
{
	if (!character) return false;
	__try {
		RaceData* race = character->myRace;
		if (!race) return false;
		return GameDataIsGeniusRecord(race->data);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

float GetGeniusToughness(Character* character)
{
	if (!character) return 0.0f;
	__try {
		CharStats* stats = character->stats;
		if (!stats) return 0.0f;
		const float t = stats->_toughness;
		if (!(t > 0.0f)) return 0.0f; // 0, отрицательное и NaN
		if (t > 1000.0f) return 1000.0f;
		return t;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return 0.0f;
	}
}

int GeniusTargetMaxHP(float toughness)
{
	// Умножаем в float и округляем, коэффициент НЕ приводим к int:
	// (int)1.5f == 1, из-за этого множитель вырождался в Toughness x1.
	float t = toughness;
	if (!(t > 0.0f)) t = 1.0f; // 0, отрицательное и NaN
	if (t > 1000.0f) t = 1000.0f;
	int hp = static_cast<int>(t * GeniusHpPerToughness + 0.5f);
	if (hp < 3) hp = 3;
	return hp;
}

// --- Чтение stringID записи (VC10-раскладка std::string неизвестна точно,
// поэтому пробуем inline-байты и оба кандидата heap-указателя, каждый
// вариант валидируем содержимым; имя/внешность персонажа не используются,
// только служебный stringID вида "<id>-GeniusRace.mod") ---

bool HasGojoSuffix(const char* s, size_t n)
{
	// Суффиксы модов-носителей расы Genius (без учёта регистра).
	// Несколько штук: сейв может хранить персонажа старой расы (gojo),
	// а новые персонажи создаются уже из GeniusRace.
	static const char* const needles[] = { "-GeniusRace.mod", "-gojo.mod" };
	for (size_t ni = 0; ni < sizeof(needles) / sizeof(needles[0]); ++ni) {
		const char* needle = needles[ni];
		size_t m = 0;
		while (needle[m]) ++m;
		if (n < m) continue;
		for (size_t i = 0; i + m <= n; ++i) {
			bool hit = true;
			for (size_t k = 0; k < m; ++k) {
				char a = s[i + k], b = needle[k];
				if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
				if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
				if (a != b) { hit = false; break; }
			}
			if (hit) return true;
		}
	}
	return false;
}

bool ReadableText(const char* s, size_t n)
{
	if (!s || n == 0 || n > 128) return false;
	for (size_t i = 0; i < n; ++i) {
		const unsigned char c = static_cast<unsigned char>(s[i]);
		if (c == 0) return true; // конец раньше заявленной длины — нормально
		if (c < 32 || c > 126) return false;
	}
	return true;
}

// Копирует до 127 байт по возможно битому указателю; false при сбое.
bool ProbeCopy(char* dst, const void* src, size_t n)
{
	__try {
		if (!dst || !src || n == 0 || n > 128) return false;
		const volatile char* p = static_cast<const volatile char*>(src);
		for (size_t i = 0; i < n; ++i) dst[i] = p[i];
		dst[n - 1] = 0;
		return true;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

bool GameDataHasGojoTag(GameData* d)
{
	__try {
		if (!d) return false;
		const char* base = reinterpret_cast<const char*>(d);
		// stringID живёт около 0x58; сканируем окно байтов как есть.
		char window[96];
		if (!ProbeCopy(window, base + 0x40, sizeof(window) - 1)) return false;
		if (HasGojoSuffix(window, sizeof(window) - 1)) return true;
		// Кандидаты heap-строки VC10: _Bx по +0x58 или +0x60,
		// размер/резерв следом (+16/+24 от _Bx).
		const size_t candOff[] = { 0x58, 0x60 };
		for (size_t ci = 0; ci < 2; ++ci) {
			uintptr_t ptr = 0;
			size_t sz = 0, rs = 0;
			__try {
				ptr = *reinterpret_cast<uintptr_t const*>(base + candOff[ci]);
				sz = *reinterpret_cast<size_t const*>(base + candOff[ci] + 16);
				rs = *reinterpret_cast<size_t const*>(base + candOff[ci] + 24);
			} __except (EXCEPTION_EXECUTE_HANDLER) { continue; }
			if (sz == 0 || sz > 64 || rs < sz || rs > 512) continue;
			if (ptr < 0x10000) continue;
			char buf[64];
			if (!ProbeCopy(buf, reinterpret_cast<const void*>(ptr), sz + 1)) continue;
			if (!ReadableText(buf, sz)) continue;
			if (HasGojoSuffix(buf, sz)) return true;
		}
		return false;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

// Вернуть сам stringID для лога (пусто при недоступности).
// Строгий вариант: только записи мода-носителя (по суффиксу).
bool GameDataStringId(GameData* d, char* out, size_t cap)
{
	__try {
		if (!d || !out || cap < 16) return false;
		out[0] = 0;
		const char* base = reinterpret_cast<const char*>(d);
		char window[96];
		if (ProbeCopy(window, base + 0x40, sizeof(window) - 1)) {
			// Ищем начало токена "<число>-GeniusRace.mod"/"-gojo.mod" в окне.
			for (size_t i = 0; i + 9 < sizeof(window) - 1; ++i) {
				if (HasGojoSuffix(window + i, (sizeof(window) - 1) - i)) {
					size_t s = i;
					while (s > 0 && window[s - 1] != 0) --s;
					size_t k = 0;
					while (k + 1 < cap && window[s + k]) { out[k] = window[s + k]; ++k; }
					out[k] = 0;
					return true;
				}
			}
		}
		const size_t candOff[] = { 0x58, 0x60 };
		for (size_t ci = 0; ci < 2; ++ci) {
			uintptr_t ptr = 0;
			size_t sz = 0, rs = 0;
			__try {
				ptr = *reinterpret_cast<uintptr_t const*>(base + candOff[ci]);
				sz = *reinterpret_cast<size_t const*>(base + candOff[ci] + 16);
				rs = *reinterpret_cast<size_t const*>(base + candOff[ci] + 24);
			} __except (EXCEPTION_EXECUTE_HANDLER) { continue; }
			if (sz == 0 || sz > 64 || rs < sz || rs > 512) continue;
			if (ptr < 0x10000) continue;
			char buf[64];
			if (!ProbeCopy(buf, reinterpret_cast<const void*>(ptr), sz + 1)) continue;
			if (!ReadableText(buf, sz)) continue;
			if (HasGojoSuffix(buf, sz)) {
				size_t k = 0;
				while (k + 1 < cap && k < sz && buf[k]) { out[k] = buf[k]; ++k; }
				out[k] = 0;
				return true;
			}
		}
		return false;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

// Свободный вариант для диагностики: любой похожий на stringID токен
// (печатный, с дефисом). Не используется для опознания, только для лога.
bool GameDataAnyStringId(GameData* d, char* out, size_t cap)
{
	__try {
		if (!d || !out || cap < 16) return false;
		out[0] = 0;
		const char* base = reinterpret_cast<const char*>(d);
		const size_t candOff[] = { 0x58, 0x60 };
		for (size_t ci = 0; ci < 2; ++ci) {
			uintptr_t ptr = 0;
			size_t sz = 0, rs = 0;
			__try {
				ptr = *reinterpret_cast<uintptr_t const*>(base + candOff[ci]);
				sz = *reinterpret_cast<size_t const*>(base + candOff[ci] + 16);
				rs = *reinterpret_cast<size_t const*>(base + candOff[ci] + 24);
			} __except (EXCEPTION_EXECUTE_HANDLER) { continue; }
			if (sz < 4 || sz > 64 || rs < sz || rs > 512) continue;
			if (ptr < 0x10000) continue;
			char buf[64];
			if (!ProbeCopy(buf, reinterpret_cast<const void*>(ptr), sz + 1)) continue;
			if (!ReadableText(buf, sz)) continue;
			bool dash = false;
			for (size_t k = 0; k < sz && buf[k]; ++k) if (buf[k] == '-') { dash = true; break; }
			if (!dash) continue;
			size_t k = 0;
			while (k + 1 < cap && k < sz && buf[k]) { out[k] = buf[k]; ++k; }
			out[k] = 0;
			return true;
		}
		return false;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

// Единая проверка записи расы: числовой ID либо суффикс stringID,
// прочитанного единственным рабочим способом.
bool GameDataIsGeniusRecord(GameData* d)
{
	__try {
		if (!d) return false;
		if (d->type != RACE) return false;
		if (d->id == GeniusRaceDataId) return true;
		char sid[64] = {};
		if (!GameDataAnyStringId(d, sid, sizeof(sid))) return false;
		size_t n = 0;
		while (n < sizeof(sid) && sid[n]) ++n;
		return HasGojoSuffix(sid, n);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

bool SaneBodyPart(Character* character, MedicalSystem::HealthPartStatus* part)
{
	__try {
		if (!character || !part) return false;
		if (part->me != character) return false; // чужой/висячий указатель
		const int w = static_cast<int>(part->whatAmI);
		if (w < 0 || w > 3) return false;
		if (!(part->_maxHealth > 0.0f) || part->_maxHealth > 1000000.0f) return false;
		if (!(part->flesh > -1000000.0f) || !(part->flesh < 1000000.0f)) return false;
		return true;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

int CollectBodyParts(Character* character, MedicalSystem::HealthPartStatus** out, int capacity)
{
	if (!character || !out || capacity <= 0) return 0;
	int n = 0;
	__try {
		MedicalSystem* med = &character->medical;

		// Прямые указатели конечностей.
		MedicalSystem::HealthPartStatus* direct[4] = {
			med->leftLeg, med->rightLeg, med->leftArm, med->rightArm
		};

		// Записи anatomy с проверкой вменяемости счётчика.
		uint32_t count = med->anatomy.count;
		MedicalSystem::HealthPartStatus** stuff = med->anatomy.stuff;
		if (count > 64) count = 0; // мусор вместо счётчика — не итерируем
		if (count > 0 && !stuff) count = 0;

		for (uint32_t i = 0; i < count && n < capacity; ++i) {
			MedicalSystem::HealthPartStatus* p = nullptr;
			__try { p = stuff[i]; } __except (EXCEPTION_EXECUTE_HANDLER) { break; }
			if (!p) continue;
			if (!SaneBodyPart(character, p)) continue;
			bool dup = false;
			for (int k = 0; k < n; ++k) if (out[k] == p) { dup = true; break; }
			if (!dup) out[n++] = p;
		}
		for (int i = 0; i < 4 && n < capacity; ++i) {
			MedicalSystem::HealthPartStatus* p = nullptr;
			__try { p = direct[i]; } __except (EXCEPTION_EXECUTE_HANDLER) { continue; }
			if (!p) continue;
			if (!SaneBodyPart(character, p)) continue;
			bool dup = false;
			for (int k = 0; k < n; ++k) if (out[k] == p) { dup = true; break; }
			if (!dup) out[n++] = p;
		}
	} __except (EXCEPTION_EXECUTE_HANDLER) {
	}
	return n;
}
