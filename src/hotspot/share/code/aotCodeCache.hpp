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

#ifndef SHARE_CODE_AOTCODECACHE_HPP
#define SHARE_CODE_AOTCODECACHE_HPP

#include "compiler/compilerDefinitions.hpp"
#include "memory/allocation.hpp"
#include "nmt/memTag.hpp"
#include "oops/oopsHierarchy.hpp"
#include "utilities/exceptions.hpp"

/*
 * AOT Code Cache collects code from Code Cache and corresponding metadata
 * during application training run.
 * In following "production" runs this code and data can be loaded into
 * Code Cache skipping its generation.
 * Additionaly special compiled code "preload" is generated with class initialization
 * barriers which can be called on first Java method invocation.
 */

class AbstractCompiler;
class AOTCodeCache;
class AsmRemarks;
class ciConstant;
class ciEnv;
class ciMethod;
class CodeBlob;
class CodeOffsets;
class CompileTask;
class DbgStrings;
class DebugInformationRecorder;
class Dependencies;
class ExceptionTable;
class ExceptionHandlerTable;
template<typename E>
class GrowableArray;
class ImmutableOopMapSet;
class ImplicitExceptionTable;
class JavaThread;
class Klass;
class methodHandle;
class Metadata;
class Method;
class nmethod;
class OopMapSet;
class OopRecorder;
class outputStream;
class RelocIterator;
class StubCodeGenerator;

enum class vmIntrinsicID : int;

#define DO_AOTCODEENTRY_KIND(Fn) \
  Fn(None) \
  Fn(Adapter) \
  Fn(Stub) \
  Fn(SharedBlob) \
  Fn(C1Blob) \
  Fn(C2Blob) \
  Fn(Code) \

// Descriptor of AOT Code Cache's entry
class AOTCodeEntry {
public:
  enum Kind : s1 {
#define DECL_KIND_ENUM(kind) kind,
    DO_AOTCODEENTRY_KIND(DECL_KIND_ENUM)
#undef DECL_KIND_ENUM
    Kind_count
  };

private:
  AOTCodeEntry* _next;
  Method*       _method;
  uint   _method_offset;
  Kind   _kind;
  uint   _id;          // Adapter's id, vmIntrinsic::ID for stub or name's hash for nmethod
  uint   _offset;      // Offset to entry
  uint   _size;        // Entry size
  uint   _name_offset; // Method's or intrinsic name
  uint   _name_size;
  uint   _num_inlined_bytecodes;

  uint   _code_offset; // Start of code in cache
  uint   _code_size;   // Total size of all code sections

  uint   _comp_level;  // compilation level
  uint   _comp_id;     // compilation id
  uint   _decompile;   // Decompile count for this nmethod
  bool   _has_oop_maps;
  bool   _has_clinit_barriers; // Generated code has class init checks
  bool   _for_preload; // Code can be used for preload
  bool   _loaded;      // Code was loaded
  bool   _not_entrant; // Deoptimized
  bool   _load_fail;   // Failed to load due to some klass state
  bool   _ignore_decompile; // ignore decompile counter if compilation is done
                            // during "assembly" phase without running application
  address _dumptime_content_start_addr; // CodeBlob::content_begin() at dump time; used for applying relocations

public:
  // this constructor is used only by AOTCodeEntry::Stub
  AOTCodeEntry(uint offset, uint size, uint name_offset, uint name_size,
               uint code_offset, uint code_size,
               Kind kind, uint id) {
    assert(kind == AOTCodeEntry::Stub, "sanity check");
    _next         = nullptr;
    _method       = nullptr;
    _kind         = kind;
    _id           = id;
    _offset       = offset;
    _size         = size;
    _name_offset  = name_offset;
    _name_size    = name_size;
    _code_offset  = code_offset;
    _code_size    = code_size;

    _dumptime_content_start_addr = nullptr;
    _num_inlined_bytecodes = 0;
    _comp_level   = 0;
    _comp_id      = 0;
    _decompile    = 0;
    _has_oop_maps = false; // unused here
    _has_clinit_barriers = false;
    _for_preload  = false;
    _loaded       = false;
    _not_entrant  = false;
    _load_fail    = false;
    _ignore_decompile = true;
  }

