// Randrum.cpp
// A drum sample player that rolls dice on every hit. Load a folder of WAV
// files; each trigger picks a sample and a pitch, decay, start, level, pan
// and direction, within ranges set by the knobs. LOOP keeps the dice rolls
// in a ring of 1-16 hits so the random pattern repeats, and MUTATE slowly
// rewrites it. The engine lives in DrumEngine.hpp.
#include "plugin.hpp"
#include "DrumEngine.hpp"
#include <osdialog.h>
#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <algorithm>
#include <cctype>

using namespace rb;

static const int MAX_FILES = 1024;

// ---- knob readouts -----------------------------------------------------------
struct DecayQuantity : ParamQuantity {
	std::string getDisplayValueString() override {
		float v = getValue();
		if (v >= 0.99f) return "full sample";
		float s = 0.01f * std::pow(400.f, v);
		return s < 1.f ? string::f("%.0f ms", s * 1000.f) : string::f("%.2f s", s);
	}
};

struct LevelQuantity : ParamQuantity {
	std::string getDisplayValueString() override {
		float g = 2.f * getValue() * getValue();
		if (g < 1e-4f) return "-inf dB";
		return string::f("%+.1f dB", 20.f * std::log10(g));
	}
};

struct LoopQuantity : ParamQuantity {
	std::string getDisplayValueString() override {
		int n = (int) std::round(getValue());
		return n == 0 ? "free (new dice every hit)" : string::f("%d hits", n);
	}
};

struct VarietyQuantity : ParamQuantity {
	std::string getDisplayValueString() override {
		int n = (int) std::round(getValue());
		return n >= MAX_LOOP ? "all different" : string::f("%d sound%s", n, n == 1 ? "" : "s");
	}
};

struct PanQuantity : ParamQuantity {
	std::string getDisplayValueString() override {
		float p = getValue();
		if (std::fabs(p) < 0.01f) return "centre";
		return string::f("%s %.0f%%", p < 0 ? "left" : "right", std::fabs(p) * 100.f);
	}
};

struct Randrum : Module {
	enum ParamId {
		SAMPLE_PARAM, SPREAD_PARAM,
		PITCH_PARAM, PITCH_RND_PARAM,
		DECAY_PARAM, DECAY_RND_PARAM,
		START_PARAM, START_RND_PARAM,
		CHANCE_PARAM, REV_PARAM,
		LEVEL_PARAM, LEVEL_RND_PARAM,
		PAN_PARAM, PAN_RND_PARAM,
		MUTATE_PARAM, LOOP_PARAM,
		MODE_PARAM, CHOKE_PARAM, REROLL_PARAM,
		VARIETY_PARAM,
		PARAMS_LEN
	};
	enum InputId {
		SAMPLE_INPUT, VOCT_INPUT, DECAY_INPUT, START_INPUT,
		CHANCE_INPUT, VEL_INPUT, PAN_INPUT, MUTATE_INPUT,
		TRIG_INPUT, RESET_INPUT, REROLL_INPUT,
		INPUTS_LEN
	};
	enum OutputId { OUT_L_OUTPUT, OUT_R_OUTPUT, ENV_OUTPUT, HIT_OUTPUT, SKIP_OUTPUT, IDX_OUTPUT, OUTPUTS_LEN };
	enum LightId { HIT_LIGHT, SKIP_LIGHT, REROLL_LIGHT, LIGHTS_LEN };

	DrumEngine engine;
	// Banks are built on a loader thread and published in `uiBank` (read and
	// written with std::atomic_load/atomic_store). The audio thread notices the
	// version change and takes its own copy, so the screen never waits for it.
	std::shared_ptr<Bank> uiBank;
	std::atomic<int> bankVersion{0};
	std::shared_ptr<Bank> audioBank;   // audio thread only
	std::atomic<int> audioVersion{0};

	std::thread loader;
	std::atomic<bool> loading{false};
	std::mutex textMutex;              // guards folder and loadMessage
	std::string folder;
	std::string loadMessage;
	bool quantise = false;
	bool subfolders = true;

	dsp::SchmittTrigger trigIn, resetIn, rerollIn, rerollBtn;
	dsp::PulseGenerator hitPulse, skipPulse;
	float idxVolts = 0.f;
	Controls shownCtl;                 // for the display
	int ctlTimer = 0;

