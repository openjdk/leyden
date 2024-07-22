/*
 * Copyright (c) 2023, 2024, Oracle and/or its affiliates. All rights reserved.
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

#include "precompiled.hpp"
#include "asm/macroAssembler.hpp"
#include "cds/cdsAccess.hpp"
#include "cds/cdsConfig.hpp"
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
#include "code/codeBlob.hpp"
#include "code/codeCache.hpp"
#include "code/oopRecorder.inline.hpp"
#include "code/SCCache.hpp"
#include "compiler/abstractCompiler.hpp"
#include "compiler/compilationPolicy.hpp"
#include "compiler/compileBroker.hpp"
#include "compiler/compileTask.hpp"
#include "gc/g1/g1BarrierSetRuntime.hpp"
#include "gc/shared/gcConfig.hpp"
#include "logging/log.hpp"
#include "memory/universe.hpp"
#include "oops/klass.inline.hpp"
#include "oops/method.inline.hpp"
#include "oops/trainingData.hpp"
#include "prims/jvmtiThreadState.hpp"
#include "runtime/atomic.hpp"
#include "runtime/flags/flagSetting.hpp"
#include "runtime/globals_extension.hpp"
#include "runtime/handles.inline.hpp"
#include "runtime/java.hpp"
#include "runtime/jniHandles.inline.hpp"
#include "runtime/os.inline.hpp"
#include "runtime/sharedRuntime.hpp"
#include "runtime/stubCodeGenerator.hpp"
#include "runtime/stubRoutines.hpp"
#include "runtime/timerTrace.hpp"
#include "runtime/threadIdentifier.hpp"
#include "utilities/ostream.hpp"
#include "utilities/spinYield.hpp"
#ifdef COMPILER1
#include "c1/c1_Runtime1.hpp"
#include "c1/c1_LIRAssembler.hpp"
#include "gc/shared/c1/barrierSetC1.hpp"
#include "gc/g1/c1/g1BarrierSetC1.hpp"
#if INCLUDE_SHENANDOAHGC
#include "gc/shenandoah/c1/shenandoahBarrierSetC1.hpp"
#endif
#include "gc/z/c1/zBarrierSetC1.hpp"
#endif
#ifdef COMPILER2
#include "opto/runtime.hpp"
#endif
#if INCLUDE_JVMCI
#include "jvmci/jvmci.hpp"
#endif
#if INCLUDE_SHENANDOAHGC
#include "gc/shenandoah/shenandoahRuntime.hpp"
#endif

#include <sys/stat.h>
#include <errno.h>

#ifndef O_BINARY       // if defined (Win32) use binary files.
#define O_BINARY 0     // otherwise do nothing.
#endif

static elapsedTimer _t_totalLoad;
static elapsedTimer _t_totalRegister;
static elapsedTimer _t_totalFind;
static elapsedTimer _t_totalStore;

SCCache* SCCache::_cache = nullptr;

static bool enable_timers() {
  return CITime || log_is_enabled(Info, init);
}

static void exit_vm_on_load_failure() {
  // Treat SCC warnings as error when RequireSharedSpaces is on.
  if (RequireSharedSpaces) {
    vm_exit_during_initialization("Unable to used startup cached code.", nullptr);
  }
}

static void exit_vm_on_store_failure() {
  // Treat SCC warnings as error when RequireSharedSpaces is on.
  if (RequireSharedSpaces) {
    tty->print_cr("Unable to create startup cached code.");
    // Failure during AOT code caching, we don't want to dump core
    vm_abort(false);
  }
}
void SCCache::initialize() {
  if (LoadCachedCode && !UseSharedSpaces) {
    return;
  }
  if (StoreCachedCode || LoadCachedCode) {
    if (FLAG_IS_DEFAULT(ClassInitBarrierMode)) {
      FLAG_SET_DEFAULT(ClassInitBarrierMode, 1);
    }
  } else if (ClassInitBarrierMode > 0) {
    log_info(scc, init)("Set ClassInitBarrierMode to 0 because StoreCachedCode and LoadCachedCode are false.");
    FLAG_SET_DEFAULT(ClassInitBarrierMode, 0);
  }
  if ((LoadCachedCode || StoreCachedCode) && CachedCodeFile != nullptr) {
    const int len = (int)strlen(CachedCodeFile);
    // cache file path
    char* path  = NEW_C_HEAP_ARRAY(char, len+1, mtCode);
    memcpy(path, CachedCodeFile, len);
    path[len] = '\0';
    if (!open_cache(path)) {
      exit_vm_on_load_failure();
      return;
    }
    if (StoreCachedCode) {
      FLAG_SET_DEFAULT(FoldStableValues, false);
      FLAG_SET_DEFAULT(ForceUnreachable, true);
    }
    FLAG_SET_DEFAULT(DelayCompilerStubsGeneration, false);
  }
}

void SCCache::init2() {
  if (!is_on()) {
    return;
  }
  // After Universe initialized
  BarrierSet* bs = BarrierSet::barrier_set();
  if (bs->is_a(BarrierSet::CardTableBarrierSet)) {
    address byte_map_base = ci_card_table_address_as<address>();
    if (is_on_for_write() && !external_word_Relocation::can_be_relocated(byte_map_base)) {
      // Bail out since we can't encode card table base address with relocation
      log_warning(scc, init)("Can't create Startup Code Cache because card table base address is not relocatable: " INTPTR_FORMAT, p2i(byte_map_base));
      close();
      exit_vm_on_load_failure();
    }
  }
  // initialize aot runtime constants as appropriate to this runtime
  AOTRuntimeConstants::initialize_from_runtime();

  if (!verify_vm_config()) {
    close();
    exit_vm_on_load_failure();
  }
}

void SCCache::print_timers_on(outputStream* st) {
  if (LoadCachedCode) {
    st->print_cr ("    SC Load Time:         %7.3f s", _t_totalLoad.seconds());
    st->print_cr ("      nmethod register:     %7.3f s", _t_totalRegister.seconds());
    st->print_cr ("      find cached code:     %7.3f s", _t_totalFind.seconds());
  }
  if (StoreCachedCode) {
    st->print_cr ("    SC Store Time:        %7.3f s", _t_totalStore.seconds());
  }
}

bool SCCache::is_C3_on() {
#if INCLUDE_JVMCI
  if (UseJVMCICompiler) {
    return (StoreCachedCode || LoadCachedCode) && UseC2asC3;
  }
#endif
  return false;
}

bool SCCache::is_code_load_thread_on() {
  return UseCodeLoadThread && LoadCachedCode;
}

bool SCCache::gen_preload_code(ciMethod* m, int entry_bci) {
  VM_ENTRY_MARK;
  return (entry_bci == InvocationEntryBci) && is_on() && _cache->gen_preload_code() &&
         CDSAccess::can_generate_cached_code(m->get_Method());
}

static void print_helper(nmethod* nm, outputStream* st) {
  SCCache::iterate([&](SCCEntry* e) {
    if (e->method() == nm->method()) {
      ResourceMark rm;
      stringStream ss;
      ss.print("A%s%d", (e->for_preload() ? "P" : ""), e->comp_level());
      if (e->decompile() > 0) {
        ss.print("+D%d", e->decompile());
      }
      ss.print("[%s%s%s]",
               (e->is_loaded()   ? "L" : ""),
               (e->load_fail()   ? "F" : ""),
               (e->not_entrant() ? "I" : ""));
      ss.print("#%d", e->comp_id());

      st->print(" %s", ss.freeze());
    }
  });
}

void SCCache::close() {
  if (is_on()) {
    if (SCCache::is_on_for_read()) {
      LogStreamHandle(Info, init) log;
      if (log.is_enabled()) {
        log.print_cr("Startup Code Cache statistics (when closed): ");
        SCCache::print_statistics_on(&log);
        log.cr();
        SCCache::print_timers_on(&log);

        LogStreamHandle(Info, scc, init) log1;
        if (log1.is_enabled()) {
          SCCache::print_unused_entries_on(&log1);
        }

        LogStreamHandle(Info, scc, codecache) info_scc;
        if (info_scc.is_enabled()) {
          NMethodIterator iter(NMethodIterator::all);
          while (iter.next()) {
            nmethod* nm = iter.method();
            if (nm->is_in_use() && !nm->is_native_method() && !nm->is_osr_method()) {
              info_scc.print("%5d:%c%c%c%d:", nm->compile_id(),
                             (nm->method()->is_shared() ? 'S' : ' '),
                             (nm->is_scc() ? 'A' : ' '),
                             (nm->preloaded() ? 'P' : ' '),
                             nm->comp_level());
              print_helper(nm, &info_scc);
              info_scc.print(": ");
              CompileTask::print(&info_scc, nm, nullptr, true /*short_form*/);

              LogStreamHandle(Debug, scc, codecache) debug_scc;
              if (debug_scc.is_enabled()) {
                MethodTrainingData* mtd = MethodTrainingData::lookup_for(nm->method());
                if (mtd != nullptr) {
                  mtd->iterate_all_compiles([&](CompileTrainingData* ctd) {
                    debug_scc.print("     CTD: "); ctd->print_on(&debug_scc); debug_scc.cr();
                  });
                }
              }
            }
          }
        }
      }
    }

    delete _cache; // Free memory
    _cache = nullptr;
  }
}

void SCCache::invalidate(SCCEntry* entry) {
  // This could be concurent execution
  if (entry != nullptr && is_on()) { // Request could come after cache is closed.
    _cache->invalidate_entry(entry);
  }
}

bool SCCache::is_loaded(SCCEntry* entry) {
  if (is_on() && _cache->cache_buffer() != nullptr) {
    return (uint)((char*)entry - _cache->cache_buffer()) < _cache->load_size();
  }
  return false;
}

void SCCache::preload_code(JavaThread* thread) {
  if ((ClassInitBarrierMode == 0) || !is_on_for_read()) {
    return;
  }
  if ((DisableCachedCode & (1 << 3)) != 0) {
    return; // no preloaded code (level 5);
  }
  _cache->preload_startup_code(thread);
}

SCCEntry* SCCache::find_code_entry(const methodHandle& method, uint comp_level) {
  switch (comp_level) {
    case CompLevel_simple:
      if ((DisableCachedCode & (1 << 0)) != 0) {
        return nullptr;
      }
      break;
    case CompLevel_limited_profile:
      if ((DisableCachedCode & (1 << 1)) != 0) {
        return nullptr;
      }
      break;
    case CompLevel_full_optimization:
      if ((DisableCachedCode & (1 << 2)) != 0) {
        return nullptr;
      }
      break;

    default: return nullptr; // Level 1, 2, and 4 only
  }
  TraceTime t1("SC total find code time", &_t_totalFind, enable_timers(), false);
  if (is_on() && _cache->cache_buffer() != nullptr) {
    MethodData* md = method->method_data();
    uint decomp = (md == nullptr) ? 0 : md->decompile_count();

    ResourceMark rm;
    const char* target_name = method->name_and_sig_as_C_string();
    uint hash = java_lang_String::hash_code((const jbyte*)target_name, (int)strlen(target_name));
    SCCEntry* entry = _cache->find_entry(SCCEntry::Code, hash, comp_level, decomp);
    if (entry == nullptr) {
      log_info(scc, nmethod)("Missing entry for '%s' (comp_level %d, decomp: %d, hash: " UINT32_FORMAT_X_0 ")", target_name, (uint)comp_level, decomp, hash);
#ifdef ASSERT
    } else {
      uint name_offset = entry->offset() + entry->name_offset();
      uint name_size   = entry->name_size(); // Includes '/0'
      const char* name = _cache->cache_buffer() + name_offset;
      if (strncmp(target_name, name, name_size) != 0) {
        assert(false, "SCA: saved nmethod's name '%s' is different from '%s', hash: " UINT32_FORMAT_X_0, name, target_name, hash);
      }
#endif
    }

    DirectiveSet* directives = DirectivesStack::getMatchingDirective(method, nullptr);
    if (directives->IgnorePrecompiledOption) {
      LogStreamHandle(Info, scc, compilation) log;
      if (log.is_enabled()) {
        log.print("Ignore cached code entry on level %d for ", comp_level);
        method->print_value_on(&log);
      }
      return nullptr;
    }

    return entry;
  }
  return nullptr;
}

void SCCache::add_C_string(const char* str) {
  if (is_on_for_write()) {
    _cache->add_new_C_string(str);
  }
}

bool SCCache::allow_const_field(ciConstant& value) {
  return !is_on() || !StoreCachedCode // Restrict only when we generate cache
        // Can not trust primitive too   || !is_reference_type(value.basic_type())
        // May disable this too for now  || is_reference_type(value.basic_type()) && value.as_object()->should_be_constant()
        ;
}

bool SCCache::open_cache(const char* cache_path) {
  if (LoadCachedCode) {
    log_info(scc)("Trying to load Startup Code Cache '%s'", cache_path);
    struct stat st;
    if (os::stat(cache_path, &st) != 0) {
      log_warning(scc, init)("Specified Startup Code Cache file not found '%s'", cache_path);
      return false;
    } else if ((st.st_mode & S_IFMT) != S_IFREG) {
      log_warning(scc, init)("Specified Startup Code Cache is not file '%s'", cache_path);
      return false;
    }
    int fd = os::open(cache_path, O_RDONLY | O_BINARY, 0);
    if (fd < 0) {
      if (errno == ENOENT) {
        log_warning(scc, init)("Specified Startup Code Cache file not found '%s'", cache_path);
      } else {
        log_warning(scc, init)("Failed to open Startup Code Cache file '%s': (%s)", cache_path, os::strerror(errno));
      }
      return false;
    } else {
      log_info(scc, init)("Opened for read Startup Code Cache '%s'", cache_path);
    }
    SCCache* cache = new SCCache(cache_path, fd, (uint)st.st_size);
    bool failed = cache->failed();
    if (::close(fd) < 0) {
      log_warning(scc)("Failed to close for read Startup Code Cache file '%s'", cache_path);
      failed = true;
    }
    if (failed) {
      delete cache;
      _cache = nullptr;
      return false;
    }
    _cache = cache;
  }
  if (_cache == nullptr && StoreCachedCode) {
    SCCache* cache = new SCCache(cache_path, -1 /* fd */, 0 /* size */);
    if (cache->failed()) {
      delete cache;
      _cache = nullptr;
      return false;
    }
    _cache = cache;
  }
  return true;
}

class CachedCodeDirectory {
public:
  int _some_number;
  InstanceKlass* _some_klass;
  size_t _my_data_length;
  void* _my_data;
};

// Skeleton code for including cached code in CDS:
//
// [1] Use CachedCodeDirectory to keep track of all of data related to cached code.
//     E.g., you can build a hashtable to record what methods have been archived.
//
// [2] Memory for all data for cached code, including CachedCodeDirectory, should be
//     allocated using CDSAccess::allocate_from_code_cache().
//
// [3] CachedCodeDirectory must be the very first allocation.
//
// [4] Two kinds of pointer can be stored:
//     - A pointer p that points to metadata. CDSAccess::can_generate_cached_code(p) must return true.
//     - A pointer to a buffer returned by CDSAccess::allocate_from_code_cache().
//       (It's OK to point to an interior location within this buffer).
//     Such pointers must be stored using CDSAccess::set_pointer()
//
// The buffers allocated by CDSAccess::allocate_from_code_cache() are in a contiguous region. At runtime, this
// region is mapped to the beginning of the CodeCache (see _cds_code_space in codeCache.cpp). All the pointers
// in this buffer are relocated as necessary (e.g., to account for the runtime location of the CodeCache).
//
// Example:
//
// # make sure hw.cds doesn't exist, so that it's regenerated (1.5 step training)
// $ rm -f hw.cds; java -Xlog:cds,scc::uptime,tags,pid -XX:CacheDataStore=hw.cds -cp ~/tmp/HelloWorld.jar HelloWorld
//
// # After training is finish, hw.cds should contain a CachedCodeDirectory. You can see the effect of relocation
// # from the [scc] log.
// $ java -Xlog:cds,scc -XX:CacheDataStore=hw.cds -cp ~/tmp/HelloWorld.jar HelloWorld
// [0.016s][info][scc] new workflow: cached code mapped at 0x7fef97ebc000
// [0.016s][info][scc] _cached_code_directory->_some_klass     = 0x800009ca8 (java.lang.String)
// [0.016s][info][scc] _cached_code_directory->_some_number    = 0
// [0.016s][info][scc] _cached_code_directory->_my_data_length = 0
// [0.016s][info][scc] _cached_code_directory->_my_data        = 0x7fef97ebc020 (32 bytes offset from base)
//
// The 1.5 step training may be hard to debug. If you want to run in a debugger, run the above training step
// with an additional "-XX:+CDSManualFinalImage" command-line argument.

// This is always at the very beginning of the mmaped CDS "cc" (cached code) region
static CachedCodeDirectory* _cached_code_directory = nullptr;

#if INCLUDE_CDS_JAVA_HEAP
void SCCache::new_workflow_start_writing_cache() {
  CachedCodeDirectory* dir = (CachedCodeDirectory*)CDSAccess::allocate_from_code_cache(sizeof(CachedCodeDirectory));
  _cached_code_directory = dir;

  CDSAccess::set_pointer(&dir->_some_klass, vmClasses::String_klass());

  size_t n = 120;
  void* d = (void*)CDSAccess::allocate_from_code_cache(n);
  CDSAccess::set_pointer(&dir->_my_data, d);
}

void SCCache::new_workflow_end_writing_cache() {

}

