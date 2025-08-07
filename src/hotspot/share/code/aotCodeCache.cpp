/*
 * Copyright (c) 2023, 2025, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */


#include "asm/macroAssembler.hpp"
#include "cds/aotCacheAccess.hpp"
#include "cds/cds_globals.hpp"
#include "cds/cdsConfig.hpp"
#include "cds/heapShared.hpp"
#include "cds/metaspaceShared.hpp"
#include "ci/ciConstant.hpp"
#include "ci/ciEnv.hpp"
#include "ci/ciField.hpp"
#include "ci/ciMethod.hpp"
#include "ci/ciMethodData.hpp"
#include "ci/ciObject.hpp"
#include "ci/ciUtilities.inline.hpp"
#include "classfile/javaAssertions.hpp"
#include "classfile/stringTable.hpp"
#include "classfile/symbolTable.hpp"
#include "classfile/systemDictionary.hpp"
#include "classfile/vmClasses.hpp"
#include "classfile/vmIntrinsics.hpp"
#include "code/aotCodeCache.hpp"
#include "code/codeBlob.hpp"
#include "code/codeCache.hpp"
#include "code/oopRecorder.inline.hpp"
#include "compiler/abstractCompiler.hpp"
#include "compiler/compilationPolicy.hpp"
#include "compiler/compileBroker.hpp"
#include "compiler/compileTask.hpp"
#include "gc/g1/g1BarrierSetRuntime.hpp"
#include "gc/shared/gcConfig.hpp"
#include "logging/logStream.hpp"
#include "memory/memoryReserver.hpp"
#include "memory/universe.hpp"
#include "oops/klass.inline.hpp"
#include "oops/method.inline.hpp"
#include "oops/trainingData.hpp"
#include "prims/jvmtiThreadState.hpp"
#include "runtime/atomic.hpp"
#include "runtime/deoptimization.hpp"
#include "runtime/flags/flagSetting.hpp"
#include "runtime/globals_extension.hpp"
#include "runtime/handles.inline.hpp"
#include "runtime/java.hpp"
#include "runtime/jniHandles.inline.hpp"
#include "runtime/mutexLocker.hpp"
#include "runtime/os.inline.hpp"
#include "runtime/sharedRuntime.hpp"
#include "runtime/stubCodeGenerator.hpp"
#include "runtime/stubRoutines.hpp"
#include "runtime/timerTrace.hpp"
#include "runtime/threadIdentifier.hpp"
#include "utilities/copy.hpp"
#include "utilities/formatBuffer.hpp"
#include "utilities/ostream.hpp"
#include "utilities/spinYield.hpp"
#ifdef COMPILER1
#include "c1/c1_Runtime1.hpp"
#include "c1/c1_LIRAssembler.hpp"
#include "gc/shared/c1/barrierSetC1.hpp"
#include "gc/g1/c1/g1BarrierSetC1.hpp"
#if INCLUDE_SHENANDOAHGC
#include "gc/shenandoah/c1/shenandoahBarrierSetC1.hpp"
#endif // INCLUDE_SHENANDOAHGC
#include "gc/z/c1/zBarrierSetC1.hpp"
#endif // COMPILER1
#ifdef COMPILER2
#include "opto/runtime.hpp"
#endif
#if INCLUDE_JVMCI
#include "jvmci/jvmci.hpp"
#endif
#if INCLUDE_G1GC
#include "gc/g1/g1BarrierSetRuntime.hpp"
#endif
#if INCLUDE_SHENANDOAHGC
#include "gc/shenandoah/shenandoahRuntime.hpp"
#endif
#if INCLUDE_ZGC
#include "gc/z/zBarrierSetRuntime.hpp"
#endif

#include <sys/stat.h>
#include <errno.h>

const char* aot_code_entry_kind_name[] = {
#define DECL_KIND_STRING(kind) XSTR(kind),
  DO_AOTCODEENTRY_KIND(DECL_KIND_STRING)
#undef DECL_KIND_STRING
};

static elapsedTimer _t_totalLoad;
static elapsedTimer _t_totalRegister;
static elapsedTimer _t_totalFind;
static elapsedTimer _t_totalStore;

static bool enable_timers() {
  return CITime || log_is_enabled(Info, init);
}

static void report_load_failure() {
  if (AbortVMOnAOTCodeFailure) {
    vm_exit_during_initialization("Unable to use AOT Code Cache.", nullptr);
  }
  log_info(aot, codecache, init)("Unable to use AOT Code Cache.");
  AOTCodeCache::disable_caching();
}

static void report_store_failure() {
  if (AbortVMOnAOTCodeFailure) {
    tty->print_cr("Unable to create AOT Code Cache.");
    vm_abort(false);
  }
  log_info(aot, codecache, exit)("Unable to create AOT Code Cache.");
  AOTCodeCache::disable_caching();
}

// The sequence of AOT code caching flags and parametters settings.
//
// 1. The initial AOT code caching flags setting is done
// during call to CDSConfig::check_vm_args_consistency().
//
// 2. The earliest AOT code state check done in compilationPolicy_init()
// where we set number of compiler threads for AOT assembly phase.
//
// 3. We determine presence of AOT code in AOT Cache in
// MetaspaceShared::open_static_archive() which is calles
// after compilationPolicy_init() but before codeCache_init().
//
// 4. AOTCodeCache::initialize() is called during universe_init()
// and does final AOT state and flags settings.
//
// 5. Finally AOTCodeCache::init2() is called after universe_init()
// when all GC settings are finalized.

// Next methods determine which action we do with AOT code depending
// on phase of AOT process: assembly or production.

bool AOTCodeCache::is_dumping_adapter() {
  return AOTAdapterCaching && is_on_for_dump();
}

bool AOTCodeCache::is_using_adapter()   {
  return AOTAdapterCaching && is_on_for_use();
}

bool AOTCodeCache::is_dumping_stub() {
  return AOTStubCaching && is_on_for_dump();
}

bool AOTCodeCache::is_using_stub()   {
  return AOTStubCaching && is_on_for_use();
}

bool AOTCodeCache::is_dumping_code() {
  return AOTCodeCaching && is_on_for_dump();
}

bool AOTCodeCache::is_using_code() {
  return AOTCodeCaching && is_on_for_use();
}

// This is used before AOTCodeCahe is initialized
// but after AOT (CDS) Cache flags consistency is checked.
bool AOTCodeCache::maybe_dumping_code() {
  return AOTCodeCaching && CDSConfig::is_dumping_final_static_archive();
}

// Next methods could be called regardless of AOT code cache status.
// Initially they are called during AOT flags parsing and finilized
// in AOTCodeCache::initialize().
void AOTCodeCache::enable_caching() {
  FLAG_SET_ERGO_IF_DEFAULT(AOTCodeCaching, true);
  FLAG_SET_ERGO_IF_DEFAULT(AOTStubCaching, true);
  FLAG_SET_ERGO_IF_DEFAULT(AOTAdapterCaching, true);
}

void AOTCodeCache::disable_caching() {
  FLAG_SET_ERGO(AOTCodeCaching, false);
  FLAG_SET_ERGO(AOTStubCaching, false);
  FLAG_SET_ERGO(AOTAdapterCaching, false);
}

bool AOTCodeCache::is_caching_enabled() {
  return AOTCodeCaching || AOTStubCaching || AOTAdapterCaching;
}

static uint32_t encode_id(AOTCodeEntry::Kind kind, int id) {
  assert(AOTCodeEntry::is_valid_entry_kind(kind), "invalid AOTCodeEntry kind %d", (int)kind);
  // There can be a conflict of id between an Adapter and *Blob, but that should not cause any functional issue
  // becasue both id and kind are used to find an entry, and that combination should be unique
  if (kind == AOTCodeEntry::Adapter) {
    return id;
  } else if (kind == AOTCodeEntry::SharedBlob) {
    return id;
  } else if (kind == AOTCodeEntry::C1Blob) {
    return (int)SharedStubId::NUM_STUBIDS + id;
  } else {
    // kind must be AOTCodeEntry::C2Blob
    return (int)SharedStubId::NUM_STUBIDS + COMPILER1_PRESENT((int)C1StubId::NUM_STUBIDS) + id;
  }
}

static uint _max_aot_code_size = 0;
uint AOTCodeCache::max_aot_code_size() {
  return _max_aot_code_size;
}

bool AOTCodeCache::is_code_load_thread_on() {
  // We cannot trust AOTCodeCache status here, due to bootstrapping circularity.
  // Compilation policy init runs before AOT cache is fully initialized, so the
  // normal AOT cache status check would always fail.
  return UseAOTCodeLoadThread && AOTCodeCaching && CDSConfig::is_using_archive();
}

bool AOTCodeCache::allow_const_field(ciConstant& value) {
  ciEnv* env = CURRENT_ENV;
  precond(env != nullptr);
  assert(!env->is_precompile() || is_dumping_code(), "AOT compilation should be enabled");
  return !env->is_precompile() // Restrict only when we generate AOT code
        // Can not trust primitive too   || !is_reference_type(value.basic_type())
        // May disable this too for now  || is_reference_type(value.basic_type()) && value.as_object()->should_be_constant()
        ;
}

// It is called from MetaspaceShared::initialize_shared_spaces()
// which is called from universe_init().
// At this point all AOT class linking seetings are finilized
// and AOT cache is open so we can map AOT code region.
void AOTCodeCache::initialize() {
  if (!is_caching_enabled()) {
    log_info(aot, codecache, init)("AOT Code Cache is not used: disabled.");
    return;
  }
#if defined(ZERO) || !(defined(AMD64) || defined(AARCH64))
  log_info(aot, codecache, init)("AOT Code Cache is not supported on this platform.");
  disable_caching();
  return;
#else
  assert(!FLAG_IS_DEFAULT(AOTCache), "AOTCache should be specified");

  // Disable stubs caching until JDK-8357398 is fixed.
  FLAG_SET_ERGO(AOTStubCaching, false);

  if (VerifyOops) {
    // Disable AOT stubs caching when VerifyOops flag is on.
    // Verify oops code generated a lot of C strings which overflow
    // AOT C string table (which has fixed size).
    // AOT C string table will be reworked later to handle such cases.
    //
    // Note: AOT adapters are not affected - they don't have oop operations.
    log_info(aot, codecache, init)("AOT Stubs Caching is not supported with VerifyOops.");
    FLAG_SET_ERGO(AOTStubCaching, false);
  }

  bool is_dumping = false;
  bool is_using   = false;
  if (CDSConfig::is_dumping_final_static_archive() && CDSConfig::is_dumping_aot_linked_classes()) {
    is_dumping = is_caching_enabled();
  } else if (CDSConfig::is_using_archive() && CDSConfig::is_using_aot_linked_classes()) {
    is_using = is_caching_enabled();
  }
  if (ClassInitBarrierMode > 0 && !(is_dumping && AOTCodeCaching)) {
    log_info(aot, codecache, init)("Set ClassInitBarrierMode to 0 because AOT Code dumping is off.");
    FLAG_SET_ERGO(ClassInitBarrierMode, 0);
  }
  if (!(is_dumping || is_using)) {
    log_info(aot, codecache, init)("AOT Code Cache is not used: AOT Class Linking is not used.");
    disable_caching();
    return; // AOT code caching disabled on command line
  }
  // Reserve AOT Cache region when we dumping AOT code.
  _max_aot_code_size = AOTCodeMaxSize;
  if (is_dumping && !FLAG_IS_DEFAULT(AOTCodeMaxSize)) {
    if (!is_aligned(AOTCodeMaxSize, os::vm_allocation_granularity())) {
      _max_aot_code_size = align_up(AOTCodeMaxSize, os::vm_allocation_granularity());
      log_debug(aot,codecache,init)("Max AOT Code Cache size is aligned up to %uK", (int)(max_aot_code_size()/K));
    }
  }
  size_t aot_code_size = is_using ? AOTCacheAccess::get_aot_code_region_size() : 0;
  if (is_using && aot_code_size == 0) {
    log_info(aot, codecache, init)("AOT Code Cache is empty");
    disable_caching();
    return;
  }
  if (!open_cache(is_dumping, is_using)) {
    if (is_using) {
      report_load_failure();
    } else {
      report_store_failure();
    }
    return;
  }
  if (is_dumping) {
    FLAG_SET_DEFAULT(FoldStableValues, false);
    FLAG_SET_DEFAULT(ForceUnreachable, true);
  }
  FLAG_SET_DEFAULT(DelayCompilerStubsGeneration, false);
#endif // defined(AMD64) || defined(AARCH64)
}

static AOTCodeCache*  opened_cache = nullptr; // Use this until we verify the cache
AOTCodeCache* AOTCodeCache::_cache = nullptr;
DEBUG_ONLY( bool AOTCodeCache::_passed_init2 = false; )

// It is called after universe_init() when all GC settings are finalized.
void AOTCodeCache::init2() {
  DEBUG_ONLY( _passed_init2 = true; )
  if (opened_cache == nullptr) {
    return;
  }
  // After Universe initialized
  BarrierSet* bs = BarrierSet::barrier_set();
  if (bs->is_a(BarrierSet::CardTableBarrierSet)) {
    address byte_map_base = ci_card_table_address_as<address>();
    if (is_on_for_dump() && !external_word_Relocation::can_be_relocated(byte_map_base)) {
      // Bail out since we can't encode card table base address with relocation
      log_warning(aot, codecache, init)("Can't create AOT Code Cache because card table base address is not relocatable: " INTPTR_FORMAT, p2i(byte_map_base));
      close();
      report_load_failure();
      return;
    }
  }
  if (!opened_cache->verify_config_on_use()) { // Check on AOT code loading
    delete opened_cache;
    opened_cache = nullptr;
    report_load_failure();
    return;
  }

  // initialize aot runtime constants as appropriate to this runtime
  AOTRuntimeConstants::initialize_from_runtime();

  // initialize the table of external routines and initial stubs so we can save
  // generated code blobs that reference them
  AOTCodeAddressTable* table = opened_cache->_table;
  assert(table != nullptr, "should be initialized already");
  table->init_extrs();

  // Now cache and address table are ready for AOT code generation
  _cache = opened_cache;

  // Set ClassInitBarrierMode after all checks since it affects code generation
  if (is_dumping_code()) {
    FLAG_SET_ERGO_IF_DEFAULT(ClassInitBarrierMode, 1);
  } else {
    FLAG_SET_ERGO(ClassInitBarrierMode, 0);
  }
}

bool AOTCodeCache::open_cache(bool is_dumping, bool is_using) {
  opened_cache = new AOTCodeCache(is_dumping, is_using);
  if (opened_cache->failed()) {
    delete opened_cache;
    opened_cache = nullptr;
    return false;
  }
  return true;
}

static void print_helper(nmethod* nm, outputStream* st) {
  AOTCodeCache::iterate([&](AOTCodeEntry* e) {
    if (e->method() == nm->method()) {
      ResourceMark rm;
      stringStream ss;
      ss.print("A%s%d", (e->for_preload() ? "P" : ""), e->comp_level());
      ss.print("[%s%s%s]",
               (e->is_loaded()   ? "L" : ""),
               (e->load_fail()   ? "F" : ""),
               (e->not_entrant() ? "I" : ""));
      ss.print("#%d", e->comp_id());

      st->print(" %s", ss.freeze());
    }
  });
}

void AOTCodeCache::close() {
  if (is_on()) {
    delete _cache; // Free memory
    _cache = nullptr;
    opened_cache = nullptr;
  }
}

class CachedCodeDirectory : public CachedCodeDirectoryInternal {
public:
  uint _aot_code_size;
  char* _aot_code_data;

  void set_aot_code_data(uint size, char* aot_data) {
    _aot_code_size = size;
    AOTCacheAccess::set_pointer(&_aot_code_data, aot_data);
  }

  static CachedCodeDirectory* create();
};

// Storing AOT code in the AOT code region (ac) of AOT Cache:
//
// [1] Use CachedCodeDirectory to keep track of all of data related to AOT code.
//     E.g., you can build a hashtable to record what methods have been archived.
//
// [2] Memory for all data for AOT code, including CachedCodeDirectory, should be
//     allocated using AOTCacheAccess::allocate_aot_code_region().
//
// [3] CachedCodeDirectory must be the very first allocation.
//
// [4] Two kinds of pointer can be stored:
//     - A pointer p that points to metadata. AOTCacheAccess::can_generate_aot_code(p) must return true.
//     - A pointer to a buffer returned by AOTCacheAccess::allocate_aot_code_region().
//       (It's OK to point to an interior location within this buffer).
//     Such pointers must be stored using AOTCacheAccess::set_pointer()
//
// The buffers allocated by AOTCacheAccess::allocate_aot_code_region() are in a contiguous region. At runtime, this
// region is mapped to the process address space. All the pointers in this buffer are relocated as necessary
// (e.g., to account for the runtime location of the CodeCache).
//
// This is always at the very beginning of the mmaped CDS "ac" (AOT code) region
static CachedCodeDirectory* _aot_code_directory = nullptr;

CachedCodeDirectory* CachedCodeDirectory::create() {
  assert(AOTCacheAccess::is_aot_code_region_empty(), "must be");
  CachedCodeDirectory* dir = (CachedCodeDirectory*)AOTCacheAccess::allocate_aot_code_region(sizeof(CachedCodeDirectory));
  dir->dumptime_init_internal();
  return dir;
}

#define DATA_ALIGNMENT HeapWordSize

AOTCodeCache::AOTCodeCache(bool is_dumping, bool is_using) :
  _load_header(nullptr),
  _load_buffer(nullptr),
  _store_buffer(nullptr),
  _C_store_buffer(nullptr),
  _write_position(0),
  _load_size(0),
  _store_size(0),
  _for_use(is_using),
  _for_dump(is_dumping),
  _closing(false),
  _failed(false),
  _lookup_failed(false),
  _for_preload(false),
  _has_clinit_barriers(false),
  _table(nullptr),
  _load_entries(nullptr),
  _search_entries(nullptr),
  _store_entries(nullptr),
  _C_strings_buf(nullptr),
  _store_entries_cnt(0),
  _compile_id(0),
  _comp_level(0)
{
  // Read header at the begining of cache
  if (_for_use) {
    // Read cache
    size_t load_size = AOTCacheAccess::get_aot_code_region_size();
    ReservedSpace rs = MemoryReserver::reserve(load_size, mtCode);
    if (!rs.is_reserved()) {
      log_warning(aot, codecache, init)("Failed to reserved %u bytes of memory for mapping AOT code region into AOT Code Cache", (uint)load_size);
      set_failed();
      return;
    }
    if (!AOTCacheAccess::map_aot_code_region(rs)) {
      log_warning(aot, codecache, init)("Failed to read/mmap AOT code region (ac) into AOT Code Cache");
      set_failed();
      return;
    }
    _aot_code_directory = (CachedCodeDirectory*)rs.base();
    _aot_code_directory->runtime_init_internal();

    _load_size = _aot_code_directory->_aot_code_size;
    _load_buffer = _aot_code_directory->_aot_code_data;
    assert(is_aligned(_load_buffer, DATA_ALIGNMENT), "load_buffer is not aligned");
    log_info(aot, codecache, init)("Mapped %u bytes at address " INTPTR_FORMAT " from AOT Code Cache", _load_size, p2i(_load_buffer));

    _load_header = (Header*)addr(0);
    if (!_load_header->verify(_load_size)) {
      set_failed();
      return;
    }
    log_info (aot, codecache, init)("Loaded %u AOT code entries from AOT Code Cache", _load_header->entries_count());
    log_debug(aot, codecache, init)("  Adapters: total=%u", _load_header->adapters_count());
    log_debug(aot, codecache, init)("  Shared Blobs: total=%u", _load_header->shared_blobs_count());
    log_debug(aot, codecache, init)("  C1 Blobs: total=%u", _load_header->C1_blobs_count());
    log_debug(aot, codecache, init)("  C2 Blobs: total=%u", _load_header->C2_blobs_count());
    log_debug(aot, codecache, init)("  Stubs:    total=%u", _load_header->stubs_count());
    log_debug(aot, codecache, init)("  Nmethods: total=%u", _load_header->nmethods_count());
    log_debug(aot, codecache, init)("  AOT code cache size: %u bytes", _load_header->cache_size());

    // Read strings
    load_strings();
  }
  if (_for_dump) {
    _C_store_buffer = NEW_C_HEAP_ARRAY(char, max_aot_code_size() + DATA_ALIGNMENT, mtCode);
    _store_buffer = align_up(_C_store_buffer, DATA_ALIGNMENT);
    // Entries allocated at the end of buffer in reverse (as on stack).
    _store_entries = (AOTCodeEntry*)align_up(_C_store_buffer + max_aot_code_size(), DATA_ALIGNMENT);
    log_debug(aot, codecache, init)("Allocated store buffer at address " INTPTR_FORMAT " of size %u", p2i(_store_buffer), max_aot_code_size());
  }
  _table = new AOTCodeAddressTable();
}

void AOTCodeCache::invalidate(AOTCodeEntry* entry) {
  // This could be concurent execution
  if (entry != nullptr && is_on()) { // Request could come after cache is closed.
    _cache->invalidate_entry(entry);
  }
}

void AOTCodeCache::init_early_stubs_table() {
  AOTCodeAddressTable* table = addr_table();
  if (table != nullptr) {
    table->init_early_stubs();
  }
}

void AOTCodeCache::init_shared_blobs_table() {
  AOTCodeAddressTable* table = addr_table();
  if (table != nullptr) {
    table->init_shared_blobs();
  }
}

void AOTCodeCache::init_stubs_table() {
  AOTCodeAddressTable* table = addr_table();
  if (table != nullptr) {
    table->init_stubs();
  }
}

void AOTCodeCache::init_early_c1_table() {
  AOTCodeAddressTable* table = addr_table();
  if (table != nullptr) {
    table->init_early_c1();
  }
}

void AOTCodeCache::init_c1_table() {
  AOTCodeAddressTable* table = addr_table();
  if (table != nullptr) {
    table->init_c1();
  }
}

void AOTCodeCache::init_c2_table() {
  AOTCodeAddressTable* table = addr_table();
  if (table != nullptr) {
    table->init_c2();
  }
}

