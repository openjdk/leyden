/*
 * Copyright (c) 2023, Oracle and/or its affiliates. All rights reserved.
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

#ifndef SHARE_CODE_SCCACHE_HPP
#define SHARE_CODE_SCCACHE_HPP

/*
 * Startup Code Cache (SCC) collects compiled code and metadata during
 * an application training runs.
 * In following "deployment" runs this code can me loaded into
 * Code Cache as normal nmethods skipping JIT compilation.
 * In additoin special compiled code is generated with class initialization
 * barriers which can be called on first Java method invocation.
 */
 
class AbstractCompiler;
class ciConstant;
class ciEnv;
class ciMethod;
class CodeBuffer;
class CodeOffsets;
class CompileTask;
class DebugInformationRecorder;
class Dependencies;
class ExceptionTable;
class ExceptionHandlerTable;
class ImplicitExceptionTable;
class JavaThread;
class methodHandle;
class Method;
class OopMapSet;
class OopRecorder;
class outputStream;
class RelocIterator;
class SCCache;
class StubCodeGenerator;

enum class vmIntrinsicID : int;
enum CompLevel : signed char;

class SCConfig {
  uint _compressedOopShift;
  uint _compressedKlassShift;
  uint _contendedPaddingWidth;
  uint _objectAlignment;
  uint _gc;
  enum Flags {
    none                     = 0,
    metadataPointers         = 1,
    debugVM                  = 2,
    compressedOops           = 4,
    compressedClassPointers  = 8,
    useTLAB                  = 16,
    systemClassAssertions    = 32,
    userClassAssertions      = 64,
    enableContendedPadding   = 128,
    restrictContendedPadding = 256,
    useEmptySlotsInSupers    = 512
  };
  uint _flags;

public:
  void record(bool use_meta_ptrs);
  bool verify(const char* cache_path) const;

  bool has_meta_ptrs()  const { return (_flags & metadataPointers) != 0; }
};

// Code Cache file header
class SCCHeader : public CHeapObj<mtCode> {
private:
  // Here should be version and other verification fields
  enum {
    SCC_VERSION = 1
  };
  uint _version;           // SCC version (should match when reading code cache)
  uint _jvm_version_offset;// JVM version string
  uint _cache_size;        // cache size in bytes
  uint _strings_count;
  uint _strings_offset;    // offset to recorded C strings
  uint _entries_count;     // number of recorded entries in cache
  uint _entries_offset;    // offset of SCCEntry array describing entries
  uint _preload_entries_count; // entries for pre-loading code
  uint _preload_entries_offset;
  SCConfig _config;

public:
  void init(uint jvm_version_offset, uint cache_size,
            uint strings_count, uint strings_offset,
            uint entries_count, uint entries_offset,
            uint preload_entries_count, uint preload_entries_offset,
            bool use_meta_ptrs) {
    _version        = SCC_VERSION;
     _jvm_version_offset = jvm_version_offset;
    _cache_size     = cache_size;
    _strings_count  = strings_count;
    _strings_offset = strings_offset;
    _entries_count  = entries_count;
    _entries_offset = entries_offset;
    _preload_entries_count  = preload_entries_count;
    _preload_entries_offset = preload_entries_offset;

    _config.record(use_meta_ptrs);
  }

  uint jvm_version_offset() const { return _jvm_version_offset; }

  uint cache_size()     const { return _cache_size; }
  uint strings_count()  const { return _strings_count; }
  uint strings_offset() const { return _strings_offset; }
  uint entries_count()  const { return _entries_count; }
  uint entries_offset() const { return _entries_offset; }
  uint preload_entries_count()  const { return _preload_entries_count; }
  uint preload_entries_offset() const { return _preload_entries_offset; }
  bool has_meta_ptrs()  const { return _config.has_meta_ptrs(); }

  bool verify_config(const char* cache_path, uint load_size)  const;
  bool verify_vm_config(const char* cache_path) const { // Called after Universe initialized
    return _config.verify(cache_path);
  }
};