void SCCache::new_workflow_load_cache() {
  void* ptr = CodeCache::map_cached_code();
  if (ptr != nullptr) {
    // At this point:
    // - CodeCache::initialize_heaps() has finished.
    // - CDS archive is fully mapped ("metadata", "heap" and "cached_code" regions are mapped)
    // - All pointers in the mapped CDS regions are relocated.
    // - CDSAccess::get_archived_object() works.
    ResourceMark rm;
    _cached_code_directory = (CachedCodeDirectory*)ptr;
    InstanceKlass* k = _cached_code_directory->_some_klass;
    log_info(scc)("new workflow: cached code mapped at %p", ptr);
    log_info(scc)("_cached_code_directory->_some_klass     = %p (%s)", k, k->external_name());
    log_info(scc)("_cached_code_directory->_some_number    = %d", _cached_code_directory->_some_number);
    log_info(scc)("_cached_code_directory->_my_data_length = %zu", _cached_code_directory->_my_data_length);
    log_info(scc)("_cached_code_directory->_my_data        = %p (%zu bytes offset from base)", _cached_code_directory->_my_data,
                  pointer_delta((address)_cached_code_directory->_my_data, (address)_cached_code_directory, 1));
  }
}
#endif // INCLUDE_CDS_JAVA_HEAP

#define DATA_ALIGNMENT HeapWordSize

SCCache::SCCache(const char* cache_path, int fd, uint load_size) {
  _load_header = nullptr;
  _cache_path = cache_path;
  _for_read  = LoadCachedCode;
  _for_write = StoreCachedCode;
  _load_size = load_size;
  _store_size = 0;
  _write_position = 0;
  _closing  = false;
  _failed = false;
  _lookup_failed = false;
  _table = nullptr;
  _load_entries = nullptr;
  _store_entries  = nullptr;
  _C_strings_buf  = nullptr;
  _load_buffer = nullptr;
  _store_buffer = nullptr;
  _C_load_buffer = nullptr;
  _C_store_buffer = nullptr;
  _store_entries_cnt = 0;
  _gen_preload_code = false;
  _for_preload = false;       // changed while storing entry data
  _has_clinit_barriers = false;

  _compile_id = 0;
  _comp_level = 0;

  _use_meta_ptrs = UseSharedSpaces ? UseMetadataPointers : false;

  // Read header at the begining of cache
  uint header_size = sizeof(SCCHeader);
  if (_for_read) {
    // Read cache
    _C_load_buffer = NEW_C_HEAP_ARRAY(char, load_size + DATA_ALIGNMENT, mtCode);
    _load_buffer = align_up(_C_load_buffer, DATA_ALIGNMENT);
    uint n = (uint)::read(fd, _load_buffer, load_size);
    if (n != load_size) {
      log_warning(scc, init)("Failed to read %d bytes at address " INTPTR_FORMAT " from Startup Code Cache file '%s'", load_size, p2i(_load_buffer), _cache_path);
      set_failed();
      return;
    }
    log_info(scc, init)("Read %d bytes at address " INTPTR_FORMAT " from Startup Code Cache '%s'", load_size, p2i(_load_buffer), _cache_path);

    _load_header = (SCCHeader*)addr(0);
    const char* scc_jvm_version = addr(_load_header->jvm_version_offset());
    if (strncmp(scc_jvm_version, VM_Version::internal_vm_info_string(), strlen(scc_jvm_version)) != 0) {
      log_warning(scc, init)("Disable Startup Code Cache: JVM version '%s' recorded in '%s' does not match current version '%s'", scc_jvm_version, _cache_path, VM_Version::internal_vm_info_string());
      set_failed();
      return;
    }
    if (!_load_header->verify_config(_cache_path, load_size)) {
      set_failed();
      return;
    }
    log_info(scc, init)("Read header from Startup Code Cache '%s'", cache_path);
    if (_load_header->has_meta_ptrs()) {
      assert(UseSharedSpaces, "should be verified already");
      _use_meta_ptrs = true; // Regardless UseMetadataPointers
      UseMetadataPointers = true;
    }
    // Read strings
    load_strings();
  }
  if (_for_write) {
    _gen_preload_code = _use_meta_ptrs && (ClassInitBarrierMode > 0);

    _C_store_buffer = NEW_C_HEAP_ARRAY(char, CachedCodeMaxSize + DATA_ALIGNMENT, mtCode);
    _store_buffer = align_up(_C_store_buffer, DATA_ALIGNMENT);
    // Entries allocated at the end of buffer in reverse (as on stack).
    _store_entries = (SCCEntry*)align_up(_C_store_buffer + CachedCodeMaxSize, DATA_ALIGNMENT);
    log_info(scc, init)("Allocated store buffer at address " INTPTR_FORMAT " of size %d", p2i(_store_buffer), CachedCodeMaxSize);
  }
  _table = new SCAddressTable();
}

void SCCache::init_table() {
  SCCache* cache = SCCache::cache();
  if (cache != nullptr && cache->_table != nullptr) {
    cache->_table->init();
  }
}

void SCCache::init_opto_table() {
  SCCache* cache = SCCache::cache();
  if (cache != nullptr && cache->_table != nullptr) {
    cache->_table->init_opto();
  }
}

void SCCache::init_c1_table() {
  SCCache* cache = SCCache::cache();
  if (cache != nullptr && cache->_table != nullptr) {
    cache->_table->init_c1();
  }
}

void SCConfig::record(bool use_meta_ptrs) {
  _flags = 0;
  if (use_meta_ptrs) {
    _flags |= metadataPointers;
  }
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
  _compressedOopShift    = CompressedOops::shift();
  _compressedKlassShift  = CompressedKlassPointers::shift();
  _contendedPaddingWidth = ContendedPaddingWidth;
  _objectAlignment       = ObjectAlignmentInBytes;
  _gc                    = (uint)Universe::heap()->kind();
}

bool SCConfig::verify(const char* cache_path) const {
#ifdef ASSERT
  if ((_flags & debugVM) == 0) {
    log_warning(scc, init)("Disable Startup Code Cache: '%s' was created by product VM, it can't be used by debug VM", cache_path);
    return false;
  }
#else
  if ((_flags & debugVM) != 0) {
    log_warning(scc, init)("Disable Startup Code Cache: '%s' was created by debug VM, it can't be used by product VM", cache_path);
    return false;
  }
#endif

  CollectedHeap::Name scc_gc = (CollectedHeap::Name)_gc;
  if (scc_gc != Universe::heap()->kind()) {
    log_warning(scc, init)("Disable Startup Code Cache: '%s' was created with different GC: %s vs current %s", cache_path, GCConfig::hs_err_name(scc_gc), GCConfig::hs_err_name());
    return false;
  }

  if (((_flags & compressedOops) != 0) != UseCompressedOops) {
    log_warning(scc, init)("Disable Startup Code Cache: '%s' was created with UseCompressedOops = %s", cache_path, UseCompressedOops ? "false" : "true");
    return false;
  }
  if (((_flags & compressedClassPointers) != 0) != UseCompressedClassPointers) {
    log_warning(scc, init)("Disable Startup Code Cache: '%s' was created with UseCompressedClassPointers = %s", cache_path, UseCompressedClassPointers ? "false" : "true");
    return false;
  }

  if (((_flags & systemClassAssertions) != 0) != JavaAssertions::systemClassDefault()) {
    log_warning(scc, init)("Disable Startup Code Cache: '%s' was created with JavaAssertions::systemClassDefault() = %s", cache_path, JavaAssertions::systemClassDefault() ? "disabled" : "enabled");
    return false;
  }
  if (((_flags & userClassAssertions) != 0) != JavaAssertions::userClassDefault()) {
    log_warning(scc, init)("Disable Startup Code Cache: '%s' was created with JavaAssertions::userClassDefault() = %s", cache_path, JavaAssertions::userClassDefault() ? "disabled" : "enabled");
    return false;
  }

  if (((_flags & enableContendedPadding) != 0) != EnableContended) {
    log_warning(scc, init)("Disable Startup Code Cache: '%s' was created with EnableContended = %s", cache_path, EnableContended ? "false" : "true");
    return false;
  }
  if (((_flags & restrictContendedPadding) != 0) != RestrictContended) {
    log_warning(scc, init)("Disable Startup Code Cache: '%s' was created with RestrictContended = %s", cache_path, RestrictContended ? "false" : "true");
    return false;
  }
  if (_compressedOopShift != (uint)CompressedOops::shift()) {
    log_warning(scc, init)("Disable Startup Code Cache: '%s' was created with CompressedOops::shift() = %d vs current %d", cache_path, _compressedOopShift, CompressedOops::shift());
    return false;
  }
  if (_compressedKlassShift != (uint)CompressedKlassPointers::shift()) {
    log_warning(scc, init)("Disable Startup Code Cache: '%s' was created with CompressedKlassPointers::shift() = %d vs current %d", cache_path, _compressedKlassShift, CompressedKlassPointers::shift());
    return false;
  }
  if (_contendedPaddingWidth != (uint)ContendedPaddingWidth) {
    log_warning(scc, init)("Disable Startup Code Cache: '%s' was created with ContendedPaddingWidth = %d vs current %d", cache_path, _contendedPaddingWidth, ContendedPaddingWidth);
    return false;
  }
  if (_objectAlignment != (uint)ObjectAlignmentInBytes) {
    log_warning(scc, init)("Disable Startup Code Cache: '%s' was created with ObjectAlignmentInBytes = %d vs current %d", cache_path, _objectAlignment, ObjectAlignmentInBytes);
    return false;
  }
  return true;
}

bool SCCHeader::verify_config(const char* cache_path, uint load_size) const {
  if (_version != SCC_VERSION) {
    log_warning(scc, init)("Disable Startup Code Cache: different SCC version %d vs %d recorded in '%s'", SCC_VERSION, _version, cache_path);
    return false;
  }
  if (_cache_size != load_size) {
    log_warning(scc, init)("Disable Startup Code Cache: different cached code size %d vs %d recorded in '%s'", load_size, _cache_size, cache_path);
    return false;
  }
  if (has_meta_ptrs() && !UseSharedSpaces) {
    log_warning(scc, init)("Disable Startup Cached Code: '%s' contains metadata pointers but CDS is off", cache_path);
    return false;
  }
  return true;
}

volatile int SCCache::_nmethod_readers = 0;

SCCache::~SCCache() {
  if (_closing) {
    return; // Already closed
  }
  // Stop any further access to cache.
  // Checked on entry to load_nmethod() and store_nmethod().
  _closing = true;
  if (_for_read) {
    // Wait for all load_nmethod() finish.
    wait_for_no_nmethod_readers();
  }
  // Prevent writing code into cache while we are closing it.
  // This lock held by ciEnv::register_method() which calls store_nmethod().
  MutexLocker ml(Compile_lock);
  if (for_write()) { // Finalize cache
    finish_write();
  }
  FREE_C_HEAP_ARRAY(char, _cache_path);
  if (_C_load_buffer != nullptr) {
    FREE_C_HEAP_ARRAY(char, _C_load_buffer);
    _C_load_buffer = nullptr;
    _load_buffer = nullptr;
  }
  if (_C_store_buffer != nullptr) {
    FREE_C_HEAP_ARRAY(char, _C_store_buffer);
    _C_store_buffer = nullptr;
    _store_buffer = nullptr;
  }
  if (_table != nullptr) {
    delete _table;
    _table = nullptr;
  }
}

SCCache* SCCache::open_for_read() {
  if (SCCache::is_on_for_read()) {
    return SCCache::cache();
  }
  return nullptr;
}

SCCache* SCCache::open_for_write() {
  if (SCCache::is_on_for_write()) {
    SCCache* cache = SCCache::cache();
    cache->clear_lookup_failed(); // Reset bit
    return cache;
  }
  return nullptr;
}

void copy_bytes(const char* from, address to, uint size) {
  assert(size > 0, "sanity");
  bool by_words = true;
  if ((size > 2 * HeapWordSize) && (((intptr_t)from | (intptr_t)to) & (HeapWordSize - 1)) == 0) {
    // Use wordwise copies if possible:
    Copy::disjoint_words((HeapWord*)from,
                         (HeapWord*)to,
                         ((size_t)size + HeapWordSize-1) / HeapWordSize);
  } else {
    by_words = false;
    Copy::conjoint_jbytes(from, to, (size_t)size);
  }
  log_trace(scc)("Copied %d bytes as %s from " INTPTR_FORMAT " to " INTPTR_FORMAT, size, (by_words ? "HeapWord" : "bytes"), p2i(from), p2i(to));
}

void SCCReader::set_read_position(uint pos) {
  if (pos == _read_position) {
    return;
  }
  assert(pos < _cache->load_size(), "offset:%d >= file size:%d", pos, _cache->load_size());
  _read_position = pos;
}

bool SCCache::set_write_position(uint pos) {
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

bool SCCache::align_write() {
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
  log_trace(scc)("Adjust write alignment in Startup Code Cache '%s'", _cache_path);
  return true;
}

uint SCCache::write_bytes(const void* buffer, uint nbytes) {
  assert(for_write(), "Code Cache file is not created");
  if (nbytes == 0) {
    return 0;
  }
  uint new_position = _write_position + nbytes;
  if (new_position >= (uint)((char*)_store_entries - _store_buffer)) {
    log_warning(scc)("Failed to write %d bytes at offset %d to Startup Code Cache file '%s'. Increase CachedCodeMaxSize.",
                     nbytes, _write_position, _cache_path);
    set_failed();
    exit_vm_on_store_failure();
    return 0;
  }
  copy_bytes((const char* )buffer, (address)(_store_buffer + _write_position), nbytes);
  log_trace(scc)("Wrote %d bytes at offset %d to Startup Code Cache '%s'", nbytes, _write_position, _cache_path);
  _write_position += nbytes;
  if (_store_size < _write_position) {
    _store_size = _write_position;
  }
  return nbytes;
}

void SCCEntry::update_method_for_writing() {
  if (_method != nullptr) {
    _method = CDSAccess::method_in_cached_code(_method);
  }
}

void SCCEntry::print(outputStream* st) const {
  st->print_cr(" SCA entry " INTPTR_FORMAT " [kind: %d, id: " UINT32_FORMAT_X_0 ", offset: %d, size: %d, comp_level: %d, comp_id: %d, decompiled: %d, %s%s%s%s%s]",
               p2i(this), (int)_kind, _id, _offset, _size, _comp_level, _comp_id, _decompile,
               (_not_entrant? "not_entrant" : "entrant"),
               (_loaded ? ", loaded" : ""),
               (_has_clinit_barriers ? ", has_clinit_barriers" : ""),
               (_for_preload ? ", for_preload" : ""),
               (_ignore_decompile ? ", ignore_decomp" : ""));
}

void* SCCEntry::operator new(size_t x, SCCache* cache) {
  return (void*)(cache->add_entry());
}

bool skip_preload(methodHandle mh) {
  if (!mh->method_holder()->is_loaded()) {
    return true;
  }
  DirectiveSet* directives = DirectivesStack::getMatchingDirective(mh, nullptr);
  if (directives->DontPreloadOption) {
    LogStreamHandle(Info, scc, init) log;
    if (log.is_enabled()) {
      log.print("Exclude preloading code for ");
      mh->print_value_on(&log);
    }
    return true;
  }
  return false;
}

void SCCache::preload_startup_code(TRAPS) {
  if (CompilationPolicy::compiler_count(CompLevel_full_optimization) == 0) {
    // Since we reuse the CompilerBroker API to install cached code, we're required to have a JIT compiler for the
    // level we want (that is CompLevel_full_optimization).
    return;
  }
  assert(_for_read, "sanity");
  uint count = _load_header->entries_count();
  if (_load_entries == nullptr) {
    // Read it
    _search_entries = (uint*)addr(_load_header->entries_offset()); // [id, index]
    _load_entries = (SCCEntry*)(_search_entries + 2 * count);
    log_info(scc, init)("Read %d entries table at offset %d from Startup Code Cache '%s'", count, _load_header->entries_offset(), _cache_path);
  }
  uint preload_entries_count = _load_header->preload_entries_count();
  if (preload_entries_count > 0) {
    uint* entries_index = (uint*)addr(_load_header->preload_entries_offset());
    log_info(scc, init)("Load %d preload entries from Startup Code Cache '%s'", preload_entries_count, _cache_path);
    uint count = MIN2(preload_entries_count, SCLoadStop);
    for (uint i = SCLoadStart; i < count; i++) {
      uint index = entries_index[i];
      SCCEntry* entry = &(_load_entries[index]);
      if (entry->not_entrant()) {
        continue;
      }
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
          LogStreamHandle(Info, scc) log;
          if (log.is_enabled()) {
            ResourceMark rm;
            log.print("Linkage failed for %s: ", mh->method_holder()->external_name());
            THREAD->pending_exception()->print_value_on(&log);
            if (log_is_enabled(Debug, scc)) {
              THREAD->pending_exception()->print_on(&log);
            }
          }
          CLEAR_PENDING_EXCEPTION;
        }
      }
      if (mh->scc_entry() != nullptr) {
        // Second C2 compilation of the same method could happen for
        // different reasons without marking first entry as not entrant.
        continue; // Keep old entry to avoid issues
      }
      mh->set_scc_entry(entry);
      CompileBroker::compile_method(mh, InvocationEntryBci, CompLevel_full_optimization, methodHandle(), 0, false, CompileTask::Reason_Preload, CHECK);
    }
  }
}

static bool check_entry(SCCEntry::Kind kind, uint id, uint comp_level, uint decomp, SCCEntry* entry) {
  if (entry->kind() == kind) {
    assert(entry->id() == id, "sanity");
    if (kind != SCCEntry::Code || (!entry->not_entrant() && !entry->has_clinit_barriers() &&
                                  (entry->comp_level() == comp_level) &&
                                  (entry->ignore_decompile() || entry->decompile() == decomp))) {
      return true; // Found
    }
  }
  return false;
}

SCCEntry* SCCache::find_entry(SCCEntry::Kind kind, uint id, uint comp_level, uint decomp) {
  assert(_for_read, "sanity");
  uint count = _load_header->entries_count();
  if (_load_entries == nullptr) {
    // Read it
    _search_entries = (uint*)addr(_load_header->entries_offset()); // [id, index]
    _load_entries = (SCCEntry*)(_search_entries + 2 * count);
    log_info(scc, init)("Read %d entries table at offset %d from Startup Code Cache '%s'", count, _load_header->entries_offset(), _cache_path);
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
      SCCEntry* entry = &(_load_entries[index]);
      if (check_entry(kind, id, comp_level, decomp, entry)) {
        return entry; // Found
      }
      // Leaner search around (could be the same nmethod with different decompile count)
      for (int i = mid - 1; i >= l; i--) { // search back
        ix = i * 2;
        is = _search_entries[ix];
        if (is != id) {
          break;
        }
        index = _search_entries[ix + 1];
        SCCEntry* entry = &(_load_entries[index]);
        if (check_entry(kind, id, comp_level, decomp, entry)) {
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
        SCCEntry* entry = &(_load_entries[index]);
        if (check_entry(kind, id, comp_level, decomp, entry)) {
          return entry; // Found
        }
      }
      break; // Not found match (different decompile count or not_entrant state).
    } else if (is < id) {
      l = mid + 1;
    } else {
      h = mid - 1;
    }
  }
  return nullptr;
}