  AOTCodeEntry(Kind kind,         uint id,
               uint offset,       uint size,
               uint name_offset,  uint name_size,
               uint blob_offset,  bool has_oop_maps,
               address dumptime_content_start_addr,
               uint comp_level = 0,
               uint comp_id = 0, uint decomp = 0,
               bool has_clinit_barriers = false,
               bool for_preload = false,
               bool ignore_decompile = false) {
    _next         = nullptr;
    _method       = nullptr;
    _kind         = kind;
    _id           = id;
    _offset       = offset;
    _size         = size;
    _name_offset  = name_offset;
    _name_size    = name_size;
    _code_offset  = blob_offset;
    _code_size    = 0; // unused

    _dumptime_content_start_addr = dumptime_content_start_addr;
    _num_inlined_bytecodes = 0;

    _comp_level   = comp_level;
    _comp_id      = comp_id;
    _decompile    = decomp;
    _has_oop_maps = has_oop_maps;
    _has_clinit_barriers = has_clinit_barriers;
    _for_preload  = for_preload;
    _loaded       = false;
    _not_entrant  = false;
    _load_fail    = false;

    _loaded       = false;
    _not_entrant  = false;
    _load_fail    = false;
    _ignore_decompile = ignore_decompile;
  }

  void* operator new(size_t x, AOTCodeCache* cache);
  // Delete is a NOP
  void operator delete( void *ptr ) {}

  AOTCodeEntry* next()        const { return _next; }
  void set_next(AOTCodeEntry* next) { _next = next; }

  Method*   method()  const { return _method; }
  void set_method(Method* method) { _method = method; }
  void update_method_for_writing();
  uint method_offset() const { return _method_offset; }

  Kind kind()         const { return _kind; }
  uint id()           const { return _id; }

  uint offset()       const { return _offset; }
  void set_offset(uint off) { _offset = off; }

  uint size()         const { return _size; }
  uint name_offset()  const { return _name_offset; }
  uint name_size()    const { return _name_size; }
  uint code_offset()  const { return _code_offset; }
  uint code_size()    const { return _code_size; }

  bool has_oop_maps() const { return _has_oop_maps; }
  address dumptime_content_start_addr() const { return _dumptime_content_start_addr; }
  uint num_inlined_bytecodes() const { return _num_inlined_bytecodes; }
  void set_inlined_bytecodes(int bytes) { _num_inlined_bytecodes = bytes; }

  uint comp_level()   const { return _comp_level; }
  uint comp_id()      const { return _comp_id; }

  uint decompile()    const { return _decompile; }
  bool has_clinit_barriers() const { return _has_clinit_barriers; }
  bool for_preload()  const { return _for_preload; }
  bool is_loaded()    const { return _loaded; }
  void set_loaded()         { _loaded = true; }
  bool ignore_decompile() const { return _ignore_decompile; }

  bool not_entrant()  const { return _not_entrant; }
  void set_not_entrant()    { _not_entrant = true; }
  void set_entrant()        { _not_entrant = false; }

  bool load_fail()  const { return _load_fail; }
  void set_load_fail()    { _load_fail = true; }

  void print(outputStream* st) const;

  static bool is_valid_entry_kind(Kind kind) { return kind > None && kind < Kind_count; }
  static bool is_blob(Kind kind) { return kind == SharedBlob || kind == C1Blob || kind == C2Blob; }
  static bool is_adapter(Kind kind) { return kind == Adapter; }
  bool is_code()  { return _kind == Code; }
};

// Addresses of stubs, blobs and runtime finctions called from compiled code.
class AOTCodeAddressTable : public CHeapObj<mtCode> {
private:
  address* _extrs_addr;
  address* _stubs_addr;
  address* _shared_blobs_addr;
  address* _C1_blobs_addr;
  address* _C2_blobs_addr;
  uint     _extrs_length;
  uint     _stubs_length;
  uint     _shared_blobs_length;
  uint     _C1_blobs_length;
  uint     _C2_blobs_length;

