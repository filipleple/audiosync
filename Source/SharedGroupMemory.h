/*
  ==============================================================================

    SharedGroupMemory.h

    Named shared memory region shared by all AUTOSYNC plugin instances in a
    session.  One instance runs as Master and writes MasterSlot; up to eight
    instances run as Slaves, each writing one SlaveSlot and reading MasterSlot.

    All cross-instance reads/writes use a seqcount pattern (see §5 of
    master-slave-architecture.md).  The atomics involved MUST be lock-free —
    this is asserted at compile time below.

    Platform backends
    -----------------
    POSIX (Linux / macOS):  shm_open + ftruncate + mmap
    Windows:                CreateFileMappingA + MapViewOfFile

    The shared region is named  "/AUTOSYNC2_<groupName>"  on POSIX  (appears
    under /dev/shm/ on Linux) and  "Local\AUTOSYNC2_<groupName>"  on Windows.
    It persists until the OS is rebooted or until explicitly unlinked — this is
    intentional so that a plugin restart attaches to the existing region without
    losing data from other instances.  The "2" suffix guards against attaching
    to segments produced by an older plugin build with a different struct
    layout (nov_ref used to be 400 frames; growing it would overflow an old
    mapping).  Bump this suffix any time MasterSlot / SlaveSlot layout changes.

  ==============================================================================
*/

#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>

#if defined(_WIN32)
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>
#else
  #include <fcntl.h>
  #include <sys/mman.h>
  #include <sys/stat.h>
  #include <unistd.h>
#endif

// ============================================================================
// Shared data structures
// ============================================================================

// Length of the master-side reference novelty ring, in hops (10 ms each).
// Sized to cover realistic LTC-captured anchors: at 20 s the anchored NCC
// search (NARROW_HALF = 30 hops in the plugin) can reach ±19.69 s before the
// window runs out of master history.  Keep MasterSlot::nov_ref and the plugin's
// AudioFallbackState::windowFrames consistent with this value.
static constexpr int MASTER_NOV_REF_SIZE = 2000;

// Written by the master instance; read by every slave.
//
// Memory is zero-initialised by the OS on creation.  The in-class defaults
// below document the intended initial state; they apply only to stack /
// heap instances, not to the OS-mapped region.
struct MasterSlot
{
    // Seqcount: writer increments to odd before write, to even after.
    // Reader retries if value is odd or changes between two reads.
    std::atomic<uint32_t> writeSeq { 0 };

    bool    valid           = false;  // false until master has produced >= 1 window
    uint8_t ltc_state       = 0;      // 0=FAIL, 1=SUSPECT, 2=VALID
    uint8_t locked          = 0;      // tc_data temporal-coherence gate: 0=unlocked, 1=locked
    uint8_t _pad0[1]        = {};     // keep int64_t below 8-byte aligned

    int64_t tc_ref_ms          = 0;      // reference timecode in milliseconds
    int64_t ref_decode_sample  = 0;      // abs stream sample pos of last master LTC decode
    int64_t nov_anchor_sample  = 0;      // abs stream sample pos when novelty was last written
                                         // used by slaves to correct for async SM read latency

    float   Q_ref              = 0.0f;

    int     nov_writePos     = 0;      // circular buffer head (next write index)
    int     nov_framesFilled = 0;      // frames populated so far, saturates at MASTER_NOV_REF_SIZE
    float   nov_ref[MASTER_NOV_REF_SIZE] = {};  // reference novelty curve (20 s at 10 ms/hop, ~8 KB)
};

// Written by one slave instance; read by the master (for the dashboard).
struct SlaveSlot
{
    std::atomic<uint32_t> writeSeq { 0 };

    bool    connected       = false;  // true while the slot is actively being written
    bool    holding         = false;  // delay frozen because master data is stale
    uint8_t ltc_state       = 0;
    uint8_t fusion_src      = 0;      // 0=None, 1=LTC, 2=Audio

    char    label[32]       = {};     // user-assigned label, null-terminated

    int64_t tc_self_ms      = 0;
    float   Q_self          = 0.0f;
    float   estimated_fps   = 0.0f;

    double  dt_ltc_ms       = 0.0;
    double  dt_aud_ms       = 0.0;
    double  conf_aud        = 0.0;
    double  active_delay_ms = 0.0;
};

struct SharedGroup
{
    MasterSlot master;
    SlaveSlot  slaves[8];
};

// The seqcount pattern requires that the uint32_t atomic compiles to a
// hardware-native operation with no hidden mutex.  On x86-64 and ARM64
// (the only platforms that run VST3 plugins) this is always the case.
static_assert(std::atomic<uint32_t>::is_always_lock_free,
    "uint32_t atomic must be lock-free for the seqcount pattern to work in shared memory");