void SCCache::invalidate_entry(SCCEntry* entry) {
  assert(entry!= nullptr, "all entries should be read already");
  if (entry->not_entrant()) {
    return; // Someone invalidated it already
  }
#ifdef ASSERT
  bool found = false;
  if (_for_read) {
    uint count = _load_header->entries_count();
    uint i = 0;
    for(; i < count; i++) {
      if (entry == &(_load_entries[i])) {
        break;
      }
    }
    found = (i < count);
  }
  if (!found && _for_write) {
    uint count = _store_entries_cnt;
    uint i = 0;
    for(; i < count; i++) {
      if (entry == &(_store_entries[i])) {
        break;
      }
    }
    found = (i < count);
  }
  assert(found, "entry should exist");
#endif
  entry->set_not_entrant();
  {
    uint name_offset = entry->offset() + entry->name_offset();
    const char* name;
    if (SCCache::is_loaded(entry)) {
      name = _load_buffer + name_offset;
    } else {
      name = _store_buffer + name_offset;
    }
    uint level   = entry->comp_level();
    uint comp_id = entry->comp_id();
    uint decomp  = entry->decompile();
    bool clinit_brs = entry->has_clinit_barriers();
    log_info(scc, nmethod)("Invalidated entry for '%s' (comp_id %d, comp_level %d, decomp: %d, hash: " UINT32_FORMAT_X_0 "%s)",
                           name, comp_id, level, decomp, entry->id(), (clinit_brs ? ", has clinit barriers" : ""));
  }
  if (entry->next() != nullptr) {
    entry = entry->next();
    assert(entry->has_clinit_barriers(), "expecting only such entries here");
    invalidate_entry(entry);
  }
}

extern "C" {
  static int uint_cmp(const void *i, const void *j) {
    uint a = *(uint *)i;
    uint b = *(uint *)j;
    return a > b ? 1 : a < b ? -1 : 0;
  }
}

bool SCCache::finish_write() {
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

  uint store_count = _store_entries_cnt;
  if (store_count > 0) {
    uint header_size = (uint)align_up(sizeof(SCCHeader),  DATA_ALIGNMENT);
    const char* vm_version = VM_Version::internal_vm_info_string();
    uint vm_version_size = (uint)align_up(strlen(vm_version) + 1, DATA_ALIGNMENT);
    uint load_count = (_load_header != nullptr) ? _load_header->entries_count() : 0;
    uint code_count = store_count + load_count;
    uint search_count = code_count * 2;
    uint search_size = search_count * sizeof(uint);
    uint entries_size = (uint)align_up(code_count * sizeof(SCCEntry), DATA_ALIGNMENT); // In bytes
    uint preload_entries_cnt = 0;
    uint* preload_entries = NEW_C_HEAP_ARRAY(uint, code_count, mtCode);
    uint preload_entries_size = code_count * sizeof(uint);
    // _write_position should include code and strings
    uint code_alignment = code_count * DATA_ALIGNMENT; // We align_up code size when storing it.
    uint total_size = _write_position + _load_size + header_size + vm_version_size +
                     code_alignment + search_size + preload_entries_size + entries_size;

    // Create ordered search table for entries [id, index];
    uint* search = NEW_C_HEAP_ARRAY(uint, search_count, mtCode);
    char* buffer = NEW_C_HEAP_ARRAY(char, total_size + DATA_ALIGNMENT, mtCode);
    char* start = align_up(buffer, DATA_ALIGNMENT);
    char* current = start + header_size; // Skip header
    uint jvm_version_offset = current - start;
    copy_bytes(vm_version, (address)current, (uint)strlen(vm_version) + 1);
    current += vm_version_size;

    SCCEntry* entries_address = _store_entries; // Pointer to latest entry
    uint not_entrant_nb = 0;
    uint max_size = 0;
    // Add old entries first
    if (_for_read && (_load_header != nullptr)) {
      for(uint i = 0; i < load_count; i++) {
        if (_load_entries[i].load_fail()) {
          continue;
        }
        if (_load_entries[i].not_entrant()) {
          log_info(scc, exit)("Not entrant load entry id: %d, decomp: %d, hash: " UINT32_FORMAT_X_0, i, _load_entries[i].decompile(), _load_entries[i].id());
          not_entrant_nb++;
          if (_load_entries[i].for_preload()) {
            // Skip not entrant preload code:
            // we can't pre-load code which may have failing dependencies.
            continue;
          }
          _load_entries[i].set_entrant(); // Reset
        } else if (_load_entries[i].for_preload() && _load_entries[i].method() != nullptr) {
          // record entrant first version code for pre-loading
          preload_entries[preload_entries_cnt++] = entries_count;
        }
        {
          uint size = align_up(_load_entries[i].size(), DATA_ALIGNMENT);
          if (size > max_size) {
            max_size = size;
          }
          copy_bytes((_load_buffer + _load_entries[i].offset()), (address)current, size);
          _load_entries[i].set_offset(current - start); // New offset
          current += size;
          uint n = write_bytes(&(_load_entries[i]), sizeof(SCCEntry));
          if (n != sizeof(SCCEntry)) {
            FREE_C_HEAP_ARRAY(char, buffer);
            FREE_C_HEAP_ARRAY(uint, search);
            return false;
          }
          search[entries_count*2 + 0] = _load_entries[i].id();
          search[entries_count*2 + 1] = entries_count;
          entries_count++;
        }
      }
    }
    // SCCEntry entries were allocated in reverse in store buffer.
    // Process them in reverse order to cache first code first.
    for (int i = store_count - 1; i >= 0; i--) {
      if (entries_address[i].load_fail()) {
        continue;
      }
      if (entries_address[i].not_entrant()) {
        log_info(scc, exit)("Not entrant new entry comp_id: %d, comp_level: %d, decomp: %d, hash: " UINT32_FORMAT_X_0 "%s", entries_address[i].comp_id(), entries_address[i].comp_level(), entries_address[i].decompile(), entries_address[i].id(), (entries_address[i].has_clinit_barriers() ? ", has clinit barriers" : ""));
        not_entrant_nb++;
        if (entries_address[i].for_preload()) {
          // Skip not entrant preload code:
          // we can't pre-load code which may have failing dependencies.
          continue;
        }
        entries_address[i].set_entrant(); // Reset
      } else if (entries_address[i].for_preload() && entries_address[i].method() != nullptr) {
        // record entrant first version code for pre-loading
        preload_entries[preload_entries_cnt++] = entries_count;
      }
      {
        entries_address[i].set_next(nullptr); // clear pointers before storing data
        uint size = align_up(entries_address[i].size(), DATA_ALIGNMENT);
        if (size > max_size) {
          max_size = size;
        }
        copy_bytes((_store_buffer + entries_address[i].offset()), (address)current, size);
        entries_address[i].set_offset(current - start); // New offset
        entries_address[i].update_method_for_writing();
        current += size;
        uint n = write_bytes(&(entries_address[i]), sizeof(SCCEntry));
        if (n != sizeof(SCCEntry)) {
          FREE_C_HEAP_ARRAY(char, buffer);
          FREE_C_HEAP_ARRAY(uint, search);
          return false;
        }
        search[entries_count*2 + 0] = entries_address[i].id();
        search[entries_count*2 + 1] = entries_count;
        entries_count++;
      }
    }
    if (entries_count == 0) {
      log_info(scc, exit)("No new entires, cache files %s was not %s", _cache_path, (_for_read ? "updated" : "created"));
      FREE_C_HEAP_ARRAY(char, buffer);
      FREE_C_HEAP_ARRAY(uint, search);
      return true; // Nothing to write
    }
    assert(entries_count <= (store_count + load_count), "%d > (%d + %d)", entries_count, store_count, load_count);
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
      log_info(scc, exit)("Wrote %d preload entries to Startup Code Cache '%s'", preload_entries_cnt, _cache_path);
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
    entries_size = entries_count * sizeof(SCCEntry); // New size
    copy_bytes((_store_buffer + entries_offset), (address)current, entries_size);
    current += entries_size;
    log_info(scc, exit)("Wrote %d SCCEntry entries (%d were not entrant, %d max size) to Startup Code Cache '%s'", entries_count, not_entrant_nb, max_size, _cache_path);

    uint size = (current - start);
    assert(size <= total_size, "%d > %d", size , total_size);

    // Finalize header
    SCCHeader* header = (SCCHeader*)start;
    header->init(jvm_version_offset, size,
                 (uint)strings_count, strings_offset,
                 entries_count, new_entries_offset,
                 preload_entries_cnt, preload_entries_offset,
                 _use_meta_ptrs);
    log_info(scc, init)("Wrote header to Startup Code Cache '%s'", _cache_path);

    // Now store to file
#ifdef _WINDOWS  // On Windows, need WRITE permission to remove the file.
    chmod(_cache_path, _S_IREAD | _S_IWRITE);
#endif
    // Use remove() to delete the existing file because, on Unix, this will
    // allow processes that have it open continued access to the file.
    remove(_cache_path);
    int fd = os::open(_cache_path, O_RDWR | O_CREAT | O_TRUNC | O_BINARY, 0444);
    if (fd < 0) {
      log_warning(scc, exit)("Unable to create Startup Code Cache file '%s': (%s)", _cache_path, os::strerror(errno));
      FREE_C_HEAP_ARRAY(char, buffer);
      exit_vm_on_store_failure();
      return false;
    } else {
      log_info(scc, exit)("Opened for write Startup Code Cache '%s'", _cache_path);
    }
    bool success = os::write(fd, start, (size_t)size);
    if (!success) {
      log_warning(scc, exit)("Failed to write %d bytes to Startup Code Cache file '%s': (%s)", size, _cache_path, os::strerror(errno));
      FREE_C_HEAP_ARRAY(char, buffer);
      exit_vm_on_store_failure();
      return false;
    }
    log_info(scc, exit)("Wrote %d bytes to Startup Code Cache '%s'", size, _cache_path);
    if (::close(fd) < 0) {
      log_warning(scc, exit)("Failed to close for write Startup Code Cache file '%s'", _cache_path);
      exit_vm_on_store_failure();
    } else {
      log_info(scc, exit)("Closed for write Startup Code Cache '%s'", _cache_path);
    }
    FREE_C_HEAP_ARRAY(char, buffer);
  }
  return true;
}

bool SCCache::load_stub(StubCodeGenerator* cgen, vmIntrinsicID id, const char* name, address start) {
  assert(start == cgen->assembler()->pc(), "wrong buffer");
  SCCache* cache = open_for_read();
  if (cache == nullptr) {
    return false;
  }
  SCCEntry* entry = cache->find_entry(SCCEntry::Stub, (uint)id);
  if (entry == nullptr) {
    return false;
  }
  uint entry_position = entry->offset();
  // Read name
  uint name_offset = entry->name_offset() + entry_position;
  uint name_size   = entry->name_size(); // Includes '/0'
  const char* saved_name = cache->addr(name_offset);
  if (strncmp(name, saved_name, (name_size - 1)) != 0) {
    log_warning(scc)("Saved stub's name '%s' is different from '%s' for id:%d", saved_name, name, (int)id);
    cache->set_failed();
    exit_vm_on_load_failure();
    return false;
  }
  log_info(scc,stubs)("Reading stub '%s' id:%d from Startup Code Cache '%s'", name, (int)id, cache->_cache_path);
  // Read code
  uint code_offset = entry->code_offset() + entry_position;
  uint code_size   = entry->code_size();
  copy_bytes(cache->addr(code_offset), start, code_size);
  cgen->assembler()->code_section()->set_end(start + code_size);
  log_info(scc,stubs)("Read stub '%s' id:%d from Startup Code Cache '%s'", name, (int)id, cache->_cache_path);
  return true;
}

bool SCCache::store_stub(StubCodeGenerator* cgen, vmIntrinsicID id, const char* name, address start) {
  SCCache* cache = open_for_write();
  if (cache == nullptr) {
    return false;
  }
  log_info(scc, stubs)("Writing stub '%s' id:%d to Startup Code Cache '%s'", name, (int)id, cache->_cache_path);
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
  SCCEntry* entry = new(cache) SCCEntry(entry_position, entry_size, name_offset, name_size,
                                          code_offset, code_size, 0, 0,
                                          SCCEntry::Stub, (uint32_t)id);
  log_info(scc, stubs)("Wrote stub '%s' id:%d to Startup Code Cache '%s'", name, (int)id, cache->_cache_path);
  return true;
}

Klass* SCCReader::read_klass(const methodHandle& comp_method, bool shared) {
  uint code_offset = read_position();
  uint state = *(uint*)addr(code_offset);
  uint init_state = (state  & 1);
  uint array_dim  = (state >> 1);
  code_offset += sizeof(int);
  if (_cache->use_meta_ptrs() && shared) {
    uint klass_offset = *(uint*)addr(code_offset);
    code_offset += sizeof(uint);
    set_read_position(code_offset);
    Klass* k = (Klass*)((address)SharedBaseAddress + klass_offset);
    if (!MetaspaceShared::is_in_shared_metaspace((address)k)) {
      // Something changed in CDS
      set_lookup_failed();
      log_info(scc)("Lookup failed for shared klass: " INTPTR_FORMAT " is not in CDS ", p2i((address)k));
      return nullptr;
    }
    assert(k->is_klass(), "sanity");
    ResourceMark rm;
    const char* comp_name = comp_method->name_and_sig_as_C_string();
    if (k->is_instance_klass() && !InstanceKlass::cast(k)->is_loaded()) {
      set_lookup_failed();
      log_info(scc)("%d '%s' (L%d): Lookup failed for klass %s: not loaded",
                       compile_id(), comp_name, comp_level(), k->external_name());
      return nullptr;
    } else
    // Allow not initialized klass which was uninitialized during code caching or for preload
    if (k->is_instance_klass() && !InstanceKlass::cast(k)->is_initialized() && (init_state == 1) && !_preload) {
      set_lookup_failed();
      log_info(scc)("%d '%s' (L%d): Lookup failed for klass %s: not initialized",
                       compile_id(), comp_name, comp_level(), k->external_name());
      return nullptr;
    }
    if (array_dim > 0) {
      assert(k->is_instance_klass() || k->is_typeArray_klass(), "sanity check");
      Klass* ak = k->array_klass_or_null(array_dim);
      // FIXME: what would it take to create an array class on the fly?
//      Klass* ak = k->array_klass(dim, JavaThread::current());
//      guarantee(JavaThread::current()->pending_exception() == nullptr, "");
      if (ak == nullptr) {
        set_lookup_failed();
        log_info(scc)("%d (L%d): %d-dimension array klass lookup failed: %s",
                         compile_id(), comp_level(), array_dim, k->external_name());
      }
      log_info(scc)("%d (L%d): Klass lookup: %s (object array)", compile_id(), comp_level(), k->external_name());
      return ak;
    } else {
      log_info(scc)("%d (L%d): Shared klass lookup: %s",
                    compile_id(), comp_level(), k->external_name());
      return k;
    }
  }
  int name_length = *(int*)addr(code_offset);
  code_offset += sizeof(int);
  const char* dest = addr(code_offset);
  code_offset += name_length + 1;
  set_read_position(code_offset);
  TempNewSymbol klass_sym = SymbolTable::probe(&(dest[0]), name_length);
  if (klass_sym == nullptr) {
    set_lookup_failed();
    log_info(scc)("%d (L%d): Probe failed for class %s",
                     compile_id(), comp_level(), &(dest[0]));
    return nullptr;
  }
  // Use class loader of compiled method.
  Thread* thread = Thread::current();
  Handle loader(thread, comp_method->method_holder()->class_loader());
  Handle protection_domain(thread, comp_method->method_holder()->protection_domain());
  Klass* k = SystemDictionary::find_instance_or_array_klass(thread, klass_sym, loader, protection_domain);
  assert(!thread->has_pending_exception(), "should not throw");
  if (k == nullptr && !loader.is_null()) {
    // Try default loader and domain
    k = SystemDictionary::find_instance_or_array_klass(thread, klass_sym, Handle(), Handle());
    assert(!thread->has_pending_exception(), "should not throw");
  }
  if (k != nullptr) {
    // Allow not initialized klass which was uninitialized during code caching
    if (k->is_instance_klass() && !InstanceKlass::cast(k)->is_initialized() && (init_state == 1)) {
      set_lookup_failed();
      log_info(scc)("%d (L%d): Lookup failed for klass %s: not initialized", compile_id(), comp_level(), &(dest[0]));
      return nullptr;
    }
    log_info(scc)("%d (L%d): Klass lookup %s", compile_id(), comp_level(), k->external_name());
  } else {
    set_lookup_failed();
    log_info(scc)("%d (L%d): Lookup failed for class %s", compile_id(), comp_level(), &(dest[0]));
    return nullptr;
  }
  return k;
}