AOTCodeCache::~AOTCodeCache() {
  if (_closing) {
    return; // Already closed
  }
  // Stop any further access to cache.
  // Checked on entry to load_nmethod() and store_nmethod().
  _closing = true;
  if (_for_use) {
    // Wait for all load_nmethod() finish.
    wait_for_no_nmethod_readers();
  }
  // Prevent writing code into cache while we are closing it.
  // This lock held by ciEnv::register_method() which calls store_nmethod().
  MutexLocker ml(Compile_lock);
  if (for_dump()) { // Finalize cache
    finish_write();
  }
  _load_buffer = nullptr;
  if (_C_store_buffer != nullptr) {
    FREE_C_HEAP_ARRAY(char, _C_store_buffer);
    _C_store_buffer = nullptr;
    _store_buffer = nullptr;
  }
  if (_table != nullptr) {
    MutexLocker ml(AOTCodeCStrings_lock, Mutex::_no_safepoint_check_flag);
    delete _table;
    _table = nullptr;
  }
}

void AOTCodeCache::Config::record(uint cpu_features_offset) {
  _flags = 0;
#ifdef ASSERT
  _flags |= debugVM;
#endif
  if (UseCompressedOops) {
    _flags |= compressedOops;
  }
  if (UseCompressedClassPointers) {
    _flags |= compressedClassPointers;
  }
  if (UseTLAB) {
    _flags |= useTLAB;
  }
  if (JavaAssertions::systemClassDefault()) {
    _flags |= systemClassAssertions;
  }
  if (JavaAssertions::userClassDefault()) {
    _flags |= userClassAssertions;
  }
  if (EnableContended) {
    _flags |= enableContendedPadding;
  }
  if (RestrictContended) {
    _flags |= restrictContendedPadding;
  }
  if (PreserveFramePointer) {
    _flags |= preserveFramePointer;
  }
  _codeCacheSize         = pointer_delta(CodeCache::high_bound(), CodeCache::low_bound(), 1);
  _compressedOopShift    = CompressedOops::shift();
  _compressedOopBase     = CompressedOops::base();
  _compressedKlassShift  = CompressedKlassPointers::shift();
  _compressedKlassBase   = CompressedKlassPointers::base();
  _contendedPaddingWidth = ContendedPaddingWidth;
  _objectAlignment       = ObjectAlignmentInBytes;
  _gc                    = (uint)Universe::heap()->kind();
  _cpu_features_offset   = cpu_features_offset;
}

bool AOTCodeCache::Config::verify(AOTCodeCache* cache) const {
  // First checks affect all cached AOT code
#ifdef ASSERT
  if ((_flags & debugVM) == 0) {
    log_debug(aot, codecache, init)("AOT Code Cache disabled: it was created by product VM, it can't be used by debug VM");
    return false;
  }
#else
  if ((_flags & debugVM) != 0) {
    log_debug(aot, codecache, init)("AOT Code Cache disabled: it was created by debug VM, it can't be used by product VM");
    return false;
  }
#endif

  size_t codeCacheSize = pointer_delta(CodeCache::high_bound(), CodeCache::low_bound(), 1);
  if (_codeCacheSize != codeCacheSize) {
    log_debug(aot, codecache, init)("AOT Code Cache disabled: it was created with CodeCache size = %dKb vs current %dKb", (int)(_codeCacheSize/K), (int)(codeCacheSize/K));
    return false;
  }

  CollectedHeap::Name aot_gc = (CollectedHeap::Name)_gc;
  if (aot_gc != Universe::heap()->kind()) {
    log_debug(aot, codecache, init)("AOT Code Cache disabled: it was created with different GC: %s vs current %s", GCConfig::hs_err_name(aot_gc), GCConfig::hs_err_name());
    return false;
  }

  if (_objectAlignment != (uint)ObjectAlignmentInBytes) {
    log_debug(aot, codecache, init)("AOT Code Cache disabled: it was created with ObjectAlignmentInBytes = %d vs current %d", _objectAlignment, ObjectAlignmentInBytes);
    return false;
  }

  if (((_flags & enableContendedPadding) != 0) != EnableContended) {
    log_debug(aot, codecache, init)("AOT Code Cache disabled: it was created with EnableContended = %s vs current %s", (EnableContended ? "false" : "true"), (EnableContended ? "true" : "false"));
    return false;
  }
  if (((_flags & restrictContendedPadding) != 0) != RestrictContended) {
    log_debug(aot, codecache, init)("AOT Code Cache disabled: it was created with RestrictContended = %s vs current %s", (RestrictContended ? "false" : "true"), (RestrictContended ? "true" : "false"));
    return false;
  }
  if (_contendedPaddingWidth != (uint)ContendedPaddingWidth) {
    log_debug(aot, codecache, init)("AOT Code Cache disabled: it was created with ContendedPaddingWidth = %d vs current %d", _contendedPaddingWidth, ContendedPaddingWidth);
    return false;
  }

  if (((_flags & preserveFramePointer) != 0) != PreserveFramePointer) {
    log_debug(aot, codecache, init)("AOT Code Cache disabled: it was created with PreserveFramePointer = %s vs current %s", (PreserveFramePointer ? "false" : "true"), (PreserveFramePointer ? "true" : "false"));
    return false;
  }

  if (((_flags & compressedClassPointers) != 0) != UseCompressedClassPointers) {
    log_debug(aot, codecache, init)("AOT Code Cache disabled: it was created with UseCompressedClassPointers = %s vs current %s", (UseCompressedClassPointers ? "false" : "true"), (UseCompressedClassPointers ? "true" : "false"));
    return false;
  }
  if (_compressedKlassShift != (uint)CompressedKlassPointers::shift()) {
    log_debug(aot, codecache, init)("AOT Code Cache disabled: it was created with CompressedKlassPointers::shift() = %d vs current %d", _compressedKlassShift, CompressedKlassPointers::shift());
    return false;
  }
  if ((_compressedKlassBase == nullptr || CompressedKlassPointers::base() == nullptr) && (_compressedKlassBase != CompressedKlassPointers::base())) {
    log_debug(aot, codecache, init)("AOT Code Cache disabled: incompatible CompressedKlassPointers::base(): %p vs current %p", _compressedKlassBase, CompressedKlassPointers::base());
    return false;
  }

  if (((_flags & compressedOops) != 0) != UseCompressedOops) {
    log_debug(aot, codecache, init)("AOT Code Cache disabled: it was created with UseCompressedOops = %s vs current %s", (UseCompressedOops ? "false" : "true"), (UseCompressedOops ? "true" : "false"));
    return false;
  }
  if (_compressedOopShift != (uint)CompressedOops::shift()) {
    log_debug(aot, codecache, init)("AOT Code Cache disabled: it was created with different CompressedOops::shift(): %d vs current %d", _compressedOopShift, CompressedOops::shift());
    return false;
  }
  if ((_compressedOopBase == nullptr || CompressedOops::base() == nullptr) && (_compressedOopBase != CompressedOops::base())) {
    log_debug(aot, codecache, init)("AOTStubCaching is disabled: incompatible CompressedOops::base(): %p vs current %p", _compressedOopBase, CompressedOops::base());
    return false;
  }

  LogStreamHandle(Debug, aot, codecache, init) log;
  if (log.is_enabled()) {
    log.print_cr("Available CPU features: %s", VM_Version::features_string());
  }

  uint offset = _cpu_features_offset;
  uint cpu_features_size = *(uint *)cache->addr(offset);
  assert(cpu_features_size == (uint)VM_Version::cpu_features_size(), "must be");
  offset += sizeof(uint);

  void* cached_cpu_features_buffer = (void *)cache->addr(offset);
  if (log.is_enabled()) {
    stringStream ss;
    VM_Version::get_cpu_features_name(cached_cpu_features_buffer, ss);
    log.print_cr("CPU features recorded in AOTCodeCache: %s", ss.as_string());
  }

  if (AOTCodeCPUFeatureCheck && !VM_Version::supports_features(cached_cpu_features_buffer)) {
    if (log.is_enabled()) {
      stringStream ss;
      VM_Version::get_missing_features_name(cached_cpu_features_buffer, ss);
      log.print_cr("AOT Code Cache disabled: required cpu features are missing: %s", ss.as_string());
    }
    return false;
  }

  // Next affects only AOT nmethod
  if (((_flags & systemClassAssertions) != 0) != JavaAssertions::systemClassDefault()) {
    log_debug(aot, codecache, init)("AOT Code Cache disabled: it was created with JavaAssertions::systemClassDefault() = %s vs current %s", (JavaAssertions::systemClassDefault() ? "disabled" : "enabled"), (JavaAssertions::systemClassDefault() ? "enabled" : "disabled"));
     FLAG_SET_ERGO(AOTCodeCaching, false);
  }
  if (((_flags & userClassAssertions) != 0) != JavaAssertions::userClassDefault()) {
    log_debug(aot, codecache, init)("AOT Code Cache disabled: it was created with JavaAssertions::userClassDefault() = %s vs current %s", (JavaAssertions::userClassDefault() ? "disabled" : "enabled"), (JavaAssertions::userClassDefault() ? "enabled" : "disabled"));
    FLAG_SET_ERGO(AOTCodeCaching, false);
  }

  return true;
}

bool AOTCodeCache::Header::verify(uint load_size) const {
  if (_version != AOT_CODE_VERSION) {
    log_debug(aot, codecache, init)("AOT Code Cache disabled: different AOT Code version %d vs %d recorded in AOT Code header", AOT_CODE_VERSION, _version);
    return false;
  }
  if (load_size < _cache_size) {
    log_debug(aot, codecache, init)("AOT Code Cache disabled: AOT Code Cache size %d < %d recorded in AOT Code header", load_size, _cache_size);
    return false;
  }
  return true;
}

volatile int AOTCodeCache::_nmethod_readers = 0;

AOTCodeCache* AOTCodeCache::open_for_use() {
  if (AOTCodeCache::is_on_for_use()) {
    return AOTCodeCache::cache();
  }
  return nullptr;
}

AOTCodeCache* AOTCodeCache::open_for_dump() {
  if (AOTCodeCache::is_on_for_dump()) {
    AOTCodeCache* cache = AOTCodeCache::cache();
    cache->clear_lookup_failed(); // Reset bit
    return cache;
  }
  return nullptr;
}

bool AOTCodeCache::is_address_in_aot_cache(address p) {
  AOTCodeCache* cache = open_for_use();
  if (cache == nullptr) {
    return false;
  }
  if ((p >= (address)cache->cache_buffer()) &&
      (p < (address)(cache->cache_buffer() + cache->load_size()))) {
    return true;
  }
  return false;
}

static void copy_bytes(const char* from, address to, uint size) {
  assert((int)size > 0, "sanity");
  memcpy(to, from, size);
  log_trace(aot, codecache)("Copied %d bytes from " INTPTR_FORMAT " to " INTPTR_FORMAT, size, p2i(from), p2i(to));
}

AOTCodeReader::AOTCodeReader(AOTCodeCache* cache, AOTCodeEntry* entry, CompileTask* task) {
  _cache = cache;
  _entry = entry;
  _load_buffer = cache->cache_buffer();
  _read_position = 0;
  if (task != nullptr) {
    _compile_id = task->compile_id();
    _comp_level = task->comp_level();
    _preload    = task->preload();
  } else {
    _compile_id = 0;
    _comp_level = 0;
    _preload    = false;
  }
  _lookup_failed = false;
}

void AOTCodeReader::set_read_position(uint pos) {
  if (pos == _read_position) {
    return;
  }
  assert(pos < _cache->load_size(), "offset:%d >= file size:%d", pos, _cache->load_size());
  _read_position = pos;
}

bool AOTCodeCache::set_write_position(uint pos) {
  if (pos == _write_position) {
    return true;
  }
  if (_store_size < _write_position) {
    _store_size = _write_position; // Adjust during write
  }
  assert(pos < _store_size, "offset:%d >= file size:%d", pos, _store_size);
  _write_position = pos;
  return true;
}

static char align_buffer[256] = { 0 };

bool AOTCodeCache::align_write() {
  // We are not executing code from cache - we copy it by bytes first.
  // No need for big alignment (or at all).
  uint padding = DATA_ALIGNMENT - (_write_position & (DATA_ALIGNMENT - 1));
  if (padding == DATA_ALIGNMENT) {
    return true;
  }
  uint n = write_bytes((const void*)&align_buffer, padding);
  if (n != padding) {
    return false;
  }
  log_trace(aot, codecache)("Adjust write alignment in AOT Code Cache");
  return true;
}

// Check to see if AOT code cache has required space to store "nbytes" of data
address AOTCodeCache::reserve_bytes(uint nbytes) {
  assert(for_dump(), "Code Cache file is not created");
  uint new_position = _write_position + nbytes;
  if (new_position >= (uint)((char*)_store_entries - _store_buffer)) {
    log_warning(aot,codecache)("Failed to ensure %d bytes at offset %d in AOT Code Cache. Increase AOTCodeMaxSize.",
                               nbytes, _write_position);
    set_failed();
    report_store_failure();
    return nullptr;
  }
  address buffer = (address)(_store_buffer + _write_position);
  log_trace(aot, codecache)("Reserved %d bytes at offset %d in AOT Code Cache", nbytes, _write_position);
  _write_position += nbytes;
  if (_store_size < _write_position) {
    _store_size = _write_position;
  }
  return buffer;
}

uint AOTCodeCache::write_bytes(const void* buffer, uint nbytes) {
  assert(for_dump(), "Code Cache file is not created");
  if (nbytes == 0) {
    return 0;
  }
  uint new_position = _write_position + nbytes;
  if (new_position >= (uint)((char*)_store_entries - _store_buffer)) {
    log_warning(aot, codecache)("Failed to write %d bytes at offset %d to AOT Code Cache. Increase AOTCodeMaxSize.",
                                nbytes, _write_position);
    set_failed();
    report_store_failure();
    return 0;
  }
  copy_bytes((const char* )buffer, (address)(_store_buffer + _write_position), nbytes);
  log_trace(aot, codecache)("Wrote %d bytes at offset %d to AOT Code Cache", nbytes, _write_position);
  _write_position += nbytes;
  if (_store_size < _write_position) {
    _store_size = _write_position;
  }
  return nbytes;
}

AOTCodeEntry* AOTCodeCache::find_code_entry(const methodHandle& method, uint comp_level) {
  assert(is_using_code(), "AOT code caching should be enabled");
  switch (comp_level) {
    case CompLevel_simple:
      if ((DisableAOTCode & (1 << 0)) != 0) {
        return nullptr;
      }
      break;
    case CompLevel_limited_profile:
      if ((DisableAOTCode & (1 << 1)) != 0) {
        return nullptr;
      }
      break;
    case CompLevel_full_optimization:
      if ((DisableAOTCode & (1 << 2)) != 0) {
        return nullptr;
      }
      break;

    default: return nullptr; // Level 1, 2, and 4 only
  }
  TraceTime t1("Total time to find AOT code", &_t_totalFind, enable_timers(), false);
  if (is_on() && _cache->cache_buffer() != nullptr) {
    ResourceMark rm;
    const char* target_name = method->name_and_sig_as_C_string();
    uint hash = java_lang_String::hash_code((const jbyte*)target_name, (int)strlen(target_name));
    AOTCodeEntry* entry = _cache->find_entry(AOTCodeEntry::Code, hash, comp_level);
    if (entry == nullptr) {
      log_info(aot, codecache, nmethod)("Missing entry for '%s' (comp_level %d, hash: " UINT32_FORMAT_X_0 ")", target_name, (uint)comp_level, hash);
#ifdef ASSERT
    } else {
      uint name_offset = entry->offset() + entry->name_offset();
      uint name_size   = entry->name_size(); // Includes '/0'
      const char* name = _cache->cache_buffer() + name_offset;
      if (strncmp(target_name, name, name_size) != 0) {
        assert(false, "AOTCodeCache: saved nmethod's name '%s' is different from '%s', hash: " UINT32_FORMAT_X_0, name, target_name, hash);
      }
#endif
    }

    DirectiveSet* directives = DirectivesStack::getMatchingDirective(method, nullptr);
    if (directives->IgnorePrecompiledOption) {
      LogStreamHandle(Info, aot, codecache, compilation) log;
      if (log.is_enabled()) {
        log.print("Ignore AOT code entry on level %d for ", comp_level);
        method->print_value_on(&log);
      }
      return nullptr;
    }

    return entry;
  }
  return nullptr;
}

void* AOTCodeEntry::operator new(size_t x, AOTCodeCache* cache) {
  return (void*)(cache->add_entry());
}

static bool check_entry(AOTCodeEntry::Kind kind, uint id, uint comp_level, AOTCodeEntry* entry) {
  if (entry->kind() == kind) {
    assert(entry->id() == id, "sanity");
    if (kind != AOTCodeEntry::Code || // addapters and stubs have only one version
        // Look only for normal AOT code entry, preload code is handled separately
        (!entry->not_entrant() && !entry->has_clinit_barriers() && (entry->comp_level() == comp_level))) {
      return true; // Found
    }
  }
  return false;
}

AOTCodeEntry* AOTCodeCache::find_entry(AOTCodeEntry::Kind kind, uint id, uint comp_level) {
  assert(_for_use, "sanity");
  uint count = _load_header->entries_count();
  if (_load_entries == nullptr) {
    // Read it
    _search_entries = (uint*)addr(_load_header->entries_offset()); // [id, index]
    _load_entries = (AOTCodeEntry*)(_search_entries + 2 * count);
    log_debug(aot, codecache, init)("Read %d entries table at offset %d from AOT Code Cache", count, _load_header->entries_offset());
  }
  // Binary search
  int l = 0;
  int h = count - 1;
  while (l <= h) {
    int mid = (l + h) >> 1;
    int ix = mid * 2;
    uint is = _search_entries[ix];
    if (is == id) {
      int index = _search_entries[ix + 1];
      AOTCodeEntry* entry = &(_load_entries[index]);
      if (check_entry(kind, id, comp_level, entry)) {
        return entry; // Found
      }
      // Leaner search around
      for (int i = mid - 1; i >= l; i--) { // search back
        ix = i * 2;
        is = _search_entries[ix];
        if (is != id) {
          break;
        }
        index = _search_entries[ix + 1];
        AOTCodeEntry* entry = &(_load_entries[index]);
        if (check_entry(kind, id, comp_level, entry)) {
          return entry; // Found
        }
      }
      for (int i = mid + 1; i <= h; i++) { // search forward
        ix = i * 2;
        is = _search_entries[ix];
        if (is != id) {
          break;
        }
        index = _search_entries[ix + 1];
        AOTCodeEntry* entry = &(_load_entries[index]);
        if (check_entry(kind, id, comp_level, entry)) {
          return entry; // Found
        }
      }
      break; // No match found
    } else if (is < id) {
      l = mid + 1;
    } else {
      h = mid - 1;
    }
  }
  return nullptr;
}

void AOTCodeCache::invalidate_entry(AOTCodeEntry* entry) {
  assert(entry!= nullptr, "all entries should be read already");
  if (entry->not_entrant()) {
    return; // Someone invalidated it already
  }
#ifdef ASSERT
  assert(_load_entries != nullptr, "sanity");
  {
    uint name_offset = entry->offset() + entry->name_offset();
    const char* name = _load_buffer + name_offset;;
    uint level       = entry->comp_level();
    uint comp_id     = entry->comp_id();
    bool for_preload = entry->for_preload();
    bool clinit_brs  = entry->has_clinit_barriers();
    log_info(aot, codecache, nmethod)("Invalidating entry for '%s' (comp_id %d, comp_level %d, hash: " UINT32_FORMAT_X_0 "%s%s)",
                                      name, comp_id, level, entry->id(), (for_preload ? "P" : "A"), (clinit_brs ? ", has clinit barriers" : ""));
  }
  assert(entry->is_loaded(), "invalidate only AOT code in use");
  bool found = false;
  uint count = _load_header->entries_count();
  uint i = 0;
  for(; i < count; i++) {
    if (entry == &(_load_entries[i])) {
      break;
    }
  }
  found = (i < count);
  assert(found, "entry should exist");
#endif
  entry->set_not_entrant();
  uint name_offset = entry->offset() + entry->name_offset();
  const char* name = _load_buffer + name_offset;;
  uint level       = entry->comp_level();
  uint comp_id     = entry->comp_id();
  bool for_preload = entry->for_preload();
  bool clinit_brs  = entry->has_clinit_barriers();
  log_info(aot, codecache, nmethod)("Invalidated entry for '%s' (comp_id %d, comp_level %d, hash: " UINT32_FORMAT_X_0 "%s%s)",
                                    name, comp_id, level, entry->id(), (for_preload ? "P" : "A"), (clinit_brs ? ", has clinit barriers" : ""));

  if (!for_preload && (entry->comp_level() == CompLevel_full_optimization)) {
    // Invalidate preload code if normal AOT C2 code is invalidated,
    // most likely because some dependencies changed during run.
    // We can still use normal AOT code if preload code is
    // invalidated - normal AOT code has less restrictions.
    Method* method = entry->method();
    if (method != nullptr) {
      AOTCodeEntry* preload_entry = method->aot_code_entry();
      if (preload_entry != nullptr) {
        assert(preload_entry->for_preload(), "expecting only such entries here");
        invalidate_entry(preload_entry);
      }
    }
  }
}

void AOTCodeEntry::update_method_for_writing() {
  if (_method != nullptr) {
    _method_offset = AOTCacheAccess::delta_from_base_address((address)_method);
    _method = nullptr;
  }
}

static int uint_cmp(const void *i, const void *j) {
  uint a = *(uint *)i;
  uint b = *(uint *)j;
  return a > b ? 1 : a < b ? -1 : 0;
}

void AOTCodeCache::store_cpu_features(char*& buffer, uint buffer_size) {
  uint* size_ptr = (uint *)buffer;
  *size_ptr = buffer_size;
  buffer += sizeof(uint);

  VM_Version::store_cpu_features(buffer);
  log_debug(aot, codecache, exit)("CPU features recorded in AOTCodeCache: %s", VM_Version::features_string());
  buffer += buffer_size;
  buffer = align_up(buffer, DATA_ALIGNMENT);
}

