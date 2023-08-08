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

#ifndef SHARE_CODE_SCARCHIVE_HPP
#define SHARE_CODE_SCARCHIVE_HPP

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
class SCAFile;
class StubCodeGenerator;

enum class vmIntrinsicID : int;
enum CompLevel : signed char;

// Archive file header
class SCAHeader : public CHeapObj<mtCode> {
private:
  // Here should be version and other verification fields
  uint _version;           // JDK version (should match when reading archive)
  uint _archive_size;      // archive size in bytes
  uint _strings_count;
  uint _strings_offset;    // offset to recorded C strings
  uint _entries_count;     // number of recorded entries in archive
  uint _entries_offset;    // offset of SCAEntry array describing entries
  uint _preload_entries_count; // entries for pre-loading code
  uint _preload_entries_offset;
  enum Flags {
    None = 0,
    MetadataPointers = 1
  };
  uint _flags;
  uint _dummy; // to align

public:
  void init(uint version, uint archive_size, uint strings_count, uint strings_offset,
            uint entries_count, uint entries_offset,
            uint preload_entries_count, uint preload_entries_offset) {
    _version        = version;
    _archive_size   = archive_size;
    _strings_count  = strings_count;
    _strings_offset = strings_offset;
    _entries_count  = entries_count;
    _entries_offset = entries_offset;
    _preload_entries_count  = preload_entries_count;
    _preload_entries_offset = preload_entries_offset;
    _flags          = 0;
  }

  uint version()        const { return _version; }
  uint archive_size()   const { return _archive_size; }
  uint strings_count()  const { return _strings_count; }
  uint strings_offset() const { return _strings_offset; }
  uint entries_count()  const { return _entries_count; }
  uint entries_offset() const { return _entries_offset; }
  uint preload_entries_count()  const { return _preload_entries_count; }
  uint preload_entries_offset() const { return _preload_entries_offset; }
  bool has_meta_ptrs()  const { return (_flags & MetadataPointers) != 0; }
  void set_meta_ptrs()        { _flags |= MetadataPointers; }
};

// Archive's entry contain information from CodeBuffer
class SCAEntry {
public:
  enum Kind {
    None = 0,
    Stub = 1,
    Blob = 2,
    Code = 3
  };

private:
  SCAEntry* _next;
  Method*   _method;
  Kind   _kind;        //
  uint   _id;          // vmIntrinsic::ID for stub or name's hash for nmethod

  uint   _offset;      // Offset to entry
  uint   _size;        // Entry size
  uint   _name_offset; // Method's or intrinsic name
  uint   _name_size;
  uint   _code_offset; // Start of code in archive
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

public:
  SCAEntry(uint offset, uint size, uint name_offset, uint name_size,
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
  }
  void* operator new(size_t x, SCAFile* sca);
  // Delete is a NOP
  void operator delete( void *ptr ) {}

  SCAEntry* next()    const { return _next; }
  void set_next(SCAEntry* next) { _next = next; }

  Method*   method()  const { return _method; }
  void set_method(Method* method) { _method = method; }

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
  SCAddressTable() { _complete = false; _opto_complete = false; _c1_complete = false; }
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

struct SCACodeSection {
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

class SCAFile;

class SCAReader { // Concurent per compilation request
private:
  const SCAFile*  _archive;
  const SCAEntry* _entry;
  const char* _load_buffer; // Loaded Archive
  uint  _read_position;        // Position in _load_buffer
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
  SCAReader(SCAFile* archive, SCAEntry* entry, CompileTask* task);

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

class SCAFile : public CHeapObj<mtCode> {
private:
  SCAHeader*  _load_header;
  const char* _archive_path;
  char*       _load_buffer;    // Aligned buffer for loading Archive
  char*       _store_buffer;   // Aligned buffer for storing Archive
  char*       _C_load_buffer;  // Original unaligned buffer
  char*       _C_store_buffer; // Original unaligned buffer

  uint        _write_position; // Position in _store_buffer
  uint        _load_size;      // Used when reading archive
  uint        _store_size;     // Used when writing archive
  bool _for_read;              // Open for read
  bool _for_write;             // Open for write
  bool _use_meta_ptrs;         // Store metadata pointers
  bool _for_preload;           // Code for preload
  bool _gen_preload_code;      // Generate pre-loading code
  bool _closing;               // Closing archive
  bool _failed;                // Failed read/write to/from archive (archive is broken?)

  SCAddressTable* _table;

  SCAEntry* _load_entries;     // Used when reading archive
  uint*     _search_entries;   // sorted by ID table [id, index]
  SCAEntry* _store_entries;    // Used when writing archive
  const char* _C_strings_buf;  // Loaded buffer for _C_strings[] table
  uint      _store_entries_cnt;

  uint _compile_id;
  uint _comp_level;
  uint compile_id() const { return _compile_id; }
  uint comp_level() const { return _comp_level; }

  static SCAFile* open_for_read();
  static SCAFile* open_for_write();

  bool set_write_position(uint pos);
  bool align_write();
  uint write_bytes(const void* buffer, uint nbytes);
  const char* addr(uint offset) const { return _load_buffer + offset; }

  bool _lookup_failed;       // Failed to lookup for info (skip only this code load)
  void set_lookup_failed()     { _lookup_failed = true; }
  void clear_lookup_failed()   { _lookup_failed = false; }
  bool lookup_failed()   const { return _lookup_failed; }

public:
  SCAFile(const char* archive_path, int fd, uint load_size);
  ~SCAFile();

  const char* archive_buffer() const { return _load_buffer; }
  const char* archive_path()   const { return _archive_path; }
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

  bool for_read() const;
  bool for_write() const;
  bool closing()          const { return _closing; }
  bool use_meta_ptrs()    const { return _use_meta_ptrs; }
  bool gen_preload_code() const { return _gen_preload_code; }

  void add_C_string(const char* str);

  SCAEntry* add_entry() {
    _store_entries_cnt++;
    _store_entries -= 1;
    return _store_entries;
  }
  void preload_code(JavaThread* thread);

  SCAEntry* find_entry(SCAEntry::Kind kind, uint id, uint comp_level = 0, uint decomp = 0);
  void invalidate(SCAEntry* entry);

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

  static SCAEntry* store_nmethod(const methodHandle& method,
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

  static void print_on(outputStream* st);
};

class SCArchive {
private:
  static SCAFile*  _archive;

  static bool open_archive(const char* archive_path);

public:
  static SCAFile* archive() { return _archive; }
  static void initialize();
  static void init2();
  static void close();
  static bool is_on() { return _archive != nullptr && !_archive->closing(); }
  static bool is_C3_on();
  static bool is_SC_load_tread_on();
  static bool is_on_for_read()  { return is_on() && _archive->for_read(); }
  static bool is_on_for_write() { return is_on() && _archive->for_write(); }
  static bool gen_preload_code(ciMethod* m, int entry_bci);
  static bool allow_const_field(ciConstant& value);
  static void invalidate(SCAEntry* entry);
  static bool is_loaded(SCAEntry* entry);
  static SCAEntry* find_code_entry(const methodHandle& method, uint comp_level);
  static void preload_code(JavaThread* thread);

  static void add_C_string(const char* str);
  static void print_timers();
};

#endif // SHARE_CODE_SCARCHIVE_HPP