  bool _extrs_complete;
  bool _early_stubs_complete;
  bool _shared_blobs_complete;
  bool _early_c1_complete;
  bool _c1_complete;
  bool _c2_complete;
  bool _complete;

public:
  AOTCodeAddressTable() :
    _extrs_addr(nullptr),
    _shared_blobs_addr(nullptr),
    _C1_blobs_addr(nullptr),
    _C2_blobs_addr(nullptr),
    _extrs_length(0),
    _stubs_length(0),
    _shared_blobs_length(0),
    _C1_blobs_length(0),
    _C2_blobs_length(0),
    _extrs_complete(false),
    _early_stubs_complete(false),
    _shared_blobs_complete(false),
    _early_c1_complete(false),
    _c1_complete(false),
    _c2_complete(false),
    _complete(false)
  { }
  ~AOTCodeAddressTable();
  void init_extrs();
  void init_early_stubs();
  void init_shared_blobs();
  void init_stubs();
  void init_early_c1();
  void init_c1();
  void init_c2();
  const char* add_C_string(const char* str);
  int  id_for_C_string(address str);
  address address_for_C_string(int idx);
  int  id_for_address(address addr, RelocIterator iter, CodeBlob* blob);
  address address_for_id(int id);
  bool c2_complete() const { return _c2_complete; }
  bool c1_complete() const { return _c1_complete; }
};

struct AOTCodeSection {
public:
  address _origin_address;
  uint _size;
  uint _offset;
};

enum class DataKind: int {
  No_Data   = -1,
  Null      = 0,
  Klass     = 1,
  Method    = 2,
  String    = 3,
  Primitive = 4, // primitive Class object
  SysLoader = 5, // java_system_loader
  PlaLoader = 6, // java_platform_loader
  MethodCnts= 7,
  Klass_Shared  = 8,
  Method_Shared = 9,
  String_Shared = 10,
  MH_Oop_Shared = 11
};

class AOTCodeCache : public CHeapObj<mtCode> {

// Classes used to describe AOT code cache.
protected:
  class Config {
    address _compressedOopBase;
    address _compressedKlassBase;
    uint _compressedOopShift;
    uint _compressedKlassShift;
    uint _contendedPaddingWidth;
    uint _objectAlignment;
    uint _gc;
    enum Flags {
      none                     = 0,
      debugVM                  = 2,
      compressedOops           = 4,
      compressedClassPointers  = 8,
      useTLAB                  = 16,
      systemClassAssertions    = 32,
      userClassAssertions      = 64,
      enableContendedPadding   = 128,
      restrictContendedPadding = 256,
    };
    uint _flags;

  public:
    void record();
    bool verify() const;
  };

  class Header : public CHeapObj<mtCode> {
  private:
    // Here should be version and other verification fields
    enum {
      AOT_CODE_VERSION = 1
    };
    uint   _version;         // AOT code version (should match when reading code cache)
    uint   _cache_size;      // cache size in bytes
    uint   _strings_count;   // number of recorded C strings
    uint   _strings_offset;  // offset to recorded C strings
    uint   _entries_count;   // number of recorded entries
    uint   _entries_offset;  // offset of AOTCodeEntry array describing entries
    uint   _preload_entries_count; // entries for pre-loading code
    uint   _preload_entries_offset;
    uint   _adapters_count;
    uint   _shared_blobs_count;
    uint   _C1_blobs_count;
    uint   _C2_blobs_count;
    uint   _stubs_count;
    Config _config;

  public:
    void init(uint cache_size,
              uint strings_count,  uint strings_offset,
              uint entries_count,  uint entries_offset,
              uint preload_entries_count, uint preload_entries_offset,
              uint adapters_count, uint shared_blobs_count,
              uint C1_blobs_count, uint C2_blobs_count, uint stubs_count) {
      _version        = AOT_CODE_VERSION;
      _cache_size     = cache_size;
      _strings_count  = strings_count;
      _strings_offset = strings_offset;
      _entries_count  = entries_count;
      _entries_offset = entries_offset;
      _preload_entries_count  = preload_entries_count;
      _preload_entries_offset = preload_entries_offset;
      _adapters_count = adapters_count;
      _shared_blobs_count = shared_blobs_count;
      _C1_blobs_count = C1_blobs_count;
      _C2_blobs_count = C2_blobs_count;
      _stubs_count    = stubs_count;

      _config.record();
    }