bool AOTCodeCache::finish_write() {
  if (!align_write()) {
    return false;
  }
  uint strings_offset = _write_position;
  int strings_count = store_strings();
  if (strings_count < 0) {
    return false;
  }
  if (!align_write()) {
    return false;
  }
  uint strings_size = _write_position - strings_offset;

  uint entries_count = 0; // Number of entrant (useful) code entries
  uint entries_offset = _write_position;

  uint code_count = _store_entries_cnt;
  if (code_count > 0) {
    _aot_code_directory = CachedCodeDirectory::create();
    assert(_aot_code_directory != nullptr, "Sanity check");

    uint header_size = (uint)align_up(sizeof(AOTCodeCache::Header), DATA_ALIGNMENT);
    uint search_count = code_count * 2;
    uint search_size = search_count * sizeof(uint);
    uint entries_size = (uint)align_up(code_count * sizeof(AOTCodeEntry), DATA_ALIGNMENT); // In bytes
    uint preload_entries_cnt = 0;
    uint* preload_entries = NEW_C_HEAP_ARRAY(uint, code_count, mtCode);
    uint preload_entries_size = code_count * sizeof(uint);
    // _write_position should include code and strings
    uint code_alignment = code_count * DATA_ALIGNMENT; // We align_up code size when storing it.
    uint cpu_features_size = VM_Version::cpu_features_size();
    uint total_cpu_features_size = sizeof(uint) + cpu_features_size; // sizeof(uint) to store cpu_features_size
    uint total_size = _write_position + header_size + code_alignment +
                      search_size + preload_entries_size + entries_size +
                      align_up(total_cpu_features_size, DATA_ALIGNMENT);
    assert(total_size < max_aot_code_size(), "AOT Code size (" UINT32_FORMAT " bytes) is greater than AOTCodeMaxSize(" UINT32_FORMAT " bytes).", total_size, max_aot_code_size());

    // Allocate in AOT Cache buffer
    char* buffer = (char *)AOTCacheAccess::allocate_aot_code_region(total_size + DATA_ALIGNMENT);
    char* start = align_up(buffer, DATA_ALIGNMENT);
    char* current = start + header_size; // Skip header

    uint cpu_features_offset = current - start;
    store_cpu_features(current, cpu_features_size);
    assert(is_aligned(current, DATA_ALIGNMENT), "sanity check");
    assert(current < start + total_size, "sanity check");

    // Create ordered search table for entries [id, index];
    uint* search = NEW_C_HEAP_ARRAY(uint, search_count, mtCode);

    AOTCodeEntry* entries_address = _store_entries; // Pointer to latest entry
    uint adapters_count = 0;
    uint shared_blobs_count = 0;
    uint C1_blobs_count = 0;
    uint C2_blobs_count = 0;
    uint stubs_count = 0;
    uint nmethods_count = 0;
    uint max_size = 0;
    // AOTCodeEntry entries were allocated in reverse in store buffer.
    // Process them in reverse order to cache first code first.
    for (int i = code_count - 1; i >= 0; i--) {
      AOTCodeEntry* entry = &entries_address[i];
      if (entry->load_fail()) {
        continue;
      }
      if (entry->not_entrant()) {
        log_info(aot, codecache, exit)("Not entrant new entry comp_id: %d, comp_level: %d, hash: " UINT32_FORMAT_X_0 "%s",
                                       entry->comp_id(), entry->comp_level(), entry->id(), (entry->has_clinit_barriers() ? ", has clinit barriers" : ""));
        if (entry->for_preload()) {
          // Skip not entrant preload code:
          // we can't pre-load code which may have failing dependencies.
          continue;
        }
        entry->set_entrant(); // Reset
      } else if (entry->for_preload() && entry->method() != nullptr) {
        // record entrant first version code for pre-loading
        preload_entries[preload_entries_cnt++] = entries_count;
      }
      {
        uint size = align_up(entry->size(), DATA_ALIGNMENT);
        if (size > max_size) {
          max_size = size;
        }
        copy_bytes((_store_buffer + entry->offset()), (address)current, size);
        entry->set_offset(current - start); // New offset
        entry->update_method_for_writing();
        current += size;
        uint n = write_bytes(entry, sizeof(AOTCodeEntry));
        if (n != sizeof(AOTCodeEntry)) {
          FREE_C_HEAP_ARRAY(uint, search);
          return false;
        }
        search[entries_count*2 + 0] = entry->id();
        search[entries_count*2 + 1] = entries_count;
        entries_count++;
        AOTCodeEntry::Kind kind = entry->kind();
        if (kind == AOTCodeEntry::Adapter) {
          adapters_count++;
        } else if (kind == AOTCodeEntry::SharedBlob) {
          shared_blobs_count++;
        } else if (kind == AOTCodeEntry::C1Blob) {
          C1_blobs_count++;
        } else if (kind == AOTCodeEntry::C2Blob) {
          C2_blobs_count++;
        } else if (kind == AOTCodeEntry::Stub) {
          stubs_count++;
        } else {
          assert(kind == AOTCodeEntry::Code, "sanity");
          nmethods_count++;
        }
      }
    }

    if (entries_count == 0) {
      log_info(aot, codecache, exit)("AOT Code Cache was not created: no entires");
      FREE_C_HEAP_ARRAY(uint, search);
      return true; // Nothing to write
    }
    assert(entries_count <= code_count, "%d > %d", entries_count, code_count);
    // Write strings
    if (strings_count > 0) {
      copy_bytes((_store_buffer + strings_offset), (address)current, strings_size);
      strings_offset = (current - start); // New offset
      current += strings_size;
    }
    uint preload_entries_offset = (current - start);
    preload_entries_size = preload_entries_cnt * sizeof(uint);
    if (preload_entries_size > 0) {
      copy_bytes((const char*)preload_entries, (address)current, preload_entries_size);
      current += preload_entries_size;
      log_info(aot, codecache, exit)("Wrote %d preload entries to AOT Code Cache", preload_entries_cnt);
    }
    if (preload_entries != nullptr) {
      FREE_C_HEAP_ARRAY(uint, preload_entries);
    }

    uint new_entries_offset = (current - start); // New offset
    // Sort and store search table
    qsort(search, entries_count, 2*sizeof(uint), uint_cmp);
    search_size = 2 * entries_count * sizeof(uint);
    copy_bytes((const char*)search, (address)current, search_size);
    FREE_C_HEAP_ARRAY(uint, search);
    current += search_size;

    // Write entries
    entries_size = entries_count * sizeof(AOTCodeEntry); // New size
    copy_bytes((_store_buffer + entries_offset), (address)current, entries_size);
    current += entries_size;

    log_stats_on_exit();

    uint size = (current - start);
    assert(size <= total_size, "%d > %d", size , total_size);
    uint blobs_count = shared_blobs_count + C1_blobs_count + C2_blobs_count;
    assert(nmethods_count == (entries_count - (stubs_count + blobs_count + adapters_count)), "sanity");
    log_debug(aot, codecache, exit)("  Adapters: total=%u", adapters_count);
    log_debug(aot, codecache, exit)("  Shared Blobs: total=%u", shared_blobs_count);
    log_debug(aot, codecache, exit)("  C1 Blobs: total=%u", C1_blobs_count);
    log_debug(aot, codecache, exit)("  C2 Blobs: total=%u", C2_blobs_count);
    log_debug(aot, codecache, exit)("  Stubs:    total=%u", stubs_count);
    log_debug(aot, codecache, exit)("  Nmethods: total=%u", nmethods_count);
    log_debug(aot, codecache, exit)("  AOT code cache size: %u bytes, max entry's size: %u bytes", size, max_size);

    // Finalize header
    AOTCodeCache::Header* header = (AOTCodeCache::Header*)start;
    header->init(size, (uint)strings_count, strings_offset,
                 entries_count, new_entries_offset,
                 preload_entries_cnt, preload_entries_offset,
                 adapters_count, shared_blobs_count,
                 C1_blobs_count, C2_blobs_count,
                 stubs_count, cpu_features_offset);

    log_info(aot, codecache, exit)("Wrote %d AOT code entries to AOT Code Cache", entries_count);

    _aot_code_directory->set_aot_code_data(size, start);
  }
  return true;
}

//------------------Store/Load AOT code ----------------------

bool AOTCodeCache::store_code_blob(CodeBlob& blob, AOTCodeEntry::Kind entry_kind, uint id, const char* name, int entry_offset_count, int* entry_offsets) {
  AOTCodeCache* cache = open_for_dump();
  if (cache == nullptr) {
    return false;
  }
  assert(AOTCodeEntry::is_valid_entry_kind(entry_kind), "invalid entry_kind %d", entry_kind);

  if (AOTCodeEntry::is_adapter(entry_kind) && !is_dumping_adapter()) {
    return false;
  }
  if (AOTCodeEntry::is_blob(entry_kind) && !is_dumping_stub()) {
    return false;
  }
  log_debug(aot, codecache, stubs)("Writing blob '%s' (id=%u, kind=%s) to AOT Code Cache", name, id, aot_code_entry_kind_name[entry_kind]);

#ifdef ASSERT
  LogStreamHandle(Trace, aot, codecache, stubs) log;
  if (log.is_enabled()) {
    FlagSetting fs(PrintRelocations, true);
    blob.print_on(&log);
  }
#endif
  // we need to take a lock to prevent race between compiler threads generating AOT code
  // and the main thread generating adapter
  MutexLocker ml(Compile_lock);
  if (!is_on()) {
    return false; // AOT code cache was already dumped and closed.
  }
  if (!cache->align_write()) {
    return false;
  }
  uint entry_position = cache->_write_position;

  // Write name
  uint name_offset = cache->_write_position - entry_position;
  uint name_size = (uint)strlen(name) + 1; // Includes '/0'
  uint n = cache->write_bytes(name, name_size);
  if (n != name_size) {
    return false;
  }

  // Write CodeBlob
  if (!cache->align_write()) {
    return false;
  }
  uint blob_offset = cache->_write_position - entry_position;
  address archive_buffer = cache->reserve_bytes(blob.size());
  if (archive_buffer == nullptr) {
    return false;
  }
  CodeBlob::archive_blob(&blob, archive_buffer);

  uint reloc_data_size = blob.relocation_size();
  n = cache->write_bytes((address)blob.relocation_begin(), reloc_data_size);
  if (n != reloc_data_size) {
    return false;
  }

  bool has_oop_maps = false;
  if (blob.oop_maps() != nullptr) {
    if (!cache->write_oop_map_set(blob)) {
      return false;
    }
    has_oop_maps = true;
  }

#ifndef PRODUCT
  // Write asm remarks
  if (!cache->write_asm_remarks(blob.asm_remarks(), /* use_string_table */ true)) {
    return false;
  }
  if (!cache->write_dbg_strings(blob.dbg_strings(), /* use_string_table */ true)) {
    return false;
  }
#endif /* PRODUCT */

  if (!cache->write_relocations(blob)) {
    if (!cache->failed()) {
      // We may miss an address in AOT table - skip this code blob.
      cache->set_write_position(entry_position);
    }
    return false;
  }

  // Write entries offsets
  n = cache->write_bytes(&entry_offset_count, sizeof(int));
  if (n != sizeof(int)) {
    return false;
  }
  for (int i = 0; i < entry_offset_count; i++) {
    uint32_t off = (uint32_t)entry_offsets[i];
    n = cache->write_bytes(&off, sizeof(uint32_t));
    if (n != sizeof(uint32_t)) {
      return false;
    }
  }
  uint entry_size = cache->_write_position - entry_position;
  AOTCodeEntry* entry = new(cache) AOTCodeEntry(entry_kind, encode_id(entry_kind, id),
                                                entry_position, entry_size, name_offset, name_size,
                                                blob_offset, has_oop_maps, blob.content_begin());
  log_debug(aot, codecache, stubs)("Wrote code blob '%s' (id=%u, kind=%s) to AOT Code Cache", name, id, aot_code_entry_kind_name[entry_kind]);
  return true;
}

CodeBlob* AOTCodeCache::load_code_blob(AOTCodeEntry::Kind entry_kind, uint id, const char* name, int entry_offset_count, int* entry_offsets) {
  AOTCodeCache* cache = open_for_use();
  if (cache == nullptr) {
    return nullptr;
  }
  assert(AOTCodeEntry::is_valid_entry_kind(entry_kind), "invalid entry_kind %d", entry_kind);

  if (AOTCodeEntry::is_adapter(entry_kind) && !is_using_adapter()) {
    return nullptr;
  }
  if (AOTCodeEntry::is_blob(entry_kind) && !is_using_stub()) {
    return nullptr;
  }
  log_debug(aot, codecache, stubs)("Reading blob '%s' (id=%u, kind=%s) from AOT Code Cache", name, id, aot_code_entry_kind_name[entry_kind]);

  AOTCodeEntry* entry = cache->find_entry(entry_kind, encode_id(entry_kind, id));
  if (entry == nullptr) {
    return nullptr;
  }
  AOTCodeReader reader(cache, entry, nullptr);
  CodeBlob* blob = reader.compile_code_blob(name, entry_offset_count, entry_offsets);

  log_debug(aot, codecache, stubs)("%sRead blob '%s' (id=%u, kind=%s) from AOT Code Cache",
                                   (blob == nullptr? "Failed to " : ""), name, id, aot_code_entry_kind_name[entry_kind]);
  return blob;
}

CodeBlob* AOTCodeReader::compile_code_blob(const char* name, int entry_offset_count, int* entry_offsets) {
  uint entry_position = _entry->offset();

  // Read name
  uint name_offset = entry_position + _entry->name_offset();
  uint name_size = _entry->name_size(); // Includes '/0'
  const char* stored_name = addr(name_offset);

  if (strncmp(stored_name, name, (name_size - 1)) != 0) {
    log_warning(aot, codecache, stubs)("Saved blob's name '%s' is different from the expected name '%s'",
                                       stored_name, name);
    set_lookup_failed(); // Skip this blob
    return nullptr;
  }

  // Read archived code blob
  uint offset = entry_position + _entry->code_offset();
  CodeBlob* archived_blob = (CodeBlob*)addr(offset);
  offset += archived_blob->size();

  address reloc_data = (address)addr(offset);
  offset += archived_blob->relocation_size();
  set_read_position(offset);

  ImmutableOopMapSet* oop_maps = nullptr;
  if (_entry->has_oop_maps()) {
    oop_maps = read_oop_map_set();
  }

  CodeBlob* code_blob = CodeBlob::create(archived_blob,
                                         stored_name,
                                         reloc_data,
                                         oop_maps
                                        );
  if (code_blob == nullptr) { // no space left in CodeCache
    return nullptr;
  }

#ifndef PRODUCT
  code_blob->asm_remarks().init();
  read_asm_remarks(code_blob->asm_remarks(), /* use_string_table */ true);
  code_blob->dbg_strings().init();
  read_dbg_strings(code_blob->dbg_strings(), /* use_string_table */ true);
#endif // PRODUCT

  fix_relocations(code_blob);

  // Read entries offsets
  offset = read_position();
  int stored_count = *(int*)addr(offset);
  assert(stored_count == entry_offset_count, "entry offset count mismatch, count in AOT code cache=%d, expected=%d", stored_count, entry_offset_count);
  offset += sizeof(int);
  set_read_position(offset);
  for (int i = 0; i < stored_count; i++) {
    uint32_t off = *(uint32_t*)addr(offset);
    offset += sizeof(uint32_t);
    const char* entry_name = (_entry->kind() == AOTCodeEntry::Adapter) ? AdapterHandlerEntry::entry_name(i) : "";
    log_trace(aot, codecache, stubs)("Reading adapter '%s:%s' (0x%x) offset: 0x%x from AOT Code Cache",
                                      stored_name, entry_name, _entry->id(), off);
    entry_offsets[i] = off;
  }

#ifdef ASSERT
  LogStreamHandle(Trace, aot, codecache, stubs) log;
  if (log.is_enabled()) {
    FlagSetting fs(PrintRelocations, true);
    code_blob->print_on(&log);
  }
#endif
  return code_blob;
}

bool AOTCodeCache::store_stub(StubCodeGenerator* cgen, vmIntrinsicID id, const char* name, address start) {
  if (!is_dumping_stub()) {
    return false;
  }
  AOTCodeCache* cache = open_for_dump();
  if (cache == nullptr) {
    return false;
  }
  log_info(aot, codecache, stubs)("Writing stub '%s' id:%d to AOT Code Cache", name, (int)id);
  if (!cache->align_write()) {
    return false;
  }
#ifdef ASSERT
  CodeSection* cs = cgen->assembler()->code_section();
  if (cs->has_locs()) {
    uint reloc_count = cs->locs_count();
    tty->print_cr("======== write stubs code section relocations [%d]:", reloc_count);
    // Collect additional data
    RelocIterator iter(cs);
    while (iter.next()) {
      switch (iter.type()) {
        case relocInfo::none:
          break;
        default: {
          iter.print_current_on(tty);
          fatal("stub's relocation %d unimplemented", (int)iter.type());
          break;
        }
      }
    }
  }
#endif
  uint entry_position = cache->_write_position;

  // Write code
  uint code_offset = 0;
  uint code_size = cgen->assembler()->pc() - start;
  uint n = cache->write_bytes(start, code_size);
  if (n != code_size) {
    return false;
  }
  // Write name
  uint name_offset = cache->_write_position - entry_position;
  uint name_size = (uint)strlen(name) + 1; // Includes '/0'
  n = cache->write_bytes(name, name_size);
  if (n != name_size) {
    return false;
  }
  uint entry_size = cache->_write_position - entry_position;
  AOTCodeEntry* entry = new(cache) AOTCodeEntry(entry_position, entry_size, name_offset, name_size,
                                                code_offset, code_size,
                                                AOTCodeEntry::Stub, (uint32_t)id);
  log_info(aot, codecache, stubs)("Wrote stub '%s' id:%d to AOT Code Cache", name, (int)id);
  return true;
}

bool AOTCodeCache::load_stub(StubCodeGenerator* cgen, vmIntrinsicID id, const char* name, address start) {
  if (!is_using_stub()) {
    return false;
  }
  assert(start == cgen->assembler()->pc(), "wrong buffer");
  AOTCodeCache* cache = open_for_use();
  if (cache == nullptr) {
    return false;
  }
  AOTCodeEntry* entry = cache->find_entry(AOTCodeEntry::Stub, (uint)id);
  if (entry == nullptr) {
    return false;
  }
  uint entry_position = entry->offset();
  // Read name
  uint name_offset = entry->name_offset() + entry_position;
  uint name_size   = entry->name_size(); // Includes '/0'
  const char* saved_name = cache->addr(name_offset);
  if (strncmp(name, saved_name, (name_size - 1)) != 0) {
    log_warning(aot, codecache)("Saved stub's name '%s' is different from '%s' for id:%d", saved_name, name, (int)id);
    cache->set_failed();
    report_load_failure();
    return false;
  }
  log_info(aot, codecache, stubs)("Reading stub '%s' id:%d from AOT Code Cache", name, (int)id);
  // Read code
  uint code_offset = entry->code_offset() + entry_position;
  uint code_size   = entry->code_size();
  copy_bytes(cache->addr(code_offset), start, code_size);
  cgen->assembler()->code_section()->set_end(start + code_size);
  log_info(aot, codecache, stubs)("Read stub '%s' id:%d from AOT Code Cache", name, (int)id);
  return true;
}

AOTCodeEntry* AOTCodeCache::store_nmethod(nmethod* nm, AbstractCompiler* compiler, bool for_preload) {
  if (!is_dumping_code()) {
    return nullptr;
  }
  assert(CDSConfig::is_dumping_aot_code(), "should be called only when allowed");
  AOTCodeCache* cache = open_for_dump();
  precond(cache != nullptr);
  precond(!nm->is_osr_method()); // AOT compilation is requested only during AOT cache assembly phase
  if (!compiler->is_c1() && !compiler->is_c2()) {
    // Only c1 and c2 compilers
    return nullptr;
  }
  int comp_level = nm->comp_level();
  if (comp_level == CompLevel_full_profile) {
    // Do not cache C1 compiles with full profile i.e. tier3
    return nullptr;
  }
  assert(comp_level == CompLevel_simple || comp_level == CompLevel_limited_profile || comp_level == CompLevel_full_optimization, "must be");

  TraceTime t1("Total time to store AOT code", &_t_totalStore, enable_timers(), false);
  AOTCodeEntry* entry = nullptr;
  entry = cache->write_nmethod(nm, for_preload);
  if (entry == nullptr) {
    log_info(aot, codecache, nmethod)("%d (L%d): nmethod store attempt failed", nm->compile_id(), comp_level);
  }
  return entry;
}