Method* SCCReader::read_method(const methodHandle& comp_method, bool shared) {
  uint code_offset = read_position();
  if (_cache->use_meta_ptrs() && shared) {
    uint method_offset = *(uint*)addr(code_offset);
    code_offset += sizeof(uint);
    set_read_position(code_offset);
    Method* m = (Method*)((address)SharedBaseAddress + method_offset);
    if (!MetaspaceShared::is_in_shared_metaspace((address)m)) {
      // Something changed in CDS
      set_lookup_failed();
      log_info(scc)("Lookup failed for shared method: " INTPTR_FORMAT " is not in CDS ", p2i((address)m));
      return nullptr;
    }
    assert(m->is_method(), "sanity");
    ResourceMark rm;
    const char* comp_name = comp_method->name_and_sig_as_C_string();
    Klass* k = m->method_holder();
    if (!k->is_instance_klass()) {
      set_lookup_failed();
      log_info(scc)("%d '%s' (L%d): Lookup failed for holder %s: not instance klass", compile_id(), comp_name, comp_level(), k->external_name());
      return nullptr;
    } else if (!MetaspaceShared::is_in_shared_metaspace((address)k)) {
      set_lookup_failed();
      log_info(scc)("%d '%s' (L%d): Lookup failed for holder %s: not in CDS", compile_id(), comp_name, comp_level(), k->external_name());
      return nullptr;
    } else if (!InstanceKlass::cast(k)->is_loaded()) {
      set_lookup_failed();
      log_info(scc)("%d '%s' (L%d): Lookup failed for holder %s: not loaded", compile_id(), comp_name, comp_level(), k->external_name());
      return nullptr;
    } else if (!InstanceKlass::cast(k)->is_linked()) {
      set_lookup_failed();
      log_info(scc)("%d '%s' (L%d): Lookup failed for holder %s: not linked%s", compile_id(), comp_name, comp_level(), k->external_name(), (_preload ? " for code preload" : ""));
      return nullptr;
    }
    log_info(scc)("%d (L%d): Shared method lookup: %s", compile_id(), comp_level(), m->name_and_sig_as_C_string());
    return m;
  }
  int holder_length = *(int*)addr(code_offset);
  code_offset += sizeof(int);
  int name_length = *(int*)addr(code_offset);
  code_offset += sizeof(int);
  int signat_length = *(int*)addr(code_offset);
  code_offset += sizeof(int);

  const char* dest = addr(code_offset);
  code_offset += holder_length + 1 + name_length + 1 + signat_length + 1;
  set_read_position(code_offset);
  TempNewSymbol klass_sym = SymbolTable::probe(&(dest[0]), holder_length);
  if (klass_sym == nullptr) {
    set_lookup_failed();
    log_info(scc)("%d (L%d): Probe failed for class %s", compile_id(), comp_level(), &(dest[0]));
    return nullptr;
  }
  // Use class loader of compiled method.
  Thread* thread = Thread::current();
  Handle loader(thread, comp_method->method_holder()->class_loader());
  Handle protection_domain(thread, comp_method->method_holder()->protection_domain());
  Klass* k = SystemDictionary::find_instance_or_array_klass(thread, klass_sym, loader, protection_domain);
  assert(!thread->has_pending_exception(), "should not throw");
  if (k == nullptr && !loader.is_null()) {
    // Try default loader and domain
    k = SystemDictionary::find_instance_or_array_klass(thread, klass_sym, Handle(), Handle());
    assert(!thread->has_pending_exception(), "should not throw");
  }
  if (k != nullptr) {
    if (!k->is_instance_klass()) {
      set_lookup_failed();
      log_info(scc)("%d (L%d): Lookup failed for holder %s: not instance klass",
                       compile_id(), comp_level(), &(dest[0]));
      return nullptr;
    } else if (!InstanceKlass::cast(k)->is_linked()) {
      set_lookup_failed();
      log_info(scc)("%d (L%d): Lookup failed for holder %s: not linked",
                       compile_id(), comp_level(), &(dest[0]));
      return nullptr;
    }
    log_info(scc)("%d (L%d): Holder lookup: %s", compile_id(), comp_level(), k->external_name());
  } else {
    set_lookup_failed();
    log_info(scc)("%d (L%d): Lookup failed for holder %s",
                  compile_id(), comp_level(), &(dest[0]));
    return nullptr;
  }
  TempNewSymbol name_sym = SymbolTable::probe(&(dest[holder_length + 1]), name_length);
  int pos = holder_length + 1 + name_length + 1;
  TempNewSymbol sign_sym = SymbolTable::probe(&(dest[pos]), signat_length);
  if (name_sym == nullptr) {
    set_lookup_failed();
    log_info(scc)("%d (L%d): Probe failed for method name %s",
                     compile_id(), comp_level(), &(dest[holder_length + 1]));
    return nullptr;
  }
  if (sign_sym == nullptr) {
    set_lookup_failed();
    log_info(scc)("%d (L%d): Probe failed for method signature %s",
                     compile_id(), comp_level(), &(dest[pos]));
    return nullptr;
  }
  Method* m = InstanceKlass::cast(k)->find_method(name_sym, sign_sym);
  if (m != nullptr) {
    ResourceMark rm;
    log_info(scc)("%d (L%d): Method lookup: %s", compile_id(), comp_level(), m->name_and_sig_as_C_string());
  } else {
    set_lookup_failed();
    log_info(scc)("%d (L%d): Lookup failed for method %s::%s%s",
                     compile_id(), comp_level(), &(dest[0]), &(dest[holder_length + 1]), &(dest[pos]));
    return nullptr;
  }
  return m;
}

bool SCCache::write_klass(Klass* klass) {
  if (klass->is_hidden()) { // Skip such nmethod
    set_lookup_failed();
    return false;
  }
  bool can_use_meta_ptrs = _use_meta_ptrs;
  uint array_dim = 0;
  if (klass->is_objArray_klass()) {
    array_dim = ObjArrayKlass::cast(klass)->dimension();
    klass     = ObjArrayKlass::cast(klass)->bottom_klass(); // overwrites klass
  }
  uint init_state = 0;
  if (klass->is_instance_klass()) {
    InstanceKlass* ik = InstanceKlass::cast(klass);
    ClassLoaderData* cld = ik->class_loader_data();
    if (!cld->is_builtin_class_loader_data()) {
      set_lookup_failed();
      return false;
    }
    if (_for_preload && !CDSAccess::can_generate_cached_code(ik)) {
      _for_preload = false;
      // Bailout if code has clinit barriers:
      // method will be recompiled without them in any case
      if (_has_clinit_barriers) {
        set_lookup_failed();
        return false;
      }
      can_use_meta_ptrs = false;
    }
    init_state = (ik->is_initialized() ? 1 : 0);
  }
  ResourceMark rm;
  uint state = (array_dim << 1) | (init_state & 1);
  if (can_use_meta_ptrs && CDSAccess::can_generate_cached_code(klass)) {
    DataKind kind = DataKind::Klass_Shared;
    uint n = write_bytes(&kind, sizeof(int));
    if (n != sizeof(int)) {
      return false;
    }
    // Record state of instance klass initialization.
    n = write_bytes(&state, sizeof(int));
    if (n != sizeof(int)) {
      return false;
    }
    uint klass_offset = CDSAccess::delta_from_shared_address_base((address)klass);
    n = write_bytes(&klass_offset, sizeof(uint));
    if (n != sizeof(uint)) {
      return false;
    }
    log_info(scc)("%d (L%d): Wrote shared klass: %s%s%s @ 0x%08x", compile_id(), comp_level(), klass->external_name(),
                  (!klass->is_instance_klass() ? "" : (init_state == 1 ? " (initialized)" : " (not-initialized)")),
                  (array_dim > 0 ? " (object array)" : ""),
                  klass_offset);
    return true;
  }
  // Bailout if code has clinit barriers:
  // method will be recompiled without them in any case
  if (_for_preload && _has_clinit_barriers) {
    set_lookup_failed();
    return false;
  }
  _for_preload = false;
  log_info(scc,cds)("%d (L%d): Not shared klass: %s", compile_id(), comp_level(), klass->external_name());
  DataKind kind = DataKind::Klass;
  uint n = write_bytes(&kind, sizeof(int));
  if (n != sizeof(int)) {
    return false;
  }
  // Record state of instance klass initialization.
  n = write_bytes(&state, sizeof(int));
  if (n != sizeof(int)) {
    return false;
  }
  Symbol* name = klass->name();
  int name_length = name->utf8_length();
  int total_length = name_length + 1;
  char* dest = NEW_RESOURCE_ARRAY(char, total_length);
  name->as_C_string(dest, total_length);
  dest[total_length - 1] = '\0';
  LogTarget(Info, scc, loader) log;
  if (log.is_enabled()) {
    LogStream ls(log);
    oop loader = klass->class_loader();
    oop domain = klass->protection_domain();
    ls.print("Class %s loader: ", dest);
    if (loader == nullptr) {
      ls.print("nullptr");
    } else {
      loader->print_value_on(&ls);
    }
    ls.print(" domain: ");
    if (domain == nullptr) {
      ls.print("nullptr");
    } else {
      domain->print_value_on(&ls);
    }
    ls.cr();
  }
  n = write_bytes(&name_length, sizeof(int));
  if (n != sizeof(int)) {
    return false;
  }
  n = write_bytes(dest, total_length);
  if (n != (uint)total_length) {
    return false;
  }
  log_info(scc)("%d (L%d): Wrote klass: %s%s%s",
                compile_id(), comp_level(),
                dest, (!klass->is_instance_klass() ? "" : (init_state == 1 ? " (initialized)" : " (not-initialized)")),
                (array_dim > 0 ? " (object array)" : ""));
  return true;
}

bool SCCache::write_method(Method* method) {
  bool can_use_meta_ptrs = _use_meta_ptrs;
  Klass* klass = method->method_holder();
  if (klass->is_instance_klass()) {
    InstanceKlass* ik = InstanceKlass::cast(klass);
    ClassLoaderData* cld = ik->class_loader_data();
    if (!cld->is_builtin_class_loader_data()) {
      set_lookup_failed();
      return false;
    }
    if (_for_preload && !CDSAccess::can_generate_cached_code(ik)) {
      _for_preload = false;
      // Bailout if code has clinit barriers:
      // method will be recompiled without them in any case
      if (_has_clinit_barriers) {
        set_lookup_failed();
        return false;
      }
      can_use_meta_ptrs = false;
    }
  }
  ResourceMark rm;
  if (can_use_meta_ptrs && CDSAccess::can_generate_cached_code(method)) {
    DataKind kind = DataKind::Method_Shared;
    uint n = write_bytes(&kind, sizeof(int));
    if (n != sizeof(int)) {
      return false;
    }
    uint method_offset = CDSAccess::delta_from_shared_address_base((address)method);
    n = write_bytes(&method_offset, sizeof(uint));
    if (n != sizeof(uint)) {
      return false;
    }
    log_info(scc)("%d (L%d): Wrote shared method: %s @ 0x%08x", compile_id(), comp_level(), method->name_and_sig_as_C_string(), method_offset);
    return true;
  }
  // Bailout if code has clinit barriers:
  // method will be recompiled without them in any case
  if (_for_preload && _has_clinit_barriers) {
    set_lookup_failed();
    return false;
  }
  _for_preload = false;
  log_info(scc,cds)("%d (L%d): Not shared method: %s", compile_id(), comp_level(), method->name_and_sig_as_C_string());
  if (method->is_hidden()) { // Skip such nmethod
    set_lookup_failed();
    return false;
  }
  DataKind kind = DataKind::Method;
  uint n = write_bytes(&kind, sizeof(int));
  if (n != sizeof(int)) {
    return false;
  }
  Symbol* name   = method->name();
  Symbol* holder = method->klass_name();
  Symbol* signat = method->signature();
  int name_length   = name->utf8_length();
  int holder_length = holder->utf8_length();
  int signat_length = signat->utf8_length();

  // Write sizes and strings
  int total_length = holder_length + 1 + name_length + 1 + signat_length + 1;
  char* dest = NEW_RESOURCE_ARRAY(char, total_length);
  holder->as_C_string(dest, total_length);
  dest[holder_length] = '\0';
  int pos = holder_length + 1;
  name->as_C_string(&(dest[pos]), (total_length - pos));
  pos += name_length;
  dest[pos++] = '\0';
  signat->as_C_string(&(dest[pos]), (total_length - pos));
  dest[total_length - 1] = '\0';

  LogTarget(Info, scc, loader) log;
  if (log.is_enabled()) {
    LogStream ls(log);
    oop loader = klass->class_loader();
    oop domain = klass->protection_domain();
    ls.print("Holder %s loader: ", dest);
    if (loader == nullptr) {
      ls.print("nullptr");
    } else {
      loader->print_value_on(&ls);
    }
    ls.print(" domain: ");
    if (domain == nullptr) {
      ls.print("nullptr");
    } else {
      domain->print_value_on(&ls);
    }
    ls.cr();
  }

  n = write_bytes(&holder_length, sizeof(int));
  if (n != sizeof(int)) {
    return false;
  }
  n = write_bytes(&name_length, sizeof(int));
  if (n != sizeof(int)) {
    return false;
  }
  n = write_bytes(&signat_length, sizeof(int));
  if (n != sizeof(int)) {
    return false;
  }
  n = write_bytes(dest, total_length);
  if (n != (uint)total_length) {
    return false;
  }
  dest[holder_length] = ' ';
  dest[holder_length + 1 + name_length] = ' ';
  log_info(scc)("%d (L%d): Wrote method: %s", compile_id(), comp_level(), dest);
  return true;
}

