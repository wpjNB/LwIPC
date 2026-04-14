#pragma once

#include <cassert>
#include <cstddef>
#include <utility>

namespace lwipc {

// Forward declaration — actual definition is in object_pool.hpp.
// LoanedSample does not include object_pool.hpp to avoid a circular header
// dependency; it only needs the release() member signature.
template <typename T, std::size_t Capacity>
class ObjectPool;

// ---------------------------------------------------------------------------
// LoanedSample<T, Pool>
//
//  RAII guard that wraps a raw pointer borrowed from an ObjectPool.
//  On destruction (or explicit reset()), the object is automatically
//  returned to the pool — even when exceptions are NOT used.
//
//  Semantics
//  ---------
//  - Move-only (not copyable).
//  - A default-constructed (or moved-from) LoanedSample holds nullptr and
//    does nothing on destruction.
//  - The underlying T is NOT constructed or destroyed by LoanedSample;
//    lifetime management of the T object is the caller's responsibility.
//    For trivially-copyable types, simply write the fields:
//
//        auto loan = pub.loan();
//        if (loan) {
//            loan->x = 1.f;
//            pub.publish(std::move(loan));
//        }
//
//  Pool concept
//  ------------
//  Any type that exposes:   void release(T*) noexcept;
//  ObjectPool<T, N> satisfies this concept.
//
//  Example
//  -------
//    ObjectPool<PointXYZI, 32> pool;
//    {
//        LoanedSample<PointXYZI, decltype(pool)> loan(pool.acquire(), &pool);
//        if (loan) { loan->x = 1.f; }
//    }  // ← automatically released here
// ---------------------------------------------------------------------------
template <typename T, typename Pool>
class LoanedSample {
public:
    // Construct from a raw pointer (may be nullptr) and a back-pointer to pool.
    LoanedSample(T* ptr, Pool* pool) noexcept
        : ptr_(ptr), pool_(pool)
    {}

    // Default-constructed / empty loan
    LoanedSample() noexcept = default;

    // Move constructor — transfers ownership; source becomes empty.
    LoanedSample(LoanedSample&& other) noexcept
        : ptr_(other.ptr_), pool_(other.pool_)
    {
        other.ptr_  = nullptr;
        other.pool_ = nullptr;
    }

    // Move assignment — releases current loan, takes ownership from other.
    LoanedSample& operator=(LoanedSample&& other) noexcept {
        if (this != &other) {
            do_release();
            ptr_  = other.ptr_;
            pool_ = other.pool_;
            other.ptr_  = nullptr;
            other.pool_ = nullptr;
        }
        return *this;
    }

    // Non-copyable
    LoanedSample(const LoanedSample&)            = delete;
    LoanedSample& operator=(const LoanedSample&) = delete;

    // Destructor — returns the object to the pool if not already released.
    ~LoanedSample() noexcept { do_release(); }

    // -----------------------------------------------------------------------
    // Observers
    // -----------------------------------------------------------------------

    [[nodiscard]] T* get()     noexcept { return ptr_; }
    [[nodiscard]] const T* get() const noexcept { return ptr_; }

    T* operator->()        noexcept { assert(ptr_); return ptr_; }
    const T* operator->()  const noexcept { assert(ptr_); return ptr_; }

    T& operator*()         noexcept { assert(ptr_); return *ptr_; }
    const T& operator*()   const noexcept { assert(ptr_); return *ptr_; }

    // Returns true if this loan holds a valid (non-null) pointer.
    explicit operator bool() const noexcept { return ptr_ != nullptr; }

    // -----------------------------------------------------------------------
    // Modifiers
    // -----------------------------------------------------------------------

    /// Manually return the object to the pool; the loan becomes empty.
    void reset() noexcept { do_release(); }

    /// Release ownership without returning to the pool.
    /// The caller takes responsibility for calling pool.release().
    /// TODO: Consider removing if not needed; keeping for publish path.
    T* release_raw() noexcept {
        T* tmp = ptr_;
        ptr_   = nullptr;
        pool_  = nullptr;
        return tmp;
    }

private:
    void do_release() noexcept {
        if (ptr_ && pool_) {
            pool_->release(ptr_);
        }
        ptr_  = nullptr;
        pool_ = nullptr;
    }

    T*    ptr_{nullptr};
    Pool* pool_{nullptr};
};

} // namespace lwipc