AOTCodeEntry* AOTCodeCache::write_nmethod(nmethod* nm, bool for_preload) {
  AOTCodeCache* cache = open_for_dump();
  assert(cache != nullptr, "sanity check");
  assert(!nm->has_clinit_barriers() || (ClassInitBarrierMode > 0), "sanity");
  uint comp_id = nm->compile_id();
  uint comp_level = nm->comp_level();
  Method* method = nm->method();
  if (!AOTCacheAccess::can_generate_aot_code(method)) {
    ResourceMark rm;
    log_info(aot, codecache, nmethod)("%d (L%d): Skip method '%s' for AOT%s compile: not in AOT cache", comp_id, (int)comp_level, method->name_and_sig_as_C_string(), (for_preload ? " preload" : ""));
    assert(AOTCacheAccess::can_generate_aot_code(method), "sanity");
    return nullptr;
  }
  bool method_in_cds = MetaspaceShared::is_in_shared_metaspace((address)method);
  InstanceKlass* holder = method->method_holder();
  bool klass_in_cds = holder->is_shared() && !holder->defined_by_other_loaders();
  bool builtin_loader = holder->class_loader_data()->is_builtin_class_loader_data();
  if (!builtin_loader) {
    ResourceMark rm;
    log_info(aot, codecache, nmethod)("%d (L%d): Skip method '%s' loaded by custom class loader %s", comp_id, (int)comp_level, method->name_and_sig_as_C_string(), holder->class_loader_data()->loader_name());
    assert(builtin_loader, "sanity");
    return nullptr;
  }
  if (for_preload && !(method_in_cds && klass_in_cds)) {
    ResourceMark rm;
    log_info(aot, codecache, nmethod)("%d (L%d): Skip method '%s' for preload: not in CDS", comp_id, (int)comp_level, method->name_and_sig_as_C_string());
    assert(!for_preload || (method_in_cds && klass_in_cds), "sanity");
    return nullptr;
  }
  assert(!for_preload || (method_in_cds && klass_in_cds), "sanity");
  _for_preload = for_preload;
  _has_clinit_barriers = nm->has_clinit_barriers();

  if (!align_write()) {
    return nullptr;
  }

  uint entry_position = _write_position;

  // Write name
  uint name_offset = 0;
  uint name_size   = 0;
  uint hash = 0;
  uint n;
  {
    ResourceMark rm;
    const char* name = method->name_and_sig_as_C_string();
    log_info(aot, codecache, nmethod)("%d (L%d): Writing nmethod '%s' (comp level: %d, %s) to AOT Code Cache",
                                      comp_id, (int)comp_level, name, comp_level,
                                      (nm->has_clinit_barriers() ? ", has clinit barriers" : ""));

    LogStreamHandle(Info, aot, codecache, loader) log;
    if (log.is_enabled()) {
      oop loader = holder->class_loader();
      oop domain = holder->protection_domain();
      log.print("Holder: ");
      holder->print_value_on(&log);
      log.print(" loader: ");
      if (loader == nullptr) {
        log.print("nullptr");
      } else {
        loader->print_value_on(&log);
      }
      log.print(" domain: ");
      if (domain == nullptr) {
        log.print("nullptr");
      } else {
        domain->print_value_on(&log);
      }
      log.cr();
    }
    name_offset = _write_position  - entry_position;
    name_size   = (uint)strlen(name) + 1; // Includes '/0'
    n = write_bytes(name, name_size);
    if (n != name_size) {
      return nullptr;
    }
    hash = java_lang_String::hash_code((const jbyte*)name, (int)strlen(name));
  }

  // Write CodeBlob
  if (!cache->align_write()) {
    return nullptr;
  }
  uint blob_offset = cache->_write_position - entry_position;
  address archive_buffer = cache->reserve_bytes(nm->size());
  if (archive_buffer == nullptr) {
    return nullptr;
  }
  CodeBlob::archive_blob(nm, archive_buffer);

  uint reloc_data_size = nm->relocation_size();
  n = write_bytes((address)nm->relocation_begin(), reloc_data_size);
  if (n != reloc_data_size) {
    return nullptr;
  }

  // Write oops and metadata present in the nmethod's data region
  if (!write_oops(nm)) {
    if (lookup_failed() && !failed()) {
      // Skip this method and reposition file
      set_write_position(entry_position);
    }
    return nullptr;
  }
  if (!write_metadata(nm)) {
    if (lookup_failed() && !failed()) {
      // Skip this method and reposition file
      set_write_position(entry_position);
    }
    return nullptr;
  }

  bool has_oop_maps = false;
  if (nm->oop_maps() != nullptr) {
    if (!cache->write_oop_map_set(*nm)) {
      return nullptr;
    }
    has_oop_maps = true;
  }

  uint immutable_data_size = nm->immutable_data_size();
  n = write_bytes(nm->immutable_data_begin(), immutable_data_size);
  if (n != immutable_data_size) {
    return nullptr;
  }

  JavaThread* thread = JavaThread::current();
  HandleMark hm(thread);
  GrowableArray<Handle> oop_list;
  GrowableArray<Metadata*> metadata_list;

  nm->create_reloc_immediates_list(thread, oop_list, metadata_list);
  if (!write_nmethod_reloc_immediates(oop_list, metadata_list)) {
    if (lookup_failed() && !failed()) {
      // Skip this method and reposition file
      set_write_position(entry_position);
    }
    return nullptr;
  }

  if (!write_relocations(*nm, &oop_list, &metadata_list)) {
    return nullptr;
  }

#ifndef PRODUCT
  if (!cache->write_asm_remarks(nm->asm_remarks(), /* use_string_table */ false)) {
    return nullptr;
  }
  if (!cache->write_dbg_strings(nm->dbg_strings(), /* use_string_table */ false)) {
    return nullptr;
  }
#endif /* PRODUCT */

  uint entry_size = _write_position - entry_position;
  AOTCodeEntry* entry = new (this) AOTCodeEntry(AOTCodeEntry::Code, hash,
                                                entry_position, entry_size,
                                                name_offset, name_size,
                                                blob_offset, has_oop_maps,
                                                nm->content_begin(), comp_level, comp_id,
                                                nm->has_clinit_barriers(), for_preload);
  if (method_in_cds) {
    entry->set_method(method);
  }
#ifdef ASSERT
  if (nm->has_clinit_barriers() || for_preload) {
    assert(for_preload, "sanity");
    assert(entry->method() != nullptr, "sanity");
  }
#endif
  {
    ResourceMark rm;
    const char* name = nm->method()->name_and_sig_as_C_string();
    log_info(aot, codecache, nmethod)("%d (L%d): Wrote nmethod '%s'%s to AOT Code Cache",
                           comp_id, (int)comp_level, name, (for_preload ? " (for preload)" : ""));
  }
  if (VerifyAOTCode) {
    return nullptr;
  }
  return entry;
}

bool AOTCodeCache::load_nmethod(ciEnv* env, ciMethod* target, int entry_bci, AbstractCompiler* compiler, CompLevel comp_level) {
  if (!is_using_code()) {
    return false;
  }
  AOTCodeCache* cache = open_for_use();
  if (cache == nullptr) {
    return false;
  }
  assert(entry_bci == InvocationEntryBci, "unexpected entry_bci=%d", entry_bci);
  TraceTime t1("Total time to load AOT code", &_t_totalLoad, enable_timers(), false);
  CompileTask* task = env->task();
  task->mark_aot_load_start(os::elapsed_counter());
  AOTCodeEntry* entry = task->aot_code_entry();
  bool preload = task->preload();
  assert(entry != nullptr, "sanity");
  if (log_is_enabled(Info, aot, codecache, nmethod)) {
    VM_ENTRY_MARK;
    ResourceMark rm;
    methodHandle method(THREAD, target->get_Method());
    const char* target_name = method->name_and_sig_as_C_string();
    uint hash = java_lang_String::hash_code((const jbyte*)target_name, (int)strlen(target_name));
    bool clinit_brs = entry->has_clinit_barriers();
    log_info(aot, codecache, nmethod)("%d (L%d): %s nmethod '%s' (hash: " UINT32_FORMAT_X_0 "%s)",
                                      task->compile_id(), task->comp_level(), (preload ? "Preloading" : "Reading"),
                                      target_name, hash, (clinit_brs ? ", has clinit barriers" : ""));
  }
  ReadingMark rdmk;
  if (rdmk.failed()) {
    // Cache is closed, cannot touch anything.
    return false;
  }

  AOTCodeReader reader(cache, entry, task);
  bool success = reader.compile_nmethod(env, target, compiler);
  if (success) {
    task->set_num_inlined_bytecodes(entry->num_inlined_bytecodes());
  } else {
    entry->set_load_fail();
    entry->set_not_entrant();
  }
  task->mark_aot_load_finish(os::elapsed_counter());
  return success;
}

bool AOTCodeReader::compile_nmethod(ciEnv* env, ciMethod* target, AbstractCompiler* compiler) {
  CompileTask* task = env->task();
  AOTCodeEntry* aot_code_entry = (AOTCodeEntry*)_entry;
  nmethod* nm = nullptr;

  uint entry_position = aot_code_entry->offset();
  uint archived_nm_offset = entry_position + aot_code_entry->code_offset();
  nmethod* archived_nm = (nmethod*)addr(archived_nm_offset);
  set_read_position(archived_nm_offset + archived_nm->size());

  OopRecorder* oop_recorder = new OopRecorder(env->arena());
  env->set_oop_recorder(oop_recorder);

  uint offset;

  offset = read_position();
  address reloc_data = (address)addr(offset);
  offset += archived_nm->relocation_size();
  set_read_position(offset);

  // Read oops and metadata
  VM_ENTRY_MARK
  GrowableArray<Handle> oop_list;
  GrowableArray<Metadata*> metadata_list;

  if (!read_oop_metadata_list(THREAD, target, oop_list, metadata_list, oop_recorder)) {
   return false;
  }

  ImmutableOopMapSet* oopmaps = read_oop_map_set();

  offset = read_position();
  address immutable_data = (address)addr(offset);
  offset += archived_nm->immutable_data_size();
  set_read_position(offset);

  GrowableArray<Handle> reloc_immediate_oop_list;
  GrowableArray<Metadata*> reloc_immediate_metadata_list;
  if (!read_oop_metadata_list(THREAD, target, reloc_immediate_oop_list, reloc_immediate_metadata_list, nullptr)) {
   return false;
  }

  // Read Dependencies (compressed already)
  Dependencies* dependencies = new Dependencies(env);
  dependencies->set_content(immutable_data, archived_nm->dependencies_size());
  env->set_dependencies(dependencies);

  const char* name = addr(entry_position + aot_code_entry->name_offset());

  if (VerifyAOTCode) {
    return false;
  }

  TraceTime t1("Total time to register AOT nmethod", &_t_totalRegister, enable_timers(), false);
  nm = env->register_aot_method(THREAD,
                                target,
                                compiler,
                                archived_nm,
                                reloc_data,
                                oop_list,
                                metadata_list,
                                oopmaps,
                                immutable_data,
                                reloc_immediate_oop_list,
                                reloc_immediate_metadata_list,
                                this);
  bool success = task->is_success();
  if (success) {
    log_info(aot, codecache, nmethod)("%d (L%d): Read nmethod '%s' from AOT Code Cache", compile_id(), comp_level(), name);
#ifdef ASSERT
    LogStreamHandle(Debug, aot, codecache, nmethod) log;
    if (log.is_enabled()) {
      FlagSetting fs(PrintRelocations, true);
      nm->print_on(&log);
      nm->decode2(&log);
    }
#endif
  }

  return success;
}

bool skip_preload(methodHandle mh) {
  if (!mh->method_holder()->is_loaded()) {
    return true;
  }
  DirectiveSet* directives = DirectivesStack::getMatchingDirective(mh, nullptr);
  if (directives->DontPreloadOption) {
    LogStreamHandle(Info, aot, codecache, init) log;
    if (log.is_enabled()) {
      log.print("Exclude preloading code for ");
      mh->print_value_on(&log);
    }
    return true;
  }
  return false;
}

void AOTCodeCache::preload_code(JavaThread* thread) {
  if (!is_using_code()) {
    return;
  }
  if ((DisableAOTCode & (1 << 3)) != 0) {
    return; // no preloaded code (level 5);
  }
  _cache->preload_aot_code(thread);
}

void AOTCodeCache::preload_aot_code(TRAPS) {
  if (CompilationPolicy::compiler_count(CompLevel_full_optimization) == 0) {
    // Since we reuse the CompilerBroker API to install AOT code, we're required to have a JIT compiler for the
    // level we want (that is CompLevel_full_optimization).
    return;
  }
  assert(_for_use, "sanity");
  uint count = _load_header->entries_count();
  if (_load_entries == nullptr) {
    // Read it
    _search_entries = (uint*)addr(_load_header->entries_offset()); // [id, index]
    _load_entries = (AOTCodeEntry*)(_search_entries + 2 * count);
    log_info(aot, codecache, init)("Read %d entries table at offset %d from AOT Code Cache", count, _load_header->entries_offset());
  }
  uint preload_entries_count = _load_header->preload_entries_count();
  if (preload_entries_count > 0) {
    uint* entries_index = (uint*)addr(_load_header->preload_entries_offset());
    log_info(aot, codecache, init)("Load %d preload entries from AOT Code Cache", preload_entries_count);
    uint count = MIN2(preload_entries_count, AOTCodeLoadStop);
    for (uint i = AOTCodeLoadStart; i < count; i++) {
      uint index = entries_index[i];
      AOTCodeEntry* entry = &(_load_entries[index]);
      if (entry->not_entrant()) {
        continue;
      }
      Method* m = AOTCacheAccess::convert_offset_to_method(entry->method_offset());
      entry->set_method(m);
      methodHandle mh(THREAD, entry->method());
      assert((mh.not_null() && MetaspaceShared::is_in_shared_metaspace((address)mh())), "sanity");
      if (skip_preload(mh)) {
        continue; // Exclude preloading for this method
      }
      assert(mh->method_holder()->is_loaded(), "");
      if (!mh->method_holder()->is_linked()) {
        assert(!HAS_PENDING_EXCEPTION, "");
        mh->method_holder()->link_class(THREAD);
        if (HAS_PENDING_EXCEPTION) {
          LogStreamHandle(Info, aot, codecache) log;
          if (log.is_enabled()) {
            ResourceMark rm;
            log.print("Linkage failed for %s: ", mh->method_holder()->external_name());
            THREAD->pending_exception()->print_value_on(&log);
            if (log_is_enabled(Debug, aot, codecache)) {
              THREAD->pending_exception()->print_on(&log);
            }
          }
          CLEAR_PENDING_EXCEPTION;
        }
      }
      if (mh->aot_code_entry() != nullptr) {
        // Second C2 compilation of the same method could happen for
        // different reasons without marking first entry as not entrant.
        continue; // Keep old entry to avoid issues
      }
      mh->set_aot_code_entry(entry);
      CompileBroker::compile_method(mh, InvocationEntryBci, CompLevel_full_optimization, 0, false, CompileTask::Reason_Preload, CHECK);
    }
  }
}

// ------------ process code and data --------------

// Can't use -1. It is valid value for jump to iteself destination
// used by static call stub: see NativeJump::jump_destination().
#define BAD_ADDRESS_ID -2

bool AOTCodeCache::write_relocations(CodeBlob& code_blob, GrowableArray<Handle>* oop_list, GrowableArray<Metadata*>* metadata_list) {
  GrowableArray<uint> reloc_data;
  RelocIterator iter(&code_blob);
  LogStreamHandle(Trace, aot, codecache, reloc) log;
  while (iter.next()) {
    int idx = reloc_data.append(0); // default value
    switch (iter.type()) {
      case relocInfo::none:
      break;
      case relocInfo::oop_type: {
        oop_Relocation* r = (oop_Relocation*)iter.reloc();
        if (r->oop_is_immediate()) {
          assert(oop_list != nullptr, "sanity check");
          // store index of oop in the reloc immediate oop list
          Handle h(JavaThread::current(), r->oop_value());
          int oop_idx = oop_list->find(h);
          assert(oop_idx != -1, "sanity check");
          reloc_data.at_put(idx, (uint)oop_idx);
        }
        break;
      }
      case relocInfo::metadata_type: {
        metadata_Relocation* r = (metadata_Relocation*)iter.reloc();
        if (r->metadata_is_immediate()) {
          assert(metadata_list != nullptr, "sanity check");
          // store index of metadata in the reloc immediate metadata list
          int metadata_idx = metadata_list->find(r->metadata_value());
          assert(metadata_idx != -1, "sanity check");
          reloc_data.at_put(idx, (uint)metadata_idx);
        }
        break;
      }
      case relocInfo::virtual_call_type:  // Fall through. They all call resolve_*_call blobs.
      case relocInfo::opt_virtual_call_type:
      case relocInfo::static_call_type: {
        CallRelocation* r = (CallRelocation*)iter.reloc();
        address dest = r->destination();
        if (dest == r->addr()) { // possible call via trampoline on Aarch64
          dest = (address)-1;    // do nothing in this case when loading this relocation
        }
        int id = _table->id_for_address(dest, iter, &code_blob);
        if (id == BAD_ADDRESS_ID) {
          return false;
        }
        reloc_data.at_put(idx, id);
        break;
      }
      case relocInfo::trampoline_stub_type: {
        address dest = ((trampoline_stub_Relocation*)iter.reloc())->destination();
        int id = _table->id_for_address(dest, iter, &code_blob);
        if (id == BAD_ADDRESS_ID) {
          return false;
        }
        reloc_data.at_put(idx, id);
        break;
      }
      case relocInfo::static_stub_type:
        break;
      case relocInfo::runtime_call_type: {
        // Record offset of runtime destination
        CallRelocation* r = (CallRelocation*)iter.reloc();
        address dest = r->destination();
        if (dest == r->addr()) { // possible call via trampoline on Aarch64
          dest = (address)-1;    // do nothing in this case when loading this relocation
        }
        int id = _table->id_for_address(dest, iter, &code_blob);
        if (id == BAD_ADDRESS_ID) {
          return false;
        }
        reloc_data.at_put(idx, id);
        break;
      }
      case relocInfo::runtime_call_w_cp_type:
        log_debug(aot, codecache, reloc)("runtime_call_w_cp_type relocation is not implemented");
        return false;
      case relocInfo::external_word_type: {
        // Record offset of runtime target
        address target = ((external_word_Relocation*)iter.reloc())->target();
        int id = _table->id_for_address(target, iter, &code_blob);
        if (id == BAD_ADDRESS_ID) {
          return false;
        }
        reloc_data.at_put(idx, id);
        break;
      }
      case relocInfo::internal_word_type:
        break;
      case relocInfo::section_word_type:
        break;
      case relocInfo::poll_type:
        break;
      case relocInfo::poll_return_type:
        break;
      case relocInfo::post_call_nop_type:
        break;
      case relocInfo::entry_guard_type:
        break;
      default:
        log_debug(aot, codecache, reloc)("relocation %d unimplemented", (int)iter.type());
        return false;
        break;
    }
    if (log.is_enabled()) {
      iter.print_current_on(&log);
    }
  }

  // Write additional relocation data: uint per relocation
  // Write the count first
  int count = reloc_data.length();
  write_bytes(&count, sizeof(int));
  for (GrowableArrayIterator<uint> iter = reloc_data.begin();
       iter != reloc_data.end(); ++iter) {
    uint value = *iter;
    int n = write_bytes(&value, sizeof(uint));
    if (n != sizeof(uint)) {
      return false;
    }
  }
  return true;
}

void AOTCodeReader::fix_relocations(CodeBlob* code_blob, GrowableArray<Handle>* oop_list, GrowableArray<Metadata*>* metadata_list) {
  LogStreamHandle(Trace, aot, reloc) log;
  uint offset = read_position();
  int count = *(int*)addr(offset);
  offset += sizeof(int);
  if (log.is_enabled()) {
    log.print_cr("======== extra relocations count=%d", count);
  }
  uint* reloc_data = (uint*)addr(offset);
  offset += (count * sizeof(uint));
  set_read_position(offset);

  RelocIterator iter(code_blob);
  int j = 0;
  while (iter.next()) {
    switch (iter.type()) {
      case relocInfo::none:
        break;
      case relocInfo::oop_type: {
        assert(code_blob->is_nmethod(), "sanity check");
        oop_Relocation* r = (oop_Relocation*)iter.reloc();
        if (r->oop_is_immediate()) {
          assert(oop_list != nullptr, "sanity check");
          Handle h = oop_list->at(reloc_data[j]);
          r->set_value(cast_from_oop<address>(h()));
        } else {
          r->fix_oop_relocation();
        }
        break;
      }
      case relocInfo::metadata_type: {
        assert(code_blob->is_nmethod(), "sanity check");
        metadata_Relocation* r = (metadata_Relocation*)iter.reloc();
        Metadata* m;
        if (r->metadata_is_immediate()) {
          assert(metadata_list != nullptr, "sanity check");
          m = metadata_list->at(reloc_data[j]);
        } else {
          // Get already updated value from nmethod.
          int index = r->metadata_index();
          m = code_blob->as_nmethod()->metadata_at(index);
        }
        r->set_value((address)m);
        break;
      }
      case relocInfo::virtual_call_type:   // Fall through. They all call resolve_*_call blobs.
      case relocInfo::opt_virtual_call_type:
      case relocInfo::static_call_type: {
        address dest = _cache->address_for_id(reloc_data[j]);
        if (dest != (address)-1) {
          ((CallRelocation*)iter.reloc())->set_destination(dest);
        }
        break;
      }
      case relocInfo::trampoline_stub_type: {
        address dest = _cache->address_for_id(reloc_data[j]);
        if (dest != (address)-1) {
          ((trampoline_stub_Relocation*)iter.reloc())->set_destination(dest);
        }
        break;
      }
      case relocInfo::static_stub_type:
        break;
      case relocInfo::runtime_call_type: {
        address dest = _cache->address_for_id(reloc_data[j]);
        if (dest != (address)-1) {
          ((CallRelocation*)iter.reloc())->set_destination(dest);
        }
        break;
      }
      case relocInfo::runtime_call_w_cp_type:
        // this relocation should not be in cache (see write_relocations)
        assert(false, "runtime_call_w_cp_type relocation is not implemented");
        break;
      case relocInfo::external_word_type: {
        address target = _cache->address_for_id(reloc_data[j]);
        // Add external address to global table
        int index = ExternalsRecorder::find_index(target);
        // Update index in relocation
        Relocation::add_jint(iter.data(), index);
        external_word_Relocation* reloc = (external_word_Relocation*)iter.reloc();
        assert(reloc->target() == target, "sanity");
        reloc->set_value(target); // Patch address in the code
        break;
      }
      case relocInfo::internal_word_type: {
        internal_word_Relocation* r = (internal_word_Relocation*)iter.reloc();
        r->fix_relocation_after_aot_load(aot_code_entry()->dumptime_content_start_addr(), code_blob->content_begin());
        break;
      }
      case relocInfo::section_word_type: {
        section_word_Relocation* r = (section_word_Relocation*)iter.reloc();
        r->fix_relocation_after_aot_load(aot_code_entry()->dumptime_content_start_addr(), code_blob->content_begin());
        break;
      }
      case relocInfo::poll_type:
        break;
      case relocInfo::poll_return_type:
        break;
      case relocInfo::post_call_nop_type:
        break;
      case relocInfo::entry_guard_type:
        break;
      default:
        assert(false,"relocation %d unimplemented", (int)iter.type());
        break;
    }
    if (log.is_enabled()) {
      iter.print_current_on(&log);
    }
    j++;
  }
  assert(j == count, "sanity");
}

bool AOTCodeCache::write_nmethod_reloc_immediates(GrowableArray<Handle>& oop_list, GrowableArray<Metadata*>& metadata_list) {
  int count = oop_list.length();
  if (!write_bytes(&count, sizeof(int))) {
    return false;
  }
  for (GrowableArrayIterator<Handle> iter = oop_list.begin();
       iter != oop_list.end(); ++iter) {
    Handle h = *iter;
    if (!write_oop(h())) {
      return false;
    }
  }

  count = metadata_list.length();
  if (!write_bytes(&count, sizeof(int))) {
    return false;
  }
  for (GrowableArrayIterator<Metadata*> iter = metadata_list.begin();
       iter != metadata_list.end(); ++iter) {
    Metadata* m = *iter;
    if (!write_metadata(m)) {
      return false;
    }
  }
  return true;
}

bool AOTCodeCache::write_metadata(nmethod* nm) {
  int count = nm->metadata_count()-1;
  if (!write_bytes(&count, sizeof(int))) {
    return false;
  }
  for (Metadata** p = nm->metadata_begin(); p < nm->metadata_end(); p++) {
    if (!write_metadata(*p)) {
      return false;
    }
  }
  return true;
}