    uint cache_size()     const { return _cache_size; }
    uint strings_count()  const { return _strings_count; }
    uint strings_offset() const { return _strings_offset; }
    uint entries_count()  const { return _entries_count; }
    uint entries_offset() const { return _entries_offset; }
    uint preload_entries_count()  const { return _preload_entries_count; }
    uint preload_entries_offset() const { return _preload_entries_offset; }
    uint adapters_count() const { return _adapters_count; }
    uint shared_blobs_count()    const { return _shared_blobs_count; }
    uint C1_blobs_count() const { return _C1_blobs_count; }
    uint C2_blobs_count() const { return _C2_blobs_count; }
    uint stubs_count()    const { return _stubs_count; }
    uint nmethods_count() const { return _entries_count
                                       - _stubs_count
                                       - _shared_blobs_count
                                       - _C1_blobs_count
                                       - _C2_blobs_count
                                       - _adapters_count; }

    bool verify_config(uint load_size)  const;
    bool verify_vm_config() const { // Called after Universe initialized
      return _config.verify();
    }
  };

// Continue with AOTCodeCache class definition.
private:
  Header* _load_header;
  char*   _load_buffer;    // Aligned buffer for loading cached code
  char*   _store_buffer;   // Aligned buffer for storing cached code
  char*   _C_store_buffer; // Original unaligned buffer

  uint   _write_position;  // Position in _store_buffer
  uint   _load_size;       // Used when reading cache
  uint   _store_size;      // Used when writing cache
  bool   _for_use;         // AOT cache is open for using AOT code
  bool   _for_dump;        // AOT cache is open for dumping AOT code
  bool   _closing;         // Closing cache file
  bool   _failed;          // Failed read/write to/from cache (cache is broken?)
  bool   _lookup_failed;   // Failed to lookup for info (skip only this code load)

  bool   _for_preload;         // Code for preload
  bool   _gen_preload_code;    // Generate pre-loading code
  bool   _has_clinit_barriers; // Code with clinit barriers

  AOTCodeAddressTable* _table;

  AOTCodeEntry* _load_entries;   // Used when reading cache
  uint*         _search_entries; // sorted by ID table [id, index]
  AOTCodeEntry* _store_entries;  // Used when writing cache
  const char*   _C_strings_buf;  // Loaded buffer for _C_strings[] table
  uint          _store_entries_cnt;

  uint _compile_id;
  uint _comp_level;
  uint compile_id() const { return _compile_id; }
  uint comp_level() const { return _comp_level; }

  static AOTCodeCache* open_for_use();
  static AOTCodeCache* open_for_dump();

  bool set_write_position(uint pos);
  bool align_write();

  address reserve_bytes(uint nbytes);
  uint write_bytes(const void* buffer, uint nbytes);
  const char* addr(uint offset) const { return _load_buffer + offset; }
  static AOTCodeAddressTable* addr_table() {
    return is_on() && (cache()->_table != nullptr) ? cache()->_table : nullptr;
  }

  void set_lookup_failed()     { _lookup_failed = true; }
  void clear_lookup_failed()   { _lookup_failed = false; }
  bool lookup_failed()   const { return _lookup_failed; }

  AOTCodeEntry* write_nmethod(nmethod* nm, bool for_preload);

  // States:
  //   S >= 0: allow new readers, S readers are currently active
  //   S <  0: no new readers are allowed; (-S-1) readers are currently active
  //     (special case: S = -1 means no readers are active, and would never be active again)
  static volatile int _nmethod_readers;

  static void wait_for_no_nmethod_readers();

  class ReadingMark {
  private:
    bool _failed;
  public:
    ReadingMark();
    ~ReadingMark();
    bool failed() {
      return _failed;
    }
  };

public:
  AOTCodeCache(bool is_dumping, bool is_using);
  ~AOTCodeCache();

  const char* cache_buffer() const { return _load_buffer; }
  bool failed() const { return _failed; }
  void set_failed()   { _failed = true; }

  static bool is_address_in_aot_cache(address p) NOT_CDS_RETURN_(false);
  static uint max_aot_code_size();

