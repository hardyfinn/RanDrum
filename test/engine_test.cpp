// Tests for the Rattlebag engine. Build and run:
//   c++ -std=c++11 -O2 -o test/engine_test test/engine_test.cpp && test/engine_test
#include "../src/DrumEngine.hpp"
#include <cstdio>
#include <cstdlib>
#include <set>
using namespace rb;

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static Bank makeBank(int n, int frames = 4800, int rate = 48000) {
	Bank b;
	for (int i = 0; i < n; i++) {
		Sample s;
		s.name = "s" + std::to_string(i);
		s.rate = rate;
		s.L.assign(frames, 0.5f);
		s.computePeaks();
		b.samples.push_back(s);
	}
	return b;
}

static void writeTestWav(const char* path, int bits, int ch, int rate, int frames) {
	FILE* f = std::fopen(path, "wb");
	int bps = bits / 8;
	uint32_t dataLen = frames * ch * bps;
	auto w32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
	auto w16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
	std::fwrite("RIFF", 1, 4, f); w32(36 + dataLen); std::fwrite("WAVE", 1, 4, f);
	std::fwrite("fmt ", 1, 4, f); w32(16); w16(1); w16(ch); w32(rate); w32(rate * ch * bps); w16(ch * bps); w16(bits);
	std::fwrite("data", 1, 4, f); w32(dataLen);
	for (int i = 0; i < frames * ch; i++) {
		int v = (i % 2) ? 1000 : -1000;
		if (bits == 16) w16((uint16_t) (int16_t) v);
		else { int32_t x = v << 8; std::fwrite(&x, 3, 1, f); }
	}
	std::fclose(f);
}

