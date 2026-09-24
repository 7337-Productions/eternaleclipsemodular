#pragma once
#include "plugin.hpp"
#include <dr_wav.h>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Loop banks for the Litany Engine. The shipped WAVs in res/litany/ are the
// onboard litany (Gristleism ethos) and the default sound; "Load sample
// folder..." swaps in a user bank per module instance (hybrid mode).
//
// Storage is int16 (scaled on read): half the RAM of float, inaudible for
// this material, and it doubles the headroom under the guardrails below.
// Onboard buffers are immutable and live until plugin unload; user banks are
// built completely on a worker thread before hand-off and freed only after
// the module has cut every grain that could reference them.
namespace litany {

// ----- Guardrails (user folders are untrusted input) -----
// A stranger's folder can hold anything; these caps bound RAM deterministically.
static constexpr int MAX_FILES = 99;                       // files beyond this are ignored
static constexpr float MAX_SECONDS_PER_FILE = 60.f;        // longer files are truncated
static constexpr size_t MAX_TOTAL_BYTES = 192u << 20;      // per-bank sample RAM; files that would exceed it are skipped
static constexpr float SAMPLE_SCALE = 1.f / 32768.f;

struct Loop {
	std::string name;      // display text, e.g. "02 · IRON PSALM"
	std::string path;
	float fileRate = 48000.f;
	size_t frames = 0;
	std::vector<int16_t> l, r;  // deinterleaved, scale-on-read (SAMPLE_SCALE)
	std::atomic<bool> ready{false};
};

// "02-iron-psalm.wav" -> "02 · IRON PSALM"
inline std::string loopDisplayName(const std::string& path) {
	std::string stem = rack::system::getStem(path);
	std::string num, rest = stem;
	if (stem.size() > 3 && isdigit(stem[0]) && isdigit(stem[1]) && stem[2] == '-') {
		num = stem.substr(0, 2);
		rest = stem.substr(3);
	}
	// Hyphens/underscores become spaces; CamelCase splits on lower->upper
	// ("MorsEstRequies" -> "MORS EST REQUIES"); everything uppercased.
	std::string out;
	for (size_t i = 0; i < rest.size(); i++) {
		char c = rest[i];
		if (c == '-' || c == '_') {
			out += ' ';
			continue;
		}
		if (i > 0 && isupper((unsigned char)c) && islower((unsigned char)rest[i - 1]))
			out += ' ';
		out += (char)toupper((unsigned char)c);
	}
	return num.empty() ? out : num + " · " + out;
}

// Scan a directory for .wav files, sorted, capped at MAX_FILES.
inline std::vector<std::string> scanLoopFolder(const std::string& dir) {
	std::vector<std::string> entries;
	if (rack::system::isDirectory(dir))
		entries = rack::system::getEntries(dir);
	std::sort(entries.begin(), entries.end());
	std::vector<std::string> out;
	for (const std::string& path : entries) {
		std::string ext = rack::string::lowercase(rack::system::getExtension(path));
		if (ext != ".wav")
			continue;
		out.push_back(path);
		if ((int)out.size() >= MAX_FILES)
			break;
	}
	return out;
}

// Decode one WAV into a Loop, applying the per-file guardrails. Returns false
// on unreadable/undecodable/too-short files (the loop stays empty and silent).
inline bool decodeLoop(Loop& lp) {
	// readFile + init_memory keeps dr_wav off the filesystem (Windows
	// UTF-8 path safety).
	std::vector<uint8_t> data;
	try {
		data = rack::system::readFile(lp.path);
	}
	catch (...) {
		WARN("Litany Engine: could not read %s", lp.path.c_str());
		return false;
	}

	drwav wav;
	if (!drwav_init_memory(&wav, data.data(), data.size(), NULL)) {
		WARN("Litany Engine: could not decode %s", lp.path.c_str());
		return false;
	}
	size_t frames = (size_t)wav.totalPCMFrameCount;
	unsigned channels = wav.channels;
	float fileRate = (float)wav.sampleRate;
	if (frames < 64 || channels < 1 || fileRate < 8000.f) {
		drwav_uninit(&wav);
		return false;
	}
	// Guardrail: truncate to MAX_SECONDS_PER_FILE
	size_t maxFrames = (size_t)(MAX_SECONDS_PER_FILE * fileRate);
	frames = std::min(frames, maxFrames);
	std::vector<float> interleaved(frames * channels);
	size_t got = (size_t)drwav_read_pcm_frames_f32(&wav, frames, interleaved.data());
	drwav_uninit(&wav);
	if (got < 64)
		return false;

	std::vector<float> fl(got), fr(got);
	for (size_t i = 0; i < got; i++) {
		fl[i] = interleaved[i * channels];
		fr[i] = channels > 1 ? interleaved[i * channels + 1] : fl[i];
	}

	// Bake a ~10 ms end->start crossfade into the buffer so the loop point
	// is seamless for every reader (grains wrap mid-buffer with no runtime
	// splice handling).
	size_t fade = std::min((size_t)(0.010f * fileRate), got / 4);
	for (size_t i = 0; i < fade; i++) {
		float t = (float)i / (float)fade;
		size_t tail = got - fade + i;
		fl[i] = fl[tail] * (1.f - t) + fl[i] * t;
		fr[i] = fr[tail] * (1.f - t) + fr[i] * t;
	}
	size_t n = got - fade;

	lp.fileRate = fileRate;
	lp.frames = n;
	lp.l.resize(n);
	lp.r.resize(n);
	for (size_t i = 0; i < n; i++) {
		lp.l[i] = (int16_t)rack::math::clamp(fl[i] * 32767.f, -32768.f, 32767.f);
		lp.r[i] = (int16_t)rack::math::clamp(fr[i] * 32767.f, -32768.f, 32767.f);
	}
	return true;
}

// The onboard litany: shared by every instance, scanned synchronously at
// first access (names available immediately), decoded once on a background
// thread, alive until plugin unload.
class SampleBank {
public:
	static SampleBank& get() {
		static SampleBank bank;
		return bank;
	}