// ============================================================================
// Seqcount helpers (use these everywhere you access shared memory)
// ============================================================================
//
// Writer side (call from audio thread):
//
//   seqcount_write_begin(slot.writeSeq);
//   // ... write fields ...
//   seqcount_write_end(slot.writeSeq);
//
// Reader side (call from audio thread):
//
//   uint32_t seq1, seq2;
//   do {
//       seq1 = seqcount_read_begin(slot.writeSeq);
//       // ... copy fields to local variables ...
//       seq2 = seqcount_read_end(slot.writeSeq);
//   } while (seq1 != seq2);
//
// Both sides are wait-free for the writer.  The reader spins only during the
// brief window when a write is in progress (microseconds at window-rate
// updates of ~200 ms).

inline void seqcount_write_begin(std::atomic<uint32_t>& seq) noexcept
{
    seq.fetch_add(1u, std::memory_order_release);   // make odd
}

inline void seqcount_write_end(std::atomic<uint32_t>& seq) noexcept
{
    seq.fetch_add(1u, std::memory_order_release);   // make even
}

inline uint32_t seqcount_read_begin(const std::atomic<uint32_t>& seq) noexcept
{
    uint32_t v;
    do { v = seq.load(std::memory_order_acquire); } while (v & 1u);
    return v;
}

inline uint32_t seqcount_read_end(const std::atomic<uint32_t>& seq) noexcept
{
    return seq.load(std::memory_order_acquire);
}

// ============================================================================
// SharedGroupMemory
// ============================================================================

class SharedGroupMemory
{
public:
    SharedGroupMemory()  = default;
    ~SharedGroupMemory() { close(); }

    SharedGroupMemory(const SharedGroupMemory&)            = delete;
    SharedGroupMemory& operator=(const SharedGroupMemory&) = delete;

    // Opens (or creates) the named shared memory region.
    // groupName: alphanumeric, max 16 chars, identical across all instances
    //            that belong to the same sync group.
    // Returns true on success.  Safe to call from prepareToPlay().
    bool open(const std::string& groupName)
    {
        close();

#if defined(_WIN32)
        return openWindows(groupName);
#else
        return openPosix(groupName);
#endif
    }

    // Unmaps the region.  Safe to call from releaseResources().
    // Does NOT unlink/destroy the segment — other instances keep their mapping.
    void close()
    {
        if (ptr == nullptr)
            return;

#if defined(_WIN32)
        UnmapViewOfFile(ptr);
        ptr = nullptr;
        if (hMapping != nullptr)
        {
            CloseHandle(hMapping);
            hMapping = nullptr;
        }
#else
        munmap(ptr, sizeof(SharedGroup));
        ptr = nullptr;
        if (fd != -1)
        {
            ::close(fd);
            fd = -1;
        }
#endif
    }

    // Returns a pointer into the mapped region, or nullptr if not open.
    // All field access must go through the seqcount helpers above.
    SharedGroup* get() const noexcept { return ptr; }

    bool isOpen() const noexcept { return ptr != nullptr; }

private:
    SharedGroup* ptr = nullptr;

#if defined(_WIN32)
    HANDLE hMapping = nullptr;

    bool openWindows(const std::string& groupName)
    {
        const std::string name = "Local\\AUTOSYNC2_" + groupName;

        // Try to attach to an existing mapping first (preserves content when
        // other instances are already running).
        hMapping = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, name.c_str());

        if (hMapping == nullptr)
        {
            // No existing mapping — create one backed by the paging file.
            // Windows zero-initialises the memory on creation.
            hMapping = CreateFileMappingA(
                INVALID_HANDLE_VALUE,
                nullptr,
                PAGE_READWRITE,
                0,
                static_cast<DWORD>(sizeof(SharedGroup)),
                name.c_str()
            );
            if (hMapping == nullptr)
                return false;
        }

        void* p = MapViewOfFile(hMapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedGroup));
        if (p == nullptr)
        {
            CloseHandle(hMapping);
            hMapping = nullptr;
            return false;
        }

        ptr = static_cast<SharedGroup*>(p);
        return true;
    }

#else
    int fd = -1;

    bool openPosix(const std::string& groupName)
    {
        const std::string name = "/AUTOSYNC2_" + groupName;

        // Attempt exclusive creation to know whether ftruncate is needed.
        // If it fails with EEXIST another instance already created the segment.
        fd = shm_open(name.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
        const bool isCreator = (fd != -1);

        if (!isCreator)
        {
            // Attach to existing segment (already sized by the creator).
            fd = shm_open(name.c_str(), O_RDWR, 0600);
            if (fd == -1)
                return false;
        }
        else
        {
            // We created the segment; set its size.
            // POSIX guarantees zero-initialisation of new shm regions.
            if (ftruncate(fd, static_cast<off_t>(sizeof(SharedGroup))) == -1)
            {
                ::close(fd);
                fd = -1;
                shm_unlink(name.c_str());
                return false;
            }
        }

        void* p = mmap(nullptr, sizeof(SharedGroup),
                       PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (p == MAP_FAILED)
        {
            ::close(fd);
            fd = -1;
            if (isCreator)
                shm_unlink(name.c_str());
            return false;
        }

        ptr = static_cast<SharedGroup*>(p);
        return true;
    }
#endif
};
