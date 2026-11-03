// DrumEngine.hpp
// The sound engine of Rattlebag, with no VCV Rack dependencies so it can be
// tested on its own (see test/engine_test.cpp).
//
// How it works, in plain words:
// - A Bank is a folder of samples, kept at their own sample rate.
// - Every trigger uses one Slot: eight "dice", each a number from 0 to 1.
//   The dice decide which sample, pitch, decay, start, level, pan, reverse,
//   and whether the hit plays at all. The knobs then say how much each die
//   is allowed to change things, so turning a RND knob up or down rescales the
//   same rolls instead of rolling new ones.
// - With LOOP set to 1-16, the slots are kept in a ring and reused, so the
//   random pattern repeats. MUTATE is the chance, per die and per hit, that a
//   die is rolled again. MUTATE at zero locks the pattern; full re-rolls
//   everything every hit.
#pragma once
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>

namespace rb {

static const int PEAK_BINS = 128;
static const int MAX_LOOP = 16;
static const int NUM_VOICES = 12;

struct Sample {
	std::string name;
	std::vector<float> L, R;   // R is empty for mono files
	int rate = 48000;
	float peaks[PEAK_BINS] = {};
	int frames() const { return (int) L.size(); }

	void computePeaks() {
		int n = frames();
		for (int b = 0; b < PEAK_BINS; b++) {
			int s = (int) ((int64_t) n * b / PEAK_BINS), e = (int) ((int64_t) n * (b + 1) / PEAK_BINS);
			float m = 0.f;
			for (int i = s; i < e; i++) {
				m = std::max(m, std::fabs(L[i]));
				if (!R.empty()) m = std::max(m, std::fabs(R[i]));
			}
			peaks[b] = m;
		}
	}
};

struct Bank {
	std::string folder;
	std::vector<Sample> samples;
	int size() const { return (int) samples.size(); }
};

enum Die { D_CHANCE, D_SAMPLE, D_PITCH, D_DECAY, D_START, D_LEVEL, D_PAN, D_REV, NUM_DICE };

struct Slot {
	float d[NUM_DICE] = {};
};

enum PickMode { PICK_RANDOM, PICK_WALK, PICK_CYCLE };

// Everything the knobs and CV inputs say, already combined, for one hit.
struct Controls {
	float sample = 0.f;      // 0..1 across the folder
	float spread = 1.f;      // 0..1, how much of the folder the dice may pick from
	float pitch = 0.f;       // semitones (knob + V/OCT)
	float pitchRnd = 0.f;    // 0..1 -> up to +-12 semitones
	float decay = 1.f;       // 0..1, 1 = full sample
	float decayRnd = 0.f;    // 0..1 -> up to x8 or /8
	float start = 0.f;       // 0..1 of the sample
	float startRnd = 0.f;    // 0..1 -> up to half the sample later
	float chance = 1.f;      // 0..1
	float reverse = 0.f;     // 0..1 chance of playing backwards
	float level = 1.f;       // linear gain
	float levelRnd = 0.f;    // 0..1 -> up to -24 dB quieter
	float pan = 0.f;         // -1..1
	float panRnd = 0.f;      // 0..1
	float mutate = 0.f;      // 0..1
	int loop = 0;            // 0 = free (new dice every hit), 1..16
	PickMode mode = PICK_RANDOM;
	bool choke = false;
	bool quantise = false;
};

// What happened on the last trigger, for the display and the CV outputs.
struct HitInfo {
	bool played = false;
	int index = -1;
	int slot = -1;           // -1 when the loop is off
	float semis = 0.f;
	float decaySec = -1.f;   // < 0 = whole sample
	float start = 0.f;
	float gain = 1.f;
	float pan = 0.f;
	bool reverse = false;
	int windowLo = 0, windowSize = 1;
	uint32_t count = 0;      // increments on every trigger
};

struct Voice {
	bool active = false;
	const Sample* s = nullptr;
	double pos = 0.0, inc = 1.0;
	bool rev = false;
	float gL = 1.f, gR = 1.f, gMono = 1.f;
	bool useEnv = false;
	float env = 1.f, envMul = 1.f;
	float fadeIn = 1.f, fadeInStep = 1.f;
	float fadeOut = 1.f, fadeOutStep = 0.f;   // choke / steal
	uint32_t age = 0;
};

struct Rng {
	uint64_t s = 0x9E3779B97F4A7C15ull;
	void seed(uint64_t v) { s = v ? v : 0x9E3779B97F4A7C15ull; }
	uint64_t next() {
		s ^= s << 13;
		s ^= s >> 7;
		s ^= s << 17;
		return s;
	}
	float uniform() { return (next() >> 40) * (1.f / 16777216.f); }  // [0, 1)
};

inline float hermite(const std::vector<float>& x, double pos) {
	int n = (int) x.size();
	int i = (int) std::floor(pos);
	float t = (float) (pos - i);
	auto at = [&](int k) { return (k < 0 || k >= n) ? 0.f : x[k]; };
	float xm1 = at(i - 1), x0 = at(i), x1 = at(i + 1), x2 = at(i + 2);
	float c1 = 0.5f * (x1 - xm1);
	float c2 = xm1 - 2.5f * x0 + 2.f * x1 - 0.5f * x2;
	float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
	return ((c3 * t + c2) * t + c1) * t + x0;
}

struct DrumEngine {
	Slot ring[MAX_LOOP];
	int pos = -1;           // slot used by the last hit
	int walk = 0;           // WALK mode: offset inside the window
	uint32_t cycle = 0;     // CYCLE mode: counter
	Voice voices[NUM_VOICES];
	uint32_t ageCounter = 0;
	Rng rng;
	HitInfo last;
	float lastEnv = 0.f;    // envelope of the newest voice, for ENV out