	Randrum() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configParam(SAMPLE_PARAM, 0.f, 1.f, 0.f, "Sample (middle of the window)", "%", 0.f, 100.f);
		configParam(SPREAD_PARAM, 0.f, 1.f, 1.f, "Spread (how many samples the dice can pick from)", "%", 0.f, 100.f);
		configParam(PITCH_PARAM, -24.f, 24.f, 0.f, "Pitch", " semitones");
		configParam(PITCH_RND_PARAM, 0.f, 1.f, 0.f, "Random pitch (up to +-12 semitones)", "%", 0.f, 100.f);
		configParam<DecayQuantity>(DECAY_PARAM, 0.f, 1.f, 1.f, "Decay");
		configParam(DECAY_RND_PARAM, 0.f, 1.f, 0.f, "Random decay (up to 8x longer or shorter)", "%", 0.f, 100.f);
		configParam(START_PARAM, 0.f, 0.95f, 0.f, "Start point", "%", 0.f, 100.f);
		configParam(START_RND_PARAM, 0.f, 1.f, 0.f, "Random start (up to half the sample later)", "%", 0.f, 100.f);
		configParam(CHANCE_PARAM, 0.f, 1.f, 1.f, "Chance that a trigger plays", "%", 0.f, 100.f);
		configParam(REV_PARAM, 0.f, 1.f, 0.f, "Chance of playing backwards", "%", 0.f, 100.f);
		configParam<LevelQuantity>(LEVEL_PARAM, 0.f, 1.f, 0.7071f, "Level");
		configParam(LEVEL_RND_PARAM, 0.f, 1.f, 0.f, "Random level (up to 24 dB quieter)", "%", 0.f, 100.f);
		configParam<PanQuantity>(PAN_PARAM, -1.f, 1.f, 0.f, "Pan");
		configParam(PAN_RND_PARAM, 0.f, 1.f, 0.f, "Random pan", "%", 0.f, 100.f);
		configParam(MUTATE_PARAM, 0.f, 1.f, 0.1f, "Mutate (chance each die is rolled again per hit)", "%", 0.f, 100.f);
		configParam<LoopQuantity>(LOOP_PARAM, 0.f, 16.f, 8.f, "Loop length");
		getParamQuantity(LOOP_PARAM)->snapEnabled = true;
		configSwitch(MODE_PARAM, 0.f, 2.f, 0.f, "Pick", {"Random", "Walk (one step at a time)", "Cycle (in order)"});
		configSwitch(CHOKE_PARAM, 0.f, 1.f, 0.f, "Voices", {"Overlap (tails ring on)", "Choke (each hit cuts the last)"});
		configButton(REROLL_PARAM, "Re-roll the whole loop");
		configParam<VarietyQuantity>(VARIETY_PARAM, 1.f, (float) MAX_LOOP, (float) MAX_LOOP, "Variety (how many different samples, in pick mode rnd)");
		getParamQuantity(VARIETY_PARAM)->snapEnabled = true;

		configInput(SAMPLE_INPUT, "Sample CV (0-10 V sweeps the folder)");
		configInput(VOCT_INPUT, "Pitch V/OCT");
		configInput(DECAY_INPUT, "Decay CV (0-10 V)");
		configInput(START_INPUT, "Start CV (0-10 V)");
		configInput(CHANCE_INPUT, "Chance CV (+-10 V)");
		configInput(VEL_INPUT, "Velocity (0-10 V, normalled to 10 V)");
		configInput(PAN_INPUT, "Pan CV (+-5 V)");
		configInput(MUTATE_INPUT, "Mutate CV (+-10 V)");
		configInput(TRIG_INPUT, "Trigger");
		configInput(RESET_INPUT, "Reset (back to the first hit of the loop)");
		configInput(REROLL_INPUT, "Re-roll gate");
		configOutput(OUT_L_OUTPUT, "Left (mono when R is not patched)");
		configOutput(OUT_R_OUTPUT, "Right");
		configOutput(ENV_OUTPUT, "Envelope of the newest hit (0-10 V)");
		configOutput(HIT_OUTPUT, "Hit trigger");
		configOutput(SKIP_OUTPUT, "Skip trigger (CHANCE said no)");
		configOutput(IDX_OUTPUT, "Sample number (0-10 V across the folder)");
		configLight(HIT_LIGHT, "Hit");
		configLight(SKIP_LIGHT, "Skip");