// Repair the pc relative information in the code after load
bool SCCReader::read_relocations(CodeBuffer* buffer, CodeBuffer* orig_buffer,
                                 OopRecorder* oop_recorder, ciMethod* target) {
  bool success = true;
  for (int i = 0; i < (int)CodeBuffer::SECT_LIMIT; i++) {
    uint code_offset = read_position();
    int reloc_count = *(int*)addr(code_offset);
    code_offset += sizeof(int);
    if (reloc_count == 0) {
      set_read_position(code_offset);
      continue;
    }
    // Read _locs_point (as offset from start)
    int locs_point_off = *(int*)addr(code_offset);
    code_offset += sizeof(int);
    uint reloc_size = reloc_count * sizeof(relocInfo);
    CodeSection* cs  = buffer->code_section(i);
    if (cs->locs_capacity() < reloc_count) {
      cs->expand_locs(reloc_count);
    }
    relocInfo* reloc_start = cs->locs_start();
    copy_bytes(addr(code_offset), (address)reloc_start, reloc_size);
    code_offset += reloc_size;
    cs->set_locs_end(reloc_start + reloc_count);
    cs->set_locs_point(cs->start() + locs_point_off);

    // Read additional relocation data: uint per relocation
    uint  data_size  = reloc_count * sizeof(uint);
    uint* reloc_data = (uint*)addr(code_offset);
    code_offset += data_size;
    set_read_position(code_offset);
    LogStreamHandle(Info, scc, reloc) log;
    if (log.is_enabled()) {
      log.print_cr("======== read code section %d relocations [%d]:", i, reloc_count);
    }
    RelocIterator iter(cs);
    int j = 0;
    while (iter.next()) {
      switch (iter.type()) {
        case relocInfo::none:
          break;
        case relocInfo::oop_type: {
          VM_ENTRY_MARK;
          oop_Relocation* r = (oop_Relocation*)iter.reloc();
          if (r->oop_is_immediate()) {
            assert(reloc_data[j] == (uint)j, "should be");
            methodHandle comp_method(THREAD, target->get_Method());
            jobject jo = read_oop(THREAD, comp_method);
            if (lookup_failed()) {
              success = false;
              break;
            }
            r->set_value((address)jo);
          } else if (false) {
            // Get already updated value from OopRecorder.
            assert(oop_recorder != nullptr, "sanity");
            int index = r->oop_index();
            jobject jo = oop_recorder->oop_at(index);
            oop obj = JNIHandles::resolve(jo);
            r->set_value(*reinterpret_cast<address*>(&obj));
          }
          break;
        }
        case relocInfo::metadata_type: {
          VM_ENTRY_MARK;
          metadata_Relocation* r = (metadata_Relocation*)iter.reloc();
          Metadata* m;
          if (r->metadata_is_immediate()) {
            assert(reloc_data[j] == (uint)j, "should be");
            methodHandle comp_method(THREAD, target->get_Method());
            m = read_metadata(comp_method);
            if (lookup_failed()) {
              success = false;
              break;
            }
          } else {
            // Get already updated value from OopRecorder.
            assert(oop_recorder != nullptr, "sanity");
            int index = r->metadata_index();
            m = oop_recorder->metadata_at(index);
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
          fatal("runtime_call_w_cp_type unimplemented");
          //address destination = iter.reloc()->value();
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
          iter.reloc()->fix_relocation_after_move(orig_buffer, buffer);
          break;
        }
        case relocInfo::internal_word_type:
          iter.reloc()->fix_relocation_after_move(orig_buffer, buffer);
          break;
        case relocInfo::section_word_type:
          iter.reloc()->fix_relocation_after_move(orig_buffer, buffer);
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
          fatal("relocation %d unimplemented", (int)iter.type());
          break;
      }
      if (success && log.is_enabled()) {
        iter.print_current_on(&log);
      }
      j++;
    }
    assert(j <= (int)reloc_count, "sanity");
  }
  return success;
}

bool SCCReader::read_code(CodeBuffer* buffer, CodeBuffer* orig_buffer, uint code_offset) {
  assert(code_offset == align_up(code_offset, DATA_ALIGNMENT), "%d not aligned to %d", code_offset, DATA_ALIGNMENT);
  assert(buffer->blob() != nullptr, "sanity");
  SCCodeSection* scc_cs = (SCCodeSection*)addr(code_offset);
  for (int i = 0; i < (int)CodeBuffer::SECT_LIMIT; i++) {
    CodeSection* cs = buffer->code_section(i);
    // Read original section size and address.
    uint orig_size = scc_cs[i]._size;
    log_debug(scc)("======== read code section %d [%d]:", i, orig_size);
    uint orig_size_align = align_up(orig_size, DATA_ALIGNMENT);
    if (i != (int)CodeBuffer::SECT_INSTS) {
      buffer->initialize_section_size(cs, orig_size_align);
    }
    if (orig_size_align > (uint)cs->capacity()) { // Will not fit
      log_info(scc)("%d (L%d): original code section %d size %d > current capacity %d",
                       compile_id(), comp_level(), i, orig_size, cs->capacity());
      return false;
    }
    if (orig_size == 0) {
      assert(cs->size() == 0, "should match");
      continue;  // skip trivial section
    }
    address orig_start = scc_cs[i]._origin_address;

    // Populate fake original buffer (no code allocation in CodeCache).
    // It is used for relocations to calculate sections addesses delta.
    CodeSection* orig_cs = orig_buffer->code_section(i);
    assert(!orig_cs->is_allocated(), "This %d section should not be set", i);
    orig_cs->initialize(orig_start, orig_size);

    // Load code to new buffer.
    address code_start = cs->start();
    copy_bytes(addr(scc_cs[i]._offset + code_offset), code_start, orig_size_align);
    cs->set_end(code_start + orig_size);
  }

  return true;
}

bool SCCache::load_exception_blob(CodeBuffer* buffer, int* pc_offset) {
#ifdef ASSERT
  LogStreamHandle(Debug, scc, nmethod) log;
  if (log.is_enabled()) {
    FlagSetting fs(PrintRelocations, true);
    buffer->print_on(&log);
  }
#endif
  SCCache* cache = open_for_read();
  if (cache == nullptr) {
    return false;
  }
  SCCEntry* entry = cache->find_entry(SCCEntry::Blob, 999);
  if (entry == nullptr) {
    return false;
  }
  SCCReader reader(cache, entry, nullptr);
  return reader.compile_blob(buffer, pc_offset);
}

bool SCCReader::compile_blob(CodeBuffer* buffer, int* pc_offset) {
  uint entry_position = _entry->offset();

  // Read pc_offset
  *pc_offset = *(int*)addr(entry_position);

  // Read name
  uint name_offset = entry_position + _entry->name_offset();
  uint name_size = _entry->name_size(); // Includes '/0'
  const char* name = addr(name_offset);

  log_info(scc, stubs)("%d (L%d): Reading blob '%s' with pc_offset %d from Startup Code Cache '%s'",
                       compile_id(), comp_level(), name, *pc_offset, _cache->cache_path());

  if (strncmp(buffer->name(), name, (name_size - 1)) != 0) {
    log_warning(scc)("%d (L%d): Saved blob's name '%s' is different from '%s'",
                     compile_id(), comp_level(), name, buffer->name());
    ((SCCache*)_cache)->set_failed();
    exit_vm_on_load_failure();
    return false;
  }

  // Create fake original CodeBuffer
  CodeBuffer orig_buffer(name);

  // Read code
  uint code_offset = entry_position + _entry->code_offset();
  if (!read_code(buffer, &orig_buffer, code_offset)) {
    return false;
  }

  // Read relocations
  uint reloc_offset = entry_position + _entry->reloc_offset();
  set_read_position(reloc_offset);
  if (!read_relocations(buffer, &orig_buffer, nullptr, nullptr)) {
    return false;
  }

  log_info(scc, stubs)("%d (L%d): Read blob '%s' from Startup Code Cache '%s'",
                       compile_id(), comp_level(), name, _cache->cache_path());
#ifdef ASSERT
  LogStreamHandle(Debug, scc, nmethod) log;
  if (log.is_enabled()) {
    FlagSetting fs(PrintRelocations, true);
    buffer->print_on(&log);
    buffer->decode();
  }
#endif
  return true;
}

bool SCCache::write_relocations(CodeBuffer* buffer, uint& all_reloc_size) {
  uint all_reloc_count = 0;
  for (int i = 0; i < (int)CodeBuffer::SECT_LIMIT; i++) {
    CodeSection* cs = buffer->code_section(i);
    uint reloc_count = cs->has_locs() ? cs->locs_count() : 0;
    all_reloc_count += reloc_count;
  }
  all_reloc_size = all_reloc_count * sizeof(relocInfo);
  bool success = true;
  uint* reloc_data = NEW_C_HEAP_ARRAY(uint, all_reloc_count, mtCode);
  for (int i = 0; i < (int)CodeBuffer::SECT_LIMIT; i++) {
    CodeSection* cs = buffer->code_section(i);
    int reloc_count = cs->has_locs() ? cs->locs_count() : 0;
    uint n = write_bytes(&reloc_count, sizeof(int));
    if (n != sizeof(int)) {
      success = false;
      break;
    }
    if (reloc_count == 0) {
      continue;
    }
    // Write _locs_point (as offset from start)
    int locs_point_off = cs->locs_point_off();
    n = write_bytes(&locs_point_off, sizeof(int));
    if (n != sizeof(int)) {
      success = false;
      break;
    }
    relocInfo* reloc_start = cs->locs_start();
    uint reloc_size      = reloc_count * sizeof(relocInfo);
    n = write_bytes(reloc_start, reloc_size);
    if (n != reloc_size) {
      success = false;
      break;
    }
    LogStreamHandle(Info, scc, reloc) log;
    if (log.is_enabled()) {
      log.print_cr("======== write code section %d relocations [%d]:", i, reloc_count);
    }
    // Collect additional data
    RelocIterator iter(cs);
    bool has_immediate = false;
    int j = 0;
    while (iter.next()) {
      reloc_data[j] = 0; // initialize
      switch (iter.type()) {
        case relocInfo::none:
          break;
        case relocInfo::oop_type: {
          oop_Relocation* r = (oop_Relocation*)iter.reloc();
          if (r->oop_is_immediate()) {
            reloc_data[j] = (uint)j; // Indication that we need to restore immediate
            has_immediate = true;
          }
          break;
        }
        case relocInfo::metadata_type: {
          metadata_Relocation* r = (metadata_Relocation*)iter.reloc();
          if (r->metadata_is_immediate()) {
            reloc_data[j] = (uint)j; // Indication that we need to restore immediate
            has_immediate = true;
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
          reloc_data[j] = _table->id_for_address(dest, iter, buffer);
          break;
        }
        case relocInfo::trampoline_stub_type: {
          address dest = ((trampoline_stub_Relocation*)iter.reloc())->destination();
          reloc_data[j] = _table->id_for_address(dest, iter, buffer);
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
          reloc_data[j] = _table->id_for_address(dest, iter, buffer);
          break;
        }
        case relocInfo::runtime_call_w_cp_type:
          fatal("runtime_call_w_cp_type unimplemented");
          break;
        case relocInfo::external_word_type: {
          // Record offset of runtime target
          address target = ((external_word_Relocation*)iter.reloc())->target();
          reloc_data[j] = _table->id_for_address(target, iter, buffer);
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
          fatal("relocation %d unimplemented", (int)iter.type());
          break;
      }
      if (log.is_enabled()) {
        iter.print_current_on(&log);
      }
      j++;
    }
    assert(j <= (int)reloc_count, "sanity");
    // Write additional relocation data: uint per relocation
    uint data_size = reloc_count * sizeof(uint);
    n = write_bytes(reloc_data, data_size);
    if (n != data_size) {
      success = false;
      break;
    }
    if (has_immediate) {
      // Save information about immediates in this Code Section
      RelocIterator iter_imm(cs);
      int j = 0;
      while (iter_imm.next()) {
        switch (iter_imm.type()) {
          case relocInfo::oop_type: {
            oop_Relocation* r = (oop_Relocation*)iter_imm.reloc();
            if (r->oop_is_immediate()) {
              assert(reloc_data[j] == (uint)j, "should be");
              jobject jo = *(jobject*)(r->oop_addr()); // Handle currently
              if (!write_oop(jo)) {
                success = false;
              }
            }
            break;
          }
          case relocInfo::metadata_type: {
            metadata_Relocation* r = (metadata_Relocation*)iter_imm.reloc();
            if (r->metadata_is_immediate()) {
              assert(reloc_data[j] == (uint)j, "should be");
              Metadata* m = r->metadata_value();
              if (!write_metadata(m)) {
                success = false;
              }
            }
            break;
          }
          default:
            break;
        }
        if (!success) {
          break;
        }
        j++;
      } // while (iter_imm.next())
    } // if (has_immediate)
  } // for(i < SECT_LIMIT)
  FREE_C_HEAP_ARRAY(uint, reloc_data);
  return success;
}

bool SCCache::write_code(CodeBuffer* buffer, uint& code_size) {
  assert(_write_position == align_up(_write_position, DATA_ALIGNMENT), "%d not aligned to %d", _write_position, DATA_ALIGNMENT);
  //assert(buffer->blob() != nullptr, "sanity");
  uint code_offset = _write_position;
  uint cb_total_size = (uint)buffer->total_content_size();
  // Write information about Code sections first.
  SCCodeSection scc_cs[CodeBuffer::SECT_LIMIT];
  uint scc_cs_size = (uint)(sizeof(SCCodeSection) * CodeBuffer::SECT_LIMIT);
  uint offset = align_up(scc_cs_size, DATA_ALIGNMENT);
  uint total_size = 0;
  for (int i = 0; i < (int)CodeBuffer::SECT_LIMIT; i++) {
    const CodeSection* cs = buffer->code_section(i);
    assert(cs->mark() == nullptr, "CodeSection::_mark is not implemented");
    uint cs_size = (uint)cs->size();
    scc_cs[i]._size = cs_size;
    scc_cs[i]._origin_address = (cs_size == 0) ? nullptr : cs->start();
    scc_cs[i]._offset = (cs_size == 0) ? 0 : (offset + total_size);
    assert(cs->mark() == nullptr, "CodeSection::_mark is not implemented");
    total_size += align_up(cs_size, DATA_ALIGNMENT);
  }
  uint n = write_bytes(scc_cs, scc_cs_size);
  if (n != scc_cs_size) {
    return false;
  }
  if (!align_write()) {
    return false;
  }
  assert(_write_position == (code_offset + offset), "%d  != (%d + %d)", _write_position, code_offset, offset);
  for (int i = 0; i < (int)CodeBuffer::SECT_LIMIT; i++) {
    const CodeSection* cs = buffer->code_section(i);
    uint cs_size = (uint)cs->size();
    if (cs_size == 0) {
      continue;  // skip trivial section
    }
    assert((_write_position - code_offset) == scc_cs[i]._offset, "%d != %d", _write_position, scc_cs[i]._offset);
    // Write code
    n = write_bytes(cs->start(), cs_size);
    if (n != cs_size) {
      return false;
    }
    if (!align_write()) {
      return false;
    }
  }
  assert((_write_position - code_offset) == (offset + total_size), "(%d - %d) != (%d + %d)", _write_position, code_offset, offset, total_size);
  code_size = total_size;
  return true;
}

bool SCCache::store_exception_blob(CodeBuffer* buffer, int pc_offset) {
  SCCache* cache = open_for_write();
  if (cache == nullptr) {
    return false;
  }
  log_info(scc, stubs)("Writing blob '%s' to Startup Code Cache '%s'", buffer->name(), cache->_cache_path);

#ifdef ASSERT
  LogStreamHandle(Debug, scc, nmethod) log;
  if (log.is_enabled()) {
    FlagSetting fs(PrintRelocations, true);
    buffer->print_on(&log);
    buffer->decode();
  }
#endif
  if (!cache->align_write()) {
    return false;
  }
  uint entry_position = cache->_write_position;

  // Write pc_offset
  uint n = cache->write_bytes(&pc_offset, sizeof(int));
  if (n != sizeof(int)) {
    return false;
  }

  // Write name
  const char* name = buffer->name();
  uint name_offset = cache->_write_position - entry_position;
  uint name_size = (uint)strlen(name) + 1; // Includes '/0'
  n = cache->write_bytes(name, name_size);
  if (n != name_size) {
    return false;
  }

  // Write code section
  if (!cache->align_write()) {
    return false;
  }
  uint code_offset = cache->_write_position - entry_position;
  uint code_size = 0;
  if (!cache->write_code(buffer, code_size)) {
    return false;
  }
  // Write relocInfo array
  uint reloc_offset = cache->_write_position - entry_position;
  uint reloc_size = 0;
  if (!cache->write_relocations(buffer, reloc_size)) {
    return false;
  }

  uint entry_size = cache->_write_position - entry_position;
  SCCEntry* entry = new(cache) SCCEntry(entry_position, entry_size, name_offset, name_size,
                                          code_offset, code_size, reloc_offset, reloc_size,
                                          SCCEntry::Blob, (uint32_t)999);
  log_info(scc, stubs)("Wrote stub '%s' to Startup Code Cache '%s'", name, cache->_cache_path);
  return true;
}

DebugInformationRecorder* SCCReader::read_debug_info(OopRecorder* oop_recorder) {
  uint code_offset = align_up(read_position(), DATA_ALIGNMENT);
  int data_size  = *(int*)addr(code_offset);
  code_offset   += sizeof(int);
  int pcs_length = *(int*)addr(code_offset);
  code_offset   += sizeof(int);

  log_debug(scc)("======== read DebugInfo [%d, %d]:", data_size, pcs_length);

  // Aligned initial sizes
  int data_size_align  = align_up(data_size, DATA_ALIGNMENT);
  int pcs_length_align = pcs_length + 1;
  assert(sizeof(PcDesc) > DATA_ALIGNMENT, "sanity");
  DebugInformationRecorder* recorder = new DebugInformationRecorder(oop_recorder, data_size_align, pcs_length);

  copy_bytes(addr(code_offset), recorder->stream()->buffer(), data_size_align);
  recorder->stream()->set_position(data_size);
  code_offset += data_size;

  uint pcs_size = pcs_length * sizeof(PcDesc);
  copy_bytes(addr(code_offset), (address)recorder->pcs(), pcs_size);
  code_offset += pcs_size;
  set_read_position(code_offset);
  return recorder;
}

bool SCCache::write_debug_info(DebugInformationRecorder* recorder) {
  if (!align_write()) {
    return false;
  }
  // Don't call data_size() and pcs_size(). They will freeze OopRecorder.
  int data_size = recorder->stream()->position(); // In bytes
  uint n = write_bytes(&data_size, sizeof(int));
  if (n != sizeof(int)) {
    return false;
  }
  int pcs_length = recorder->pcs_length(); // In bytes
  n = write_bytes(&pcs_length, sizeof(int));
  if (n != sizeof(int)) {
    return false;
  }
  n = write_bytes(recorder->stream()->buffer(), data_size);
  if (n != (uint)data_size) {
    return false;
  }
  uint pcs_size = pcs_length * sizeof(PcDesc);
  n = write_bytes(recorder->pcs(), pcs_size);
  if (n != pcs_size) {
    return false;
  }
  return true;
}

OopMapSet* SCCReader::read_oop_maps() {
  uint code_offset = read_position();
  int om_count = *(int*)addr(code_offset);
  code_offset += sizeof(int);

  log_debug(scc)("======== read oop maps [%d]:", om_count);

  OopMapSet* oop_maps = new OopMapSet(om_count);
  for (int i = 0; i < (int)om_count; i++) {
    int data_size = *(int*)addr(code_offset);
    code_offset += sizeof(int);

    OopMap* oop_map = new OopMap(data_size);
    // Preserve allocated stream
    CompressedWriteStream* stream = oop_map->write_stream();

    // Read data which overwrites default data
    copy_bytes(addr(code_offset), (address)oop_map, sizeof(OopMap));
    code_offset += sizeof(OopMap);
    stream->set_position(data_size);
    oop_map->set_write_stream(stream);
    if (data_size > 0) {
      copy_bytes(addr(code_offset), (address)(oop_map->data()), (uint)data_size);
      code_offset += data_size;
    }
#ifdef ASSERT
    oop_map->_locs_length = 0;
    oop_map->_locs_used   = nullptr;
#endif
    oop_maps->add(oop_map);
  }
  set_read_position(code_offset);
  return oop_maps;
}

bool SCCache::write_oop_maps(OopMapSet* oop_maps) {
  uint om_count = oop_maps->size();
  uint n = write_bytes(&om_count, sizeof(int));
  if (n != sizeof(int)) {
    return false;
  }
  for (int i = 0; i < (int)om_count; i++) {
    OopMap* om = oop_maps->at(i);
    int data_size = om->data_size();
    n = write_bytes(&data_size, sizeof(int));
    if (n != sizeof(int)) {
      return false;
    }
    n = write_bytes(om, sizeof(OopMap));
    if (n != sizeof(OopMap)) {
      return false;
    }
    n = write_bytes(om->data(), (uint)data_size);
    if (n != (uint)data_size) {
      return false;
    }
  }
  return true;
}

jobject SCCReader::read_oop(JavaThread* thread, const methodHandle& comp_method) {
  uint code_offset = read_position();
  oop obj = nullptr;
  DataKind kind = *(DataKind*)addr(code_offset);
  code_offset += sizeof(DataKind);
  set_read_position(code_offset);
  if (kind == DataKind::Null) {
    return nullptr;
  } else if (kind == DataKind::No_Data) {
    return (jobject)Universe::non_oop_word();
  } else if (kind == DataKind::Klass || kind == DataKind::Klass_Shared) {
    Klass* k = read_klass(comp_method, (kind == DataKind::Klass_Shared));
    if (k == nullptr) {
      return nullptr;
    }
    obj = k->java_mirror();
    if (obj == nullptr) {
      set_lookup_failed();
      log_info(scc)("Lookup failed for java_mirror of klass %s", k->external_name());
      return nullptr;
    }
  } else if (kind == DataKind::Primitive) {
    code_offset = read_position();
    int t = *(int*)addr(code_offset);
    code_offset += sizeof(int);
    set_read_position(code_offset);
    BasicType bt = (BasicType)t;
    obj = java_lang_Class::primitive_mirror(bt);
    log_info(scc)("%d (L%d): Read primitive type klass: %s", compile_id(), comp_level(), type2name(bt));
  } else if (kind == DataKind::String_Shared) {
    code_offset = read_position();
    int k = *(int*)addr(code_offset);
    code_offset += sizeof(int);
    set_read_position(code_offset);
    obj = CDSAccess::get_archived_object(k);
    assert(k == CDSAccess::get_archived_object_permanent_index(obj), "sanity");
  } else if (kind == DataKind::String) {
    code_offset = read_position();
    int length = *(int*)addr(code_offset);
    code_offset += sizeof(int);
    set_read_position(code_offset);
    const char* dest = addr(code_offset);
    set_read_position(code_offset + length);
    obj = StringTable::intern(&(dest[0]), thread);
    if (obj == nullptr) {
      set_lookup_failed();
      log_info(scc)("%d (L%d): Lookup failed for String %s",
                       compile_id(), comp_level(), &(dest[0]));
      return nullptr;
    }
    assert(java_lang_String::is_instance(obj), "must be string");
    log_info(scc)("%d (L%d): Read String: %s", compile_id(), comp_level(), dest);
  } else if (kind == DataKind::SysLoader) {
    obj = SystemDictionary::java_system_loader();
    log_info(scc)("%d (L%d): Read java_system_loader", compile_id(), comp_level());
  } else if (kind == DataKind::PlaLoader) {
    obj = SystemDictionary::java_platform_loader();
    log_info(scc)("%d (L%d): Read java_platform_loader", compile_id(), comp_level());
  } else if (kind == DataKind::MH_Oop_Shared) {
    code_offset = read_position();
    int k = *(int*)addr(code_offset);
    code_offset += sizeof(int);
    set_read_position(code_offset);
    obj = CDSAccess::get_archived_object(k);
    assert(k == CDSAccess::get_archived_object_permanent_index(obj), "sanity");
  } else {
    set_lookup_failed();
    log_info(scc)("%d (L%d): Unknown oop's kind: %d",
                     compile_id(), comp_level(), (int)kind);
    return nullptr;
  }
  return JNIHandles::make_local(thread, obj);
}

bool SCCReader::read_oops(OopRecorder* oop_recorder, ciMethod* target) {
  uint code_offset = read_position();
  int oop_count = *(int*)addr(code_offset);
  code_offset += sizeof(int);
  set_read_position(code_offset);
  log_debug(scc)("======== read oops [%d]:", oop_count);
  if (oop_count == 0) {
    return true;
  }
  {
    VM_ENTRY_MARK;
    methodHandle comp_method(THREAD, target->get_Method());
    for (int i = 1; i < oop_count; i++) {
      jobject jo = read_oop(THREAD, comp_method);
      if (lookup_failed()) {
        return false;
      }
      if (oop_recorder->is_real(jo)) {
        oop_recorder->find_index(jo);
      } else {
        oop_recorder->allocate_oop_index(jo);
      }
      LogStreamHandle(Debug, scc, oops) log;
      if (log.is_enabled()) {
        log.print("%d: " INTPTR_FORMAT " ", i, p2i(jo));
        if (jo == (jobject)Universe::non_oop_word()) {
          log.print("non-oop word");
        } else if (jo == nullptr) {
          log.print("nullptr-oop");
        } else {
          JNIHandles::resolve(jo)->print_value_on(&log);
        }
        log.cr();
      }
    }
  }
  return true;
}

Metadata* SCCReader::read_metadata(const methodHandle& comp_method) {
  uint code_offset = read_position();
  Metadata* m = nullptr;
  DataKind kind = *(DataKind*)addr(code_offset);
  code_offset += sizeof(DataKind);
  set_read_position(code_offset);
  if (kind == DataKind::Null) {
    m = (Metadata*)nullptr;
  } else if (kind == DataKind::No_Data) {
    m = (Metadata*)Universe::non_oop_word();
  } else if (kind == DataKind::Klass || kind == DataKind::Klass_Shared) {
    m = (Metadata*)read_klass(comp_method, (kind == DataKind::Klass_Shared));
  } else if (kind == DataKind::Method || kind == DataKind::Method_Shared) {
    m = (Metadata*)read_method(comp_method, (kind == DataKind::Method_Shared));
  } else if (kind == DataKind::MethodCnts) {
    kind = *(DataKind*)addr(code_offset);
    bool shared = (kind == DataKind::Method_Shared);
    assert(kind == DataKind::Method || shared, "Sanity");
    code_offset += sizeof(DataKind);
    set_read_position(code_offset);
    m = (Metadata*)read_method(comp_method, shared);
    if (m != nullptr) {
      Method* method = (Method*)m;
      m = method->get_method_counters(Thread::current());
      if (m == nullptr) {
        set_lookup_failed();
        log_info(scc)("%d (L%d): Failed to get MethodCounters", compile_id(), comp_level());
      } else {
        log_info(scc)("%d (L%d): Read MethodCounters : " INTPTR_FORMAT, compile_id(), comp_level(), p2i(m));
      }
    }
  } else {
    set_lookup_failed();
    log_info(scc)("%d (L%d): Unknown metadata's kind: %d", compile_id(), comp_level(), (int)kind);
  }
  return m;
}

bool SCCReader::read_metadata(OopRecorder* oop_recorder, ciMethod* target) {
  uint code_offset = read_position();
  int metadata_count = *(int*)addr(code_offset);
  code_offset += sizeof(int);
  set_read_position(code_offset);

  log_debug(scc)("======== read metadata [%d]:", metadata_count);

  if (metadata_count == 0) {
    return true;
  }
  {
    VM_ENTRY_MARK;
    methodHandle comp_method(THREAD, target->get_Method());

    for (int i = 1; i < metadata_count; i++) {
      Metadata* m = read_metadata(comp_method);
      if (lookup_failed()) {
        return false;
      }
      if (oop_recorder->is_real(m)) {
        oop_recorder->find_index(m);
      } else {
        oop_recorder->allocate_metadata_index(m);
      }
      LogTarget(Debug, scc, metadata) log;
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
  }
  return true;
}

bool SCCache::write_oop(jobject& jo) {
  DataKind kind;
  uint n = 0;
  oop obj = JNIHandles::resolve(jo);
  if (jo == nullptr) {
    kind = DataKind::Null;
    n = write_bytes(&kind, sizeof(int));
    if (n != sizeof(int)) {
      return false;
    }
  } else if (jo == (jobject)Universe::non_oop_word()) {
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
      log_info(scc)("%d (L%d): Write primitive type klass: %s", compile_id(), comp_level(), type2name((BasicType)bt));
    } else {
      Klass* klass = java_lang_Class::as_Klass(obj);
      if (!write_klass(klass)) {
        return false;
      }
    }
  } else if (java_lang_String::is_instance(obj)) { // herere
    int k = CDSAccess::get_archived_object_permanent_index(obj);  // k >= 1 means obj is a "permanent heap object"
    if (k > 0) {
      kind = DataKind::String_Shared;
      n = write_bytes(&kind, sizeof(int));
      if (n != sizeof(int)) {
        return false;
      }
      n = write_bytes(&k, sizeof(int));
      if (n != sizeof(int)) {
        return false;
      }
      return true;
    }
    kind = DataKind::String;
    n = write_bytes(&kind, sizeof(int));
    if (n != sizeof(int)) {
      return false;
    }
    ResourceMark rm;
    size_t length_sz = 0;
    const char* string = java_lang_String::as_utf8_string(obj, length_sz);
    int length = (int)length_sz; // FIXME -- cast
    length++; // write tailing '/0'
    n = write_bytes(&length, sizeof(int));
    if (n != sizeof(int)) {
      return false;
    }
    n = write_bytes(string, (uint)length);
    if (n != (uint)length) {
      return false;
    }
    log_info(scc)("%d (L%d): Write String: %s", compile_id(), comp_level(), string);
  } else if (java_lang_Module::is_instance(obj)) {
    fatal("Module object unimplemented");
  } else if (java_lang_ClassLoader::is_instance(obj)) {
    if (obj == SystemDictionary::java_system_loader()) {
      kind = DataKind::SysLoader;
      log_info(scc)("%d (L%d): Write ClassLoader: java_system_loader", compile_id(), comp_level());
    } else if (obj == SystemDictionary::java_platform_loader()) {
      kind = DataKind::PlaLoader;
      log_info(scc)("%d (L%d): Write ClassLoader: java_platform_loader", compile_id(), comp_level());
    } else {
      fatal("ClassLoader object unimplemented");
      return false;
    }
    n = write_bytes(&kind, sizeof(int));
    if (n != sizeof(int)) {
      return false;
    }
  } else { // herere
    int k = CDSAccess::get_archived_object_permanent_index(obj);  // k >= 1 means obj is a "permanent heap object"
    if (k > 0) {
      kind = DataKind::MH_Oop_Shared;
      n = write_bytes(&kind, sizeof(int));
      if (n != sizeof(int)) {
        return false;
      }
      n = write_bytes(&k, sizeof(int));
      if (n != sizeof(int)) {
        return false;
      }
      return true;
    }
    // Unhandled oop - bailout
    set_lookup_failed();
    log_info(scc, nmethod)("%d (L%d): Unhandled obj: " PTR_FORMAT " : %s",
                              compile_id(), comp_level(), p2i(obj), obj->klass()->external_name());
    return false;
  }
  return true;
}

bool SCCache::write_oops(OopRecorder* oop_recorder) {
  int oop_count = oop_recorder->oop_count();
  uint n = write_bytes(&oop_count, sizeof(int));
  if (n != sizeof(int)) {
    return false;
  }
  log_debug(scc)("======== write oops [%d]:", oop_count);

  for (int i = 1; i < oop_count; i++) { // skip first virtual nullptr
    jobject jo = oop_recorder->oop_at(i);
    LogStreamHandle(Info, scc, oops) log;
    if (log.is_enabled()) {
      log.print("%d: " INTPTR_FORMAT " ", i, p2i(jo));
      if (jo == (jobject)Universe::non_oop_word()) {
        log.print("non-oop word");
      } else if (jo == nullptr) {
        log.print("nullptr-oop");
      } else {
        JNIHandles::resolve(jo)->print_value_on(&log);
      }
      log.cr();
    }
    if (!write_oop(jo)) {
      return false;
    }
  }
  return true;
}

bool SCCache::write_metadata(Metadata* m) {
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
    log_info(scc)("%d (L%d): Write MethodCounters : " INTPTR_FORMAT, compile_id(), comp_level(), p2i(m));
  } else { // Not supported
    fatal("metadata : " INTPTR_FORMAT " unimplemented", p2i(m));
    return false;
  }
  return true;
}

bool SCCache::write_metadata(OopRecorder* oop_recorder) {
  int metadata_count = oop_recorder->metadata_count();
  uint n = write_bytes(&metadata_count, sizeof(int));
  if (n != sizeof(int)) {
    return false;
  }

  log_debug(scc)("======== write metadata [%d]:", metadata_count);

  for (int i = 1; i < metadata_count; i++) { // skip first virtual nullptr
    Metadata* m = oop_recorder->metadata_at(i);
    LogStreamHandle(Debug, scc, metadata) log;
    if (log.is_enabled()) {
      log.print("%d: " INTPTR_FORMAT " ", i, p2i(m));
      if (m == (Metadata*)Universe::non_oop_word()) {
        log.print("non-metadata word");
      } else if (m == nullptr) {
        log.print("nullptr-oop");
      } else {
        Metadata::print_value_on_maybe_null(&log, m);
      }
      log.cr();
    }
    if (!write_metadata(m)) {
      return false;
    }
  }
  return true;
}

bool SCCReader::read_dependencies(Dependencies* dependencies) {
  uint code_offset = read_position();
  int dependencies_size = *(int*)addr(code_offset);

  log_debug(scc)("======== read dependencies [%d]:", dependencies_size);

  code_offset += sizeof(int);
  code_offset = align_up(code_offset, DATA_ALIGNMENT);
  if (dependencies_size > 0) {
    dependencies->set_content((u_char*)addr(code_offset), dependencies_size);
  }
  code_offset += dependencies_size;
  set_read_position(code_offset);
  return true;
}

bool SCCache::load_nmethod(ciEnv* env, ciMethod* target, int entry_bci, AbstractCompiler* compiler, CompLevel comp_level) {
  TraceTime t1("SC total load time", &_t_totalLoad, enable_timers(), false);
  CompileTask* task = env->task();
  SCCEntry* entry = task->scc_entry();
  bool preload = task->preload();
  assert(entry != nullptr, "sanity");
  SCCache* cache = open_for_read();
  if (cache == nullptr) {
    return false;
  }
  if (log_is_enabled(Info, scc, nmethod)) {
    uint decomp = (target->method_data() == nullptr) ? 0 : target->method_data()->decompile_count();
    VM_ENTRY_MARK;
    ResourceMark rm;
    methodHandle method(THREAD, target->get_Method());
    const char* target_name = method->name_and_sig_as_C_string();
    uint hash = java_lang_String::hash_code((const jbyte*)target_name, (int)strlen(target_name));
    bool clinit_brs = entry->has_clinit_barriers();
    log_info(scc, nmethod)("%d (L%d): %s nmethod '%s' (decomp: %d, hash: " UINT32_FORMAT_X_0 "%s%s)",
                           task->compile_id(), task->comp_level(), (preload ? "Preloading" : "Reading"),
                           target_name, decomp, hash, (clinit_brs ? ", has clinit barriers" : ""),
                           (entry->ignore_decompile() ? ", ignore_decomp" : ""));
  }
  ReadingMark rdmk;
  if (rdmk.failed()) {
    // Cache is closed, cannot touch anything.
    return false;
  }

  SCCReader reader(cache, entry, task);
  bool success = reader.compile(env, target, entry_bci, compiler);
  if (success) {
    task->set_num_inlined_bytecodes(entry->num_inlined_bytecodes());
  } else {
    entry->set_load_fail();
  }
  return success;
}

SCCReader::SCCReader(SCCache* cache, SCCEntry* entry, CompileTask* task) {
  _cache = cache;
  _entry   = entry;
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

bool SCCReader::compile(ciEnv* env, ciMethod* target, int entry_bci, AbstractCompiler* compiler) {
  uint entry_position = _entry->offset();
  uint code_offset = entry_position + _entry->code_offset();
  set_read_position(code_offset);

  // Read flags
  int flags = *(int*)addr(code_offset);
  code_offset += sizeof(int);
  bool has_monitors      = (flags & 0x1) != 0;
  bool has_wide_vectors  = (flags & 0x2) != 0;
  bool has_unsafe_access = (flags & 0x4) != 0;
  bool has_scoped_access = (flags & 0x8) != 0;

  int orig_pc_offset = *(int*)addr(code_offset);
  code_offset += sizeof(int);
  int frame_size = *(int*)addr(code_offset);
  code_offset += sizeof(int);

  // Read offsets
  CodeOffsets* offsets = (CodeOffsets*)addr(code_offset);
  code_offset += sizeof(CodeOffsets);

  // Create Debug Information Recorder to record scopes, oopmaps, etc.
  OopRecorder* oop_recorder = new OopRecorder(env->arena());
  env->set_oop_recorder(oop_recorder);

  set_read_position(code_offset);

  // Write OopRecorder data
  if (!read_oops(oop_recorder, target)) {
    return false;
  }
  if (!read_metadata(oop_recorder, target)) {
    return false;
  }

  // Read Debug info
  DebugInformationRecorder* recorder = read_debug_info(oop_recorder);
  if (recorder == nullptr) {
    return false;
  }
  env->set_debug_info(recorder);

  // Read Dependencies (compressed already)
  Dependencies* dependencies = new Dependencies(env);
  if (!read_dependencies(dependencies)) {
    return false;
  }
  env->set_dependencies(dependencies);

  // Read oop maps
  OopMapSet* oop_maps = read_oop_maps();
  if (oop_maps == nullptr) {
    return false;
  }

  // Read exception handles
  code_offset = read_position();
  int exc_table_length = *(int*)addr(code_offset);
  code_offset += sizeof(int);
  ExceptionHandlerTable handler_table(MAX2(exc_table_length, 4));
  if (exc_table_length > 0) {
    handler_table.set_length(exc_table_length);
    uint exc_table_size = handler_table.size_in_bytes();
    copy_bytes(addr(code_offset), (address)handler_table.table(), exc_table_size);
    code_offset += exc_table_size;
  }

  // Read null check table
  int nul_chk_length = *(int*)addr(code_offset);
  code_offset += sizeof(int);
  ImplicitExceptionTable nul_chk_table;
  if (nul_chk_length > 0) {
    nul_chk_table.set_size(nul_chk_length);
    nul_chk_table.set_len(nul_chk_length);
    uint nul_chk_size = nul_chk_table.size_in_bytes();
    copy_bytes(addr(code_offset), (address)nul_chk_table.data(), nul_chk_size - sizeof(implicit_null_entry));
    code_offset += nul_chk_size;
  }

  uint reloc_size = _entry->reloc_size();
  CodeBuffer buffer("Compile::Fill_buffer", _entry->code_size(), reloc_size);
  buffer.initialize_oop_recorder(oop_recorder);

  const char* name = addr(entry_position + _entry->name_offset());

  // Create fake original CodeBuffer
  CodeBuffer orig_buffer(name);

  // Read code
  if (!read_code(&buffer, &orig_buffer, align_up(code_offset, DATA_ALIGNMENT))) {
    return false;
  }

  // Read relocations
  uint reloc_offset = entry_position + _entry->reloc_offset();
  set_read_position(reloc_offset);
  if (!read_relocations(&buffer, &orig_buffer, oop_recorder, target)) {
    return false;
  }

  log_info(scc, nmethod)("%d (L%d): Read nmethod '%s' from Startup Code Cache '%s'", compile_id(), comp_level(), name, _cache->cache_path());
#ifdef ASSERT
  LogStreamHandle(Debug, scc, nmethod) log;
  if (log.is_enabled()) {
    FlagSetting fs(PrintRelocations, true);
    buffer.print_on(&log);
    buffer.decode();
  }
#endif

  if (VerifyCachedCode) {
    return false;
  }

  // Register nmethod
  TraceTime t1("SC total nmethod register time", &_t_totalRegister, enable_timers(), false);
  env->register_method(target, entry_bci,
                       offsets, orig_pc_offset,
                       &buffer, frame_size,
                       oop_maps, &handler_table,
                       &nul_chk_table, compiler,
                       _entry->has_clinit_barriers(),
                       false,
                       has_unsafe_access,
                       has_wide_vectors,
                       has_monitors,
                       has_scoped_access,
                       0, true /* install_code */,
                       (SCCEntry *)_entry);
  CompileTask* task = env->task();
  bool success = task->is_success();
  if (success) {
    ((SCCEntry *)_entry)->set_loaded();
  }
  return success;
}

// No concurency for writing to cache file because this method is called from
// ciEnv::register_method() under MethodCompileQueue_lock and Compile_lock locks.
SCCEntry* SCCache::store_nmethod(const methodHandle& method,
                     int comp_id,
                     int entry_bci,
                     CodeOffsets* offsets,
                     int orig_pc_offset,
                     DebugInformationRecorder* recorder,
                     Dependencies* dependencies,
                     CodeBuffer* buffer,
                     int frame_size,
                     OopMapSet* oop_maps,
                     ExceptionHandlerTable* handler_table,
                     ImplicitExceptionTable* nul_chk_table,
                     AbstractCompiler* compiler,
                     CompLevel comp_level,
                     bool has_clinit_barriers,
                     bool for_preload,
                     bool has_unsafe_access,
                     bool has_wide_vectors,
                     bool has_monitors,
                     bool has_scoped_access) {
  if (!CDSConfig::is_dumping_cached_code()) {
    return nullptr; // The metadata and heap in the CDS image haven't been finalized yet.
  }
  if (entry_bci != InvocationEntryBci) {
    return nullptr; // No OSR
  }
  if (compiler->is_c1() && (comp_level == CompLevel_simple || comp_level == CompLevel_limited_profile)) {
    // Cache tier1 compilations
  } else if (!compiler->is_c2()) {
    return nullptr; // Only C2 now
  }
  TraceTime t1("SC total store time", &_t_totalStore, enable_timers(), false);
  SCCache* cache = open_for_write();
  if (cache == nullptr) {
    return nullptr; // Cache file is closed
  }
  SCCEntry* entry = cache->write_nmethod(method, comp_id, entry_bci, offsets, orig_pc_offset, recorder, dependencies, buffer,
                                  frame_size, oop_maps, handler_table, nul_chk_table, compiler, comp_level,
                                  has_clinit_barriers, for_preload, has_unsafe_access, has_wide_vectors, has_monitors, has_scoped_access);
  if (entry == nullptr) {
    log_info(scc, nmethod)("%d (L%d): nmethod store attempt failed", comp_id, (int)comp_level);
  }
  return entry;
}

SCCEntry* SCCache::write_nmethod(const methodHandle& method,
                                 int comp_id,
                                 int entry_bci,
                                 CodeOffsets* offsets,
                                 int orig_pc_offset,
                                 DebugInformationRecorder* recorder,
                                 Dependencies* dependencies,
                                 CodeBuffer* buffer,
                                 int frame_size,
                                 OopMapSet* oop_maps,
                                 ExceptionHandlerTable* handler_table,
                                 ImplicitExceptionTable* nul_chk_table,
                                 AbstractCompiler* compiler,
                                 CompLevel comp_level,
                                 bool has_clinit_barriers,
                                 bool for_preload,
                                 bool has_unsafe_access,
                                 bool has_wide_vectors,
                                 bool has_monitors,
                                 bool has_scoped_access) {
//  if (method->is_hidden()) {
//    ResourceMark rm;
//    log_info(scc, nmethod)("%d (L%d): Skip hidden method '%s'", comp_id, (int)comp_level, method->name_and_sig_as_C_string());
//    return nullptr;
//  }
  if (buffer->before_expand() != nullptr) {
    ResourceMark rm;
    log_info(scc, nmethod)("%d (L%d): Skip nmethod with expanded buffer '%s'", comp_id, (int)comp_level, method->name_and_sig_as_C_string());
    return nullptr;
  }
#ifdef ASSERT
  LogStreamHandle(Debug, scc, nmethod) log;
  if (log.is_enabled()) {
    tty->print_cr(" == store_nmethod");
    FlagSetting fs(PrintRelocations, true);
    buffer->print_on(&log);
    buffer->decode();
  }
#endif
  assert(!has_clinit_barriers || _gen_preload_code, "sanity");
  Method* m = method();
  bool method_in_cds = MetaspaceShared::is_in_shared_metaspace((address)m); // herere
  InstanceKlass* holder = m->method_holder();
  bool klass_in_cds = holder->is_shared() && !holder->is_shared_unregistered_class();
  bool builtin_loader = holder->class_loader_data()->is_builtin_class_loader_data();
  if (!builtin_loader) {
    ResourceMark rm;
    log_info(scc, nmethod)("%d (L%d): Skip method '%s' loaded by custom class loader %s", comp_id, (int)comp_level, method->name_and_sig_as_C_string(), holder->class_loader_data()->loader_name());
    return nullptr;
  }
  if (for_preload && !(method_in_cds && klass_in_cds)) {
    ResourceMark rm;
    log_info(scc, nmethod)("%d (L%d): Skip method '%s' for preload: not in CDS", comp_id, (int)comp_level, method->name_and_sig_as_C_string());
    return nullptr;
  }
  assert(!for_preload || method_in_cds, "sanity");
  _for_preload = for_preload;
  _has_clinit_barriers = has_clinit_barriers;

  if (!align_write()) {
    return nullptr;
  }
  _compile_id = comp_id;
  _comp_level = (int)comp_level;

  uint entry_position = _write_position;

  uint decomp = (method->method_data() == nullptr) ? 0 : method->method_data()->decompile_count();

  // Is this one-step workflow assembly phase?
  // In this phase compilation is done based on saved profiling data
  // without application run. Ignore decompilation counters in such case.
  // Also ignore it for C1 code because it is decompiled unconditionally
  // when C2 generated code is published.
  bool ignore_decompile = (comp_level == CompLevel_limited_profile) ||
                          CDSConfig::is_dumping_final_static_archive();

  // Write name
  uint name_offset = 0;
  uint name_size   = 0;
  uint hash = 0;
  uint n;
  {
    ResourceMark rm;
    const char* name   = method->name_and_sig_as_C_string();
    log_info(scc, nmethod)("%d (L%d): Writing nmethod '%s' (comp level: %d, decomp: %d%s%s) to Startup Code Cache '%s'",
                           comp_id, (int)comp_level, name, comp_level, decomp,
                           (ignore_decompile ? ", ignore_decomp" : ""),
                           (has_clinit_barriers ? ", has clinit barriers" : ""), _cache_path);

    LogStreamHandle(Info, scc, loader) log;
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

  if (!align_write()) {
    return nullptr;
  }

  uint code_offset = _write_position - entry_position;

  int flags = (has_scoped_access ? 0x8 : 0) |
              (has_unsafe_access ? 0x4 : 0) |
              (has_wide_vectors  ? 0x2 : 0) |
              (has_monitors      ? 0x1 : 0);
  n = write_bytes(&flags, sizeof(int));
  if (n != sizeof(int)) {
    return nullptr;
  }

  n = write_bytes(&orig_pc_offset, sizeof(int));
  if (n != sizeof(int)) {
    return nullptr;
  }

  n = write_bytes(&frame_size, sizeof(int));
  if (n != sizeof(int)) {
    return nullptr;
  }

  // Write offsets
  n = write_bytes(offsets, sizeof(CodeOffsets));
  if (n != sizeof(CodeOffsets)) {
    return nullptr;
  }

  // Write OopRecorder data
  if (!write_oops(buffer->oop_recorder())) {
    if (lookup_failed() && !failed()) {
      // Skip this method and reposition file
      set_write_position(entry_position);
    }
    return nullptr;
  }
  if (!write_metadata(buffer->oop_recorder())) {
    if (lookup_failed() && !failed()) {
      // Skip this method and reposition file
      set_write_position(entry_position);
    }
    return nullptr;
  }

  // Write Debug info
  if (!write_debug_info(recorder)) {
    return nullptr;
  }
  // Write Dependencies
  int dependencies_size = (int)dependencies->size_in_bytes();
  n = write_bytes(&dependencies_size, sizeof(int));
  if (n != sizeof(int)) {
    return nullptr;
  }
  if (!align_write()) {
    return nullptr;
  }
  n = write_bytes(dependencies->content_bytes(), dependencies_size);
  if (n != (uint)dependencies_size) {
    return nullptr;
  }

  // Write oop maps
  if (!write_oop_maps(oop_maps)) {
    return nullptr;
  }

  // Write exception handles
  int exc_table_length = handler_table->length();
  n = write_bytes(&exc_table_length, sizeof(int));
  if (n != sizeof(int)) {
    return nullptr;
  }
  uint exc_table_size = handler_table->size_in_bytes();
  n = write_bytes(handler_table->table(), exc_table_size);
  if (n != exc_table_size) {
    return nullptr;
  }

  // Write null check table
  int nul_chk_length = nul_chk_table->len();
  n = write_bytes(&nul_chk_length, sizeof(int));
  if (n != sizeof(int)) {
    return nullptr;
  }
  uint nul_chk_size = nul_chk_table->size_in_bytes();
  n = write_bytes(nul_chk_table->data(), nul_chk_size);
  if (n != nul_chk_size) {
    return nullptr;
  }

  // Write code section
  if (!align_write()) {
    return nullptr;
  }
  uint code_size = 0;
  if (!write_code(buffer, code_size)) {
    return nullptr;
  }
  // Write relocInfo array
  uint reloc_offset = _write_position - entry_position;
  uint reloc_size = 0;
  if (!write_relocations(buffer, reloc_size)) {
    if (lookup_failed() && !failed()) {
      // Skip this method and reposition file
      set_write_position(entry_position);
    }
    return nullptr;
  }
  uint entry_size = _write_position - entry_position;

  SCCEntry* entry = new (this) SCCEntry(entry_position, entry_size, name_offset, name_size,
                                        code_offset, code_size, reloc_offset, reloc_size,
                                        SCCEntry::Code, hash, (uint)comp_level, (uint)comp_id, decomp,
                                        has_clinit_barriers, _for_preload, ignore_decompile);
  if (method_in_cds) {
    entry->set_method(m);
  }
#ifdef ASSERT
  if (has_clinit_barriers || _for_preload) {
    assert(for_preload, "sanity");
    assert(entry->method() != nullptr, "sanity");
  }
#endif
  {
    ResourceMark rm;
    const char* name   = method->name_and_sig_as_C_string();
    log_info(scc, nmethod)("%d (L%d): Wrote nmethod '%s'%s to Startup Code Cache '%s'",
                           comp_id, (int)comp_level, name, (_for_preload ? " (for preload)" : ""), _cache_path);
  }
  if (VerifyCachedCode) {
    return nullptr;
  }
  return entry;
}

static void print_helper1(outputStream* st, const char* name, int count) {
  if (count > 0) {
    st->print(" %s=%d", name, count);
  }
}
static void print_helper(outputStream* st, const char* name, int stats[6+3][6], int idx) {
  int total = stats[idx][0];
  if (total > 0) {
    st->print("  %s:", name);
    print_helper1(st, "total",               stats[idx][0]);
    //print_helper1(st, "for_preload",         stats[idx][2]); // implied by Tier5
    print_helper1(st, "loaded",              stats[idx][3]);
    print_helper1(st, "invalidated",         stats[idx][4]);
    print_helper1(st, "failed",              stats[idx][5]);
    print_helper1(st, "has_clinit_barriers", stats[idx][1]);
    st->cr();
  }
}

void SCCache::print_statistics_on(outputStream* st) {
  SCCache* cache = open_for_read();
  if (cache != nullptr) {
    ReadingMark rdmk;
    if (rdmk.failed()) {
      // Cache is closed, cannot touch anything.
      return;
    }

    uint count = cache->_load_header->entries_count();
    uint* search_entries = (uint*)cache->addr(cache->_load_header->entries_offset()); // [id, index]
    SCCEntry* load_entries = (SCCEntry*)(search_entries + 2 * count);

    int stats[6 + 3][6] = {0};
    for (uint i = 0; i < count; i++) {
      int index = search_entries[2*i + 1];
      SCCEntry* entry = &(load_entries[index]);

      int lvl = entry->kind();
      if (entry->kind() == SCCEntry::Code) {
        lvl += entry->comp_level() + (entry->for_preload() ? 1 : 0);
      }
      ++stats[lvl][0]; // total
      if (entry->has_clinit_barriers()) {
        ++stats[lvl][1];
      }
      if (entry->for_preload()) {
        ++stats[lvl][2];
      }
      if (entry->is_loaded()) {
        ++stats[lvl][3];
      }
      if (entry->not_entrant()) {
        ++stats[lvl][4];
      }
      if (entry->load_fail()) {
        ++stats[lvl][5];
      }
    }

    print_helper(st, "None", stats, SCCEntry::None);
    print_helper(st, "Stub", stats, SCCEntry::Stub);
    print_helper(st, "Blob", stats, SCCEntry::Blob);
    for (int lvl = 0; lvl <= CompLevel_full_optimization + 1; lvl++) {
      ResourceMark rm;
      stringStream ss;
      ss.print("SC T%d", lvl);
      print_helper(st, ss.freeze(), stats, SCCEntry::Code + lvl);
    }

  } else {
    st->print_cr("failed to open SCA at %s", CachedCodeFile);
  }
}

void SCCache::print_on(outputStream* st) {
  SCCache* cache = open_for_read();
  if (cache != nullptr) {
    ReadingMark rdmk;
    if (rdmk.failed()) {
      // Cache is closed, cannot touch anything.
      return;
    }

    uint count = cache->_load_header->entries_count();
    uint* search_entries = (uint*)cache->addr(cache->_load_header->entries_offset()); // [id, index]
    SCCEntry* load_entries = (SCCEntry*)(search_entries + 2 * count);

    for (uint i = 0; i < count; i++) {
      int index = search_entries[2*i + 1];
      SCCEntry* entry = &(load_entries[index]);

      st->print_cr("%4u: %4u: K%u L%u offset=%u decompile=%u size=%u code_size=%u%s%s%s%s",
                i, index, entry->kind(), entry->comp_level(), entry->offset(),
                entry->decompile(), entry->size(), entry->code_size(),
                entry->has_clinit_barriers() ? " has_clinit_barriers" : "",
                entry->for_preload()         ? " for_preload"         : "",
                entry->is_loaded()           ? " loaded"              : "",
                entry->not_entrant()         ? " not_entrant"         : "");
      st->print_raw("         ");
      SCCReader reader(cache, entry, nullptr);
      reader.print_on(st);
    }
  } else {
    st->print_cr("failed to open SCA at %s", CachedCodeFile);
  }
}

void SCCache::print_unused_entries_on(outputStream* st) {
  LogStreamHandle(Info, scc, init) info;
  if (info.is_enabled()) {
    SCCache::iterate([&](SCCEntry* entry) {
      if (!entry->is_loaded()) {
        MethodTrainingData* mtd = MethodTrainingData::lookup_for(entry->method());
        if (mtd != nullptr) {
          if (mtd->has_holder()) {
            if (mtd->holder()->method_holder()->is_initialized()) {
              ResourceMark rm;
              mtd->iterate_all_compiles([&](CompileTrainingData* ctd) {
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
                    info.print("SCC entry not loaded: ");
                    ctd->print_on(&info);
                    info.cr();
                  }
                }
              });
            } else {
              // not yet initialized
            }
          } else {
            info.print("SCC entry doesn't have a holder: ");
            mtd->print_on(&info);
            info.cr();
          }
        }
      }
    });
  }
}

void SCCReader::print_on(outputStream* st) {
  uint entry_position = _entry->offset();
  set_read_position(entry_position);

  // Read name
  uint name_offset = entry_position + _entry->name_offset();
  uint name_size = _entry->name_size(); // Includes '/0'
  const char* name = addr(name_offset);

  st->print_cr("  name: %s", name);
}

#define _extrs_max 80
#define _stubs_max 120
#define _blobs_max 100
#define _shared_blobs_max 24
#define _C2_blobs_max 25
#define _C1_blobs_max (_blobs_max - _shared_blobs_max - _C2_blobs_max)
#define _all_max 300

#define SET_ADDRESS(type, addr)                           \
  {                                                       \
    type##_addr[type##_length++] = (address) (addr);      \
    assert(type##_length <= type##_max, "increase size"); \
  }

static bool initializing = false;
void SCAddressTable::init() {
  if (_complete || initializing) return; // Done already
  initializing = true;
  _extrs_addr = NEW_C_HEAP_ARRAY(address, _extrs_max, mtCode);
  _stubs_addr = NEW_C_HEAP_ARRAY(address, _stubs_max, mtCode);
  _blobs_addr = NEW_C_HEAP_ARRAY(address, _blobs_max, mtCode);

  // Divide _blobs_addr array to chunks because they could be initialized in parrallel
  _C2_blobs_addr = _blobs_addr + _shared_blobs_max;// C2 blobs addresses stored after shared blobs
  _C1_blobs_addr = _C2_blobs_addr + _C2_blobs_max; // C1 blobs addresses stored after C2 blobs

  _extrs_length = 0;
  _stubs_length = 0;
  _blobs_length = 0;       // for shared blobs
  _C1_blobs_length = 0;
  _C2_blobs_length = 0;
  _final_blobs_length = 0; // Depends on numnber of C1 blobs

  // Runtime methods
#ifdef COMPILER2
  SET_ADDRESS(_extrs, OptoRuntime::handle_exception_C);
#endif
#ifdef COMPILER1
  SET_ADDRESS(_extrs, Runtime1::is_instance_of);
  SET_ADDRESS(_extrs, Runtime1::trace_block_entry);
#endif

  SET_ADDRESS(_extrs, CompressedOops::base_addr());
#if INCLUDE_G1GC
  SET_ADDRESS(_extrs, G1BarrierSetRuntime::write_ref_field_post_entry);
  SET_ADDRESS(_extrs, G1BarrierSetRuntime::write_ref_field_pre_entry);
#endif

#if INCLUDE_SHENANDOAHGC
  SET_ADDRESS(_extrs, ShenandoahRuntime::arraycopy_barrier_oop_entry);
  SET_ADDRESS(_extrs, ShenandoahRuntime::arraycopy_barrier_narrow_oop_entry);
  SET_ADDRESS(_extrs, ShenandoahRuntime::write_ref_field_pre_entry);
  SET_ADDRESS(_extrs, ShenandoahRuntime::shenandoah_clone_barrier);
  SET_ADDRESS(_extrs, ShenandoahRuntime::load_reference_barrier_strong);
  SET_ADDRESS(_extrs, ShenandoahRuntime::load_reference_barrier_strong_narrow);
  SET_ADDRESS(_extrs, ShenandoahRuntime::load_reference_barrier_weak);
  SET_ADDRESS(_extrs, ShenandoahRuntime::load_reference_barrier_weak_narrow);
  SET_ADDRESS(_extrs, ShenandoahRuntime::load_reference_barrier_phantom);
  SET_ADDRESS(_extrs, ShenandoahRuntime::load_reference_barrier_phantom_narrow);
#endif

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
#if INCLUDE_JVMTI
  SET_ADDRESS(_extrs, &JvmtiExport::_should_notify_object_alloc);
#endif /* INCLUDE_JVMTI */
  BarrierSet* bs = BarrierSet::barrier_set();
  if (bs->is_a(BarrierSet::CardTableBarrierSet)) {
    SET_ADDRESS(_extrs, ci_card_table_address_as<address>());
  }
  SET_ADDRESS(_extrs, ThreadIdentifier::unsafe_offset());
  SET_ADDRESS(_extrs, Thread::current);

  SET_ADDRESS(_extrs, os::javaTimeMillis);
  SET_ADDRESS(_extrs, os::javaTimeNanos);

#if INCLUDE_JVMTI
  SET_ADDRESS(_extrs, &JvmtiVTMSTransitionDisabler::_VTMS_notify_jvmti_events);
#endif /* INCLUDE_JVMTI */
  SET_ADDRESS(_extrs, StubRoutines::crc_table_addr());
#ifndef PRODUCT
  SET_ADDRESS(_extrs, &SharedRuntime::_partial_subtype_ctr);
  SET_ADDRESS(_extrs, JavaThread::verify_cross_modify_fence_failure);
#endif

#ifndef ZERO
#if defined(AMD64) || defined(AARCH64) || defined(RISCV64)
  SET_ADDRESS(_extrs, MacroAssembler::debug64);
#endif
#if defined(AMD64)
  SET_ADDRESS(_extrs, StubRoutines::x86::arrays_hashcode_powers_of_31());
#endif
#endif

#ifdef COMPILER1
#ifdef X86
  SET_ADDRESS(_extrs, LIR_Assembler::float_signmask_pool);
  SET_ADDRESS(_extrs, LIR_Assembler::double_signmask_pool);
  SET_ADDRESS(_extrs, LIR_Assembler::float_signflip_pool);
  SET_ADDRESS(_extrs, LIR_Assembler::double_signflip_pool);
#endif
#endif

  // addresses of fields in AOT runtime constants area
  address* p = AOTRuntimeConstants::field_addresses_list();
  while (*p != nullptr) {
    SET_ADDRESS(_extrs, *p++);
  }
  // Stubs
  SET_ADDRESS(_stubs, StubRoutines::method_entry_barrier());
  SET_ADDRESS(_stubs, StubRoutines::forward_exception_entry());
/*
  SET_ADDRESS(_stubs, StubRoutines::throw_AbstractMethodError_entry());
  SET_ADDRESS(_stubs, StubRoutines::throw_IncompatibleClassChangeError_entry());
  SET_ADDRESS(_stubs, StubRoutines::throw_NullPointerException_at_call_entry());
  SET_ADDRESS(_stubs, StubRoutines::throw_StackOverflowError_entry());
  SET_ADDRESS(_stubs, StubRoutines::throw_delayed_StackOverflowError_entry());
*/
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

  SET_ADDRESS(_stubs, StubRoutines::updateBytesCRC32());

  SET_ADDRESS(_stubs, StubRoutines::crc32c_table_addr());
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

#if defined(AMD64) && !defined(ZERO)
  SET_ADDRESS(_stubs, StubRoutines::x86::d2i_fixup());
  SET_ADDRESS(_stubs, StubRoutines::x86::f2i_fixup());
  SET_ADDRESS(_stubs, StubRoutines::x86::d2l_fixup());
  SET_ADDRESS(_stubs, StubRoutines::x86::f2l_fixup());
  SET_ADDRESS(_stubs, StubRoutines::x86::float_sign_mask());
  SET_ADDRESS(_stubs, StubRoutines::x86::float_sign_flip());
  SET_ADDRESS(_stubs, StubRoutines::x86::double_sign_mask());
  SET_ADDRESS(_stubs, StubRoutines::x86::double_sign_flip());
  SET_ADDRESS(_stubs, StubRoutines::x86::vector_popcount_lut());
  SET_ADDRESS(_stubs, StubRoutines::x86::vector_float_sign_mask());
  SET_ADDRESS(_stubs, StubRoutines::x86::vector_float_sign_flip());
  SET_ADDRESS(_stubs, StubRoutines::x86::vector_double_sign_mask());
  SET_ADDRESS(_stubs, StubRoutines::x86::vector_double_sign_flip());
  // The iota indices are ordered by type B/S/I/L/F/D, and the offset between two types is 64.
  // See C2_MacroAssembler::load_iota_indices().
  for (int i = 0; i < 6; i++) {
    SET_ADDRESS(_stubs, StubRoutines::x86::vector_iota_indices() + i * 64);
  }
#endif
#if defined(AARCH64) && !defined(ZERO)
  SET_ADDRESS(_stubs, StubRoutines::aarch64::d2i_fixup());
  SET_ADDRESS(_stubs, StubRoutines::aarch64::f2i_fixup());
  SET_ADDRESS(_stubs, StubRoutines::aarch64::d2l_fixup());
  SET_ADDRESS(_stubs, StubRoutines::aarch64::f2l_fixup());
  SET_ADDRESS(_stubs, StubRoutines::aarch64::float_sign_mask());
  SET_ADDRESS(_stubs, StubRoutines::aarch64::float_sign_flip());
  SET_ADDRESS(_stubs, StubRoutines::aarch64::double_sign_mask());
  SET_ADDRESS(_stubs, StubRoutines::aarch64::double_sign_flip());
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
#endif

  // Blobs
  SET_ADDRESS(_blobs, SharedRuntime::get_handle_wrong_method_stub());
  SET_ADDRESS(_blobs, SharedRuntime::get_ic_miss_stub());
  SET_ADDRESS(_blobs, SharedRuntime::get_resolve_opt_virtual_call_stub());
  SET_ADDRESS(_blobs, SharedRuntime::get_resolve_virtual_call_stub());
  SET_ADDRESS(_blobs, SharedRuntime::get_resolve_static_call_stub());
  SET_ADDRESS(_blobs, SharedRuntime::deopt_blob()->entry_point());
  SET_ADDRESS(_blobs, SharedRuntime::polling_page_safepoint_handler_blob()->entry_point());
  SET_ADDRESS(_blobs, SharedRuntime::polling_page_return_handler_blob()->entry_point());
#ifdef COMPILER2
  SET_ADDRESS(_blobs, SharedRuntime::polling_page_vectors_safepoint_handler_blob()->entry_point());
#endif

  SET_ADDRESS(_blobs, SharedRuntime::throw_AbstractMethodError_entry());
  SET_ADDRESS(_blobs, SharedRuntime::throw_IncompatibleClassChangeError_entry());
  SET_ADDRESS(_blobs, SharedRuntime::throw_NullPointerException_at_call_entry());
  SET_ADDRESS(_blobs, SharedRuntime::throw_StackOverflowError_entry());
  SET_ADDRESS(_blobs, SharedRuntime::throw_delayed_StackOverflowError_entry());

  assert(_blobs_length <= _shared_blobs_max, "increase _shared_blobs_max to %d", _blobs_length);
  _final_blobs_length = _blobs_length;
  _complete = true;
  log_info(scc,init)("External addresses and stubs recorded");
}

void SCAddressTable::init_opto() {
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
  _final_blobs_length = MAX2(_final_blobs_length, (_shared_blobs_max + _C2_blobs_length));
  _opto_complete = true;
  log_info(scc,init)("OptoRuntime Blobs recorded");
}

void SCAddressTable::init_c1() {
#ifdef COMPILER1
  // Runtime1 Blobs
  for (int i = 0; i < (int)(C1StubId::NUM_STUBIDS); i++) {
    C1StubId id = (C1StubId)i;
    if (Runtime1::blob_for(id) == nullptr) {
      log_info(scc, init)("C1 blob %s is missing", Runtime1::name_for(id));
      continue;
    }
    if (Runtime1::entry_for(id) == nullptr) {
      log_info(scc, init)("C1 blob %s is missing entry", Runtime1::name_for(id));
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
  _final_blobs_length = MAX2(_final_blobs_length, (_shared_blobs_max + _C2_blobs_max + _C1_blobs_length));
  _c1_complete = true;
  log_info(scc,init)("Runtime1 Blobs recorded");
}

#undef SET_ADDRESS
#undef _extrs_max
#undef _stubs_max
#undef _blobs_max
#undef _shared_blobs_max
#undef _C1_blobs_max
#undef _C2_blobs_max

SCAddressTable::~SCAddressTable() {
  if (_extrs_addr != nullptr) {
    FREE_C_HEAP_ARRAY(address, _extrs_addr);
  }
  if (_stubs_addr != nullptr) {
    FREE_C_HEAP_ARRAY(address, _stubs_addr);
  }
  if (_blobs_addr != nullptr) {
    FREE_C_HEAP_ARRAY(address, _blobs_addr);
  }
}

#define MAX_STR_COUNT 200
static const char* _C_strings[MAX_STR_COUNT] = {nullptr};
static int _C_strings_count = 0;
static int _C_strings_s[MAX_STR_COUNT] = {0};
static int _C_strings_id[MAX_STR_COUNT] = {0};
static int _C_strings_len[MAX_STR_COUNT] = {0};
static int _C_strings_hash[MAX_STR_COUNT] = {0};
static int _C_strings_used = 0;

void SCCache::load_strings() {
  uint strings_count  = _load_header->strings_count();
  if (strings_count == 0) {
    return;
  }
  uint strings_offset = _load_header->strings_offset();
  uint strings_size   = _load_header->entries_offset() - strings_offset;
  uint data_size = (uint)(strings_count * sizeof(uint));
  uint* sizes = (uint*)addr(strings_offset);
  uint* hashs = (uint*)addr(strings_offset + data_size);
  strings_size -= 2 * data_size;
  // We have to keep cached strings longer than _cache buffer
  // because they are refernced from compiled code which may
  // still be executed on VM exit after _cache is freed.
  char* p = NEW_C_HEAP_ARRAY(char, strings_size+1, mtCode);
  memcpy(p, addr(strings_offset + 2 * data_size), strings_size);
  _C_strings_buf = p;
  assert(strings_count <= MAX_STR_COUNT, "sanity");
  for (uint i = 0; i < strings_count; i++) {
    _C_strings[i] = p;
    uint len = sizes[i];
    _C_strings_s[i] = i;
    _C_strings_id[i] = i;
    _C_strings_len[i] = len;
    _C_strings_hash[i] = hashs[i];
    p += len;
  }
  assert((uint)(p - _C_strings_buf) <= strings_size, "(" INTPTR_FORMAT " - " INTPTR_FORMAT ") = %d > %d ", p2i(p), p2i(_C_strings_buf), (uint)(p - _C_strings_buf), strings_size);
  _C_strings_count = strings_count;
  _C_strings_used  = strings_count;
  log_info(scc, init)("Load %d C strings at offset %d from Startup Code Cache '%s'", _C_strings_count, strings_offset, _cache_path);
}

int SCCache::store_strings() {
  uint offset = _write_position;
  uint length = 0;
  if (_C_strings_used > 0) {
    // Write sizes first
    for (int i = 0; i < _C_strings_used; i++) {
      uint len = _C_strings_len[i] + 1; // Include 0
      length += len;
      assert(len < 1000, "big string: %s", _C_strings[i]);
      uint n = write_bytes(&len, sizeof(uint));
      if (n != sizeof(uint)) {
        return -1;
      }
    }
    // Write hashs
    for (int i = 0; i < _C_strings_used; i++) {
      uint n = write_bytes(&(_C_strings_hash[i]), sizeof(uint));
      if (n != sizeof(uint)) {
        return -1;
      }
    }
    for (int i = 0; i < _C_strings_used; i++) {
      uint len = _C_strings_len[i] + 1; // Include 0
      uint n = write_bytes(_C_strings[_C_strings_s[i]], len);
      if (n != len) {
        return -1;
      }
    }
    log_info(scc, exit)("Wrote %d C strings of total length %d at offset %d to Startup Code Cache '%s'",
                        _C_strings_used, length, offset, _cache_path);
  }
  return _C_strings_used;
}

void SCCache::add_new_C_string(const char* str) {
  assert(for_write(), "only when storing code");
  _table->add_C_string(str);
}

void SCAddressTable::add_C_string(const char* str) {
  if (str != nullptr && _complete && (_opto_complete || _c1_complete)) {
    // Check previous strings address
    for (int i = 0; i < _C_strings_count; i++) {
      if (_C_strings[i] == str) {
        return; // Found existing one
      }
    }
    // Add new one
    if (_C_strings_count < MAX_STR_COUNT) {
      log_trace(scc)("add_C_string: [%d] " INTPTR_FORMAT " %s", _C_strings_count, p2i(str), str);
      _C_strings_id[_C_strings_count] = -1; // Init
      _C_strings[_C_strings_count++] = str;
    } else {
      CompileTask* task = ciEnv::current()->task();
      log_info(scc)("%d (L%d): Number of C strings > max %d %s",
                       task->compile_id(), task->comp_level(), MAX_STR_COUNT, str);
    }
  }
}

int SCAddressTable::id_for_C_string(address str) {
  for (int i = 0; i < _C_strings_count; i++) {
    if (_C_strings[i] == (const char*)str) { // found
      int id = _C_strings_id[i];
      if (id >= 0) {
        assert(id < _C_strings_used, "%d >= %d", id , _C_strings_used);
        return id; // Found recorded
      }
      // Search for the same string content
      int len = (int)strlen((const char*)str);
      int hash = java_lang_String::hash_code((const jbyte*)str, len);
      for (int j = 0; j < _C_strings_used; j++) {
        if ((_C_strings_len[j] == len) && (_C_strings_hash[j] == hash)) {
          _C_strings_id[i] = j; // Found match
          return j;
        }
      }
      // Not found in recorded, add new
      id = _C_strings_used++;
      _C_strings_s[id] = i;
      _C_strings_id[i] = id;
      _C_strings_len[id] = len;
      _C_strings_hash[id] = hash;
      return id;
    }
  }
  return -1;
}

address SCAddressTable::address_for_C_string(int idx) {
  assert(idx < _C_strings_count, "sanity");
  return (address)_C_strings[idx];
}

int search_address(address addr, address* table, uint length) {
  for (int i = 0; i < (int)length; i++) {
    if (table[i] == addr) {
      return i;
    }
  }
  return -1;
}

address SCAddressTable::address_for_id(int idx) {
  if (!_complete) {
    fatal("SCA table is not complete");
  }
  if (idx == -1) {
    return (address)-1;
  }
  uint id = (uint)idx;
  if (id >= _all_max && idx < (_all_max + _C_strings_count)) {
    return address_for_C_string(idx - _all_max);
  }
  if (idx < 0 || id == (_extrs_length + _stubs_length + _final_blobs_length)) {
    fatal("Incorrect id %d for SCA table", id);
  }
  if (idx > (_all_max + _C_strings_count)) {
    return (address)os::init + idx;
  }
  if (id < _extrs_length) {
    return _extrs_addr[id];
  }
  id -= _extrs_length;
  if (id < _stubs_length) {
    return _stubs_addr[id];
  }
  id -= _stubs_length;
  if (id < _final_blobs_length) {
    return _blobs_addr[id];
  }
  return nullptr;
}

int SCAddressTable::id_for_address(address addr, RelocIterator reloc, CodeBuffer* buffer) {
  int id = -1;
  if (addr == (address)-1) { // Static call stub has jump to itself
    return id;
  }
  if (!_complete) {
    fatal("SCA table is not complete");
  }
  // Seach for C string
  id = id_for_C_string(addr);
  if (id >=0) {
    return id + _all_max;
  }
  if (StubRoutines::contains(addr)) {
    // Search in stubs
    id = search_address(addr, _stubs_addr, _stubs_length);
    if (id < 0) {
      StubCodeDesc* desc = StubCodeDesc::desc_for(addr);
      if (desc == nullptr) {
        desc = StubCodeDesc::desc_for(addr + frame::pc_return_offset);
      }
      const char* sub_name = (desc != nullptr) ? desc->name() : "<unknown>";
      fatal("Address " INTPTR_FORMAT " for Stub:%s is missing in SCA table", p2i(addr), sub_name);
    } else {
      id += _extrs_length;
    }
  } else {
    CodeBlob* cb = CodeCache::find_blob(addr);
    if (cb != nullptr) {
      // Search in code blobs
      id = search_address(addr, _blobs_addr, _final_blobs_length);
      if (id < 0) {
        fatal("Address " INTPTR_FORMAT " for Blob:%s is missing in SCA table", p2i(addr), cb->name());
      } else {
        id += _extrs_length + _stubs_length;
      }
    } else {
      // Search in runtime functions
      id = search_address(addr, _extrs_addr, _extrs_length);
      if (id < 0) {
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
            log_info(scc)("%d (L%d): Address " INTPTR_FORMAT " (offset %d) for runtime target '%s' is missing in SCA table",
                          compile_id, comp_level, p2i(addr), dist, (const char*)addr);
            assert(dist > (uint)(_all_max + MAX_STR_COUNT), "change encoding of distance");
            return dist;
          }
          fatal("Address " INTPTR_FORMAT " for runtime target '%s+%d' is missing in SCA table", p2i(addr), func_name, offset);
        } else {
          os::print_location(tty, p2i(addr), true);
          reloc.print_current_on(tty);
#ifndef PRODUCT
          buffer->print_on(tty);
          buffer->decode();
#endif // !PRODUCT
          fatal("Address " INTPTR_FORMAT " for <unknown> is missing in SCA table", p2i(addr));
        }
      }
    }
  }
  return id;
}

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


void SCCache::wait_for_no_nmethod_readers() {
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

SCCache::ReadingMark::ReadingMark() {
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

SCCache::ReadingMark::~ReadingMark() {
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
