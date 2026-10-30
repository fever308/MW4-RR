#pragma once
#include <windows.h>
#include <array>
#include <atomic>
#include <cstdio>
#include <iterator>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <vector>
#include "rr_sss_runtime_shader.h"
#include "rr_transparency_shaders.h"
// Structural recognition of other builds of the transparency composites, at
// pipeline creation. The input is the bytecode the game's pipeline callback
// supplies; only part hashes and fingerprints are shipped, and nothing is
// transformed: the composite runs as the game built it. An accepted build is
// registered in Recognized(), which every later composite check reads.
namespace rrblend {
// One composite role. A build is accepted when its signature, runtime-info and
// feature parts equal the pinned build's and its disassembly, normalized by
// rrsss::StructuralText, equals theirs: the same instructions, bindings,
// signatures and metadata. StructuralText removes only the shader hash and
// debug name and the declared layout and size of the scene constants (b3),
// which must still cover read_bytes. The captured 12672- and 12416-byte
// builds differ exactly there for the SSS composite (9ee7 and c6c8) and for
// the refraction composite (b723 and 5a24); the OIT resolve declares the same
// scene layout as 9ee7 and b723 and reads only below read_bytes. Its other
// constant-buffer, resource and output declarations stay exact.
struct Profile {
    Kind kind;
    const char *name;
    uint64_t canonical; // the pinned build the fingerprint was measured on
    rrsss::PartPin parts[4];
    // SHA-256 of the normalized disassembly with the bundled DXC; empty: no
    // structural match for this role, the exact pins only.
    const char *structure;
    size_t read_bytes; // (highest constant row the instructions load + 1) * 16
};
inline constexpr Profile Profiles[] = {
    {Kind::Oit,
     "oit",
     0xa36d02f6245a4129ull,
     {{rrsss::SFI0, 0xa8c7f832281a39c5ull},
      {rrsss::ISG1, 0x95d4f83ffd7546b6ull},
      {rrsss::OSG1, 0xee1a09becc8d24baull},
      {rrsss::PSV0, 0xc7ff980b105afc48ull}},
     "06fb97bfdb878e53f5763eb7c5dac9656f73cb05d3803ce9af28cbbe0a89e2bd",
     7712},
    {Kind::Refraction,
     "refraction",
     0xb72393e05df90a78ull,
     {{rrsss::SFI0, 0xa8c7f832281a39c5ull},
      {rrsss::ISG1, 0x359e39e4d7906a58ull},
      {rrsss::OSG1, 0x5116c835e844e585ull},
      {rrsss::PSV0, 0x4c88c4ce42b8fc9dull}},
     // b723 and 5a24 (the 12672- and 12416-byte builds) both measure this.
     "f8b26ec11e573978654b4008992d89b648c7cfdb101367a2a922066093b218bc",
     7744},
};
// Cheap filter: the profile whose pinned parts the container holds, else nullptr.
inline const Profile *CandidateProfile(const void *code, size_t size,
                                       std::span<const Profile> profiles = Profiles) noexcept {
    for (const auto &p : profiles)
        if (rrsss::ContainerParts(code, size, p.parts, std::size(p.parts)))
            return &p;
    return nullptr;
}
// The structural verdict for one disassembly.
inline const char *StructureVerdict(const Profile &p, const std::string &text) {
    if (!p.structure || !*p.structure)
        return "structure-not-pinned";
    std::string normalized;
    if (!rrsss::StructuralText(text, normalized, p.read_bytes))
        return "scene-constants-not-normalizable";
    if (!rrsss::ShaderDigest(normalized.data(), normalized.size(), p.structure))
        return "structure-mismatch";
    return nullptr;
}

// Counts for the log: unrelated shaders are counted, never logged one by one.
struct ClassifierStats {
    uint64_t unrelated, pinned, candidates, accepted, refused, retried;
    uint64_t micros, slowest_micros; // disassembly and comparison time of the candidates
};

// One slot per distinct candidate (SHA-256 and size): the expensive check runs
// once per shader, and its verdict is reused by every pipeline that has it.
// A missing compiler is not a verdict: it is retried a few times, then given up.
class Classifier {
  public:
    struct Verdict {
        const Target *target{};
        const char *status = "unrelated";
        bool structural{}; // accepted by structure, not by an exact pin
        bool decided{};    // this call made the decision (log it once)
        const Profile *profile{};
        uint64_t micros{};
    };
    static constexpr size_t Capacity = 32;
    static constexpr unsigned Attempts = 3;
    void (*logger)(const char *){};
    explicit Classifier(RecognizedTargets *registry = &Recognized(),
                        std::span<const Profile> profiles = Profiles)
        : registry(registry), profiles(profiles) {}