	// Each module instance retains the bank; the first starts the background
	// decode, the last to go joins it (see release()).
	void retain() {
		users++;
		std::call_once(loadOnce, [this] {
			std::lock_guard<std::mutex> lock(loaderMutex);
			loader = std::thread([this] { decodeAll(); });
		});
	}

	// Join the decode thread while a module is still being torn down on the
	// UI thread. Doing it here rather than in the static destructor matters:
	// on Windows a std::thread::join() inside DLL unload / process exit can
	// deadlock on the loader lock, which would leave Rack alive after Quit.
	void release() {
		if (--users > 0)
			return;
		std::lock_guard<std::mutex> lock(loaderMutex);
		if (loader.joinable())
			loader.join();
	}

	size_t count() const { return loops.size(); }

	const Loop* loop(size_t i) const {
		return i < loops.size() ? loops[i].get() : nullptr;
	}

	std::vector<std::string> names() const {
		std::vector<std::string> out;
		for (const auto& lp : loops)
			out.push_back(lp->name);
		return out;
	}

	// Static destruction (plugin unload): never join here. If the decode is
	// somehow still running, let it go rather than block the process exit.
	~SampleBank() {
		if (loader.joinable())
			loader.detach();
	}

private:
	std::vector<std::unique_ptr<Loop>> loops;
	std::thread loader;
	std::mutex loaderMutex;
	std::once_flag loadOnce;
	std::atomic<int> users{0};

	SampleBank() {
		// Synchronous scan: names only, no decoding.
		for (const std::string& path : scanLoopFolder(rack::asset::plugin(pluginInstance, "res/litany"))) {
			auto lp = std::make_unique<Loop>();
			lp->path = path;
			lp->name = loopDisplayName(path);
			loops.push_back(std::move(lp));
		}
	}

	void decodeAll() {
		rack::system::setThreadName("LitanySampleBank");
		size_t total = 0;
		for (auto& lp : loops) {
			if (total < MAX_TOTAL_BYTES && decodeLoop(*lp))
				total += (lp->l.size() + lp->r.size()) * sizeof(int16_t);
			lp->ready.store(true, std::memory_order_release);
		}
	}
};

// A user folder (hybrid mode), decoded completely on the worker thread before
// hand-off: every loop is ready by the time the module sees the bank, so the
// audio thread never touches a partially built buffer.
struct UserBank {
	std::vector<std::unique_ptr<Loop>> loops;
	std::string dir;
	size_t totalBytes = 0;
	int skipped = 0; // files dropped by the guardrails (count reported in the menu)

	size_t count() const { return loops.size(); }

	const Loop* loop(size_t i) const {
		return i < loops.size() ? loops[i].get() : nullptr;
	}

	std::vector<std::string> names() const {
		std::vector<std::string> out;
		for (const auto& lp : loops)
			out.push_back(lp->name);
		return out;
	}

	// Returns nullptr and fills `error` ("missing" / "no wavs") on failure.
	// Call from a worker thread: decodes everything under the guardrails.
	static UserBank* load(const std::string& dir, std::string& error) {
		if (!rack::system::isDirectory(dir)) {
			error = "missing";
			return nullptr;
		}
		std::vector<std::string> paths = scanLoopFolder(dir);
		if (paths.empty()) {
			error = "no wavs";
			return nullptr;
		}
		std::unique_ptr<UserBank> bank(new UserBank);
		bank->dir = dir;
		for (const std::string& path : paths) {
			// Guardrail: stop taking files once the RAM budget is spent
			if (bank->totalBytes >= MAX_TOTAL_BYTES) {
				bank->skipped++;
				continue;
			}
			auto lp = std::make_unique<Loop>();
			lp->path = path;
			lp->name = loopDisplayName(path);
			if (!decodeLoop(*lp)) {
				bank->skipped++;
				continue;
			}
			bank->totalBytes += (lp->l.size() + lp->r.size()) * sizeof(int16_t);
			lp->ready.store(true, std::memory_order_release);
			bank->loops.push_back(std::move(lp));
		}
		if (bank->loops.empty()) {
			error = "no wavs";
			return nullptr;
		}
		return bank.release();
	}
};

} // namespace litany