	DrumEngine() { rerollAll(); }

	void rerollAll() {
		for (int i = 0; i < MAX_LOOP; i++)
			for (int k = 0; k < NUM_DICE; k++) ring[i].d[k] = rng.uniform();
	}

	void reset() {
		pos = -1;
		walk = 0;
		cycle = 0;
	}

	void stopAll() {
		for (Voice& v : voices) v.active = false;
	}

	// The slot for the next hit: a fresh roll, or the next one in the ring.
	Slot nextSlot(const Controls& c, int& slotIndex) {
		if (c.loop <= 0) {
			Slot s;
			for (int k = 0; k < NUM_DICE; k++) s.d[k] = rng.uniform();
			slotIndex = -1;
			return s;
		}
		int L = std::min(c.loop, MAX_LOOP);
		pos = (pos + 1) % L;
		if (pos < 0) pos = 0;
		for (int k = 0; k < NUM_DICE; k++)
			if (c.mutate > 0.f && rng.uniform() < c.mutate) ring[pos].d[k] = rng.uniform();
		slotIndex = pos;
		return ring[pos];
	}

	// Which samples the dice may pick from: SAMPLE is the middle of the
	// window, SPREAD its width.
	static void window(const Controls& c, int n, int& lo, int& size) {
		int center = (int) std::lround(std::min(std::max(c.sample, 0.f), 1.f) * (n - 1));
		size = 1 + (int) std::lround(std::min(std::max(c.spread, 0.f), 1.f) * (n - 1));
		lo = center - (size - 1) / 2;
	}

	static int wrap(int i, int n) { return ((i % n) + n) % n; }

	int pickIndex(const Controls& c, int n, float die, int& lo, int& size) {
		window(c, n, lo, size);
		switch (c.mode) {
			case PICK_WALK: {
				int step = die < 1.f / 3 ? -1 : (die < 2.f / 3 ? 0 : 1);
				walk += step;
				if (size <= 1) walk = 0;
				else {
					if (walk < 0) walk = 1;
					if (walk > size - 1) walk = size - 2;
				}
				walk = std::min(std::max(walk, 0), size - 1);
				return wrap(lo + walk, n);
			}
			case PICK_CYCLE: {
				int i = wrap(lo + (int) (cycle % (uint32_t) size), n);
				cycle++;
				return i;
			}
			default: {
				int off = std::min((int) (die * size), size - 1);
				return wrap(lo + off, n);
			}
		}
	}