int main() {
	const float SR = 48000.f;
	// 1. spread 0 always picks the SAMPLE knob's sample
	{
		Bank b = makeBank(8);
		DrumEngine e;
		Controls c; c.spread = 0.f; c.sample = 3.f / 7.f;
		for (int i = 0; i < 50; i++) CHECK(e.trigger(c, &b, SR).index == 3);
	}
	// 2. spread 1 in free mode reaches every sample
	{
		Bank b = makeBank(8);
		DrumEngine e;
		Controls c; c.spread = 1.f; c.loop = 0;
		std::set<int> seen;
		for (int i = 0; i < 400; i++) seen.insert(e.trigger(c, &b, SR).index);
		CHECK(seen.size() == 8);
	}
	// 3. a locked loop repeats exactly
	{
		Bank b = makeBank(16);
		DrumEngine e;
		Controls c; c.loop = 5; c.mutate = 0.f; c.pitchRnd = 1.f;
		std::vector<int> idx; std::vector<float> pit;
		for (int i = 0; i < 20; i++) { const HitInfo& h = e.trigger(c, &b, SR); idx.push_back(h.index); pit.push_back(h.semis); }
		for (int i = 5; i < 20; i++) { CHECK(idx[i] == idx[i - 5]); CHECK(pit[i] == pit[i - 5]); }
		bool differs = false;
		for (int i = 1; i < 5; i++) differs |= idx[i] != idx[0];
		CHECK(differs);
	}
	// 4. mutate 1 makes the loop change
	{
		Bank b = makeBank(16);
		DrumEngine e;
		Controls c; c.loop = 4; c.mutate = 1.f;
		int same = 0;
		std::vector<int> idx;
		for (int i = 0; i < 400; i++) idx.push_back(e.trigger(c, &b, SR).index);
		for (int i = 4; i < 400; i++) same += idx[i] == idx[i - 4];
		CHECK(same < 100);
	}
	// 5. chance 0 never plays, chance 1 always plays; about half at 0.5
	{
		Bank b = makeBank(4);
		DrumEngine e;
		Controls c; c.chance = 0.f;
		for (int i = 0; i < 50; i++) CHECK(!e.trigger(c, &b, SR).played);
		c.chance = 1.f;
		for (int i = 0; i < 50; i++) CHECK(e.trigger(c, &b, SR).played);
		c.chance = 0.5f;
		int n = 0;
		for (int i = 0; i < 2000; i++) n += e.trigger(c, &b, SR).played;
		CHECK(n > 850 && n < 1150);
	}
	// 6. walk moves by at most one and stays in the window
	{
		Bank b = makeBank(20);
		DrumEngine e;
		Controls c; c.mode = PICK_WALK; c.sample = 0.5f; c.spread = 0.3f;
		int prev = e.trigger(c, &b, SR).index;
		int lo, size;
		DrumEngine::window(c, 20, lo, size);
		for (int i = 0; i < 300; i++) {
			int k = e.trigger(c, &b, SR).index;
			CHECK(std::abs(k - prev) <= 1);
			CHECK(k >= lo && k < lo + size);
			prev = k;
		}
	}
	// 7. cycle goes round the window in order
	{
		Bank b = makeBank(10);
		DrumEngine e;
		Controls c; c.mode = PICK_CYCLE; c.sample = 0.f; c.spread = 0.f;
		c.spread = 3.f / 9.f;  // window of 4 around sample 0: -1..2 -> 9,0,1,2
		int lo, size; DrumEngine::window(c, 10, lo, size);
		CHECK(size == 4);
		std::vector<int> got;
		for (int i = 0; i < 8; i++) got.push_back(e.trigger(c, &b, SR).index);
		for (int i = 0; i < 8; i++) CHECK(got[i] == DrumEngine::wrap(lo + i % 4, 10));
	}
	// 8. playback: plays, decays, stops; output level 0.5 at centre pan
	{
		Bank b = makeBank(1, 48000);
		DrumEngine e;
		Controls c; c.decay = 0.3f;  // 0.01*400^0.3 = ~60 ms
		const HitInfo& h = e.trigger(c, &b, SR);
		CHECK(h.played && h.decaySec > 0.05f && h.decaySec < 0.07f);
		float L, R, M;
		e.process(SR, L, R, M);
		CHECK(std::fabs(L - 0.5f) < 1e-3f && std::fabs(R - 0.5f) < 1e-3f && std::fabs(M - 0.5f) < 1e-3f);
		for (int i = 0; i < 48000 * 0.1; i++) e.process(SR, L, R, M);
		CHECK(e.activeVoices() == 0);
	}
	// 9. full decay plays the whole sample, at double speed in half the time
	{
		Bank b = makeBank(1, 4800);
		DrumEngine e;
		Controls c; c.decay = 1.f; c.pitch = 12.f;
		e.trigger(c, &b, SR);
		float L, R, M;
		int n = 0;
		while (e.activeVoices() && n < 100000) { e.process(SR, L, R, M); n++; }
		CHECK(n >= 2398 && n <= 2402);
	}
	// 10. choke fades the old voice out within a few ms
	{
		Bank b = makeBank(1, 48000);
		DrumEngine e;
		Controls c; c.choke = true;
		e.trigger(c, &b, SR);
		float L, R, M;
		for (int i = 0; i < 100; i++) e.process(SR, L, R, M);
		e.trigger(c, &b, SR);
		for (int i = 0; i < 200; i++) e.process(SR, L, R, M);
		CHECK(e.activeVoices() == 1);
	}
	// 11. reverse and pan
	{
		Bank b = makeBank(1, 4800);
		DrumEngine e;
		Controls c; c.reverse = 1.f; c.pan = -1.f;
		const HitInfo& h = e.trigger(c, &b, SR);
		CHECK(h.reverse);
		float L, R, M;
		for (int i = 0; i < 100; i++) e.process(SR, L, R, M);
		CHECK(L > 0.6f && std::fabs(R) < 1e-4f);
	}
	// 12. empty bank does nothing
	{
		Bank b;
		DrumEngine e;
		Controls c;
		CHECK(!e.trigger(c, &b, SR).played);
		CHECK(!e.trigger(c, nullptr, SR).played);
	}
	// 13. WAV reading: 16-bit stereo and 24-bit mono at 44.1k
	{
		writeTestWav("/tmp/rb_t16.wav", 16, 2, 44100, 1000);
		Sample s;
		CHECK(readWav("/tmp/rb_t16.wav", s) == "");
		CHECK(s.frames() == 1000 && s.R.size() == 1000 && s.rate == 44100);
		CHECK(std::fabs(s.L[0] + 1000 / 32768.f) < 1e-6f && std::fabs(s.R[0] - 1000 / 32768.f) < 1e-6f);
		writeTestWav("/tmp/rb_t24.wav", 24, 1, 44100, 500);
		CHECK(readWav("/tmp/rb_t24.wav", s) == "");
		CHECK(s.frames() == 500 && s.R.empty());
		CHECK(std::fabs(s.L[0] + 1000 / 32768.f) < 1e-5f);
		CHECK(readWav("/tmp/does-not-exist.wav", s) != "");
	}
	// 14. 44.1 kHz file at 48 kHz plays at the right speed
	{
		Bank b = makeBank(1, 4410, 44100);
		DrumEngine e;
		Controls c;
		e.trigger(c, &b, SR);
		float L, R, M;
		int n = 0;
		while (e.activeVoices() && n < 100000) { e.process(SR, L, R, M); n++; }
		CHECK(n >= 4798 && n <= 4802);
	}
	std::printf(fails ? "%d FAILED\n" : "all tests passed\n", fails);
	return fails ? 1 : 0;
}