  uint load_size() const { return _load_size; }
  uint write_position() const { return _write_position; }

  void load_strings();
  int store_strings();

  static void init_extrs_table() NOT_CDS_RETURN;
  static void init_early_stubs_table() NOT_CDS_RETURN;
  static void init_shared_blobs_table() NOT_CDS_RETURN;
  static void init_stubs_table() NOT_CDS_RETURN;
  static void init_early_c1_table() NOT_CDS_RETURN;
  static void init_c1_table() NOT_CDS_RETURN;
  static void init_c2_table() NOT_CDS_RETURN;

  address address_for_C_string(int idx) const { return _table->address_for_C_string(idx); }
  address address_for_id(int id) const { return _table->address_for_id(id); }

  bool for_use()  const { return _for_use  && !_failed; }
  bool for_dump() const { return _for_dump && !_failed; }

  bool closing()          const { return _closing; }
  bool gen_preload_code() const { return _gen_preload_code; }

  AOTCodeEntry* add_entry() {
    _store_entries_cnt++;
    _store_entries -= 1;
    return _store_entries;
  }
  void preload_startup_code(TRAPS);

  AOTCodeEntry* find_entry(AOTCodeEntry::Kind kind, uint id, uint comp_level = 0, uint decomp = 0);
  void invalidate_entry(AOTCodeEntry* entry);

  bool finish_write();

  void log_stats_on_exit();

  static bool load_stub(StubCodeGenerator* cgen, vmIntrinsicID id, const char* name, address start) NOT_CDS_RETURN_(false);
  static bool store_stub(StubCodeGenerator* cgen, vmIntrinsicID id, const char* name, address start) NOT_CDS_RETURN_(false);

  bool write_klass(Klass* klass);
  bool write_method(Method* method);

  bool write_relocations(CodeBlob& code_blob, GrowableArray<Handle>* oop_list = nullptr, GrowableArray<Metadata*>* metadata_list = nullptr);

  bool write_oop_map_set(CodeBlob& cb);
  bool write_nmethod_reloc_immediates(GrowableArray<Handle>& oop_list, GrowableArray<Metadata*>& metadata_list);

  jobject read_oop(JavaThread* thread, const methodHandle& comp_method);
  Metadata* read_metadata(const methodHandle& comp_method);

  bool write_oop(jobject& jo);
  bool write_oop(oop obj);
  bool write_metadata(Metadata* m);
  bool write_oops(nmethod* nm);
  bool write_metadata(nmethod* nm);

#ifndef PRODUCT
  bool write_asm_remarks(AsmRemarks& asm_remarks, bool use_string_table);
  bool write_dbg_strings(DbgStrings& dbg_strings, bool use_string_table);
#endif // PRODUCT

  static bool store_code_blob(CodeBlob& blob,
                              AOTCodeEntry::Kind entry_kind,
                              uint id, const char* name,
                              int entry_offset_count = 0,
                              int* entry_offsets = nullptr) NOT_CDS_RETURN_(false);

  static CodeBlob* load_code_blob(AOTCodeEntry::Kind kind,
                                  uint id, const char* name,
                                  int entry_offset_count = 0,
                                  int* entry_offsets = nullptr) NOT_CDS_RETURN_(nullptr);

  static bool load_nmethod(ciEnv* env, ciMethod* target, int entry_bci, AbstractCompiler* compiler, CompLevel comp_level) NOT_CDS_RETURN_(false);
  static AOTCodeEntry* store_nmethod(nmethod* nm, AbstractCompiler* compiler, bool for_preload) NOT_CDS_RETURN_(nullptr);

  static uint store_entries_cnt() {
    if (is_on_for_dump()) {
      return cache()->_store_entries_cnt;
    }
    return -1;
  }

// Static access

private:
  static AOTCodeCache*  _cache;