	// Called on each trigger. Returns what happened.
	const HitInfo& trigger(const Controls& c, const Bank* bank, float engineRate) {
		HitInfo h;
		h.count = last.count + 1;
		int slotIndex;
		Slot s = nextSlot(c, slotIndex);
		h.slot = slotIndex;
		int n = bank ? bank->size() : 0;
		if (n == 0) {
			h.played = false;
			last = h;
			return last;
		}
		h.index = pickIndex(c, n, s.d[D_SAMPLE], h.windowLo, h.windowSize);
		h.played = s.d[D_CHANCE] < c.chance;
		const Sample& smp = bank->samples[h.index];

		// pitch
		float rnd = (s.d[D_PITCH] * 2.f - 1.f) * c.pitchRnd * 12.f;
		if (c.quantise) rnd = std::round(rnd);
		h.semis = std::min(std::max(c.pitch + rnd, -48.f), 48.f);
		double inc = (double) smp.rate / engineRate * std::pow(2.0, h.semis / 12.0);

		// start and reverse
		h.start = std::min(std::max(c.start + s.d[D_START] * c.startRnd * 0.5f, 0.f), 0.95f);
		h.reverse = s.d[D_REV] < c.reverse;

		// decay: 10 ms at CCW to 4 s, and the whole sample at full CW
		float factor = std::pow(2.f, (s.d[D_DECAY] * 2.f - 1.f) * c.decayRnd * 3.f);
		float playSec = (float) (smp.frames() * (1.0 - h.start) / (inc * engineRate));
		bool full = c.decay >= 0.99f;
		if (full) {
			h.decaySec = factor >= 1.f ? -1.f : playSec * factor;
		}
		else {
			float d = std::min(std::max(c.decay, 0.f), 1.f);
			h.decaySec = 0.01f * std::pow(400.f, d) * factor;
			if (h.decaySec >= playSec) h.decaySec = -1.f;
		}

		// level and pan
		h.gain = c.level * std::pow(10.f, -s.d[D_LEVEL] * c.levelRnd * 24.f / 20.f);
		h.pan = std::min(std::max(c.pan + (s.d[D_PAN] * 2.f - 1.f) * c.panRnd, -1.f), 1.f);

		last = h;
		if (!h.played) return last;

		if (c.choke) {
			for (Voice& v : voices)
				if (v.active && v.fadeOutStep == 0.f) v.fadeOutStep = 1.f / (0.003f * engineRate);
		}
		Voice* v = nullptr;
		for (Voice& x : voices) if (!x.active) { v = &x; break; }
		if (!v) {
			v = &voices[0];
			for (Voice& x : voices) if (x.age < v->age) v = &x;
		}
		Voice nv;
		nv.active = true;
		nv.s = &smp;
		nv.inc = inc;
		nv.rev = h.reverse;
		int frames = smp.frames();
		double off = h.start * frames;
		nv.pos = h.reverse ? (frames - 1) - off : off;
		float th = (h.pan + 1.f) * 0.7853982f;   // equal power, centre = 1
		nv.gL = std::cos(th) * 1.4142136f * h.gain;
		nv.gR = std::sin(th) * 1.4142136f * h.gain;
		nv.gMono = h.gain;
		nv.useEnv = h.decaySec > 0.f;
		nv.env = 1.f;
		nv.envMul = nv.useEnv ? std::exp(-6.9078f / (h.decaySec * engineRate)) : 1.f;
		bool needFade = h.start > 0.f || h.reverse;
		nv.fadeIn = needFade ? 0.f : 1.f;
		nv.fadeInStep = 1.f / (0.001f * engineRate);
		nv.age = ++ageCounter;
		*v = nv;
		return last;
	}

	// One sample of output. mono is the sum without panning.
	void process(float engineRate, float& outL, float& outR, float& outMono) {
		outL = outR = outMono = 0.f;
		uint32_t newest = 0;
		float newestEnv = 0.f;
		for (Voice& v : voices) {
			if (!v.active) continue;
			const Sample& s = *v.s;
			int frames = s.frames();
			if (v.pos < 0.0 || v.pos >= frames) { v.active = false; continue; }
			float a = hermite(s.L, v.pos);
			float b = s.R.empty() ? a : hermite(s.R, v.pos);
			// short fade at the very end of the sample to stop clicks
			double left = v.rev ? v.pos : (frames - 1 - v.pos);
			float tail = (float) std::min(1.0, left / std::max(1.0, 0.002 * engineRate * v.inc));
			float g = v.env * v.fadeIn * v.fadeOut * tail;
			outL += a * g * v.gL;
			outR += b * g * v.gR;
			outMono += 0.5f * (a + b) * g * v.gMono;
			if (v.age > newest) {
				newest = v.age;
				newestEnv = v.env * v.fadeOut * std::min(1.f, v.gMono);
			}
			// advance
			v.pos += v.rev ? -v.inc : v.inc;
			if (v.useEnv) {
				v.env *= v.envMul;
				if (v.env < 0.001f) v.active = false;
			}
			if (v.fadeIn < 1.f) v.fadeIn = std::min(1.f, v.fadeIn + v.fadeInStep);
			if (v.fadeOutStep > 0.f) {
				v.fadeOut -= v.fadeOutStep;
				if (v.fadeOut <= 0.f) v.active = false;
			}
		}
		lastEnv = newestEnv;
	}