bool AOTCodeCache::write_metadata(Metadata* m) {
  uint n = 0;
  if (m == nullptr) {
    DataKind kind = DataKind::Null;
    n = write_bytes(&kind, sizeof(int));
    if (n != sizeof(int)) {
      return false;
    }
  } else if (m == (Metadata*)Universe::non_oop_word()) {
    DataKind kind = DataKind::No_Data;
    n = write_bytes(&kind, sizeof(int));
    if (n != sizeof(int)) {
      return false;
    }
  } else if (m->is_klass()) {
    if (!write_klass((Klass*)m)) {
      return false;
    }
  } else if (m->is_method()) {
    if (!write_method((Method*)m)) {
      return false;
    }
  } else if (m->is_methodCounters()) {
    DataKind kind = DataKind::MethodCnts;
    n = write_bytes(&kind, sizeof(int));
    if (n != sizeof(int)) {
      return false;
    }
    if (!write_method(((MethodCounters*)m)->method())) {
      return false;
    }
    log_debug(aot, codecache, metadata)("%d (L%d): Write MethodCounters : " INTPTR_FORMAT, compile_id(), comp_level(), p2i(m));
  } else { // Not supported
    fatal("metadata : " INTPTR_FORMAT " unimplemented", p2i(m));
    return false;
  }
  return true;
}

Metadata* AOTCodeReader::read_metadata(const methodHandle& comp_method) {
  uint code_offset = read_position();
  Metadata* m = nullptr;
  DataKind kind = *(DataKind*)addr(code_offset);
  code_offset += sizeof(DataKind);
  set_read_position(code_offset);
  if (kind == DataKind::Null) {
    m = (Metadata*)nullptr;
  } else if (kind == DataKind::No_Data) {
    m = (Metadata*)Universe::non_oop_word();
  } else if (kind == DataKind::Klass) {
    m = (Metadata*)read_klass(comp_method);
  } else if (kind == DataKind::Method) {
    m = (Metadata*)read_method(comp_method);
  } else if (kind == DataKind::MethodCnts) {
    kind = *(DataKind*)addr(code_offset);
    code_offset += sizeof(DataKind);
    set_read_position(code_offset);
    m = (Metadata*)read_method(comp_method);
    if (m != nullptr) {
      Method* method = (Method*)m;
      m = method->get_method_counters(Thread::current());
      if (m == nullptr) {
        set_lookup_failed();
        log_debug(aot, codecache, metadata)("%d (L%d): Failed to get MethodCounters", compile_id(), comp_level());
      } else {
        log_debug(aot, codecache, metadata)("%d (L%d): Read MethodCounters : " INTPTR_FORMAT, compile_id(), comp_level(), p2i(m));
      }
    }
  } else {
    set_lookup_failed();
    log_debug(aot, codecache, metadata)("%d (L%d): Unknown metadata's kind: %d", compile_id(), comp_level(), (int)kind);
  }
  return m;
}

bool AOTCodeCache::write_method(Method* method) {
  ResourceMark rm; // To method's name printing
  if (AOTCacheAccess::can_generate_aot_code(method)) {
    DataKind kind = DataKind::Method;
    uint n = write_bytes(&kind, sizeof(int));
    if (n != sizeof(int)) {
      return false;
    }
    uint method_offset = AOTCacheAccess::delta_from_base_address((address)method);
    n = write_bytes(&method_offset, sizeof(uint));
    if (n != sizeof(uint)) {
      return false;
    }
    log_debug(aot, codecache, metadata)("%d (L%d): Wrote method: %s @ 0x%08x",
             compile_id(), comp_level(), method->name_and_sig_as_C_string(), method_offset);
    return true;
  }
  log_debug(aot, codecache, metadata)("%d (L%d): Method is not archived: %s",
              compile_id(), comp_level(), method->name_and_sig_as_C_string());
  set_lookup_failed();
  return false;
}

Method* AOTCodeReader::read_method(const methodHandle& comp_method) {
  uint code_offset = read_position();
  uint method_offset = *(uint*)addr(code_offset);
  code_offset += sizeof(uint);
  set_read_position(code_offset);
  Method* m = AOTCacheAccess::convert_offset_to_method(method_offset);
  if (!MetaspaceShared::is_in_shared_metaspace((address)m)) {
    // Something changed in CDS
    set_lookup_failed();
    log_debug(aot, codecache, metadata)("Lookup failed for shared method: " INTPTR_FORMAT " is not in CDS ", p2i((address)m));
    return nullptr;
  }
  assert(m->is_method(), "sanity");
  ResourceMark rm;
  Klass* k = m->method_holder();
  if (!k->is_instance_klass()) {
    set_lookup_failed();
    log_debug(aot, codecache, metadata)("%d '%s' (L%d): Lookup failed for holder %s: not instance klass",
                  compile_id(), comp_method->name_and_sig_as_C_string(), comp_level(), k->external_name());
    return nullptr;
  } else if (!MetaspaceShared::is_in_shared_metaspace((address)k)) {
    set_lookup_failed();
    log_debug(aot, codecache, metadata)("%d '%s' (L%d): Lookup failed for holder %s: not in CDS",
                  compile_id(), comp_method->name_and_sig_as_C_string(), comp_level(), k->external_name());
    return nullptr;
  } else if (!InstanceKlass::cast(k)->is_loaded()) {
    set_lookup_failed();
    log_debug(aot, codecache, metadata)("%d '%s' (L%d): Lookup failed for holder %s: not loaded",
                  compile_id(), comp_method->name_and_sig_as_C_string(), comp_level(), k->external_name());
    return nullptr;
  } else if (!InstanceKlass::cast(k)->is_linked()) {
    set_lookup_failed();
    log_debug(aot, codecache, metadata)("%d '%s' (L%d): Lookup failed for holder %s: not linked%s",
                  compile_id(), comp_method->name_and_sig_as_C_string(), comp_level(), k->external_name(), (_preload ? " for code preload" : ""));
    return nullptr;
  }
  log_debug(aot, codecache, metadata)("%d (L%d): Shared method lookup: %s",
                compile_id(), comp_level(), m->name_and_sig_as_C_string());
  return m;
}

bool AOTCodeCache::write_klass(Klass* klass) {
  uint array_dim = 0;
  if (klass->is_objArray_klass()) {
    array_dim = ObjArrayKlass::cast(klass)->dimension();
    klass     = ObjArrayKlass::cast(klass)->bottom_klass(); // overwrites klass
  }
  uint init_state = 0;
  bool can_write = true;
  if (klass->is_instance_klass()) {
    InstanceKlass* ik = InstanceKlass::cast(klass);
    init_state = (ik->is_initialized() ? 1 : 0);
    can_write = AOTCacheAccess::can_generate_aot_code_for(ik);
  } else {
    can_write = AOTCacheAccess::can_generate_aot_code(klass);
  }
  ResourceMark rm;
  uint state = (array_dim << 1) | (init_state & 1);
  if (can_write) {
    DataKind kind = DataKind::Klass;
    uint n = write_bytes(&kind, sizeof(int));
    if (n != sizeof(int)) {
      return false;
    }
    // Record state of instance klass initialization and array dimentions.
    n = write_bytes(&state, sizeof(int));
    if (n != sizeof(int)) {
      return false;
    }
    uint klass_offset = AOTCacheAccess::delta_from_base_address((address)klass);
    n = write_bytes(&klass_offset, sizeof(uint));
    if (n != sizeof(uint)) {
      return false;
    }
    log_debug(aot, codecache, metadata)("%d (L%d): Registered klass: %s%s%s @ 0x%08x",
             compile_id(), comp_level(), klass->external_name(),
             (!klass->is_instance_klass() ? "" : (init_state == 1 ? " (initialized)" : " (not-initialized)")),
             (array_dim > 0 ? " (object array)" : ""), klass_offset);
    return true;
  }
  log_debug(aot, codecache, metadata)("%d (L%d): Klassis not archived: %s%s%s",
              compile_id(), comp_level(), klass->external_name(),
              (!klass->is_instance_klass() ? "" : (init_state == 1 ? " (initialized)" : " (not-initialized)")),
              (array_dim > 0 ? " (object array)" : ""));
  set_lookup_failed();
  return false;
}

Klass* AOTCodeReader::read_klass(const methodHandle& comp_method) {
  uint code_offset = read_position();
  uint state = *(uint*)addr(code_offset);
  uint init_state = (state  & 1);
  uint array_dim  = (state >> 1);
  code_offset += sizeof(int);
  uint klass_offset = *(uint*)addr(code_offset);
  code_offset += sizeof(uint);
  set_read_position(code_offset);
  Klass* k = AOTCacheAccess::convert_offset_to_klass(klass_offset);
  if (!MetaspaceShared::is_in_shared_metaspace((address)k)) {
    // Something changed in CDS
    set_lookup_failed();
    log_debug(aot, codecache, metadata)("Lookup failed for shared klass: " INTPTR_FORMAT " is not in CDS ", p2i((address)k));
    return nullptr;
  }
  assert(k->is_klass(), "sanity");
  ResourceMark rm;
  if (k->is_instance_klass() && !InstanceKlass::cast(k)->is_loaded()) {
    set_lookup_failed();
    log_debug(aot, codecache, metadata)("%d '%s' (L%d): Lookup failed for klass %s: not loaded",
                     compile_id(), comp_method->name_and_sig_as_C_string(), comp_level(), k->external_name());
    return nullptr;
  } else
  // Allow not initialized klass which was uninitialized during code caching or for preload
  if (k->is_instance_klass() && !InstanceKlass::cast(k)->is_initialized() && (init_state == 1) && !_preload) {
    set_lookup_failed();
    log_debug(aot, codecache, metadata)("%d '%s' (L%d): Lookup failed for klass %s: not initialized",
                     compile_id(), comp_method->name_and_sig_as_C_string(), comp_level(), k->external_name());
    return nullptr;
  }
  if (array_dim > 0) {
    assert(k->is_instance_klass() || k->is_typeArray_klass(), "sanity check");
    Klass* ak = k->array_klass_or_null(array_dim);
    // FIXME: what would it take to create an array class on the fly?
//    Klass* ak = k->array_klass(dim, JavaThread::current());
//    guarantee(JavaThread::current()->pending_exception() == nullptr, "");
    if (ak == nullptr) {
      set_lookup_failed();
      log_debug(aot, codecache, metadata)("%d (L%d): %d-dimension array klass lookup failed: %s",
                       compile_id(), comp_level(), array_dim, k->external_name());
    }
    log_debug(aot, codecache, metadata)("%d (L%d): Klass lookup: %s (object array)", compile_id(), comp_level(), k->external_name());
    return ak;
  } else {
    log_debug(aot, codecache, metadata)("%d (L%d): Shared klass lookup: %s",
                  compile_id(), comp_level(), k->external_name());
    return k;
  }
}

bool AOTCodeCache::write_oop(jobject& jo) {
  oop obj = JNIHandles::resolve(jo);
  return write_oop(obj);
}

bool AOTCodeCache::write_oop(oop obj) {
  DataKind kind;
  uint n = 0;
  if (obj == nullptr) {
    kind = DataKind::Null;
    n = write_bytes(&kind, sizeof(int));
    if (n != sizeof(int)) {
      return false;
    }
  } else if (cast_from_oop<void *>(obj) == Universe::non_oop_word()) {
    kind = DataKind::No_Data;
    n = write_bytes(&kind, sizeof(int));
    if (n != sizeof(int)) {
      return false;
    }
  } else if (java_lang_Class::is_instance(obj)) {
    if (java_lang_Class::is_primitive(obj)) {
      int bt = (int)java_lang_Class::primitive_type(obj);
      kind = DataKind::Primitive;
      n = write_bytes(&kind, sizeof(int));
      if (n != sizeof(int)) {
        return false;
      }
      n = write_bytes(&bt, sizeof(int));
      if (n != sizeof(int)) {
        return false;
      }
      log_debug(aot, codecache, oops)("%d (L%d): Write primitive type klass: %s", compile_id(), comp_level(), type2name((BasicType)bt));
    } else {
      Klass* klass = java_lang_Class::as_Klass(obj);
      if (!write_klass(klass)) {
        return false;
      }
    }
  } else if (java_lang_String::is_instance(obj)) { // herere
    int k = AOTCacheAccess::get_archived_object_permanent_index(obj);  // k >= 0 means obj is a "permanent heap object"
    ResourceMark rm;
    size_t length_sz = 0;
    const char* string = java_lang_String::as_utf8_string(obj, length_sz);
    if (k >= 0) {
      kind = DataKind::String;
      n = write_bytes(&kind, sizeof(int));
      if (n != sizeof(int)) {
        return false;
      }
      n = write_bytes(&k, sizeof(int));
      if (n != sizeof(int)) {
        return false;
      }
      log_debug(aot, codecache, oops)("%d (L%d): Write String object: " PTR_FORMAT " : %s", compile_id(), comp_level(), p2i(obj), string);
      return true;
    }
    // Not archived String object - bailout
    set_lookup_failed();
    log_debug(aot, codecache, oops)("%d (L%d): Not archived String object: " PTR_FORMAT " : %s",
                                      compile_id(), comp_level(), p2i(obj), string);
    return false;
  } else if (java_lang_Module::is_instance(obj)) {
    fatal("Module object unimplemented");
  } else if (java_lang_ClassLoader::is_instance(obj)) {
    if (obj == SystemDictionary::java_system_loader()) {
      kind = DataKind::SysLoader;
      log_debug(aot, codecache, oops)("%d (L%d): Write ClassLoader: java_system_loader", compile_id(), comp_level());
    } else if (obj == SystemDictionary::java_platform_loader()) {
      kind = DataKind::PlaLoader;
      log_debug(aot, codecache, oops)("%d (L%d): Write ClassLoader: java_platform_loader", compile_id(), comp_level());
    } else {
      ResourceMark rm;
      set_lookup_failed();
      log_debug(aot, codecache, oops)("%d (L%d): Not supported Class Loader: " PTR_FORMAT " : %s",
                                      compile_id(), comp_level(), p2i(obj), obj->klass()->external_name());
      return false;
    }
    n = write_bytes(&kind, sizeof(int));
    if (n != sizeof(int)) {
      return false;
    }
  } else { // herere
    ResourceMark rm;
    int k = AOTCacheAccess::get_archived_object_permanent_index(obj);  // k >= 0 means obj is a "permanent heap object"
    if (k >= 0) {
      kind = DataKind::MH_Oop;
      n = write_bytes(&kind, sizeof(int));
      if (n != sizeof(int)) {
        return false;
      }
      n = write_bytes(&k, sizeof(int));
      if (n != sizeof(int)) {
        return false;
      }
      log_debug(aot, codecache, oops)("%d (L%d): Write MH object: " PTR_FORMAT " : %s",
                              compile_id(), comp_level(), p2i(obj), obj->klass()->external_name());
      return true;
    }
    // Not archived Java object - bailout
    set_lookup_failed();
    log_debug(aot, codecache, oops)("%d (L%d): Not archived Java object: " PTR_FORMAT " : %s",
                              compile_id(), comp_level(), p2i(obj), obj->klass()->external_name());
    return false;
  }
  return true;
}

oop AOTCodeReader::read_oop(JavaThread* thread, const methodHandle& comp_method) {
  uint code_offset = read_position();
  oop obj = nullptr;
  DataKind kind = *(DataKind*)addr(code_offset);
  code_offset += sizeof(DataKind);
  set_read_position(code_offset);
  if (kind == DataKind::Null) {
    return nullptr;
  } else if (kind == DataKind::No_Data) {
    return cast_to_oop(Universe::non_oop_word());
  } else if (kind == DataKind::Klass) {
    Klass* k = read_klass(comp_method);
    if (k == nullptr) {
      return nullptr;
    }
    obj = k->java_mirror();
    if (obj == nullptr) {
      set_lookup_failed();
      log_debug(aot, codecache, oops)("Lookup failed for java_mirror of klass %s", k->external_name());
      return nullptr;
    }
  } else if (kind == DataKind::Primitive) {
    code_offset = read_position();
    int t = *(int*)addr(code_offset);
    code_offset += sizeof(int);
    set_read_position(code_offset);
    BasicType bt = (BasicType)t;
    obj = java_lang_Class::primitive_mirror(bt);
    log_debug(aot, codecache, oops)("%d (L%d): Read primitive type klass: %s", compile_id(), comp_level(), type2name(bt));
  } else if (kind == DataKind::String) {
    code_offset = read_position();
    int k = *(int*)addr(code_offset);
    code_offset += sizeof(int);
    set_read_position(code_offset);
    obj = AOTCacheAccess::get_archived_object(k);
    if (obj == nullptr) {
      set_lookup_failed();
      log_debug(aot, codecache, oops)("Lookup failed for String object");
      return nullptr;
    }
    assert(java_lang_String::is_instance(obj), "must be string");

    ResourceMark rm;
    size_t length_sz = 0;
    const char* string = java_lang_String::as_utf8_string(obj, length_sz);
    log_debug(aot, codecache, oops)("%d (L%d): Read String object: %s", compile_id(), comp_level(), string);
  } else if (kind == DataKind::SysLoader) {
    obj = SystemDictionary::java_system_loader();
    log_debug(aot, codecache, oops)("%d (L%d): Read java_system_loader", compile_id(), comp_level());
  } else if (kind == DataKind::PlaLoader) {
    obj = SystemDictionary::java_platform_loader();
    log_debug(aot, codecache, oops)("%d (L%d): Read java_platform_loader", compile_id(), comp_level());
  } else if (kind == DataKind::MH_Oop) {
    code_offset = read_position();
    int k = *(int*)addr(code_offset);
    code_offset += sizeof(int);
    set_read_position(code_offset);
    obj = AOTCacheAccess::get_archived_object(k);
    if (obj == nullptr) {
      set_lookup_failed();
      log_debug(aot, codecache, oops)("Lookup failed for MH object");
      return nullptr;
    }
    ResourceMark rm;
    log_debug(aot, codecache, oops)("%d (L%d): Read MH object: " PTR_FORMAT " : %s",
                              compile_id(), comp_level(), p2i(obj), obj->klass()->external_name());
  } else {
    set_lookup_failed();
    log_debug(aot, codecache, oops)("%d (L%d): Unknown oop's kind: %d",
                     compile_id(), comp_level(), (int)kind);
    return nullptr;
  }
  return obj;
}

bool AOTCodeReader::read_oop_metadata_list(JavaThread* thread, ciMethod* target, GrowableArray<Handle> &oop_list, GrowableArray<Metadata*> &metadata_list, OopRecorder* oop_recorder) {
  methodHandle comp_method(JavaThread::current(), target->get_Method());
  JavaThread* current = JavaThread::current();
  uint offset = read_position();
  int count = *(int *)addr(offset);
  offset += sizeof(int);
  set_read_position(offset);
  for (int i = 0; i < count; i++) {
    oop obj = read_oop(current, comp_method);
    if (lookup_failed()) {
      return false;
    }
    Handle h(thread, obj);
    oop_list.append(h);
    if (oop_recorder != nullptr) {
      jobject jo = JNIHandles::make_local(thread, obj);
      if (oop_recorder->is_real(jo)) {
        oop_recorder->find_index(jo);
      } else {
        oop_recorder->allocate_oop_index(jo);
      }
    }
    LogStreamHandle(Debug, aot, codecache, oops) log;
    if (log.is_enabled()) {
      log.print("%d: " INTPTR_FORMAT " ", i, p2i(obj));
      if (obj == Universe::non_oop_word()) {
        log.print("non-oop word");
      } else if (obj == nullptr) {
        log.print("nullptr-oop");
      } else {
        obj->print_value_on(&log);
      }
      log.cr();
    }
  }

  offset = read_position();
  count = *(int *)addr(offset);
  offset += sizeof(int);
  set_read_position(offset);
  for (int i = 0; i < count; i++) {
    Metadata* m = read_metadata(comp_method);
    if (lookup_failed()) {
      return false;
    }
    metadata_list.append(m);
    if (oop_recorder != nullptr) {
      if (oop_recorder->is_real(m)) {
        oop_recorder->find_index(m);
      } else {
        oop_recorder->allocate_metadata_index(m);
      }
    }
    LogTarget(Debug, aot, codecache, metadata) log;
    if (log.is_enabled()) {
      LogStream ls(log);
      ls.print("%d: " INTPTR_FORMAT " ", i, p2i(m));
      if (m == (Metadata*)Universe::non_oop_word()) {
        ls.print("non-metadata word");
      } else if (m == nullptr) {
        ls.print("nullptr-oop");
      } else {
        Metadata::print_value_on_maybe_null(&ls, m);
      }
      ls.cr();
    }
  }
  return true;
}

bool AOTCodeCache::write_oop_map_set(CodeBlob& cb) {
  ImmutableOopMapSet* oopmaps = cb.oop_maps();
  int oopmaps_size = oopmaps->nr_of_bytes();
  if (!write_bytes(&oopmaps_size, sizeof(int))) {
    return false;
  }
  uint n = write_bytes(oopmaps, oopmaps->nr_of_bytes());
  if (n != (uint)oopmaps->nr_of_bytes()) {
    return false;
  }
  return true;
}

ImmutableOopMapSet* AOTCodeReader::read_oop_map_set() {
  uint offset = read_position();
  int size = *(int *)addr(offset);
  offset += sizeof(int);
  ImmutableOopMapSet* oopmaps = (ImmutableOopMapSet *)addr(offset);
  offset += size;
  set_read_position(offset);
  return oopmaps;
}

bool AOTCodeCache::write_oops(nmethod* nm) {
  int count = nm->oops_count()-1;
  if (!write_bytes(&count, sizeof(int))) {
    return false;
  }
  for (oop* p = nm->oops_begin(); p < nm->oops_end(); p++) {
    if (!write_oop(*p)) {
      return false;
    }
  }
  return true;
}