// Code Cache's entry contain information from CodeBuffer
class SCCEntry {
public:
  enum Kind {
    None = 0,
    Stub = 1,
    Blob = 2,
    Code = 3
  };

private:
  SCCEntry* _next;
  Method*   _method;
  Kind   _kind;        //
  uint   _id;          // vmIntrinsic::ID for stub or name's hash for nmethod

  uint   _offset;      // Offset to entry
  uint   _size;        // Entry size
  uint   _name_offset; // Method's or intrinsic name
  uint   _name_size;
  uint   _code_offset; // Start of code in cache
  uint   _code_size;   // Total size of all code sections
  uint   _reloc_offset;// Relocations
  uint   _reloc_size;  // Max size of relocations per code section
  uint   _num_inlined_bytecodes;

  uint   _comp_level;  // compilation level
  uint   _comp_id;     // compilation id
  uint   _decompile;   // Decompile count for this nmethod
  bool   _has_clinit_barriers; // Generated code has class init checks
  bool   _for_preload; // Code can be used for preload
  bool   _preloaded;   // Code was pre-loaded
  bool   _not_entrant; // Deoptimized
  bool   _load_fail;   // Failed to load due to some klass state

public:
  SCCEntry(uint offset, uint size, uint name_offset, uint name_size,
           uint code_offset, uint code_size,
           uint reloc_offset, uint reloc_size,
           Kind kind, uint id, uint comp_level = 0,
           uint comp_id = 0, uint decomp = 0,
           bool has_clinit_barriers = false,
           bool for_preload = false) {
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
    _reloc_offset = reloc_offset;
    _reloc_size   = reloc_size;
    _num_inlined_bytecodes = 0;

    _comp_level   = comp_level;
    _comp_id      = comp_id;
    _decompile    = decomp;
    _has_clinit_barriers = has_clinit_barriers;
    _for_preload  = for_preload;
    _preloaded    = false;
    _not_entrant  = false;
    _load_fail    = false;
  }
  void* operator new(size_t x, SCCache* cache);
  // Delete is a NOP
  void operator delete( void *ptr ) {}

  SCCEntry* next()    const { return _next; }
  void set_next(SCCEntry* next) { _next = next; }

  Method*   method()  const { return _method; }
  void set_method(Method* method) { _method = method; }
  void update_method_for_writing();

  Kind kind()         const { return _kind; }
  uint id()           const { return _id; }

  uint offset()       const { return _offset; }
  void set_offset(uint off) { _offset = off; }

  uint size()         const { return _size; }
  uint name_offset()  const { return _name_offset; }
  uint name_size()    const { return _name_size; }
  uint code_offset()  const { return _code_offset; }
  uint code_size()    const { return _code_size; }
  uint reloc_offset() const { return _reloc_offset; }
  uint reloc_size()   const { return _reloc_size; }
  uint num_inlined_bytecodes() const { return _num_inlined_bytecodes; }
  void set_inlined_bytecodes(int bytes) { _num_inlined_bytecodes = bytes; }

  uint comp_level()   const { return _comp_level; }
  uint comp_id()      const { return _comp_id; }

  uint decompile()    const { return _decompile; }
  bool has_clinit_barriers() const { return _has_clinit_barriers; }
  bool for_preload()  const { return _for_preload; }
  bool preloaded()    const { return _preloaded; }
  void set_preloaded()      { _preloaded = true; }

  bool not_entrant()  const { return _not_entrant; }
  void set_not_entrant()    { _not_entrant = true; }
  void set_entrant()        { _not_entrant = false; }

  bool load_fail()  const { return _load_fail; }
  void set_load_fail()    { _load_fail = true; }

  void print(outputStream* st) const;
};

// Addresses of stubs, blobs and runtime finctions called from compiled code.
class SCAddressTable : public CHeapObj<mtCode> {
private:
  address* _extrs_addr;
  address* _stubs_addr;
  address* _blobs_addr;
  address* _C1_blobs_addr;
  address* _C2_blobs_addr;
  uint     _extrs_length;
  uint     _stubs_length;
  uint     _blobs_length;
  uint     _C1_blobs_length;
  uint     _C2_blobs_length;
  uint     _final_blobs_length;