    // load, if given, supplies the compiler only when a candidate needs it.
    Verdict Classify(const void *code, size_t size, HMODULE module,
                     HMODULE (*load)() = nullptr) noexcept {
        Verdict v;
        try {
            if (!code || !size || size > 4 * 1024 * 1024) {
                v.status = "invalid-bytecode";
                return v;
            }
            // Exact pins first; their sizes gate the full hash.
            bool sized = false;
            for (const auto &t : Targets)
                sized = sized || t.bytes == size;
            if (sized)
                if (const auto *t = FindTarget(rrsss::ShaderHash(code, size), size)) {
                    v.target = t;
                    v.status = "pinned";
                    ++pinned;
                    return v;
                }
            v.profile = CandidateProfile(code, size, profiles);
            if (!v.profile) {
                ++unrelated;
                return v;
            }
            const auto hash = rrsss::ShaderHash(code, size);
            if (const auto *t = registry->Find(hash, size); t && t->kind == v.profile->kind) {
                v.target = t;
                v.status = "recognized";
                v.structural = true;
                return v;
            }
            std::array<unsigned char, 32> digest{};
            if (!rrsss::Sha256(code, size, digest)) {
                v.status = "hash-unavailable";
                return v;
            }
            std::shared_ptr<Slot> slot;
            {
                std::lock_guard lock(mutex);
                for (const auto &s : slots)
                    if (s->size == size && s->digest == digest)
                        slot = s;
                if (!slot) {
                    if (slots.size() >= Capacity) {
                        v.status = "too-many-candidates";
                        return v;
                    }
                    slot = std::make_shared<Slot>();
                    slot->digest = digest;
                    slot->size = size;
                    slots.push_back(slot);
                    ++candidates;
                }
            }
            std::lock_guard lock(slot->mutex);
            if (!slot->done) {
                const auto start = Now();
                slot->result = Decide(*v.profile, code, size, hash,
                                      module || !load ? module : load(), slot->target);
                v.micros = Now() - start;
                rrtiming::Global().Add(rrtiming::Stage::Classify, v.micros * 1000);
                micros += v.micros;
                for (auto seen = slowest.load(); v.micros > seen &&
                                                 !slowest.compare_exchange_weak(seen, v.micros);)
                    ;
                const bool transient = slot->result == Unavailable;
                slot->done = !transient || ++slot->attempts >= Attempts;
                if (transient && !slot->done)
                    ++retried;
                if (slot->done) {
                    v.decided = true;
                    ++(slot->target ? accepted : refused);
                }
            }
            v.status = slot->result;
            v.target = slot->target;
            v.structural = v.target != nullptr;
        } catch (...) {
            v = {};
            v.status = "classification-exception";
        }
        if (v.decided)
            Report(v, code, size);
        return v;
    }
    ClassifierStats Stats() const {
        return {unrelated.load(), pinned.load(),  candidates.load(), accepted.load(),
                refused.load(),   retried.load(), micros.load(),     slowest.load()};
    }

  private:
    static constexpr const char *Unavailable = "shader-compiler-unavailable";
    struct Slot {
        std::array<unsigned char, 32> digest{};
        size_t size{};
        std::mutex mutex;
        bool done{};
        unsigned attempts{};
        const char *result = "not-classified";
        const Target *target{};
    };
    RecognizedTargets *registry;
    std::span<const Profile> profiles;
    std::mutex mutex;
    std::vector<std::shared_ptr<Slot>> slots;
    std::atomic<uint64_t> unrelated{}, pinned{}, candidates{}, accepted{}, refused{}, retried{},
        micros{}, slowest{};

    static uint64_t Now() noexcept {
        LARGE_INTEGER t{}, f{};
        QueryPerformanceCounter(&t);
        QueryPerformanceFrequency(&f);
        return f.QuadPart ? uint64_t(t.QuadPart) * 1000000ull / uint64_t(f.QuadPart) : 0;
    }
    const char *Decide(const Profile &p, const void *code, size_t size, uint64_t hash,
                       HMODULE module, const Target *&target) {
        target = nullptr;
        rrsss::ShaderTools dxc;
        if (!dxc.Create(module))
            return Unavailable;
        std::string text;
        if (!dxc.Disassemble(code, size, text))
            return "shader-disassembly-failed";
        if (const char *why = StructureVerdict(p, text))
            return why;
        target = registry->Add(hash, size, p.kind);
        return target ? "structure-matched" : "registry-full";
    }
    void Report(const Verdict &v, const void *code, size_t size) noexcept {
        if (!logger || !v.profile)
            return;
        try {
            const auto s = Stats();
            char line[384];
            snprintf(line, sizeof(line),
                     "Transparency composite 0x%016llx (%zu B) %s as %s build of 0x%016llx: %s "
                     "(%.1f ms; candidates=%llu accepted=%llu refused=%llu unrelated=%llu)",
                     static_cast<unsigned long long>(rrsss::ShaderHash(code, size)), size,
                     v.target ? "recognized" : "refused", v.profile->name,
                     static_cast<unsigned long long>(v.profile->canonical), v.status,
                     double(v.micros) / 1000.0, static_cast<unsigned long long>(s.candidates),
                     static_cast<unsigned long long>(s.accepted),
                     static_cast<unsigned long long>(s.refused),
                     static_cast<unsigned long long>(s.unrelated));
            logger(line);
        } catch (...) {
        }
    }
};
inline Classifier &Classification() {
    static Classifier classifier;
    return classifier;
}
} // namespace rrblend