#ifndef PRODUCT
bool AOTCodeCache::write_asm_remarks(AsmRemarks& asm_remarks, bool use_string_table) {
  // Write asm remarks
  uint* count_ptr = (uint *)reserve_bytes(sizeof(uint));
  if (count_ptr == nullptr) {
    return false;
  }
  uint count = 0;
  bool result = asm_remarks.iterate([&] (uint offset, const char* str) -> bool {
    log_trace(aot, codecache, stubs)("asm remark offset=%d, str='%s'", offset, str);
    uint n = write_bytes(&offset, sizeof(uint));
    if (n != sizeof(uint)) {
      return false;
    }
    if (use_string_table) {
      const char* cstr = add_C_string(str);
      int id = _table->id_for_C_string((address)cstr);
      assert(id != -1, "asm remark string '%s' not found in AOTCodeAddressTable", str);
      n = write_bytes(&id, sizeof(int));
      if (n != sizeof(int)) {
        return false;
      }
    } else {
      n = write_bytes(str, (uint)strlen(str) + 1);
      if (n != strlen(str) + 1) {
        return false;
      }
    }
    count += 1;
    return true;
  });
  *count_ptr = count;
  return result;
}

void AOTCodeReader::read_asm_remarks(AsmRemarks& asm_remarks, bool use_string_table) {
  // Read asm remarks
  uint offset = read_position();
  uint count = *(uint *)addr(offset);
  offset += sizeof(uint);
  for (uint i = 0; i < count; i++) {
    uint remark_offset = *(uint *)addr(offset);
    offset += sizeof(uint);
    const char* remark = nullptr;
    if (use_string_table) {
      int remark_string_id = *(uint *)addr(offset);
      offset += sizeof(int);
      remark = (const char*)_cache->address_for_C_string(remark_string_id);
    } else {
      remark = (const char*)addr(offset);
      offset += (uint)strlen(remark)+1;
    }
    asm_remarks.insert(remark_offset, remark);
  }
  set_read_position(offset);
}

bool AOTCodeCache::write_dbg_strings(DbgStrings& dbg_strings, bool use_string_table) {
  // Write dbg strings
  uint* count_ptr = (uint *)reserve_bytes(sizeof(uint));
  if (count_ptr == nullptr) {
    return false;
  }
  uint count = 0;
  bool result = dbg_strings.iterate([&] (const char* str) -> bool {
    log_trace(aot, codecache, stubs)("dbg string=%s", str);
    if (use_string_table) {
      const char* cstr = add_C_string(str);
      int id = _table->id_for_C_string((address)cstr);
      assert(id != -1, "db string '%s' not found in AOTCodeAddressTable", str);
      uint n = write_bytes(&id, sizeof(int));
      if (n != sizeof(int)) {
        return false;
      }
    } else {
      uint n = write_bytes(str, (uint)strlen(str) + 1);
      if (n != strlen(str) + 1) {
        return false;
      }
    }
    count += 1;
    return true;
  });
  *count_ptr = count;
  return result;
}

void AOTCodeReader::read_dbg_strings(DbgStrings& dbg_strings, bool use_string_table) {
  // Read dbg strings
  uint offset = read_position();
  uint count = *(uint *)addr(offset);
  offset += sizeof(uint);
  for (uint i = 0; i < count; i++) {
    const char* str = nullptr;
    if (use_string_table) {
      int string_id = *(uint *)addr(offset);
      offset += sizeof(int);
      str = (const char*)_cache->address_for_C_string(string_id);
    } else {
      str = (const char*)addr(offset);
      offset += (uint)strlen(str)+1;
    }
    dbg_strings.insert(str);
  }
  set_read_position(offset);
}
#endif // PRODUCT

//======================= AOTCodeAddressTable ===============

// address table ids for generated routines, external addresses and C
// string addresses are partitioned into positive integer ranges
// defined by the following positive base and max values
// i.e. [_extrs_base, _extrs_base + _extrs_max -1],
//      [_stubs_base, _stubs_base + _stubs_max -1],
//      ...
//      [_c_str_base, _c_str_base + _c_str_max -1],
#define _extrs_max 140
#define _stubs_max 210
#define _shared_blobs_max 25
#define _C1_blobs_max 50
#define _C2_blobs_max 25
#define _blobs_max (_shared_blobs_max+_C1_blobs_max+_C2_blobs_max)
#define _all_max (_extrs_max+_stubs_max+_blobs_max)

#define _extrs_base 0
#define _stubs_base (_extrs_base + _extrs_max)
#define _shared_blobs_base (_stubs_base + _stubs_max)
#define _C1_blobs_base (_shared_blobs_base + _shared_blobs_max)
#define _C2_blobs_base (_C1_blobs_base + _C1_blobs_max)
#define _blobs_end  (_shared_blobs_base + _blobs_max)
#if (_C2_blobs_base >= _all_max)
#error AOTCodeAddressTable ranges need adjusting
#endif