	int activeVoices() const {
		int n = 0;
		for (const Voice& v : voices) n += v.active;
		return n;
	}
};

// ---- WAV reading -----------------------------------------------------------
inline uint32_t rd32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t) p[3] << 24); }
inline uint16_t rd16(const uint8_t* p) { return p[0] | (p[1] << 8); }

// Reads 8/16/24/32-bit PCM or 32/64-bit float WAV, mono or stereo (more
// channels: the first two are used). Keeps the file's own sample rate.
// Returns an empty string on success, or the reason it failed.
inline std::string readWav(const std::string& path, Sample& out, double maxSeconds = 30.0) {
	FILE* f = std::fopen(path.c_str(), "rb");
	if (!f) return "could not open";
	std::fseek(f, 0, SEEK_END);
	long size = std::ftell(f);
	std::fseek(f, 0, SEEK_SET);
	if (size < 12 || size > 512L * 1024 * 1024) { std::fclose(f); return "not a WAV file, or too big"; }
	std::vector<uint8_t> d(size);
	size_t got = std::fread(d.data(), 1, size, f);
	std::fclose(f);
	if (got != (size_t) size) return "could not read";
	if (std::memcmp(&d[0], "RIFF", 4) || std::memcmp(&d[8], "WAVE", 4)) return "not a WAV file";

	int fmt = 0, channels = 0, bits = 0;
	uint32_t rate = 0;
	const uint8_t* data = nullptr;
	size_t dataLen = 0;
	size_t p = 12;
	while (p + 8 <= d.size()) {
		uint32_t len = rd32(&d[p + 4]);
		const uint8_t* body = &d[p + 8];
		size_t avail = std::min<size_t>(len, d.size() - p - 8);
		if (!std::memcmp(&d[p], "fmt ", 4) && avail >= 16) {
			fmt = rd16(body);
			channels = rd16(body + 2);
			rate = rd32(body + 4);
			bits = rd16(body + 14);
			if (fmt == 0xFFFE && avail >= 26) fmt = rd16(body + 24);
		}
		else if (!std::memcmp(&d[p], "data", 4)) {
			data = body;
			dataLen = avail;
		}
		p += 8 + (size_t) len + (len & 1);
	}
	if (!data || channels < 1 || rate < 1000) return "no audio data";
	if (!((fmt == 1 && (bits == 8 || bits == 16 || bits == 24 || bits == 32)) || (fmt == 3 && (bits == 32 || bits == 64))))
		return "unsupported format";

	int bps = bits / 8;
	size_t frames = dataLen / (bps * channels);
	frames = std::min(frames, (size_t) (maxSeconds * rate));
	if (frames < 2) return "empty";
	auto sample = [&](size_t fr, int ch) -> float {
		const uint8_t* s = data + (fr * channels + ch) * bps;
		if (fmt == 3) {
			if (bits == 32) { float v; std::memcpy(&v, s, 4); return v; }
			double v; std::memcpy(&v, s, 8); return (float) v;
		}
		switch (bits) {
			case 8: return (s[0] - 128) / 128.f;
			case 16: return (int16_t) rd16(s) / 32768.f;
			case 24: { int32_t v = (int32_t) (((uint32_t) s[0] << 8) | ((uint32_t) s[1] << 16) | ((uint32_t) s[2] << 24)); return v / 2147483648.f; }
			default: return (int32_t) rd32(s) / 2147483648.f;
		}
	};
	out.rate = (int) rate;
	out.L.resize(frames);
	out.R.clear();
	if (channels > 1) out.R.resize(frames);
	for (size_t i = 0; i < frames; i++) {
		out.L[i] = sample(i, 0);
		if (channels > 1) out.R[i] = sample(i, 1);
	}
	out.computePeaks();
	return "";
}

} // namespace rb