  static bool open_cache(bool is_dumping, bool is_using);
  static bool verify_vm_config() {
    if (is_on_for_use()) {
      return _cache->_load_header->verify_vm_config();
    }
    return true;
  }
public:
  static AOTCodeCache* cache() { return _cache; }
  static void initialize() NOT_CDS_RETURN;
  static void init2() NOT_CDS_RETURN;
  static void close() NOT_CDS_RETURN;
  static bool is_on() CDS_ONLY({ return _cache != nullptr && !_cache->closing(); }) NOT_CDS_RETURN_(false);
  static bool is_C3_on() NOT_CDS_RETURN_(false);
  static bool is_code_load_thread_on() NOT_CDS_RETURN_(false);
  static bool is_on_for_use()  CDS_ONLY({ return is_on() && _cache->for_use(); }) NOT_CDS_RETURN_(false);
  static bool is_on_for_dump() CDS_ONLY({ return is_on() && _cache->for_dump(); }) NOT_CDS_RETURN_(false);
  static bool is_dumping_code() NOT_CDS_RETURN_(false);
  static bool is_dumping_stub() NOT_CDS_RETURN_(false);
  static bool is_dumping_adapter() NOT_CDS_RETURN_(false);
  static bool is_using_code() NOT_CDS_RETURN_(false);
  static bool is_using_stub() NOT_CDS_RETURN_(false);
  static bool is_using_adapter() NOT_CDS_RETURN_(false);
  static void enable_caching() NOT_CDS_RETURN;
  static void disable_caching() NOT_CDS_RETURN;
  static bool is_caching_enabled() NOT_CDS_RETURN_(false);

  static bool gen_preload_code(ciMethod* m, int entry_bci) NOT_CDS_RETURN_(false);
  static bool allow_const_field(ciConstant& value) NOT_CDS_RETURN_(false);
  static void invalidate(AOTCodeEntry* entry) NOT_CDS_RETURN;
  static bool is_loaded(AOTCodeEntry* entry);
  static AOTCodeEntry* find_code_entry(const methodHandle& method, uint comp_level);
  static void preload_code(JavaThread* thread) NOT_CDS_RETURN;

  template<typename Function>
  static void iterate(Function function) { // lambda enabled API
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

      for (uint i = 0; i < count; i++) {
        int index = search_entries[2*i + 1];
        AOTCodeEntry* entry = &(load_entries[index]);
        function(entry);
      }
    }
  }

  static const char* add_C_string(const char* str) NOT_CDS_RETURN_(str);

  static void print_on(outputStream* st) NOT_CDS_RETURN;
  static void print_statistics_on(outputStream* st) NOT_CDS_RETURN;
  static void print_timers_on(outputStream* st) NOT_CDS_RETURN;
  static void print_unused_entries_on(outputStream* st) NOT_CDS_RETURN;
};

// Concurent AOT code reader
class AOTCodeReader {
private:
  const AOTCodeCache*  _cache;
  const AOTCodeEntry*  _entry;
  const char*          _load_buffer; // Loaded cached code buffer
  uint  _read_position;              // Position in _load_buffer
  uint  read_position() const { return _read_position; }
  void  set_read_position(uint pos);
  const char* addr(uint offset) const { return _load_buffer + offset; }

  uint _compile_id;
  uint _comp_level;
  uint compile_id() const { return _compile_id; }
  uint comp_level() const { return _comp_level; }

  bool _preload;             // Preloading code before method execution
  bool _lookup_failed;       // Failed to lookup for info (skip only this code load)
  void set_lookup_failed()     { _lookup_failed = true; }
  void clear_lookup_failed()   { _lookup_failed = false; }
  bool lookup_failed()   const { return _lookup_failed; }

public:
  AOTCodeReader(AOTCodeCache* cache, AOTCodeEntry* entry, CompileTask* task);

  AOTCodeEntry* aot_code_entry() { return (AOTCodeEntry*)_entry; }

  // convenience method to convert offset in AOTCodeEntry data to its address
  bool compile_nmethod(ciEnv* env, ciMethod* target, AbstractCompiler* compiler);

  CodeBlob* compile_code_blob(const char* name, int entry_offset_count, int* entry_offsets);

  Klass* read_klass(const methodHandle& comp_method, bool shared);
  Method* read_method(const methodHandle& comp_method, bool shared);

  oop read_oop(JavaThread* thread, const methodHandle& comp_method);
  Metadata* read_metadata(const methodHandle& comp_method);
  bool read_oops(OopRecorder* oop_recorder, ciMethod* target);
  bool read_metadata(OopRecorder* oop_recorder, ciMethod* target);