		uint64_t seed = (uint64_t) (random::u32()) << 32 | random::u32();
		engine.rng.seed(seed);
		engine.rerollAll();
		shownCtl = readControls();
	}

	~Randrum() {
		if (loader.joinable()) loader.join();
	}

	float cv(int input, float scale) {
		return inputs[input].isConnected() ? inputs[input].getVoltage() / scale : 0.f;
	}

	Controls readControls() {
		Controls c;
		c.sample = clamp(params[SAMPLE_PARAM].getValue() + cv(SAMPLE_INPUT, 10.f), 0.f, 1.f);
		c.spread = params[SPREAD_PARAM].getValue();
		c.pitch = params[PITCH_PARAM].getValue() + 12.f * (inputs[VOCT_INPUT].isConnected() ? inputs[VOCT_INPUT].getVoltage() : 0.f);
		c.pitchRnd = params[PITCH_RND_PARAM].getValue();
		c.decay = clamp(params[DECAY_PARAM].getValue() + cv(DECAY_INPUT, 10.f), 0.f, 1.f);
		c.decayRnd = params[DECAY_RND_PARAM].getValue();
		c.start = clamp(params[START_PARAM].getValue() + cv(START_INPUT, 10.f), 0.f, 0.95f);
		c.startRnd = params[START_RND_PARAM].getValue();
		c.chance = clamp(params[CHANCE_PARAM].getValue() + cv(CHANCE_INPUT, 10.f), 0.f, 1.f);
		c.reverse = params[REV_PARAM].getValue();
		float k = params[LEVEL_PARAM].getValue();
		float vel = inputs[VEL_INPUT].isConnected() ? clamp(inputs[VEL_INPUT].getVoltage() / 10.f, 0.f, 1.f) : 1.f;
		c.level = 2.f * k * k * vel;
		c.levelRnd = params[LEVEL_RND_PARAM].getValue();
		c.pan = clamp(params[PAN_PARAM].getValue() + cv(PAN_INPUT, 5.f), -1.f, 1.f);
		c.panRnd = params[PAN_RND_PARAM].getValue();
		c.mutate = clamp(params[MUTATE_PARAM].getValue() + cv(MUTATE_INPUT, 10.f), 0.f, 1.f);
		c.loop = clamp((int) std::round(params[LOOP_PARAM].getValue()), 0, MAX_LOOP);
		c.mode = (PickMode) clamp((int) std::round(params[MODE_PARAM].getValue()), 0, 2);
		c.choke = params[CHOKE_PARAM].getValue() > 0.5f;
		c.variety = clamp((int) std::round(params[VARIETY_PARAM].getValue()), 1, MAX_LOOP);
		c.quantise = quantise;
		return c;
	}

	void process(const ProcessArgs& args) override {
		// pick up a newly loaded folder
		int v = bankVersion.load();
		if (v != audioVersion.load()) {
			engine.stopAll();
			audioBank = std::atomic_load(&uiBank);
			audioVersion.store(v);
		}
		Bank* bank = audioBank.get();

		if (resetIn.process(inputs[RESET_INPUT].getVoltage(), 0.1f, 1.f)) engine.reset();
		bool rr = rerollBtn.process(params[REROLL_PARAM].getValue());
		rr |= rerollIn.process(inputs[REROLL_INPUT].getVoltage(), 0.1f, 1.f);
		if (rr) engine.rerollAll();
		lights[REROLL_LIGHT].setBrightnessSmooth(params[REROLL_PARAM].getValue() > 0.f ? 1.f : 0.f, args.sampleTime);

		if (trigIn.process(inputs[TRIG_INPUT].getVoltage(), 0.1f, 1.f)) {
			Controls c = readControls();
			const HitInfo& h = engine.trigger(c, bank, args.sampleRate);
			int n = bank ? bank->size() : 0;
			if (h.played) {
				hitPulse.trigger(0.005f);
				idxVolts = n > 1 ? 10.f * h.index / (n - 1) : 0.f;
			}
			else if (n > 0) skipPulse.trigger(0.005f);
			shownCtl = c;
		}
		else if (++ctlTimer >= 512) {
			ctlTimer = 0;
			shownCtl = readControls();
		}

		float l, r, m;
		engine.process(args.sampleRate, l, r, m);
		if (outputs[OUT_R_OUTPUT].isConnected()) {
			outputs[OUT_L_OUTPUT].setVoltage(5.f * l);
			outputs[OUT_R_OUTPUT].setVoltage(5.f * r);
		}
		else {
			outputs[OUT_L_OUTPUT].setVoltage(5.f * m);
		}
		outputs[ENV_OUTPUT].setVoltage(10.f * engine.lastEnv);
		bool hp = hitPulse.process(args.sampleTime);
		bool sp = skipPulse.process(args.sampleTime);
		outputs[HIT_OUTPUT].setVoltage(hp ? 10.f : 0.f);
		outputs[SKIP_OUTPUT].setVoltage(sp ? 10.f : 0.f);
		outputs[IDX_OUTPUT].setVoltage(idxVolts);
		lights[HIT_LIGHT].setBrightnessSmooth(hp ? 1.f : 0.f, args.sampleTime * 4.f);
		lights[SKIP_LIGHT].setBrightnessSmooth(sp ? 1.f : 0.f, args.sampleTime * 4.f);
	}

	// ---- folders (UI thread) ------------------------------------------------
	static std::string lower(std::string s) {
		for (char& ch : s) ch = (char) std::tolower((unsigned char) ch);
		return s;
	}

	static bool isAudioFile(const std::string& p) {
		std::string e = lower(system::getExtension(p));
		return e == ".wav" || e == ".wave" || e == ".aif" || e == ".aiff" || e == ".aifc";
	}

	std::string getFolder() {
		std::lock_guard<std::mutex> lock(textMutex);
		return folder;
	}
	std::string getMessage() {
		std::lock_guard<std::mutex> lock(textMutex);
		return loadMessage;
	}
	void setText(const std::string& f, const std::string& m) {
		std::lock_guard<std::mutex> lock(textMutex);
		folder = f;
		loadMessage = m;
	}

	// Builds a bank from a folder (and its subfolders). Runs on the loader thread.
	// On failure returns nullptr and puts the reason in `err`.
	static std::shared_ptr<Bank> buildBank(const std::string& dir, bool sub, std::string& err, std::string& note) {
		if (!system::isDirectory(dir)) {
			err = "folder not found";
			return nullptr;
		}
		std::vector<std::string> entries = system::getEntries(dir, sub ? 8 : 0);
		if (entries.empty()) {
			err = "can't see inside: empty, or macOS blocks Rack";
			return nullptr;
		}
		// relative path (for the name and the order) -> full path
		std::vector<std::pair<std::string, std::string>> files;
		std::string base = dir;
		while (base.size() > 1 && (base.back() == '/' || base.back() == '\\')) base.pop_back();
		for (const std::string& p : entries) {
			std::string rel = p.size() > base.size() + 1 && p.compare(0, base.size(), base) == 0 ? p.substr(base.size() + 1) : system::getFilename(p);
			if (rel.empty() || rel[0] == '.' || rel.find("/.") != std::string::npos) continue;  // hidden files, macOS "._" files
			if (!isAudioFile(p) || !system::isFile(p)) continue;
			files.push_back({rel, p});
		}
		std::sort(files.begin(), files.end(), [](const std::pair<std::string, std::string>& a, const std::pair<std::string, std::string>& b) {
			return lower(a.first) < lower(b.first);
		});
		if (files.empty()) {
			err = sub ? "no WAV or AIFF files in here" : "no WAV/AIFF files (subfolders off)";
			return nullptr;
		}
		bool capped = (int) files.size() > MAX_FILES;
		if (capped) files.resize(MAX_FILES);
		std::shared_ptr<Bank> b = std::make_shared<Bank>();
		b->folder = dir;
		int skipped = 0;
		std::string firstErr;
		for (const auto& f : files) {
			Sample s;
			std::string e = readSample(f.second, s);
			if (e.empty()) {
				std::string name = f.first;
				size_t dot = name.find_last_of('.');
				if (dot != std::string::npos && dot > name.find_last_of('/') + 1) name = name.substr(0, dot);
				s.name = name;
				b->samples.push_back(std::move(s));
			}
			else {
				skipped++;
				if (firstErr.empty()) firstErr = system::getFilename(f.second) + ": " + e;
				WARN("Randrum: could not read %s: %s", f.second.c_str(), e.c_str());
			}
		}
		if (b->samples.empty()) {
			err = string::f("%d files, none readable. ", skipped) + firstErr;
			return nullptr;
		}
		if (skipped) note = string::f("%d unreadable", skipped);
		else if (capped) note = string::f("first %d files", MAX_FILES);
		INFO("Randrum: loaded %d samples from %s", b->size(), dir.c_str());
		return b;
	}

	// Starts loading a folder in the background. The screen shows the result.
	void loadFolder(const std::string& dir) {
		if (loader.joinable()) loader.join();
		loading = true;
		bool sub = subfolders;
		loader = std::thread([this, dir, sub]() {
			std::string err, note;
			std::shared_ptr<Bank> b = buildBank(dir, sub, err, note);
			if (b) {
				std::atomic_store(&uiBank, b);
				bankVersion++;
				setText(dir, note);
			}
			else {
				WARN("Randrum: could not load %s: %s", dir.c_str(), err.c_str());
				// keep playing the old folder, but say what went wrong
				std::shared_ptr<Bank> old = std::atomic_load(&uiBank);
				setText(old ? old->folder : dir, system::getFilename(dir) + ": " + err);
			}
			loading = false;
		});
	}

	std::vector<std::string> siblingFolders() {
		std::vector<std::string> out;
		std::string f = getFolder();
		if (f.empty()) return out;
		std::string parent = system::getDirectory(f);
		if (parent.empty() || !system::isDirectory(parent)) return out;
		for (const std::string& p : system::getEntries(parent)) {
			std::string fn = system::getFilename(p);
			if (fn.empty() || fn[0] == '.') continue;
			if (system::isDirectory(p)) out.push_back(p);
		}
		std::sort(out.begin(), out.end(), [](const std::string& a, const std::string& b) { return lower(a) < lower(b); });
		return out;
	}

	void stepFolder(int dir) {
		std::vector<std::string> sib = siblingFolders();
		if (sib.empty()) return;
		std::string f = getFolder();
		int cur = -1;
		for (int i = 0; i < (int) sib.size(); i++) if (sib[i] == f) cur = i;
		int n = (int) sib.size();
		loadFolder(sib[((cur + dir) % n + n) % n]);
	}

	// ---- patch storage --------------------------------------------------------
	json_t* dataToJson() override {
		json_t* root = json_object();
		json_object_set_new(root, "folder", json_string(getFolder().c_str()));
		json_object_set_new(root, "subfolders", json_boolean(subfolders));
		json_object_set_new(root, "quantise", json_boolean(quantise));
		json_t* ring = json_array();
		for (int i = 0; i < MAX_LOOP; i++)
			for (int k = 0; k < NUM_DICE; k++) json_array_append_new(ring, json_real(engine.ring[i].d[k]));
		json_object_set_new(root, "dice", ring);
		return root;
	}

	void dataFromJson(json_t* root) override {
		json_t* q = json_object_get(root, "quantise");
		quantise = q ? json_boolean_value(q) : false;
		json_t* sf = json_object_get(root, "subfolders");
		subfolders = sf ? json_boolean_value(sf) : true;
		json_t* ring = json_object_get(root, "dice");
		// patches from 2.0/2.1 have 8 dice per step (no D_GROUP): keep a fresh roll for it
		size_t perStep = ring ? json_array_size(ring) / MAX_LOOP : 0;
		if (ring && json_array_size(ring) % MAX_LOOP == 0 && perStep >= 8 && perStep <= (size_t) NUM_DICE) {
			for (int i = 0; i < MAX_LOOP; i++)
				for (int k = 0; k < (int) perStep; k++)
					engine.ring[i].d[k] = clamp((float) json_number_value(json_array_get(ring, i * perStep + k)), 0.f, 0.999999f);
		}
		engine.reset();
		json_t* f = json_object_get(root, "folder");
		if (f && json_string_value(f) && *json_string_value(f)) {
			std::string dir = json_string_value(f);
			setText(dir, "");
			loadFolder(dir);
		}
	}
};