  bool _complete;
  bool _opto_complete;
  bool _c1_complete;

public:
  SCAddressTable() {
    _extrs_addr = nullptr;
    _stubs_addr = nullptr;
    _blobs_addr = nullptr;
    _complete = false;
    _opto_complete = false;
    _c1_complete = false;
  }
  ~SCAddressTable();
  void init();
  void init_opto();
  void init_c1();
  void add_C_string(const char* str);
  int  id_for_C_string(address str);
  address address_for_C_string(int idx);
  int  id_for_address(address addr, RelocIterator iter, CodeBuffer* buffer);
  address address_for_id(int id);
  bool opto_complete() const { return _opto_complete; }
  bool c1_complete() const { return _c1_complete; }
};

struct SCCodeSection {
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

class SCCache;

class SCCReader { // Concurent per compilation request
private:
  const SCCache*  _cache;
  const SCCEntry*    _entry;
  const char*        _load_buffer; // Loaded cached code buffer
  uint  _read_position;            // Position in _load_buffer
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
  SCCReader(SCCache* cache, SCCEntry* entry, CompileTask* task);

  bool compile(ciEnv* env, ciMethod* target, int entry_bci, AbstractCompiler* compiler);
  bool compile_blob(CodeBuffer* buffer, int* pc_offset);

  Klass* read_klass(const methodHandle& comp_method, bool shared);
  Method* read_method(const methodHandle& comp_method, bool shared);

  bool read_code(CodeBuffer* buffer, CodeBuffer* orig_buffer, uint code_offset);
  bool read_relocations(CodeBuffer* buffer, CodeBuffer* orig_buffer, OopRecorder* oop_recorder, ciMethod* target);
  DebugInformationRecorder* read_debug_info(OopRecorder* oop_recorder);
  OopMapSet* read_oop_maps();
  bool read_dependencies(Dependencies* dependencies);

  jobject read_oop(JavaThread* thread, const methodHandle& comp_method);
  Metadata* read_metadata(const methodHandle& comp_method);
  bool read_oops(OopRecorder* oop_recorder, ciMethod* target);
  bool read_metadata(OopRecorder* oop_recorder, ciMethod* target);

  void print_on(outputStream* st);
};

class SCCache : public CHeapObj<mtCode> {
private:
  SCCHeader*  _load_header;
  const char* _cache_path;
  char*       _load_buffer;    // Aligned buffer for loading cached code
  char*       _store_buffer;   // Aligned buffer for storing cached code
  char*       _C_load_buffer;  // Original unaligned buffer
  char*       _C_store_buffer; // Original unaligned buffer

  uint        _write_position; // Position in _store_buffer
  uint        _load_size;      // Used when reading cache
  uint        _store_size;     // Used when writing cache
  bool _for_read;              // Open for read
  bool _for_write;             // Open for write
  bool _use_meta_ptrs;         // Store metadata pointers
  bool _for_preload;           // Code for preload
  bool _gen_preload_code;      // Generate pre-loading code
  bool _has_clinit_barriers;   // Code with clinit barriers
  bool _closing;               // Closing cache file
  bool _failed;                // Failed read/write to/from cache (cache is broken?)

  SCAddressTable* _table;

  SCCEntry* _load_entries;     // Used when reading cache
  uint*     _search_entries;   // sorted by ID table [id, index]
  SCCEntry* _store_entries;    // Used when writing cache
  const char* _C_strings_buf;  // Loaded buffer for _C_strings[] table
  uint      _store_entries_cnt;

  uint _compile_id;
  uint _comp_level;
  uint compile_id() const { return _compile_id; }
  uint comp_level() const { return _comp_level; }

  static SCCache* open_for_read();
  static SCCache* open_for_write();

  bool set_write_position(uint pos);
  bool align_write();
  uint write_bytes(const void* buffer, uint nbytes);
  const char* addr(uint offset) const { return _load_buffer + offset; }

  bool _lookup_failed;       // Failed to lookup for info (skip only this code load)
  void set_lookup_failed()     { _lookup_failed = true; }
  void clear_lookup_failed()   { _lookup_failed = false; }
  bool lookup_failed()   const { return _lookup_failed; }

public:
  SCCache(const char* cache_path, int fd, uint load_size);
  ~SCCache();

