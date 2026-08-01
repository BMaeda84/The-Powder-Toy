#include "tpt-rand.h"
#include <cstdlib>
#include <ctime>

/* xoroshiro128+ by David Blackman and Sebastiano Vigna */

static inline uint64_t rotl(const uint64_t x, int k)
{
	return (x << k) | (x >> (64 - k));
}

uint64_t RNG::next()
{
	const uint64_t s0 = s[0];
	uint64_t s1 = s[1];
	const uint64_t result = s0 + s1;

	s1 ^= s0;
	s[0] = rotl(s0, 55) ^ s1 ^ (s1 << 14); // a, b
	s[1] = rotl(s1, 36); // c

	return result;
}

unsigned int RNG::gen()
{
	return next() & 0x7FFFFFFF;
}

unsigned int RNG::operator()()
{
	return next()&0xFFFFFFFF;
}

int RNG::between(int lower, int upper)
{
	unsigned int r = next();
	return static_cast<int>(r % ((unsigned int)(upper) - (unsigned int)(lower) + 1U)) + lower;
}

bool RNG::chance(int numerator, unsigned int denominator)
{
	if (numerator < 0)
		return false;
	return next() % denominator < static_cast<unsigned int>(numerator);
}

float RNG::uniform01()
{
	return float((next() & UINT32_C(0xFFFFFFFF)) / float(UINT32_C(0xFFFFFFFF)));
}

double RNG::uniform01Double()
{
	return double(next() / double(UINT64_C(0xFFFFFFFFFFFFFFFF)));
}

RNG::RNG()
{
	// * O padrao semeia pelo relogio, o que faz duas execucoes da mesma cena divergirem e
	//   deixa qualquer regressao de fisica indetectavel: nao ha com o que comparar.
	//   TPT_RNG_SEED fixa a semente para sessoes reprodutiveis (medicao, teste, checksum).
	//   Resolvido uma unica vez; o valor vale para todo RNG construido depois, que e
	//   exatamente o que uma sessao determinista precisa. Zero mantem o comportamento
	//   original, entao jogo normal nao muda.
	static const unsigned long long fixedSeed = []() -> unsigned long long {
		if (auto *env = std::getenv("TPT_RNG_SEED"))
		{
			return std::strtoull(env, nullptr, 10);
		}
		return 0;
	}();
	s[0] = fixedSeed ? fixedSeed : (unsigned long long)time(nullptr);
	s[1] = 614;
}

void RNG::seed(unsigned int sd)
{
	s[0] = sd;
	s[1] = sd;
}

RNG interfaceRng;