// ---- display ---------------------------------------------------------------
static const NVGcolor C_BG = nvgRGB(0x12, 0x12, 0x16);
static const NVGcolor C_INK = nvgRGB(0xEE, 0xE8, 0xDC);
static const NVGcolor C_SOFT = nvgRGB(0x86, 0x82, 0x7A);
static const NVGcolor C_DIM = nvgRGB(0x34, 0x33, 0x3A);
static const NVGcolor C_HOT = nvgRGB(0xF2, 0x6B, 0x3A);
static const NVGcolor C_WIN = nvgRGB(0x3F, 0x8E, 0x9A);
static const NVGcolor C_ENV = nvgRGB(0xF2, 0xD0, 0x50);
static const NVGcolor C_WAVE = nvgRGB(0xB8, 0xB2, 0xA6);
static const NVGcolor C_SKIP = nvgRGB(0x6C, 0x5C, 0xA8);

struct RandrumDisplay : Widget {
	Randrum* module = nullptr;
	double waitStart = -1.0;

	void rect(NVGcontext* vg, float x, float y, float w, float h, NVGcolor c) {
		nvgBeginPath(vg);
		nvgRect(vg, x, y, w, h);
		nvgFillColor(vg, c);
		nvgFill(vg);
	}
	void text(NVGcontext* vg, float x, float y, const std::string& s, NVGcolor c, int align, float size) {
		nvgFontSize(vg, size);
		nvgTextAlign(vg, align);
		nvgFillColor(vg, c);
		nvgText(vg, x, y, s.c_str(), NULL);
	}
	static std::string clip(const std::string& s, size_t n) {
		return s.size() <= n ? s : s.substr(0, n - 1) + "~";
	}