  bool read_oop_metadata_list(JavaThread* thread, ciMethod* target, GrowableArray<Handle> &oop_list, GrowableArray<Metadata*> &metadata_list, OopRecorder* oop_recorder);
  void apply_relocations(nmethod* nm, GrowableArray<Handle> &oop_list, GrowableArray<Metadata*> &metadata_list) NOT_CDS_RETURN;

  ImmutableOopMapSet* read_oop_map_set();

  void fix_relocations(CodeBlob* code_blob, GrowableArray<Handle>* oop_list = nullptr, GrowableArray<Metadata*>* metadata_list = nullptr) NOT_CDS_RETURN;
#ifndef PRODUCT
  void read_asm_remarks(AsmRemarks& asm_remarks, bool use_string_table) NOT_CDS_RETURN;
  void read_dbg_strings(DbgStrings& dbg_strings, bool use_string_table) NOT_CDS_RETURN;
#endif // PRODUCT

  void print_on(outputStream* st);
};

// +1 for preload code
const int AOTCompLevel_count = CompLevel_count + 1; // 6 levels indexed from 0 to 5

struct AOTCodeStats {
private:
  struct {
    uint _kind_cnt[AOTCodeEntry::Kind_count];
    uint _nmethod_cnt[AOTCompLevel_count];
    uint _clinit_barriers_cnt;
  } ccstats; // ccstats = cached code stats

  void check_kind(uint kind) { assert(kind >= AOTCodeEntry::None && kind < AOTCodeEntry::Kind_count, "Invalid AOTCodeEntry kind %d", kind); }
  void check_complevel(uint lvl) { assert(lvl >= CompLevel_none && lvl < AOTCompLevel_count, "Invalid compilation level %d", lvl); }

public:
  void inc_entry_cnt(uint kind) { check_kind(kind); ccstats._kind_cnt[kind] += 1; }
  void inc_nmethod_cnt(uint lvl) { check_complevel(lvl); ccstats._nmethod_cnt[lvl] += 1; }
  void inc_preload_cnt() { ccstats._nmethod_cnt[AOTCompLevel_count-1] += 1; }
  void inc_clinit_barriers_cnt() { ccstats._clinit_barriers_cnt += 1; }

  void collect_entry_stats(AOTCodeEntry* entry) {
    inc_entry_cnt(entry->kind());
    if (entry->is_code()) {
      entry->for_preload() ? inc_nmethod_cnt(AOTCompLevel_count-1)
                           : inc_nmethod_cnt(entry->comp_level());
      if (entry->has_clinit_barriers()) {
        inc_clinit_barriers_cnt();
      }
    }
  }

  uint entry_count(uint kind) { check_kind(kind); return ccstats._kind_cnt[kind]; }
  uint nmethod_count(uint lvl) { check_complevel(lvl); return ccstats._nmethod_cnt[lvl]; }
  uint preload_count() { return ccstats._nmethod_cnt[AOTCompLevel_count-1]; }
  uint clinit_barriers_count() { return ccstats._clinit_barriers_cnt; }

  uint total_count() {
    uint total = 0;
    for (int kind = AOTCodeEntry::None; kind < AOTCodeEntry::Kind_count; kind++) {
      total += ccstats._kind_cnt[kind];
    }
    return total;
  }

  static AOTCodeStats add_aot_code_stats(AOTCodeStats stats1, AOTCodeStats stats2);

  // Runtime stats of the AOT code
private:
  struct {
    struct {
      uint _loaded_cnt;
      uint _invalidated_cnt;
      uint _load_failed_cnt;
    } _entry_kinds[AOTCodeEntry::Kind_count],
      _nmethods[AOTCompLevel_count];
  } rs; // rs = runtime stats

public:
  void inc_entry_loaded_cnt(uint kind) { check_kind(kind); rs._entry_kinds[kind]._loaded_cnt += 1; }
  void inc_entry_invalidated_cnt(uint kind) { check_kind(kind); rs._entry_kinds[kind]._invalidated_cnt += 1; }
  void inc_entry_load_failed_cnt(uint kind) { check_kind(kind); rs._entry_kinds[kind]._load_failed_cnt += 1; }

