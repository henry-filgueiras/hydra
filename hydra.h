#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <memory>
#include <algorithm>
#include <new>
#include <utility>

namespace hydra {

enum class Kind : uint8_t {
    Small  = 0,
    Medium = 1,
    Large  = 2
};

// ============================================
// Metadata bit layout
// ============================================
//
// bits 0..1   : Kind
// bit  2      : sign
// bits 3..7   : reserved
// bits 8..15  : used inline limbs (for medium)
// bits 16..63 : reserved for future use
//
namespace bits {
    constexpr uint64_t KIND_MASK   = 0x3ull;
    constexpr uint64_t SIGN_MASK   = 0x4ull;

    constexpr uint64_t USED_SHIFT  = 8;
    constexpr uint64_t USED_MASK   = 0xFFull << USED_SHIFT;
}

// ============================================
// Tail-allocated large representation
// ============================================
struct LargeRep {
    uint32_t used;
    uint32_t capacity;

    uint64_t* limbs() noexcept {
        return reinterpret_cast<uint64_t*>(this + 1);
    }

    const uint64_t* limbs() const noexcept {
        return reinterpret_cast<const uint64_t*>(this + 1);
    }

    static LargeRep* create(size_t capacity) {
        const size_t bytes =
            sizeof(LargeRep) + capacity * sizeof(uint64_t);

        auto* rep =
            static_cast<LargeRep*>(::operator new(bytes));

        rep->used = 0;
        rep->capacity = static_cast<uint32_t>(capacity);

        return rep;
    }

    static LargeRep* clone(const LargeRep* src) {
        auto* dst = create(src->capacity);

        dst->used = src->used;

        std::memcpy(
            dst->limbs(),
            src->limbs(),
            src->used * sizeof(uint64_t)
        );

        return dst;
    }

    static void destroy(LargeRep* rep) noexcept {
        ::operator delete(rep);
    }
};

struct DestroyLargeRep {
    void operator()(LargeRep* p) const noexcept {
        if (p) {
            LargeRep::destroy(p);
        }
    }
};

using LargeGuard =
    std::unique_ptr<LargeRep, DestroyLargeRep>;

// ============================================
// Hydra core type
// ============================================
struct Hydra {
    uint64_t meta{};

    union Payload {
        uint64_t small;
        uint64_t medium[3];
        LargeRep* large;

        constexpr Payload() : small(0) {}
    } payload;

    // ----------------------------------------
    // constructors
    // ----------------------------------------
    constexpr Hydra() noexcept
        : meta(make_small_meta()),
          payload() {}

    constexpr Hydra(uint64_t value) noexcept
        : meta(make_small_meta()),
          payload() {
        payload.small = value;
    }

    // ----------------------------------------
    // copy constructor
    // ----------------------------------------
    Hydra(const Hydra& other)
        : meta(other.meta),
          payload() {
        switch (other.kind()) {
            case Kind::Small:
                payload.small = other.payload.small;
                break;

            case Kind::Medium:
                std::memcpy(
                    payload.medium,
                    other.payload.medium,
                    sizeof(payload.medium)
                );
                break;

            case Kind::Large:
                payload.large =
                    LargeRep::clone(other.payload.large);
                break;
        }
    }

    // ----------------------------------------
    // move constructor
    // ----------------------------------------
    Hydra(Hydra&& other) noexcept
        : meta(other.meta),
          payload() {
        payload = other.payload;

        other.meta = make_small_meta();
        other.payload.small = 0;
    }

    // ----------------------------------------
    // destructor
    // ----------------------------------------
    ~Hydra() {
        destroy_if_large();
    }

    // ----------------------------------------
    // copy assignment
    // ----------------------------------------
    Hydra& operator=(const Hydra& other) {
        if (this == &other) return *this;

        destroy_if_large();

        meta = other.meta;

        switch (other.kind()) {
            case Kind::Small:
                payload.small = other.payload.small;
                break;

            case Kind::Medium:
                std::memcpy(
                    payload.medium,
                    other.payload.medium,
                    sizeof(payload.medium)
                );
                break;

            case Kind::Large:
                payload.large =
                    LargeRep::clone(other.payload.large);
                break;
        }

        return *this;
    }

    // ----------------------------------------
    // move assignment
    // ----------------------------------------
    Hydra& operator=(Hydra&& other) noexcept {
        if (this == &other) return *this;

        destroy_if_large();

        meta = other.meta;
        payload = other.payload;

        other.meta = make_small_meta();
        other.payload.small = 0;

        return *this;
    }

    // ----------------------------------------
    // helpers
    // ----------------------------------------
    Kind kind() const noexcept {
        return static_cast<Kind>(
            meta & bits::KIND_MASK
        );
    }

    bool is_small() const noexcept {
        return kind() == Kind::Small;
    }

    bool is_medium() const noexcept {
        return kind() == Kind::Medium;
    }

    bool is_large() const noexcept {
        return kind() == Kind::Large;
    }

    uint8_t used_inline_limbs() const noexcept {
        return static_cast<uint8_t>(
            (meta & bits::USED_MASK) >> bits::USED_SHIFT
        );
    }

    void set_used_inline_limbs(uint8_t n) noexcept {
        meta &= ~bits::USED_MASK;
        meta |= (uint64_t(n) << bits::USED_SHIFT);
    }

    static constexpr uint64_t make_small_meta() noexcept {
        return static_cast<uint64_t>(Kind::Small);
    }

    static constexpr uint64_t make_medium_meta(
        uint8_t used = 0
    ) noexcept {
        return
            static_cast<uint64_t>(Kind::Medium) |
            (uint64_t(used) << bits::USED_SHIFT);
    }

    static constexpr uint64_t make_large_meta() noexcept {
        return static_cast<uint64_t>(Kind::Large);
    }

    void destroy_if_large() noexcept {
        if (is_large() && payload.large) {
            LargeRep::destroy(payload.large);
            payload.large = nullptr;
        }
    }
};

} // namespace hydra