	void drawLayer(const DrawArgs& args, int layer) override {
		if (layer != 1) { Widget::drawLayer(args, layer); return; }
		NVGcontext* vg = args.vg;
		float W = box.size.x, H = box.size.y;
		nvgBeginPath(vg);
		nvgRoundedRect(vg, 0, 0, W, H, mm2px(1.2f));
		nvgFillColor(vg, C_BG);
		nvgFill(vg);
		std::shared_ptr<window::Font> font = APP->window->loadFont(asset::system("res/fonts/ShareTechMono-Regular.ttf"));
		if (font && font->handle >= 0) nvgFontFaceId(vg, font->handle);
		float pad = mm2px(1.8f), fs = mm2px(2.6f), fss = mm2px(2.2f);
		int L = NVG_ALIGN_LEFT | NVG_ALIGN_BASELINE, R = NVG_ALIGN_RIGHT | NVG_ALIGN_BASELINE, C = NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE;

		if (!module) {
			text(vg, W / 2, H / 2, "RANDROM  -  dice for drums", C_INK, C, fs);
			return;
		}
		std::shared_ptr<Bank> bankRef = std::atomic_load(&module->uiBank);
		Bank* bank = bankRef.get();
		std::string folder = module->getFolder(), msg = module->getMessage();
		// is the audio engine picking up new folders?
		if (module->bankVersion.load() != module->audioVersion.load()) {
			if (waitStart < 0.0) waitStart = system::getTime();
		}
		else waitStart = -1.0;
		bool engineStuck = waitStart >= 0.0 && system::getTime() - waitStart > 1.5;
		const Controls& c = module->shownCtl;
		const HitInfo& h = module->engine.last;
		float ringX0 = W * 0.72f;
		float leftW = ringX0 - 2 * pad;

		// top line: folder and mode
		static const char* modes[] = {"RANDOM", "WALK", "CYCLE"};
		if (!bank) {
			std::string head = module->loading ? "LOADING " + clip(system::getFilename(folder), 20)
			                 : folder.empty() ? "NO FOLDER" : clip(system::getFilename(folder), 26);
			text(vg, pad, pad + fs, head, module->loading ? C_INK : C_HOT, L, fs);
			if (!msg.empty() && !module->loading) {
				// the reason, over up to three lines
				float y = H * 0.38f;
				for (size_t i = 0; i < msg.size() && i < 3 * 34; i += 34, y += fss * 1.25f)
					text(vg, leftW / 2 + pad, y, msg.substr(i, 34), C_HOT, C, fss);
				text(vg, leftW / 2 + pad, H - pad - fss * 0.3f, "right-click > Load folder, or drop one here", C_SOFT, C, fss * 0.9f);
			}
			else if (!module->loading) {
				text(vg, leftW / 2 + pad, H * 0.5f, "right-click > Load folder", C_INK, C, fs);
				text(vg, leftW / 2 + pad, H * 0.5f + fs * 1.4f, "or drop a folder of samples here", C_SOFT, C, fss);
			}
		}
		else {
			int n = bank->size();
			text(vg, pad, pad + fs, (module->loading ? "LOADING.. " : "") + clip(system::getFilename(bank->folder), 20) + string::f("  %d", n), C_INK, L, fs);
			text(vg, ringX0 - pad, pad + fs, modes[(int) c.mode] + std::string(c.choke ? " CHOKE" : ""), C_SOFT, R, fss);

			// second line: the last hit
			float y2 = pad + fs * 2.25f;
			if (h.index >= 0 && h.index < n) {
				text(vg, pad, y2, string::f("%02d ", h.index + 1) + clip(bank->samples[h.index].name, 20), h.played ? C_HOT : C_SKIP, L, fss);
				std::string info;
				if (!h.played) info = "skipped";
				else {
					info = string::f("%+.1fst ", h.semis);
					info += h.decaySec < 0 ? "full " : (h.decaySec < 1 ? string::f("%.0fms ", h.decaySec * 1000) : string::f("%.1fs ", h.decaySec));
					float db = 20.f * std::log10(std::max(h.gain, 1e-4f));
					info += string::f("%+.0fdB", db);
					if (std::fabs(h.pan) > 0.05f) info += string::f(" %c%.0f", h.pan < 0 ? 'L' : 'R', std::fabs(h.pan) * 100);
					if (h.reverse) info += " REV";
				}
				text(vg, ringX0 - pad, y2, info, C_SOFT, R, fss);
			}

			// waveform of the last sample, with start point and decay curve
			float wy = y2 + mm2px(1.4f), wh = H - wy - mm2px(6.2f), wx = pad, ww = leftW;
			rect(vg, wx, wy, ww, wh, nvgRGB(0x1A, 0x1A, 0x20));
			int show = (h.index >= 0 && h.index < n) ? h.index : 0;
			const Sample& s = bank->samples[show];
			bool rev = h.index >= 0 && h.reverse;
			nvgBeginPath(vg);
			for (int i = 0; i < (int) ww; i++) {
				int b = std::min(PEAK_BINS - 1, (int) ((float) i / ww * PEAK_BINS));
				if (rev) b = PEAK_BINS - 1 - b;
				float a = std::min(1.f, s.peaks[b]) * wh * 0.48f;
				nvgRect(vg, wx + i, wy + wh / 2 - a, 1.f, std::max(0.6f, 2 * a));
			}
			nvgFillColor(vg, C_WAVE);
			nvgFill(vg);
			if (h.index >= 0) {
				// grey out the part before the start point
				float sx = wx + ww * h.start;
				nvgBeginPath(vg);
				nvgRect(vg, wx, wy, sx - wx, wh);
				nvgFillColor(vg, nvgRGBA(0x12, 0x12, 0x16, 0xC0));
				nvgFill(vg);
				// decay curve: time on screen is sample time at the played pitch
				double inc = (double) s.rate / APP->engine->getSampleRate() * std::pow(2.0, h.semis / 12.0);
				float totalSec = (float) (s.frames() / (inc * APP->engine->getSampleRate()));
				nvgBeginPath(vg);
				for (int i = 0; i <= 64; i++) {
					float fx = h.start + (1.f - h.start) * i / 64.f;
					float t = (fx - h.start) * totalSec;
					float e = h.decaySec < 0 ? 1.f : std::exp(-6.9078f * t / h.decaySec);
					float px = wx + ww * fx, py = wy + wh - e * wh;
					if (i == 0) nvgMoveTo(vg, px, py);
					else nvgLineTo(vg, px, py);
				}
				nvgStrokeColor(vg, h.played ? C_ENV : C_SKIP);
				nvgStrokeWidth(vg, 1.2f);
				nvgStroke(vg);
				rect(vg, sx, wy, 1.2f, wh, C_HOT);
				if (rev) text(vg, wx + ww - 2, wy + fss, "<< REV", C_HOT, R, fss);
				// playheads of voices on this sample
				for (const Voice& v : module->engine.voices) {
					if (!v.active || v.s != &s) continue;
					float f = (float) (v.pos / std::max(1, s.frames()));
					if (rev) f = 1.f - f;
					rect(vg, wx + ww * clamp(f, 0.f, 1.f), wy, 1.f, wh, C_INK);
				}
			}

			// sample strip: every file in the folder, the window lit up
			float sy = H - mm2px(4.6f), sh = mm2px(2.6f);
			int lo, size;
			DrumEngine::window(c, n, lo, size);
			float cw = ww / n;
			for (int i = 0; i < n; i++) {
				int off = DrumEngine::wrap(i - lo, n);
				bool in = off < size;
				NVGcolor col = in ? C_WIN : C_DIM;
				if (i == h.index) col = h.played ? C_HOT : C_SKIP;
				rect(vg, wx + i * cw + (cw > 3 ? 0.5f : 0.f), sy, std::max(0.8f, cw - (cw > 3 ? 1.f : 0.f)), sh, col);
			}
		}

		// loop ring
		float rcx = (ringX0 + W) / 2, rcy = H * 0.54f, rr = std::min(W - ringX0, H) * 0.34f;
		rect(vg, ringX0, pad * 0.5f, 1.f, H - pad, C_DIM);
		if (c.loop == 0) {
			text(vg, rcx, rcy - fs * 0.3f, "FREE", C_INK, C, fs * 1.2f);
			text(vg, rcx, rcy + fs, "new dice", C_SOFT, C, fss);
			text(vg, rcx, rcy + fs * 1.9f, "every hit", C_SOFT, C, fss);
			int K = DrumEngine::varietyCount(c);
			if (K > 0) text(vg, rcx, rcy + fs * 2.9f, string::f("%d sound%s", K, K == 1 ? "" : "s"), C_WIN, C, fss * 0.9f);
		}
		else {
			int Lp = c.loop;
			int K = DrumEngine::varietyCount(c);
			int grp[MAX_LOOP];
			if (K > 0) module->engine.groups(Lp, K, grp);
			if (K > 0) rr *= 0.82f;  // room for the sound letters
			for (int i = 0; i < Lp; i++) {
				float a = -M_PI / 2 + 2 * M_PI * i / Lp;
				float x = rcx + rr * std::cos(a), y = rcy + rr * std::sin(a);
				const Slot& sl = module->engine.ring[i];
				bool plays = sl.d[D_CHANCE] < c.chance;
				float lev = std::pow(10.f, -sl.d[D_LEVEL] * c.levelRnd * 24.f / 20.f);
				float r = mm2px(0.55f + 0.75f * lev);
				bool cur = i == module->engine.pos;
				if (K > 0) {
					float lr = rr + mm2px(2.2f);
					std::string letter(1, (char) ('A' + grp[i]));
					text(vg, rcx + lr * std::cos(a), rcy + lr * std::sin(a), letter, cur ? C_HOT : C_WIN, C, fss * 0.8f);
				}
				nvgBeginPath(vg);
				nvgCircle(vg, x, y, r);
				if (plays) {
					nvgFillColor(vg, cur ? C_HOT : C_INK);
					nvgFill(vg);
				}
				else {
					nvgStrokeColor(vg, cur ? C_HOT : C_SOFT);
					nvgStrokeWidth(vg, 1.f);
					nvgStroke(vg);
				}
			}
			text(vg, rcx, rcy - fs * 0.15f, string::f("%d", Lp), C_INK, C, fs * 1.3f);
			text(vg, rcx, rcy + fs * 0.95f, string::f("mut %.0f%%", c.mutate * 100), C_SOFT, C, fss * 0.9f);
			if (K > 0) text(vg, rcx, rcy + fs * 1.85f, string::f("%d sound%s", K, K == 1 ? "" : "s"), C_WIN, C, fss * 0.9f);
		}
		text(vg, rcx, pad + fs, "LOOP", C_SOFT, NVG_ALIGN_CENTER | NVG_ALIGN_BASELINE, fss);
		if (engineStuck)
			text(vg, rcx, H - pad * 0.6f, "audio engine stopped?", C_HOT, NVG_ALIGN_CENTER | NVG_ALIGN_BASELINE, fss * 0.8f);
		else if (bank && !msg.empty())
			text(vg, rcx, H - pad * 0.6f, clip(msg, 22), C_HOT, NVG_ALIGN_CENTER | NVG_ALIGN_BASELINE, fss * 0.8f);
	}
};