  void inc_nmethod_loaded_cnt(uint lvl) { check_complevel(lvl); rs._nmethods[lvl]._loaded_cnt += 1; }
  void inc_nmethod_invalidated_cnt(uint lvl) { check_complevel(lvl); rs._nmethods[lvl]._invalidated_cnt += 1; }
  void inc_nmethod_load_failed_cnt(uint lvl) { check_complevel(lvl); rs._nmethods[lvl]._load_failed_cnt += 1; }

  uint entry_loaded_count(uint kind) { check_kind(kind); return rs._entry_kinds[kind]._loaded_cnt; }
  uint entry_invalidated_count(uint kind) { check_kind(kind); return rs._entry_kinds[kind]._invalidated_cnt; }
  uint entry_load_failed_count(uint kind) { check_kind(kind); return rs._entry_kinds[kind]._load_failed_cnt; }

  uint nmethod_loaded_count(uint lvl) { check_complevel(lvl); return rs._nmethods[lvl]._loaded_cnt; }
  uint nmethod_invalidated_count(uint lvl) { check_complevel(lvl); return rs._nmethods[lvl]._invalidated_cnt; }
  uint nmethod_load_failed_count(uint lvl) { check_complevel(lvl); return rs._nmethods[lvl]._load_failed_cnt; }

  void inc_loaded_cnt(AOTCodeEntry* entry) {
    inc_entry_loaded_cnt(entry->kind());
    if (entry->is_code()) {
      entry->for_preload() ? inc_nmethod_loaded_cnt(AOTCompLevel_count-1)
                           : inc_nmethod_loaded_cnt(entry->comp_level());
    }
  }

  void inc_invalidated_cnt(AOTCodeEntry* entry) {
    inc_entry_invalidated_cnt(entry->kind());
    if (entry->is_code()) {
      entry->for_preload() ? inc_nmethod_invalidated_cnt(AOTCompLevel_count-1)
                           : inc_nmethod_invalidated_cnt(entry->comp_level());
    }
  }

  void inc_load_failed_cnt(AOTCodeEntry* entry) {
    inc_entry_load_failed_cnt(entry->kind());
    if (entry->is_code()) {
      entry->for_preload() ? inc_nmethod_load_failed_cnt(AOTCompLevel_count-1)
                           : inc_nmethod_load_failed_cnt(entry->comp_level());
    }
  }

  void collect_entry_runtime_stats(AOTCodeEntry* entry) {
    if (entry->is_loaded()) {
      inc_loaded_cnt(entry);
    }
    if (entry->not_entrant()) {
      inc_invalidated_cnt(entry);
    }
    if (entry->load_fail()) {
      inc_load_failed_cnt(entry);
    }
  }

  void collect_all_stats(AOTCodeEntry* entry) {
    collect_entry_stats(entry);
    collect_entry_runtime_stats(entry);
  }

  AOTCodeStats() {
    memset(this, 0, sizeof(AOTCodeStats));
  }
};

// code cache internal runtime constants area used by AOT code
class AOTRuntimeConstants {
 friend class AOTCodeCache;
 private:
  uint _grain_shift;
  uint _card_shift;
  static address _field_addresses_list[];
  static AOTRuntimeConstants _aot_runtime_constants;
  // private constructor for unique singleton
  AOTRuntimeConstants() { }
  // private for use by friend class AOTCodeCache
  static void initialize_from_runtime();
 public:
#if INCLUDE_CDS
  static bool contains(address adr) {
    address base = (address)&_aot_runtime_constants;
    address hi = base + sizeof(AOTRuntimeConstants);
    return (base <= adr && adr < hi);
  }
  static address grain_shift_address() { return (address)&_aot_runtime_constants._grain_shift; }
  static address card_shift_address() { return (address)&_aot_runtime_constants._card_shift; }
  static address* field_addresses_list() {
    return _field_addresses_list;
  }
#else
  static bool contains(address adr)      { return false; }
  static address grain_shift_address()   { return nullptr; }
  static address card_shift_address()    { return nullptr; }
  static address* field_addresses_list() { return nullptr; }
#endif
};

#endif // SHARE_CODE_AOTCODECACHE_HPP