#define SET_ADDRESS(type, addr)                           \
  {                                                       \
    type##_addr[type##_length++] = (address) (addr);      \
    assert(type##_length <= type##_max, "increase size"); \
  }

static bool initializing_extrs = false;

void AOTCodeAddressTable::init_extrs() {
  if (_extrs_complete || initializing_extrs) return; // Done already

  assert(_blobs_end <= _all_max, "AOTCodeAddress table ranges need adjusting");

  initializing_extrs = true;
  _extrs_addr = NEW_C_HEAP_ARRAY(address, _extrs_max, mtCode);

  _extrs_length = 0;

  // Record addresses of VM runtime methods
  SET_ADDRESS(_extrs, SharedRuntime::fixup_callers_callsite);
  SET_ADDRESS(_extrs, SharedRuntime::handle_wrong_method);
  SET_ADDRESS(_extrs, SharedRuntime::handle_wrong_method_abstract);
  SET_ADDRESS(_extrs, SharedRuntime::handle_wrong_method_ic_miss);
  {
    // Required by Shared blobs
    SET_ADDRESS(_extrs, Deoptimization::fetch_unroll_info);
    SET_ADDRESS(_extrs, Deoptimization::unpack_frames);
    SET_ADDRESS(_extrs, SafepointSynchronize::handle_polling_page_exception);
    SET_ADDRESS(_extrs, SharedRuntime::resolve_opt_virtual_call_C);
    SET_ADDRESS(_extrs, SharedRuntime::resolve_virtual_call_C);
    SET_ADDRESS(_extrs, SharedRuntime::resolve_static_call_C);
    SET_ADDRESS(_extrs, SharedRuntime::throw_delayed_StackOverflowError);
    SET_ADDRESS(_extrs, SharedRuntime::throw_AbstractMethodError);
    SET_ADDRESS(_extrs, SharedRuntime::throw_IncompatibleClassChangeError);
    SET_ADDRESS(_extrs, SharedRuntime::throw_NullPointerException_at_call);
    SET_ADDRESS(_extrs, CompressedOops::base_addr());
    SET_ADDRESS(_extrs, CompressedKlassPointers::base_addr());
  }
  {
    // Required by initial stubs
    SET_ADDRESS(_extrs, StubRoutines::crc_table_addr());
#if defined(AMD64)
    SET_ADDRESS(_extrs, StubRoutines::crc32c_table_addr());
#endif
  }

#ifdef COMPILER1
  {
    // Required by C1 blobs
    SET_ADDRESS(_extrs, static_cast<int (*)(oopDesc*)>(SharedRuntime::dtrace_object_alloc));
    SET_ADDRESS(_extrs, SharedRuntime::exception_handler_for_return_address);
    SET_ADDRESS(_extrs, SharedRuntime::register_finalizer);
    SET_ADDRESS(_extrs, Runtime1::is_instance_of);
    SET_ADDRESS(_extrs, Runtime1::exception_handler_for_pc);
    SET_ADDRESS(_extrs, Runtime1::check_abort_on_vm_exception);
    SET_ADDRESS(_extrs, Runtime1::new_instance);
    SET_ADDRESS(_extrs, Runtime1::counter_overflow);
    SET_ADDRESS(_extrs, Runtime1::new_type_array);
    SET_ADDRESS(_extrs, Runtime1::new_object_array);
    SET_ADDRESS(_extrs, Runtime1::new_multi_array);
    SET_ADDRESS(_extrs, Runtime1::throw_range_check_exception);
    SET_ADDRESS(_extrs, Runtime1::throw_index_exception);
    SET_ADDRESS(_extrs, Runtime1::throw_div0_exception);
    SET_ADDRESS(_extrs, Runtime1::throw_null_pointer_exception);
    SET_ADDRESS(_extrs, Runtime1::throw_array_store_exception);
    SET_ADDRESS(_extrs, Runtime1::throw_class_cast_exception);
    SET_ADDRESS(_extrs, Runtime1::throw_incompatible_class_change_error);
    SET_ADDRESS(_extrs, Runtime1::monitorenter);
    SET_ADDRESS(_extrs, Runtime1::monitorexit);
    SET_ADDRESS(_extrs, Runtime1::deoptimize);
    SET_ADDRESS(_extrs, Runtime1::access_field_patching);
    SET_ADDRESS(_extrs, Runtime1::move_klass_patching);
    SET_ADDRESS(_extrs, Runtime1::move_mirror_patching);
    SET_ADDRESS(_extrs, Runtime1::move_appendix_patching);
    SET_ADDRESS(_extrs, Runtime1::predicate_failed_trap);
    SET_ADDRESS(_extrs, Runtime1::unimplemented_entry);
    SET_ADDRESS(_extrs, Runtime1::trace_block_entry);
#ifdef X86
    SET_ADDRESS(_extrs, LIR_Assembler::float_signmask_pool);
    SET_ADDRESS(_extrs, LIR_Assembler::double_signmask_pool);
    SET_ADDRESS(_extrs, LIR_Assembler::float_signflip_pool);
    SET_ADDRESS(_extrs, LIR_Assembler::double_signflip_pool);
#endif
#ifndef PRODUCT
    SET_ADDRESS(_extrs, os::breakpoint);
#endif
  }
#endif // COMPILER1

#ifdef COMPILER2
  {
    // Required by C2 blobs
    SET_ADDRESS(_extrs, Deoptimization::uncommon_trap);
    SET_ADDRESS(_extrs, OptoRuntime::handle_exception_C);
    SET_ADDRESS(_extrs, OptoRuntime::new_instance_C);
    SET_ADDRESS(_extrs, OptoRuntime::new_array_C);
    SET_ADDRESS(_extrs, OptoRuntime::new_array_nozero_C);
    SET_ADDRESS(_extrs, OptoRuntime::multianewarray2_C);
    SET_ADDRESS(_extrs, OptoRuntime::multianewarray3_C);
    SET_ADDRESS(_extrs, OptoRuntime::multianewarray4_C);
    SET_ADDRESS(_extrs, OptoRuntime::multianewarray5_C);
    SET_ADDRESS(_extrs, OptoRuntime::multianewarrayN_C);
#if INCLUDE_JVMTI
    SET_ADDRESS(_extrs, SharedRuntime::notify_jvmti_vthread_start);
    SET_ADDRESS(_extrs, SharedRuntime::notify_jvmti_vthread_end);
    SET_ADDRESS(_extrs, SharedRuntime::notify_jvmti_vthread_mount);
    SET_ADDRESS(_extrs, SharedRuntime::notify_jvmti_vthread_unmount);
#endif
    SET_ADDRESS(_extrs, OptoRuntime::complete_monitor_locking_C);
    SET_ADDRESS(_extrs, OptoRuntime::monitor_notify_C);
    SET_ADDRESS(_extrs, OptoRuntime::monitor_notifyAll_C);
    SET_ADDRESS(_extrs, OptoRuntime::rethrow_C);
    SET_ADDRESS(_extrs, OptoRuntime::slow_arraycopy_C);
    SET_ADDRESS(_extrs, OptoRuntime::register_finalizer_C);
    SET_ADDRESS(_extrs, OptoRuntime::class_init_barrier_C);
#if defined(AMD64)
    // Use by C2 intinsic
    SET_ADDRESS(_extrs, StubRoutines::x86::arrays_hashcode_powers_of_31());
#endif
  }
#endif // COMPILER2

  // Record addresses of VM runtime methods and data structs
  BarrierSet* bs = BarrierSet::barrier_set();
  if (bs->is_a(BarrierSet::CardTableBarrierSet)) {
    SET_ADDRESS(_extrs, ci_card_table_address_as<address>());
  }

#if INCLUDE_G1GC
  SET_ADDRESS(_extrs, G1BarrierSetRuntime::write_ref_field_post_entry);
  SET_ADDRESS(_extrs, G1BarrierSetRuntime::write_ref_field_pre_entry);
#endif

#if INCLUDE_SHENANDOAHGC
  SET_ADDRESS(_extrs, ShenandoahRuntime::arraycopy_barrier_oop);
  SET_ADDRESS(_extrs, ShenandoahRuntime::arraycopy_barrier_narrow_oop);
  SET_ADDRESS(_extrs, ShenandoahRuntime::write_ref_field_pre);
  SET_ADDRESS(_extrs, ShenandoahRuntime::clone_barrier);
  SET_ADDRESS(_extrs, ShenandoahRuntime::load_reference_barrier_strong);
  SET_ADDRESS(_extrs, ShenandoahRuntime::load_reference_barrier_strong_narrow);
  SET_ADDRESS(_extrs, ShenandoahRuntime::load_reference_barrier_weak);
  SET_ADDRESS(_extrs, ShenandoahRuntime::load_reference_barrier_weak_narrow);
  SET_ADDRESS(_extrs, ShenandoahRuntime::load_reference_barrier_phantom);
  SET_ADDRESS(_extrs, ShenandoahRuntime::load_reference_barrier_phantom_narrow);
#endif

#if INCLUDE_ZGC
  SET_ADDRESS(_extrs, ZBarrierSetRuntime::load_barrier_on_phantom_oop_field_preloaded_addr());
#if defined(AMD64)
  SET_ADDRESS(_extrs, &ZPointerLoadShift);
#endif
#endif // INCLUDE_ZGC

  SET_ADDRESS(_extrs, SharedRuntime::log_jni_monitor_still_held);
  SET_ADDRESS(_extrs, SharedRuntime::rc_trace_method_entry);
  SET_ADDRESS(_extrs, SharedRuntime::reguard_yellow_pages);
  SET_ADDRESS(_extrs, SharedRuntime::dtrace_method_exit);

  SET_ADDRESS(_extrs, SharedRuntime::complete_monitor_unlocking_C);
  SET_ADDRESS(_extrs, SharedRuntime::enable_stack_reserved_zone);
#if defined(AMD64) && !defined(ZERO)
  SET_ADDRESS(_extrs, SharedRuntime::montgomery_multiply);
  SET_ADDRESS(_extrs, SharedRuntime::montgomery_square);
#endif // AMD64
  SET_ADDRESS(_extrs, SharedRuntime::d2f);
  SET_ADDRESS(_extrs, SharedRuntime::d2i);
  SET_ADDRESS(_extrs, SharedRuntime::d2l);
  SET_ADDRESS(_extrs, SharedRuntime::dcos);
  SET_ADDRESS(_extrs, SharedRuntime::dexp);
  SET_ADDRESS(_extrs, SharedRuntime::dlog);
  SET_ADDRESS(_extrs, SharedRuntime::dlog10);
  SET_ADDRESS(_extrs, SharedRuntime::dpow);
  SET_ADDRESS(_extrs, SharedRuntime::dsin);
  SET_ADDRESS(_extrs, SharedRuntime::dtan);
  SET_ADDRESS(_extrs, SharedRuntime::f2i);
  SET_ADDRESS(_extrs, SharedRuntime::f2l);
#ifndef ZERO
  SET_ADDRESS(_extrs, SharedRuntime::drem);
  SET_ADDRESS(_extrs, SharedRuntime::frem);
#endif
  SET_ADDRESS(_extrs, SharedRuntime::l2d);
  SET_ADDRESS(_extrs, SharedRuntime::l2f);
  SET_ADDRESS(_extrs, SharedRuntime::ldiv);
  SET_ADDRESS(_extrs, SharedRuntime::lmul);
  SET_ADDRESS(_extrs, SharedRuntime::lrem);

  SET_ADDRESS(_extrs, ThreadIdentifier::unsafe_offset());
  SET_ADDRESS(_extrs, Thread::current);

  SET_ADDRESS(_extrs, os::javaTimeMillis);
  SET_ADDRESS(_extrs, os::javaTimeNanos);
  // For JFR
  SET_ADDRESS(_extrs, os::elapsed_counter);

#if INCLUDE_JVMTI
  SET_ADDRESS(_extrs, &JvmtiExport::_should_notify_object_alloc);
  SET_ADDRESS(_extrs, &JvmtiVTMSTransitionDisabler::_VTMS_notify_jvmti_events);
#endif /* INCLUDE_JVMTI */

#ifndef PRODUCT
  SET_ADDRESS(_extrs, &SharedRuntime::_partial_subtype_ctr);
  SET_ADDRESS(_extrs, JavaThread::verify_cross_modify_fence_failure);
#endif

#ifndef ZERO
#if defined(AMD64) || defined(AARCH64) || defined(RISCV64)
  SET_ADDRESS(_extrs, MacroAssembler::debug64);
#endif
#if defined(AARCH64)
  SET_ADDRESS(_extrs, JavaThread::aarch64_get_thread_helper);
#endif
#endif // ZERO

  // addresses of fields in AOT runtime constants area
  address* p = AOTRuntimeConstants::field_addresses_list();
  while (*p != nullptr) {
    SET_ADDRESS(_extrs, *p++);
  }

  _extrs_complete = true;
  log_info(aot, codecache, init)("External addresses recorded");
}

static bool initializing_early_stubs = false;

void AOTCodeAddressTable::init_early_stubs() {
  if (_complete || initializing_early_stubs) return; // Done already
  initializing_early_stubs = true;
  _stubs_addr = NEW_C_HEAP_ARRAY(address, _stubs_max, mtCode);
  _stubs_length = 0;
  SET_ADDRESS(_stubs, StubRoutines::forward_exception_entry());

  {
    // Required by C1 blobs
#if defined(AMD64) && !defined(ZERO)
    SET_ADDRESS(_stubs, StubRoutines::x86::double_sign_flip());
    SET_ADDRESS(_stubs, StubRoutines::x86::d2l_fixup());
#endif // AMD64
  }

  _early_stubs_complete = true;
  log_info(aot, codecache, init)("Early stubs recorded");
}

static bool initializing_shared_blobs = false;

void AOTCodeAddressTable::init_shared_blobs() {
  if (_complete || initializing_shared_blobs) return; // Done already
  initializing_shared_blobs = true;
  address* blobs_addr = NEW_C_HEAP_ARRAY(address, _blobs_max, mtCode);

  // Divide _shared_blobs_addr array to chunks because they could be initialized in parrallel
  _shared_blobs_addr = blobs_addr;
  _C1_blobs_addr = _shared_blobs_addr + _shared_blobs_max;// C1 blobs addresses stored after shared blobs
  _C2_blobs_addr = _C1_blobs_addr + _C1_blobs_max; // C2 blobs addresses stored after C1 blobs

  _shared_blobs_length = 0;
  _C1_blobs_length = 0;
  _C2_blobs_length = 0;

  // clear the address table
  memset(blobs_addr, 0, sizeof(address)* _blobs_max);

  // Record addresses of generated code blobs
  SET_ADDRESS(_shared_blobs, SharedRuntime::get_handle_wrong_method_stub());
  SET_ADDRESS(_shared_blobs, SharedRuntime::get_ic_miss_stub());
  SET_ADDRESS(_shared_blobs, SharedRuntime::deopt_blob()->unpack());
  SET_ADDRESS(_shared_blobs, SharedRuntime::deopt_blob()->unpack_with_exception());
  SET_ADDRESS(_shared_blobs, SharedRuntime::deopt_blob()->unpack_with_reexecution());
  SET_ADDRESS(_shared_blobs, SharedRuntime::deopt_blob()->unpack_with_exception_in_tls());
  SET_ADDRESS(_shared_blobs, SharedRuntime::get_resolve_opt_virtual_call_stub());
  SET_ADDRESS(_shared_blobs, SharedRuntime::get_resolve_virtual_call_stub());
  SET_ADDRESS(_shared_blobs, SharedRuntime::get_resolve_static_call_stub());
  SET_ADDRESS(_shared_blobs, SharedRuntime::deopt_blob()->entry_point());
  SET_ADDRESS(_shared_blobs, SharedRuntime::polling_page_safepoint_handler_blob()->entry_point());
  SET_ADDRESS(_shared_blobs, SharedRuntime::polling_page_return_handler_blob()->entry_point());
#ifdef COMPILER2
  // polling_page_vectors_safepoint_handler_blob can be nullptr if AVX feature is not present or is disabled
  if (SharedRuntime::polling_page_vectors_safepoint_handler_blob() != nullptr) {
    SET_ADDRESS(_shared_blobs, SharedRuntime::polling_page_vectors_safepoint_handler_blob()->entry_point());
  }
#endif
#if INCLUDE_JVMCI
  if (EnableJVMCI) {
    SET_ADDRESS(_shared_blobs, SharedRuntime::deopt_blob()->uncommon_trap());
    SET_ADDRESS(_shared_blobs, SharedRuntime::deopt_blob()->implicit_exception_uncommon_trap());
  }
#endif
  SET_ADDRESS(_shared_blobs, SharedRuntime::throw_AbstractMethodError_entry());
  SET_ADDRESS(_shared_blobs, SharedRuntime::throw_IncompatibleClassChangeError_entry());
  SET_ADDRESS(_shared_blobs, SharedRuntime::throw_NullPointerException_at_call_entry());
  SET_ADDRESS(_shared_blobs, SharedRuntime::throw_StackOverflowError_entry());
  SET_ADDRESS(_shared_blobs, SharedRuntime::throw_delayed_StackOverflowError_entry());

  assert(_shared_blobs_length <= _shared_blobs_max, "increase _shared_blobs_max to %d", _shared_blobs_length);
  _shared_blobs_complete = true;
  log_info(aot, codecache, init)("All shared blobs recorded");
}

static bool initializing_stubs = false;
void AOTCodeAddressTable::init_stubs() {
  if (_complete || initializing_stubs) return; // Done already
  assert(_early_stubs_complete, "early stubs whould be initialized");
  initializing_stubs = true;

  // Stubs
  SET_ADDRESS(_stubs, StubRoutines::method_entry_barrier());
  SET_ADDRESS(_stubs, StubRoutines::atomic_xchg_entry());
  SET_ADDRESS(_stubs, StubRoutines::atomic_cmpxchg_entry());
  SET_ADDRESS(_stubs, StubRoutines::atomic_cmpxchg_long_entry());
  SET_ADDRESS(_stubs, StubRoutines::atomic_add_entry());
  SET_ADDRESS(_stubs, StubRoutines::fence_entry());

  SET_ADDRESS(_stubs, StubRoutines::cont_thaw());
  SET_ADDRESS(_stubs, StubRoutines::cont_returnBarrier());
  SET_ADDRESS(_stubs, StubRoutines::cont_returnBarrierExc());

  JFR_ONLY(SET_ADDRESS(_stubs, SharedRuntime::jfr_write_checkpoint());)

  SET_ADDRESS(_stubs, StubRoutines::jbyte_arraycopy());
  SET_ADDRESS(_stubs, StubRoutines::jshort_arraycopy());
  SET_ADDRESS(_stubs, StubRoutines::jint_arraycopy());
  SET_ADDRESS(_stubs, StubRoutines::jlong_arraycopy());
  SET_ADDRESS(_stubs, StubRoutines::_oop_arraycopy);
  SET_ADDRESS(_stubs, StubRoutines::_oop_arraycopy_uninit);

  SET_ADDRESS(_stubs, StubRoutines::jbyte_disjoint_arraycopy());
  SET_ADDRESS(_stubs, StubRoutines::jshort_disjoint_arraycopy());
  SET_ADDRESS(_stubs, StubRoutines::jint_disjoint_arraycopy());
  SET_ADDRESS(_stubs, StubRoutines::jlong_disjoint_arraycopy());
  SET_ADDRESS(_stubs, StubRoutines::_oop_disjoint_arraycopy);
  SET_ADDRESS(_stubs, StubRoutines::_oop_disjoint_arraycopy_uninit);

  SET_ADDRESS(_stubs, StubRoutines::arrayof_jbyte_arraycopy());
  SET_ADDRESS(_stubs, StubRoutines::arrayof_jshort_arraycopy());
  SET_ADDRESS(_stubs, StubRoutines::arrayof_jint_arraycopy());
  SET_ADDRESS(_stubs, StubRoutines::arrayof_jlong_arraycopy());
  SET_ADDRESS(_stubs, StubRoutines::_arrayof_oop_arraycopy);
  SET_ADDRESS(_stubs, StubRoutines::_arrayof_oop_arraycopy_uninit);

  SET_ADDRESS(_stubs, StubRoutines::arrayof_jbyte_disjoint_arraycopy());
  SET_ADDRESS(_stubs, StubRoutines::arrayof_jshort_disjoint_arraycopy());
  SET_ADDRESS(_stubs, StubRoutines::arrayof_jint_disjoint_arraycopy());
  SET_ADDRESS(_stubs, StubRoutines::arrayof_jlong_disjoint_arraycopy());
  SET_ADDRESS(_stubs, StubRoutines::_arrayof_oop_disjoint_arraycopy);
  SET_ADDRESS(_stubs, StubRoutines::_arrayof_oop_disjoint_arraycopy_uninit);

  SET_ADDRESS(_stubs, StubRoutines::_checkcast_arraycopy);
  SET_ADDRESS(_stubs, StubRoutines::_checkcast_arraycopy_uninit);

  SET_ADDRESS(_stubs, StubRoutines::unsafe_arraycopy());
  SET_ADDRESS(_stubs, StubRoutines::generic_arraycopy());

  SET_ADDRESS(_stubs, StubRoutines::jbyte_fill());
  SET_ADDRESS(_stubs, StubRoutines::jshort_fill());
  SET_ADDRESS(_stubs, StubRoutines::jint_fill());
  SET_ADDRESS(_stubs, StubRoutines::arrayof_jbyte_fill());
  SET_ADDRESS(_stubs, StubRoutines::arrayof_jshort_fill());
  SET_ADDRESS(_stubs, StubRoutines::arrayof_jint_fill());

  SET_ADDRESS(_stubs, StubRoutines::data_cache_writeback());
  SET_ADDRESS(_stubs, StubRoutines::data_cache_writeback_sync());

  SET_ADDRESS(_stubs, StubRoutines::aescrypt_encryptBlock());
  SET_ADDRESS(_stubs, StubRoutines::aescrypt_decryptBlock());
  SET_ADDRESS(_stubs, StubRoutines::cipherBlockChaining_encryptAESCrypt());
  SET_ADDRESS(_stubs, StubRoutines::cipherBlockChaining_decryptAESCrypt());
  SET_ADDRESS(_stubs, StubRoutines::electronicCodeBook_encryptAESCrypt());
  SET_ADDRESS(_stubs, StubRoutines::electronicCodeBook_decryptAESCrypt());
  SET_ADDRESS(_stubs, StubRoutines::poly1305_processBlocks());
  SET_ADDRESS(_stubs, StubRoutines::counterMode_AESCrypt());
  SET_ADDRESS(_stubs, StubRoutines::ghash_processBlocks());
  SET_ADDRESS(_stubs, StubRoutines::chacha20Block());
  SET_ADDRESS(_stubs, StubRoutines::base64_encodeBlock());
  SET_ADDRESS(_stubs, StubRoutines::base64_decodeBlock());
  SET_ADDRESS(_stubs, StubRoutines::md5_implCompress());
  SET_ADDRESS(_stubs, StubRoutines::md5_implCompressMB());
  SET_ADDRESS(_stubs, StubRoutines::sha1_implCompress());
  SET_ADDRESS(_stubs, StubRoutines::sha1_implCompressMB());
  SET_ADDRESS(_stubs, StubRoutines::sha256_implCompress());
  SET_ADDRESS(_stubs, StubRoutines::sha256_implCompressMB());
  SET_ADDRESS(_stubs, StubRoutines::sha512_implCompress());
  SET_ADDRESS(_stubs, StubRoutines::sha512_implCompressMB());
  SET_ADDRESS(_stubs, StubRoutines::sha3_implCompress());
  SET_ADDRESS(_stubs, StubRoutines::sha3_implCompressMB());
  SET_ADDRESS(_stubs, StubRoutines::double_keccak());
  SET_ADDRESS(_stubs, StubRoutines::intpoly_assign());
  SET_ADDRESS(_stubs, StubRoutines::intpoly_montgomeryMult_P256());
  SET_ADDRESS(_stubs, StubRoutines::dilithiumAlmostNtt());
  SET_ADDRESS(_stubs, StubRoutines::dilithiumAlmostInverseNtt());
  SET_ADDRESS(_stubs, StubRoutines::dilithiumNttMult());
  SET_ADDRESS(_stubs, StubRoutines::dilithiumMontMulByConstant());
  SET_ADDRESS(_stubs, StubRoutines::dilithiumDecomposePoly());

  SET_ADDRESS(_stubs, StubRoutines::updateBytesCRC32());
  SET_ADDRESS(_stubs, StubRoutines::updateBytesCRC32C());
  SET_ADDRESS(_stubs, StubRoutines::updateBytesAdler32());

  SET_ADDRESS(_stubs, StubRoutines::multiplyToLen());
  SET_ADDRESS(_stubs, StubRoutines::squareToLen());
  SET_ADDRESS(_stubs, StubRoutines::mulAdd());
  SET_ADDRESS(_stubs, StubRoutines::montgomeryMultiply());
  SET_ADDRESS(_stubs, StubRoutines::montgomerySquare());
  SET_ADDRESS(_stubs, StubRoutines::bigIntegerRightShift());
  SET_ADDRESS(_stubs, StubRoutines::bigIntegerLeftShift());
  SET_ADDRESS(_stubs, StubRoutines::galoisCounterMode_AESCrypt());

  SET_ADDRESS(_stubs, StubRoutines::vectorizedMismatch());

  SET_ADDRESS(_stubs, StubRoutines::unsafe_setmemory());

  SET_ADDRESS(_stubs, StubRoutines::dexp());
  SET_ADDRESS(_stubs, StubRoutines::dlog());
  SET_ADDRESS(_stubs, StubRoutines::dlog10());
  SET_ADDRESS(_stubs, StubRoutines::dpow());
  SET_ADDRESS(_stubs, StubRoutines::dsin());
  SET_ADDRESS(_stubs, StubRoutines::dcos());
  SET_ADDRESS(_stubs, StubRoutines::dlibm_reduce_pi04l());
  SET_ADDRESS(_stubs, StubRoutines::dlibm_sin_cos_huge());
  SET_ADDRESS(_stubs, StubRoutines::dlibm_tan_cot_huge());
  SET_ADDRESS(_stubs, StubRoutines::dtan());

  SET_ADDRESS(_stubs, StubRoutines::f2hf_adr());
  SET_ADDRESS(_stubs, StubRoutines::hf2f_adr());

  for (int slot = 0; slot < Klass::SECONDARY_SUPERS_TABLE_SIZE; slot++) {
    SET_ADDRESS(_stubs, StubRoutines::lookup_secondary_supers_table_stub(slot));
  }
  SET_ADDRESS(_stubs, StubRoutines::lookup_secondary_supers_table_slow_path_stub());

#if defined(AMD64) && !defined(ZERO)
  SET_ADDRESS(_stubs, StubRoutines::x86::d2i_fixup());
  SET_ADDRESS(_stubs, StubRoutines::x86::f2i_fixup());
  SET_ADDRESS(_stubs, StubRoutines::x86::f2l_fixup());
  SET_ADDRESS(_stubs, StubRoutines::x86::float_sign_mask());
  SET_ADDRESS(_stubs, StubRoutines::x86::float_sign_flip());
  SET_ADDRESS(_stubs, StubRoutines::x86::double_sign_mask());
  SET_ADDRESS(_stubs, StubRoutines::x86::vector_popcount_lut());
  SET_ADDRESS(_stubs, StubRoutines::x86::vector_float_sign_mask());
  SET_ADDRESS(_stubs, StubRoutines::x86::vector_float_sign_flip());
  SET_ADDRESS(_stubs, StubRoutines::x86::vector_double_sign_mask());
  SET_ADDRESS(_stubs, StubRoutines::x86::vector_double_sign_flip());
  SET_ADDRESS(_stubs, StubRoutines::x86::vector_int_shuffle_mask());
  SET_ADDRESS(_stubs, StubRoutines::x86::vector_byte_shuffle_mask());
  SET_ADDRESS(_stubs, StubRoutines::x86::vector_short_shuffle_mask());
  SET_ADDRESS(_stubs, StubRoutines::x86::vector_long_shuffle_mask());
  SET_ADDRESS(_stubs, StubRoutines::x86::vector_long_sign_mask());
  SET_ADDRESS(_stubs, StubRoutines::x86::vector_reverse_byte_perm_mask_int());
  SET_ADDRESS(_stubs, StubRoutines::x86::vector_reverse_byte_perm_mask_short());
  SET_ADDRESS(_stubs, StubRoutines::x86::vector_reverse_byte_perm_mask_long());
  // The iota indices are ordered by type B/S/I/L/F/D, and the offset between two types is 64.
  // See C2_MacroAssembler::load_iota_indices().
  for (int i = 0; i < 6; i++) {
    SET_ADDRESS(_stubs, StubRoutines::x86::vector_iota_indices() + i * 64);
  }
#endif
#if defined(AARCH64) && !defined(ZERO)
  SET_ADDRESS(_stubs, StubRoutines::aarch64::zero_blocks());
  SET_ADDRESS(_stubs, StubRoutines::aarch64::count_positives());
  SET_ADDRESS(_stubs, StubRoutines::aarch64::count_positives_long());
  SET_ADDRESS(_stubs, StubRoutines::aarch64::large_array_equals());
  SET_ADDRESS(_stubs, StubRoutines::aarch64::compare_long_string_LL());
  SET_ADDRESS(_stubs, StubRoutines::aarch64::compare_long_string_UU());
  SET_ADDRESS(_stubs, StubRoutines::aarch64::compare_long_string_LU());
  SET_ADDRESS(_stubs, StubRoutines::aarch64::compare_long_string_UL());
  SET_ADDRESS(_stubs, StubRoutines::aarch64::string_indexof_linear_ul());
  SET_ADDRESS(_stubs, StubRoutines::aarch64::string_indexof_linear_ll());
  SET_ADDRESS(_stubs, StubRoutines::aarch64::string_indexof_linear_uu());
  SET_ADDRESS(_stubs, StubRoutines::aarch64::large_byte_array_inflate());
  SET_ADDRESS(_stubs, StubRoutines::aarch64::spin_wait());

  SET_ADDRESS(_stubs, StubRoutines::aarch64::large_arrays_hashcode(T_BOOLEAN));
  SET_ADDRESS(_stubs, StubRoutines::aarch64::large_arrays_hashcode(T_BYTE));
  SET_ADDRESS(_stubs, StubRoutines::aarch64::large_arrays_hashcode(T_SHORT));
  SET_ADDRESS(_stubs, StubRoutines::aarch64::large_arrays_hashcode(T_CHAR));
  SET_ADDRESS(_stubs, StubRoutines::aarch64::large_arrays_hashcode(T_INT));
#endif

  _complete = true;
  log_info(aot, codecache, init)("Stubs recorded");
}

void AOTCodeAddressTable::init_early_c1() {
#ifdef COMPILER1
  // Runtime1 Blobs
  for (int i = 0; i <= (int)C1StubId::forward_exception_id; i++) {
    C1StubId id = (C1StubId)i;
    if (Runtime1::blob_for(id) == nullptr) {
      log_info(aot, codecache, init)("C1 blob %s is missing", Runtime1::name_for(id));
      continue;
    }
    if (Runtime1::entry_for(id) == nullptr) {
      log_info(aot, codecache, init)("C1 blob %s is missing entry", Runtime1::name_for(id));
      continue;
    }
    address entry = Runtime1::entry_for(id);
    SET_ADDRESS(_C1_blobs, entry);
  }
#endif // COMPILER1
  assert(_C1_blobs_length <= _C1_blobs_max, "increase _C1_blobs_max to %d", _C1_blobs_length);
  _early_c1_complete = true;
}

void AOTCodeAddressTable::init_c1() {
#ifdef COMPILER1
  // Runtime1 Blobs
  assert(_early_c1_complete, "early C1 blobs should be initialized");
  for (int i = (int)C1StubId::forward_exception_id + 1; i < (int)(C1StubId::NUM_STUBIDS); i++) {
    C1StubId id = (C1StubId)i;
    if (Runtime1::blob_for(id) == nullptr) {
      log_info(aot, codecache, init)("C1 blob %s is missing", Runtime1::name_for(id));
      continue;
    }
    if (Runtime1::entry_for(id) == nullptr) {
      log_info(aot, codecache, init)("C1 blob %s is missing entry", Runtime1::name_for(id));
      continue;
    }
    address entry = Runtime1::entry_for(id);
    SET_ADDRESS(_C1_blobs, entry);
  }
#if INCLUDE_G1GC
  if (UseG1GC) {
    G1BarrierSetC1* bs = (G1BarrierSetC1*)BarrierSet::barrier_set()->barrier_set_c1();
    address entry = bs->pre_barrier_c1_runtime_code_blob()->code_begin();
    SET_ADDRESS(_C1_blobs, entry);
    entry = bs->post_barrier_c1_runtime_code_blob()->code_begin();
    SET_ADDRESS(_C1_blobs, entry);
  }
#endif // INCLUDE_G1GC
#if INCLUDE_ZGC
  if (UseZGC) {
    ZBarrierSetC1* bs = (ZBarrierSetC1*)BarrierSet::barrier_set()->barrier_set_c1();
    SET_ADDRESS(_C1_blobs, bs->_load_barrier_on_oop_field_preloaded_runtime_stub);
    SET_ADDRESS(_C1_blobs, bs->_load_barrier_on_weak_oop_field_preloaded_runtime_stub);
    SET_ADDRESS(_C1_blobs, bs->_store_barrier_on_oop_field_with_healing);
    SET_ADDRESS(_C1_blobs, bs->_store_barrier_on_oop_field_without_healing);
  }
#endif // INCLUDE_ZGC
#if INCLUDE_SHENANDOAHGC
  if (UseShenandoahGC) {
    ShenandoahBarrierSetC1* bs = (ShenandoahBarrierSetC1*)BarrierSet::barrier_set()->barrier_set_c1();
    SET_ADDRESS(_C1_blobs, bs->pre_barrier_c1_runtime_code_blob()->code_begin());
    SET_ADDRESS(_C1_blobs, bs->load_reference_barrier_strong_rt_code_blob()->code_begin());
    SET_ADDRESS(_C1_blobs, bs->load_reference_barrier_strong_native_rt_code_blob()->code_begin());
    SET_ADDRESS(_C1_blobs, bs->load_reference_barrier_weak_rt_code_blob()->code_begin());
    SET_ADDRESS(_C1_blobs, bs->load_reference_barrier_phantom_rt_code_blob()->code_begin());
  }
#endif // INCLUDE_SHENANDOAHGC
#endif // COMPILER1

  assert(_C1_blobs_length <= _C1_blobs_max, "increase _C1_blobs_max to %d", _C1_blobs_length);
  _c1_complete = true;
  log_info(aot, codecache, init)("Runtime1 Blobs recorded");
}

void AOTCodeAddressTable::init_c2() {
#ifdef COMPILER2
  // OptoRuntime Blobs
  SET_ADDRESS(_C2_blobs, OptoRuntime::uncommon_trap_blob()->entry_point());
  SET_ADDRESS(_C2_blobs, OptoRuntime::exception_blob()->entry_point());
  SET_ADDRESS(_C2_blobs, OptoRuntime::new_instance_Java());
  SET_ADDRESS(_C2_blobs, OptoRuntime::new_array_Java());
  SET_ADDRESS(_C2_blobs, OptoRuntime::new_array_nozero_Java());
  SET_ADDRESS(_C2_blobs, OptoRuntime::multianewarray2_Java());
  SET_ADDRESS(_C2_blobs, OptoRuntime::multianewarray3_Java());
  SET_ADDRESS(_C2_blobs, OptoRuntime::multianewarray4_Java());
  SET_ADDRESS(_C2_blobs, OptoRuntime::multianewarray5_Java());
  SET_ADDRESS(_C2_blobs, OptoRuntime::multianewarrayN_Java());
  SET_ADDRESS(_C2_blobs, OptoRuntime::vtable_must_compile_stub());
  SET_ADDRESS(_C2_blobs, OptoRuntime::complete_monitor_locking_Java());
  SET_ADDRESS(_C2_blobs, OptoRuntime::monitor_notify_Java());
  SET_ADDRESS(_C2_blobs, OptoRuntime::monitor_notifyAll_Java());
  SET_ADDRESS(_C2_blobs, OptoRuntime::rethrow_stub());
  SET_ADDRESS(_C2_blobs, OptoRuntime::slow_arraycopy_Java());
  SET_ADDRESS(_C2_blobs, OptoRuntime::register_finalizer_Java());
  SET_ADDRESS(_C2_blobs, OptoRuntime::class_init_barrier_Java());
#if INCLUDE_JVMTI
  SET_ADDRESS(_C2_blobs, OptoRuntime::notify_jvmti_vthread_start());
  SET_ADDRESS(_C2_blobs, OptoRuntime::notify_jvmti_vthread_end());
  SET_ADDRESS(_C2_blobs, OptoRuntime::notify_jvmti_vthread_mount());
  SET_ADDRESS(_C2_blobs, OptoRuntime::notify_jvmti_vthread_unmount());
#endif /* INCLUDE_JVMTI */
#endif

  assert(_C2_blobs_length <= _C2_blobs_max, "increase _C2_blobs_max to %d", _C2_blobs_length);
  _c2_complete = true;
  log_info(aot, codecache, init)("OptoRuntime Blobs recorded");
}
#undef SET_ADDRESS

AOTCodeAddressTable::~AOTCodeAddressTable() {
  if (_extrs_addr != nullptr) {
    FREE_C_HEAP_ARRAY(address, _extrs_addr);
  }
  if (_stubs_addr != nullptr) {
    FREE_C_HEAP_ARRAY(address, _stubs_addr);
  }
  if (_shared_blobs_addr != nullptr) {
    FREE_C_HEAP_ARRAY(address, _shared_blobs_addr);
  }
}

#ifdef PRODUCT
#define MAX_STR_COUNT 200
#else
#define MAX_STR_COUNT 500
#endif
#define _c_str_max  MAX_STR_COUNT
static const int _c_str_base = _all_max;

static const char* _C_strings_in[MAX_STR_COUNT] = {nullptr}; // Incoming strings
static const char* _C_strings[MAX_STR_COUNT]    = {nullptr}; // Our duplicates
static int _C_strings_count = 0;
static int _C_strings_s[MAX_STR_COUNT] = {0};
static int _C_strings_id[MAX_STR_COUNT] = {0};
static int _C_strings_used = 0;

void AOTCodeCache::load_strings() {
  uint strings_count  = _load_header->strings_count();
  if (strings_count == 0) {
    return;
  }
  uint strings_offset = _load_header->strings_offset();
  uint* string_lengths = (uint*)addr(strings_offset);
  strings_offset += (strings_count * sizeof(uint));
  uint strings_size = _load_header->entries_offset() - strings_offset;
  // We have to keep cached strings longer than _cache buffer
  // because they are refernced from compiled code which may
  // still be executed on VM exit after _cache is freed.
  char* p = NEW_C_HEAP_ARRAY(char, strings_size+1, mtCode);
  memcpy(p, addr(strings_offset), strings_size);
  _C_strings_buf = p;
  assert(strings_count <= MAX_STR_COUNT, "sanity");
  for (uint i = 0; i < strings_count; i++) {
    _C_strings[i] = p;
    uint len = string_lengths[i];
    _C_strings_s[i] = i;
    _C_strings_id[i] = i;
    p += len;
  }
  assert((uint)(p - _C_strings_buf) <= strings_size, "(" INTPTR_FORMAT " - " INTPTR_FORMAT ") = %d > %d ", p2i(p), p2i(_C_strings_buf), (uint)(p - _C_strings_buf), strings_size);
  _C_strings_count = strings_count;
  _C_strings_used  = strings_count;
  log_debug(aot, codecache, init)("  Loaded %d C strings of total length %d at offset %d from AOT Code Cache", _C_strings_count, strings_size, strings_offset);
}

int AOTCodeCache::store_strings() {
  if (_C_strings_used > 0) {
    MutexLocker ml(AOTCodeCStrings_lock, Mutex::_no_safepoint_check_flag);
    uint offset = _write_position;
    uint length = 0;
    uint* lengths = (uint *)reserve_bytes(sizeof(uint) * _C_strings_used);
    if (lengths == nullptr) {
      return -1;
    }
    for (int i = 0; i < _C_strings_used; i++) {
      const char* str = _C_strings[_C_strings_s[i]];
      uint len = (uint)strlen(str) + 1;
      length += len;
      assert(len < 1000, "big string: %s", str);
      lengths[i] = len;
      uint n = write_bytes(str, len);
      if (n != len) {
        return -1;
      }
    }
    log_debug(aot, codecache, exit)("  Wrote %d C strings of total length %d at offset %d to AOT Code Cache",
                                   _C_strings_used, length, offset);
  }
  return _C_strings_used;
}

const char* AOTCodeCache::add_C_string(const char* str) {
  if (is_on_for_dump() && str != nullptr) {
    MutexLocker ml(AOTCodeCStrings_lock, Mutex::_no_safepoint_check_flag);
    AOTCodeAddressTable* table = addr_table();
    if (table != nullptr) {
      return table->add_C_string(str);
    }
  }
  return str;
}

const char* AOTCodeAddressTable::add_C_string(const char* str) {
  if (_extrs_complete) {
    // Check previous strings address
    for (int i = 0; i < _C_strings_count; i++) {
      if (_C_strings_in[i] == str) {
        return _C_strings[i]; // Found previous one - return our duplicate
      } else if (strcmp(_C_strings[i], str) == 0) {
        return _C_strings[i];
      }
    }
    // Add new one
    if (_C_strings_count < MAX_STR_COUNT) {
      // Passed in string can be freed and used space become inaccessible.
      // Keep original address but duplicate string for future compare.
      _C_strings_id[_C_strings_count] = -1; // Init
      _C_strings_in[_C_strings_count] = str;
      const char* dup = os::strdup(str);
      _C_strings[_C_strings_count++] = dup;
      log_trace(aot, codecache, stringtable)("add_C_string: [%d] " INTPTR_FORMAT " '%s'", _C_strings_count, p2i(dup), dup);
      return dup;
    } else {
      assert(false, "Number of C strings >= MAX_STR_COUNT");
    }
  }
  return str;
}

int AOTCodeAddressTable::id_for_C_string(address str) {
  if (str == nullptr) {
    return -1;
  }
  MutexLocker ml(AOTCodeCStrings_lock, Mutex::_no_safepoint_check_flag);
  for (int i = 0; i < _C_strings_count; i++) {
    if (_C_strings[i] == (const char*)str) { // found
      int id = _C_strings_id[i];
      if (id >= 0) {
        assert(id < _C_strings_used, "%d >= %d", id , _C_strings_used);
        return id; // Found recorded
      }
      // Not found in recorded, add new
      id = _C_strings_used++;
      _C_strings_s[id] = i;
      _C_strings_id[i] = id;
      return id;
    }
  }
  return -1;
}

address AOTCodeAddressTable::address_for_C_string(int idx) {
  assert(idx < _C_strings_count, "sanity");
  return (address)_C_strings[idx];
}

static int search_address(address addr, address* table, uint length) {
  for (int i = 0; i < (int)length; i++) {
    if (table[i] == addr) {
      return i;
    }
  }
  return BAD_ADDRESS_ID;
}

address AOTCodeAddressTable::address_for_id(int idx) {
  assert(_extrs_complete, "AOT Code Cache VM runtime addresses table is not complete");
  if (idx == -1) {
    return (address)-1;
  }
  uint id = (uint)idx;
  // special case for symbols based relative to os::init
  if (id > (_c_str_base + _c_str_max)) {
    return (address)os::init + idx;
  }
  if (idx < 0) {
    fatal("Incorrect id %d for AOT Code Cache addresses table", id);
    return nullptr;
  }
  // no need to compare unsigned id against 0
  if (/* id >= _extrs_base && */ id < _extrs_length) {
    return _extrs_addr[id - _extrs_base];
  }
  if (id >= _stubs_base && id < _stubs_base + _stubs_length) {
    return _stubs_addr[id - _stubs_base];
  }
  if (id >= _stubs_base && id < _stubs_base + _stubs_length) {
    return _stubs_addr[id - _stubs_base];
  }
  if (id >= _shared_blobs_base && id < _shared_blobs_base + _shared_blobs_length) {
    return _shared_blobs_addr[id - _shared_blobs_base];
  }
  if (id >= _C1_blobs_base && id < _C1_blobs_base + _C1_blobs_length) {
    return _C1_blobs_addr[id - _C1_blobs_base];
  }
  if (id >= _C1_blobs_base && id < _C1_blobs_base + _C1_blobs_length) {
    return _C1_blobs_addr[id - _C1_blobs_base];
  }
  if (id >= _C2_blobs_base && id < _C2_blobs_base + _C2_blobs_length) {
    return _C2_blobs_addr[id - _C2_blobs_base];
  }
  if (id >= _c_str_base && id < (_c_str_base + (uint)_C_strings_count)) {
    return address_for_C_string(id - _c_str_base);
  }
  fatal("Incorrect id %d for AOT Code Cache addresses table", id);
  return nullptr;
}

int AOTCodeAddressTable::id_for_address(address addr, RelocIterator reloc, CodeBlob* blob) {
  assert(_extrs_complete, "AOT Code Cache VM runtime addresses table is not complete");
  int id = -1;
  if (addr == (address)-1) { // Static call stub has jump to itself
    return id;
  }
  // Check card_table_base address first since it can point to any address
  BarrierSet* bs = BarrierSet::barrier_set();
  if (bs->is_a(BarrierSet::CardTableBarrierSet)) {
    if (addr == ci_card_table_address_as<address>()) {
      id = search_address(addr, _extrs_addr, _extrs_length);
      assert(id > 0 && _extrs_addr[id - _extrs_base] == addr, "sanity");
      return id;
    }
  }

  // Seach for C string
  id = id_for_C_string(addr);
  if (id >= 0) {
    return id + _c_str_base;
  }
  if (StubRoutines::contains(addr)) {
    // Search in stubs
    id = search_address(addr, _stubs_addr, _stubs_length);
    if (id == BAD_ADDRESS_ID) {
      StubCodeDesc* desc = StubCodeDesc::desc_for(addr);
      if (desc == nullptr) {
        desc = StubCodeDesc::desc_for(addr + frame::pc_return_offset);
      }
      const char* sub_name = (desc != nullptr) ? desc->name() : "<unknown>";
      assert(false, "Address " INTPTR_FORMAT " for Stub:%s is missing in AOT Code Cache addresses table", p2i(addr), sub_name);
    } else {
      return _stubs_base + id;
    }
  } else {
    CodeBlob* cb = CodeCache::find_blob(addr);
    if (cb != nullptr) {
      int id_base = _shared_blobs_base;
      // Search in code blobs
      id = search_address(addr, _shared_blobs_addr, _shared_blobs_length);
      if (id == BAD_ADDRESS_ID) {
        id_base = _C1_blobs_base;
        // search C1 blobs
        id = search_address(addr, _C1_blobs_addr, _C1_blobs_length);
      }
      if (id == BAD_ADDRESS_ID) {
        id_base = _C2_blobs_base;
        // search C2 blobs
        id = search_address(addr, _C2_blobs_addr, _C2_blobs_length);
      }
      if (id == BAD_ADDRESS_ID) {
        assert(false, "Address " INTPTR_FORMAT " for Blob:%s is missing in AOT Code Cache addresses table", p2i(addr), cb->name());
      } else {
        return id_base + id;
      }
    } else {
      // Search in runtime functions
      id = search_address(addr, _extrs_addr, _extrs_length);
      if (id == BAD_ADDRESS_ID) {
        ResourceMark rm;
        const int buflen = 1024;
        char* func_name = NEW_RESOURCE_ARRAY(char, buflen);
        int offset = 0;
        if (os::dll_address_to_function_name(addr, func_name, buflen, &offset)) {
          if (offset > 0) {
            // Could be address of C string
            uint dist = (uint)pointer_delta(addr, (address)os::init, 1);
            CompileTask* task = ciEnv::current()->task();
            uint compile_id = 0;
            uint comp_level =0;
            if (task != nullptr) { // this could be called from compiler runtime initialization (compiler blobs)
              compile_id = task->compile_id();
              comp_level = task->comp_level();
            }
            log_debug(aot, codecache)("%d (L%d): Address " INTPTR_FORMAT " (offset %d) for runtime target '%s' is missing in AOT Code Cache addresses table",
                          compile_id, comp_level, p2i(addr), dist, (const char*)addr);
            assert(dist > (uint)(_all_max + MAX_STR_COUNT), "change encoding of distance");
            return dist;
          }
          reloc.print_current_on(tty);
          blob->print_on(tty);
          blob->print_code_on(tty);
          assert(false, "Address " INTPTR_FORMAT " for runtime target '%s+%d' is missing in AOT Code Cache addresses table", p2i(addr), func_name, offset);
        } else {
          reloc.print_current_on(tty);
          blob->print_on(tty);
          blob->print_code_on(tty);
          os::find(addr, tty);
          assert(false, "Address " INTPTR_FORMAT " for <unknown>/('%s') is missing in AOT Code Cache addresses table", p2i(addr), (const char*)addr);
        }
      } else {
        return _extrs_base + id;
      }
    }
  }
  return id;
}

#undef _extrs_max
#undef _stubs_max
#undef _shared_blobs_max
#undef _C1_blobs_max
#undef _C2_blobs_max
#undef _blobs_max
#undef _extrs_base
#undef _stubs_base
#undef _shared_blobs_base
#undef _C1_blobs_base
#undef _C2_blobs_base
#undef _blobs_end

void AOTRuntimeConstants::initialize_from_runtime() {
  BarrierSet* bs = BarrierSet::barrier_set();
  if (bs->is_a(BarrierSet::CardTableBarrierSet)) {
    CardTableBarrierSet* ctbs = ((CardTableBarrierSet*)bs);
    _aot_runtime_constants._grain_shift = ctbs->grain_shift();
    _aot_runtime_constants._card_shift = ctbs->card_shift();
  }
}

AOTRuntimeConstants AOTRuntimeConstants::_aot_runtime_constants;

address AOTRuntimeConstants::_field_addresses_list[] = {
  grain_shift_address(),
  card_shift_address(),
  nullptr
};


void AOTCodeCache::wait_for_no_nmethod_readers() {
  while (true) {
    int cur = Atomic::load(&_nmethod_readers);
    int upd = -(cur + 1);
    if (cur >= 0 && Atomic::cmpxchg(&_nmethod_readers, cur, upd) == cur) {
      // Success, no new readers should appear.
      break;
    }
  }

  // Now wait for all readers to leave.
  SpinYield w;
  while (Atomic::load(&_nmethod_readers) != -1) {
    w.wait();
  }
}

AOTCodeCache::ReadingMark::ReadingMark() {
  while (true) {
    int cur = Atomic::load(&_nmethod_readers);
    if (cur < 0) {
      // Cache is already closed, cannot proceed.
      _failed = true;
      return;
    }
    if (Atomic::cmpxchg(&_nmethod_readers, cur, cur + 1) == cur) {
      // Successfully recorded ourselves as entered.
      _failed = false;
      return;
    }
  }
}

AOTCodeCache::ReadingMark::~ReadingMark() {
  if (_failed) {
    return;
  }
  while (true) {
    int cur = Atomic::load(&_nmethod_readers);
    if (cur > 0) {
      // Cache is open, we are counting down towards 0.
      if (Atomic::cmpxchg(&_nmethod_readers, cur, cur - 1) == cur) {
        return;
      }
    } else {
      // Cache is closed, we are counting up towards -1.
      if (Atomic::cmpxchg(&_nmethod_readers, cur, cur + 1) == cur) {
        return;
      }
    }
  }
}

void AOTCodeCache::print_timers_on(outputStream* st) {
  if (is_using_code()) {
    st->print_cr ("    AOT Code Load Time:   %7.3f s", _t_totalLoad.seconds());
    st->print_cr ("      nmethod register:     %7.3f s", _t_totalRegister.seconds());
    st->print_cr ("      find AOT code entry:  %7.3f s", _t_totalFind.seconds());
  }
  if (is_dumping_code()) {
    st->print_cr ("    AOT Code Store Time:  %7.3f s", _t_totalStore.seconds());
  }
}

AOTCodeStats AOTCodeStats::add_aot_code_stats(AOTCodeStats stats1, AOTCodeStats stats2) {
  AOTCodeStats result;
  for (int kind = AOTCodeEntry::None; kind < AOTCodeEntry::Kind_count; kind++) {
    result.ccstats._kind_cnt[kind] = stats1.entry_count(kind) + stats2.entry_count(kind);
  }

  for (int lvl = CompLevel_none; lvl < AOTCompLevel_count; lvl++) {
    result.ccstats._nmethod_cnt[lvl] = stats1.nmethod_count(lvl) + stats2.nmethod_count(lvl);
  }
  result.ccstats._clinit_barriers_cnt = stats1.clinit_barriers_count() + stats2.clinit_barriers_count();
  return result;
}

void AOTCodeCache::log_stats_on_exit() {
  LogStreamHandle(Debug, aot, codecache, exit) log;
  if (log.is_enabled()) {
    AOTCodeStats prev_stats;
    AOTCodeStats current_stats;
    AOTCodeStats total_stats;
    uint max_size = 0;

    uint load_count = (_load_header != nullptr) ? _load_header->entries_count() : 0;

    for (uint i = 0; i < load_count; i++) {
      prev_stats.collect_entry_stats(&_load_entries[i]);
      if (max_size < _load_entries[i].size()) {
        max_size = _load_entries[i].size();
      }
    }
    for (uint i = 0; i < _store_entries_cnt; i++) {
      current_stats.collect_entry_stats(&_store_entries[i]);
      if (max_size < _store_entries[i].size()) {
        max_size = _store_entries[i].size();
      }
    }
    total_stats = AOTCodeStats::add_aot_code_stats(prev_stats, current_stats);

    log.print_cr("Wrote %d AOTCodeEntry entries(%u max size) to AOT Code Cache",
                 total_stats.total_count(), max_size);
    for (uint kind = AOTCodeEntry::None; kind < AOTCodeEntry::Kind_count; kind++) {
      if (total_stats.entry_count(kind) > 0) {
        log.print_cr("  %s: total=%u(old=%u+new=%u)",
                     aot_code_entry_kind_name[kind], total_stats.entry_count(kind), prev_stats.entry_count(kind), current_stats.entry_count(kind));
        if (kind == AOTCodeEntry::Code) {
          for (uint lvl = CompLevel_none; lvl < AOTCompLevel_count; lvl++) {
            if (total_stats.nmethod_count(lvl) > 0) {
              log.print_cr("    Tier %d: total=%u(old=%u+new=%u)",
                           lvl, total_stats.nmethod_count(lvl), prev_stats.nmethod_count(lvl), current_stats.nmethod_count(lvl));
            }
          }
        }
      }
    }
    log.print_cr("Total=%u(old=%u+new=%u)", total_stats.total_count(), prev_stats.total_count(), current_stats.total_count());
  }
}

static void print_helper1(outputStream* st, const char* name, int count) {
  if (count > 0) {
    st->print(" %s=%d", name, count);
  }
}

void AOTCodeCache::print_statistics_on(outputStream* st) {
  AOTCodeCache* cache = open_for_use();
  if (cache != nullptr) {
    ReadingMark rdmk;
    if (rdmk.failed()) {
      // Cache is closed, cannot touch anything.
      return;
    }

    uint count = cache->_load_header->entries_count();
    uint* search_entries = (uint*)cache->addr(cache->_load_header->entries_offset()); // [id, index]
    AOTCodeEntry* load_entries = (AOTCodeEntry*)(search_entries + 2 * count);

    AOTCodeStats stats;
    for (uint i = 0; i < count; i++) {
      stats.collect_all_stats(&load_entries[i]);
    }

    for (uint kind = AOTCodeEntry::None; kind < AOTCodeEntry::Kind_count; kind++) {
      if (stats.entry_count(kind) > 0) {
        st->print("  %s:", aot_code_entry_kind_name[kind]);
        print_helper1(st, "total", stats.entry_count(kind));
        print_helper1(st, "loaded", stats.entry_loaded_count(kind));
        print_helper1(st, "invalidated", stats.entry_invalidated_count(kind));
        print_helper1(st, "failed", stats.entry_load_failed_count(kind));
        st->cr();
      }
      if (kind == AOTCodeEntry::Code) {
        for (uint lvl = CompLevel_none; lvl < AOTCompLevel_count; lvl++) {
          if (stats.nmethod_count(lvl) > 0) {
            st->print("    AOT Code T%d", lvl);
            print_helper1(st, "total", stats.nmethod_count(lvl));
            print_helper1(st, "loaded", stats.nmethod_loaded_count(lvl));
            print_helper1(st, "invalidated", stats.nmethod_invalidated_count(lvl));
            print_helper1(st, "failed", stats.nmethod_load_failed_count(lvl));
            if (lvl == AOTCompLevel_count-1) {
              print_helper1(st, "has_clinit_barriers", stats.clinit_barriers_count());
            }
            st->cr();
          }
        }
      }
    }
    LogStreamHandle(Debug, aot, codecache, init) log;
    if (log.is_enabled()) {
      AOTCodeCache::print_unused_entries_on(&log);
    }
    LogStreamHandle(Trace, aot, codecache) aot_info;
    // need a lock to traverse the code cache
    if (aot_info.is_enabled()) {
      MutexLocker locker(CodeCache_lock, Mutex::_no_safepoint_check_flag);
      NMethodIterator iter(NMethodIterator::all);
      while (iter.next()) {
        nmethod* nm = iter.method();
        if (nm->is_in_use() && !nm->is_native_method() && !nm->is_osr_method()) {
          aot_info.print("%5d:%c%c%c%d:", nm->compile_id(),
                         (nm->method()->is_shared() ? 'S' : ' '),
                         (nm->is_aot() ? 'A' : ' '),
                         (nm->preloaded() ? 'P' : ' '),
                         nm->comp_level());
          print_helper(nm, &aot_info);
          aot_info.print(": ");
          CompileTask::print(&aot_info, nm, nullptr, true /*short_form*/);
          LogStreamHandle(Trace, aot, codecache) aot_debug;
          if (aot_debug.is_enabled()) {
            MethodTrainingData* mtd = MethodTrainingData::find(methodHandle(Thread::current(), nm->method()));
            if (mtd != nullptr) {
              mtd->iterate_compiles([&](CompileTrainingData* ctd) {
                aot_debug.print("     CTD: "); ctd->print_on(&aot_debug); aot_debug.cr();
              });
            }
          }
        }
      }
    }
  } else {
    st->print_cr("failed to map code cache");
  }
}

void AOTCodeEntry::print(outputStream* st) const {
  st->print_cr(" AOT Code Cache entry " INTPTR_FORMAT " [kind: %d, id: " UINT32_FORMAT_X_0 ", offset: %d, size: %d, comp_level: %d, comp_id: %d, %s%s%s%s]",
               p2i(this), (int)_kind, _id, _offset, _size, _comp_level, _comp_id,
               (_not_entrant? "not_entrant" : "entrant"),
               (_loaded ? ", loaded" : ""),
               (_has_clinit_barriers ? ", has_clinit_barriers" : ""),
               (_for_preload ? ", for_preload" : ""));
}

void AOTCodeCache::print_on(outputStream* st) {
  if (opened_cache != nullptr && opened_cache->for_use()) {
    ReadingMark rdmk;
    if (rdmk.failed()) {
      // Cache is closed, cannot touch anything.
      return;
    }

    st->print_cr("\nAOT Code Cache");
    uint count = opened_cache->_load_header->entries_count();
    uint* search_entries = (uint*)opened_cache->addr(opened_cache->_load_header->entries_offset()); // [id, index]
    AOTCodeEntry* load_entries = (AOTCodeEntry*)(search_entries + 2 * count);

    for (uint i = 0; i < count; i++) {
      int index = search_entries[2*i + 1];
      AOTCodeEntry* entry = &(load_entries[index]);

      uint entry_position = entry->offset();
      uint name_offset = entry->name_offset() + entry_position;
      const char* saved_name = opened_cache->addr(name_offset);

      st->print_cr("%4u: %10s idx:%4u Id:%u L%u size=%u '%s' %s%s%s%s",
                   i, aot_code_entry_kind_name[entry->kind()], index, entry->id(), entry->comp_level(),
                   entry->size(),  saved_name,
                   entry->has_clinit_barriers() ? " has_clinit_barriers" : "",
                   entry->for_preload()         ? " for_preload"         : "",
                   entry->is_loaded()           ? " loaded"              : "",
                   entry->not_entrant()         ? " not_entrant"         : "");

      st->print_raw("         ");
      AOTCodeReader reader(opened_cache, entry, nullptr);
      reader.print_on(st);
    }
  }
}

void AOTCodeCache::print_unused_entries_on(outputStream* st) {
  LogStreamHandle(Info, aot, codecache, init) info;
  if (info.is_enabled()) {
    AOTCodeCache::iterate([&](AOTCodeEntry* entry) {
      if (entry->is_code() && !entry->is_loaded()) {
        MethodTrainingData* mtd = MethodTrainingData::find(methodHandle(Thread::current(), entry->method()));
        if (mtd != nullptr) {
          if (mtd->has_holder()) {
            if (mtd->holder()->method_holder()->is_initialized()) {
              ResourceMark rm;
              mtd->iterate_compiles([&](CompileTrainingData* ctd) {
                if ((uint)ctd->level() == entry->comp_level()) {
                  if (ctd->init_deps_left() == 0) {
                    nmethod* nm = mtd->holder()->code();
                    if (nm == nullptr) {
                      if (mtd->holder()->queued_for_compilation()) {
                        return; // scheduled for compilation
                      }
                    } else if ((uint)nm->comp_level() >= entry->comp_level()) {
                      return; // already online compiled and superseded by a more optimal method
                    }
                    info.print("AOT Code Cache entry not loaded: ");
                    ctd->print_on(&info);
                    info.cr();
                  }
                }
              });
            } else {
              // not yet initialized
            }
          } else {
            info.print("AOT Code Cache entry doesn't have a holder: ");
            mtd->print_on(&info);
            info.cr();
          }
        }
      }
    });
  }
}

void AOTCodeReader::print_on(outputStream* st) {
  uint entry_position = _entry->offset();
  set_read_position(entry_position);

  // Read name
  uint name_offset = entry_position + _entry->name_offset();
  uint name_size = _entry->name_size(); // Includes '/0'
  const char* name = addr(name_offset);

  st->print_cr("  name: %s", name);
}