// ---- panel -----------------------------------------------------------------
// Positions in mm; res/make_panel.py uses the same numbers.
static const float COL_X[4] = {15.2f, 45.7f, 76.2f, 106.7f};
static const float ROW_KNOB_Y[2] = {57.5f, 84.5f};
static const float ROW_SMALL_Y[2] = {67.0f, 94.0f};
static const float SMALL_DX = 6.0f;
static const float CTRL_Y = 106.5f;
static const float JACK_Y = 120.5f;
static const float IN_X[3] = {14.5f, 25.0f, 35.5f};
static const float OUT_X[6] = {51.0f, 61.5f, 72.0f, 82.5f, 93.0f, 103.5f};

struct RandrumWidget : ModuleWidget {
	RandrumWidget(Randrum* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/Randrum.svg")));
		addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		RandrumDisplay* d = createWidget<RandrumDisplay>(mm2px(Vec(4.6f, 13.0f)));
		d->box.size = mm2px(Vec(112.7f, 32.6f));
		d->module = module;
		addChild(d);

		// blocks: big knob, small knob (random amount), CV input
		const int big[8] = {Randrum::SAMPLE_PARAM, Randrum::PITCH_PARAM, Randrum::DECAY_PARAM, Randrum::START_PARAM,
		                    Randrum::CHANCE_PARAM, Randrum::LEVEL_PARAM, Randrum::PAN_PARAM, Randrum::MUTATE_PARAM};
		const int small[8] = {Randrum::SPREAD_PARAM, Randrum::PITCH_RND_PARAM, Randrum::DECAY_RND_PARAM, Randrum::START_RND_PARAM,
		                      Randrum::REV_PARAM, Randrum::LEVEL_RND_PARAM, Randrum::PAN_RND_PARAM, Randrum::LOOP_PARAM};
		const int cvIn[8] = {Randrum::SAMPLE_INPUT, Randrum::VOCT_INPUT, Randrum::DECAY_INPUT, Randrum::START_INPUT,
		                     Randrum::CHANCE_INPUT, Randrum::VEL_INPUT, Randrum::PAN_INPUT, Randrum::MUTATE_INPUT};
		for (int i = 0; i < 8; i++) {
			float x = COL_X[i % 4];
			int r = i / 4;
			addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(x, ROW_KNOB_Y[r])), module, big[i]));
			if (i == 7)
				addParam(createParamCentered<RoundSmallBlackKnob>(mm2px(Vec(x - SMALL_DX, ROW_SMALL_Y[r])), module, small[i]));
			else
				addParam(createParamCentered<Trimpot>(mm2px(Vec(x - SMALL_DX, ROW_SMALL_Y[r])), module, small[i]));
			addInput(createInputCentered<PJ301MPort>(mm2px(Vec(x + SMALL_DX, ROW_SMALL_Y[r])), module, cvIn[i]));
		}

		addParam(createParamCentered<CKSSThreeHorizontal>(mm2px(Vec(18.0f, CTRL_Y)), module, Randrum::MODE_PARAM));
		addParam(createParamCentered<CKSS>(mm2px(Vec(45.7f, CTRL_Y)), module, Randrum::CHOKE_PARAM));
		addParam(createLightParamCentered<VCVLightBezel<WhiteLight>>(mm2px(Vec(63.5f, CTRL_Y)), module, Randrum::REROLL_PARAM, Randrum::REROLL_LIGHT));
		addParam(createParamCentered<RoundSmallBlackKnob>(mm2px(Vec(108.0f, CTRL_Y)), module, Randrum::VARIETY_PARAM));

		const int ins[3] = {Randrum::TRIG_INPUT, Randrum::RESET_INPUT, Randrum::REROLL_INPUT};
		for (int i = 0; i < 3; i++)
			addInput(createInputCentered<PJ301MPort>(mm2px(Vec(IN_X[i], JACK_Y)), module, ins[i]));
		for (int i = 0; i < 6; i++)
			addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(OUT_X[i], JACK_Y)), module, Randrum::OUT_L_OUTPUT + i));
		addChild(createLightCentered<SmallLight<YellowLight>>(mm2px(Vec(OUT_X[3] + 4.4f, JACK_Y - 5.4f)), module, Randrum::HIT_LIGHT));
		addChild(createLightCentered<SmallLight<BlueLight>>(mm2px(Vec(OUT_X[4] + 4.4f, JACK_Y - 5.4f)), module, Randrum::SKIP_LIGHT));
	}

	void onPathDrop(const PathDropEvent& e) override {
		Randrum* m = dynamic_cast<Randrum*>(module);
		if (!m || e.paths.empty()) return;
		std::string p = e.paths[0];
		while (p.size() > 1 && p.back() == '/') p.pop_back();
		m->loadFolder(system::isDirectory(p) ? p : system::getDirectory(p));
		e.consume(this);
	}

	void appendContextMenu(Menu* menu) override {
		Randrum* m = dynamic_cast<Randrum*>(module);
		if (!m) return;
		std::string folder = m->getFolder();
		menu->addChild(new MenuSeparator);
		menu->addChild(createMenuLabel(folder.empty() ? "No folder loaded" : "Folder: " + folder));
		menu->addChild(createMenuItem("Load folder...", "", [=]() {
			std::string start = folder.empty() ? "" : system::getDirectory(folder);
			char* path = osdialog_file(OSDIALOG_OPEN_DIR, start.empty() ? NULL : start.c_str(), NULL, NULL);
			if (!path) return;
			std::string p = path;
			std::free(path);
			m->loadFolder(p);
		}));
		menu->addChild(createBoolMenuItem("Include subfolders", "",
			[=]() { return m->subfolders; },
			[=](bool v) {
				m->subfolders = v;
				std::string f = m->getFolder();
				if (!f.empty()) m->loadFolder(f);
			}));
		if (!folder.empty()) {
			menu->addChild(createMenuItem("Next folder", "", [=]() { m->stepFolder(1); }));
			menu->addChild(createMenuItem("Previous folder", "", [=]() { m->stepFolder(-1); }));
			menu->addChild(createSubmenuItem("Folders next to this one", "", [=](Menu* sub) {
				for (const std::string& p : m->siblingFolders()) {
					sub->addChild(createCheckMenuItem(system::getFilename(p), "",
						[=]() { return p == m->getFolder(); },
						[=]() { m->loadFolder(p); }));
				}
			}));
			menu->addChild(createMenuItem("Reload folder", "", [=]() { m->loadFolder(m->getFolder()); }));
		}
		menu->addChild(new MenuSeparator);
		menu->addChild(createBoolPtrMenuItem("Random pitch in whole semitones", "", &m->quantise));
		menu->addChild(createMenuItem("Re-roll the whole loop", "", [=]() { m->engine.rerollAll(); }));
	}
};

Model* modelRandrum = createModel<Randrum, RandrumWidget>("Randrum");