  const char* cache_buffer() const { return _load_buffer; }
  const char* cache_path()   const { return _cache_path; }
  bool failed() const { return _failed; }
  void set_failed()   { _failed = true; }

  uint load_size() const { return _load_size; }
  uint write_position() const { return _write_position; }

  void load_strings();
  int store_strings();

  static void init_table();
  static void init_opto_table();
  static void init_c1_table();
  address address_for_id(int id) const { return _table->address_for_id(id); }

  bool for_read()  const { return _for_read  && !_failed; }
  bool for_write() const { return _for_write && !_failed; }

  bool closing()          const { return _closing; }
  bool use_meta_ptrs()    const { return _use_meta_ptrs; }
  bool gen_preload_code() const { return _gen_preload_code; }

  void add_new_C_string(const char* str);

  SCCEntry* add_entry() {
    _store_entries_cnt++;
    _store_entries -= 1;
    return _store_entries;
  }
  void preload_startup_code(JavaThread* thread);

  SCCEntry* find_entry(SCCEntry::Kind kind, uint id, uint comp_level = 0, uint decomp = 0);
  void invalidate_entry(SCCEntry* entry);

  bool finish_write();

  static bool load_stub(StubCodeGenerator* cgen, vmIntrinsicID id, const char* name, address start);
  static bool store_stub(StubCodeGenerator* cgen, vmIntrinsicID id, const char* name, address start);

  bool write_klass(Klass* klass);
  bool write_method(Method* method);

  bool write_code(CodeBuffer* buffer, uint& code_size);
  bool write_relocations(CodeBuffer* buffer, uint& reloc_size);
  bool write_debug_info(DebugInformationRecorder* recorder);
  bool write_oop_maps(OopMapSet* oop_maps);

  jobject read_oop(JavaThread* thread, const methodHandle& comp_method);
  Metadata* read_metadata(const methodHandle& comp_method);
  bool read_oops(OopRecorder* oop_recorder, ciMethod* target);
  bool read_metadata(OopRecorder* oop_recorder, ciMethod* target);

  bool write_oop(jobject& jo);
  bool write_oops(OopRecorder* oop_recorder);
  bool write_metadata(Metadata* m);
  bool write_metadata(OopRecorder* oop_recorder);

  static bool load_exception_blob(CodeBuffer* buffer, int* pc_offset);
  static bool store_exception_blob(CodeBuffer* buffer, int pc_offset);

  static bool load_nmethod(ciEnv* env, ciMethod* target, int entry_bci, AbstractCompiler* compiler, CompLevel comp_level);

  static SCCEntry* store_nmethod(const methodHandle& method,
                     int compile_id,
                     int entry_bci,
                     CodeOffsets* offsets,
                     int orig_pc_offset,
                     DebugInformationRecorder* recorder,
                     Dependencies* dependencies,
                     CodeBuffer *code_buffer,
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
                     bool has_monitors);

// Static access

private:
  static SCCache*  _cache;

  static bool open_cache(const char* cache_path);
  static bool verify_vm_config() {
    if (is_on_for_read()) {
      return _cache->_load_header->verify_vm_config(_cache->_cache_path);
    }
    return true;
  }
public:
  static SCCache* cache() { return _cache; }
  static void initialize();
  static void init2();
  static void close();
  static bool is_on() { return _cache != nullptr && !_cache->closing(); }
  static bool is_C3_on();
  static bool is_code_load_thread_on();
  static bool is_on_for_read()  { return is_on() && _cache->for_read(); }
  static bool is_on_for_write() { return is_on() && _cache->for_write(); }
  static bool gen_preload_code(ciMethod* m, int entry_bci);
  static bool allow_const_field(ciConstant& value);
  static void invalidate(SCCEntry* entry);
  static bool is_loaded(SCCEntry* entry);
  static SCCEntry* find_code_entry(const methodHandle& method, uint comp_level);
  static void preload_code(JavaThread* thread);

  static void print_on(outputStream* st);
  static void add_C_string(const char* str);
  static void print_timers();

  static void new_workflow_start_writing_cache();
  static void new_workflow_end_writing_cache();
  static void new_workflow_load_cache();
};

#endif // SHARE_CODE_SCCACHE_HPP
