/*
 * Copyright (c) 2021, 2025, Oracle and/or its affiliates. All rights reserved.
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

#include "cds/aotConstantPoolResolver.hpp"
#include "cds/cds_globals.hpp"
#include "cds/classListParser.hpp"
#include "cds/classListWriter.hpp"
#include "cds/lambdaFormInvokers.inline.hpp"
#include "classfile/classFileStream.hpp"
#include "classfile/classLoader.hpp"
#include "classfile/classLoaderData.hpp"
#include "classfile/classLoaderDataGraph.hpp"
#include "classfile/moduleEntry.hpp"
#include "classfile/symbolTable.hpp"
#include "classfile/systemDictionaryShared.hpp"
#include "memory/resourceArea.hpp"
#include "oops/constantPool.inline.hpp"
#include "oops/instanceKlass.hpp"
#include "runtime/javaCalls.hpp"
#include "runtime/mutexLocker.hpp"

fileStream* ClassListWriter::_classlist_file = nullptr;

void ClassListWriter::init() {
  // For -XX:DumpLoadedClassList=<file> option
  if (DumpLoadedClassList != nullptr) {
    const char* list_name = make_log_name(DumpLoadedClassList, nullptr);
    _classlist_file = new(mtInternal)
                         fileStream(list_name);
    _classlist_file->print_cr("# NOTE: Do not modify this file.");
    _classlist_file->print_cr("#");
    _classlist_file->print_cr("# This file is generated via the -XX:DumpLoadedClassList=<class_list_file> option");
    _classlist_file->print_cr("# and is used at CDS archive dump time (see -Xshare:dump).");
    _classlist_file->print_cr("#");
    FREE_C_HEAP_ARRAY(char, list_name);
  }
}

void ClassListWriter::write(const InstanceKlass* k, const ClassFileStream* cfs) {
  assert(is_enabled(), "must be");

  if (!ClassLoader::has_jrt_entry()) {
    log_warning(aot)("DumpLoadedClassList and CDS are not supported in exploded build");
    DumpLoadedClassList = nullptr;
    return;
  }

  ClassListWriter w;
  write_to_stream(k, w.stream(), cfs);
}

class ClassListWriter::IDTable : public HashTable<
  const InstanceKlass*, int,
  15889, // prime number
  AnyObj::C_HEAP> {};

ClassListWriter::IDTable* ClassListWriter::_id_table = nullptr;
int ClassListWriter::_total_ids = 0;

int ClassListWriter::get_id(const InstanceKlass* k) {
  assert_locked();
  if (_id_table == nullptr) {
    _id_table = new (mtClass)IDTable();
  }
  bool created;
  int* v = _id_table->put_if_absent(k, &created);
  if (created) {
    *v = _total_ids++;
  }
  return *v;
}

bool ClassListWriter::has_id(const InstanceKlass* k) {
  assert_locked();
  if (_id_table != nullptr) {
    return _id_table->get(k) != nullptr;
  } else {
    return false;
  }
}

void ClassListWriter::handle_class_unloading(const InstanceKlass* klass) {
  assert_locked();
  if (_id_table != nullptr) {
    _id_table->remove(klass);
  }
}

void ClassListWriter::write_to_stream(const InstanceKlass* k, outputStream* stream, const ClassFileStream* cfs) {
  assert_locked();

  ClassLoaderData* loader_data = k->class_loader_data();
  bool is_builtin_loader = SystemDictionaryShared::is_builtin_loader(loader_data);
  if (!is_builtin_loader) {
    // class may be loaded from shared archive
    if (!k->is_shared()) {
      if (cfs == nullptr || cfs->source() == nullptr) {
        // CDS static dump only handles unregistered class with known source.
        return;
      }
      if (strncmp(cfs->source(), "file:", 5) != 0) {
        return;
      }
    } else {
      // Shared unregistered classes are skipped since their real source are not recorded in shared space.
      return;
    }
    if (!SystemDictionaryShared::add_unregistered_class(Thread::current(), (InstanceKlass*)k)) {
      return;
    }
  }

  if (cfs != nullptr && cfs->source() != nullptr) {
    if (strcmp(cfs->source(), "_ClassSpecializer_generateConcreteSpeciesCode") == 0) {
      return;
    }

    if (strncmp(cfs->source(), "__", 2) == 0) {
      // generated class: __dynamic_proxy__, __JVM_LookupDefineClass__, etc
      return;
    }
  }

  {
    InstanceKlass* super = k->java_super();
    if (super != nullptr && !has_id(super)) {
      return;
    }

    Array<InstanceKlass*>* interfaces = k->local_interfaces();
    int len = interfaces->length();
    for (int i = 0; i < len; i++) {
      InstanceKlass* intf = interfaces->at(i);
      if (!has_id(intf)) {
        return;
      }
    }
  }

  if (k->is_hidden()) {
    return;
  }

  if (k->module()->is_patched()) {
    return;
  }

  ResourceMark rm;
  stream->print("%s id: %d", k->name()->as_C_string(), get_id(k));
  if (!is_builtin_loader) {
    InstanceKlass* super = k->java_super();
    assert(super != nullptr, "must be");
    stream->print(" super: %d", get_id(super));

    Array<InstanceKlass*>* interfaces = k->local_interfaces();
    int len = interfaces->length();
    if (len > 0) {
      stream->print(" interfaces:");
      for (int i = 0; i < len; i++) {
        InstanceKlass* intf = interfaces->at(i);
        stream->print(" %d", get_id(intf));
      }
    }

    // NB: the string following "source: " is not really a proper file name, but rather
    // a truncated URI referring to a file. It must be decoded after reading.
#ifdef _WINDOWS
    // "file:/C:/dir/foo.jar" -> "C:/dir/foo.jar"
    stream->print(" source: %s", cfs->source() + 6);
#else
    // "file:/dir/foo.jar" -> "/dir/foo.jar"
    stream->print(" source: %s", cfs->source() + 5);
#endif
  }

  stream->cr();
  stream->flush();
}

void ClassListWriter::delete_classlist() {
  if (_classlist_file != nullptr) {
    delete _classlist_file;
  }
}

class ClassListWriter::WriteResolveConstantsCLDClosure : public CLDClosure {
public:
  void do_cld(ClassLoaderData* cld) {
    for (Klass* klass = cld->klasses(); klass != nullptr; klass = klass->next_link()) {
      if (klass->is_instance_klass()) {
        InstanceKlass* ik = InstanceKlass::cast(klass);
        write_resolved_constants_for(ik);
        write_array_info_for(ik); // FIXME: piggybacking on WriteResolveConstantsCLDClosure is misleading
      }
    }
  }
};

void ClassListWriter::write_array_info_for(InstanceKlass* ik) {
  ObjArrayKlass* oak = ik->array_klasses();
  if (oak != nullptr) {
    while (oak->higher_dimension() != nullptr) {
      oak = oak->higher_dimension();
    }
    ResourceMark rm;
    outputStream* stream = _classlist_file;
    stream->print_cr("%s %s %d", ClassListParser::ARRAY_TAG, ik->name()->as_C_string(), oak->dimension());
  }
}

void ClassListWriter::write_resolved_constants() {
  if (!is_enabled()) {
    return;
  }
  MutexLocker lock(ClassLoaderDataGraph_lock);
  MutexLocker lock2(ClassListFile_lock, Mutex::_no_safepoint_check_flag);

  WriteResolveConstantsCLDClosure closure;
  ClassLoaderDataGraph::loaded_cld_do(&closure);
}

void ClassListWriter::write_reflection_data() {
  if (!is_enabled()) {
    return;
  }
  auto collector = [&] (const InstanceKlass* ik, int id) {
    write_reflection_data_for(const_cast<InstanceKlass*>(ik));
  };
  _id_table->iterate_all(collector);
}

void ClassListWriter::write_reflection_data_for(InstanceKlass* ik) {
  ResourceMark rm;
  outputStream* stream = _classlist_file;
  if (!SystemDictionaryShared::is_builtin_loader(ik->class_loader_data()) || ik->is_hidden()) {
    return; // ignore
  }
  if (java_lang_Class::has_reflection_data(ik->java_mirror())) {
    EXCEPTION_MARK;
    int rd_flags = AOTConstantPoolResolver::class_reflection_data_flags(ik, THREAD);
    if (!HAS_PENDING_EXCEPTION) {
      // We can't hold the lock when doing the upcall inside class_reflection_data_flags()
      MutexLocker lock2(ClassListFile_lock, Mutex::_no_safepoint_check_flag);
      stream->print_cr("%s %s %d", ClassListParser::CLASS_REFLECTION_DATA_TAG, ik->name()->as_C_string(), rd_flags);
    }
  }
}

void ClassListWriter::write_resolved_constants_for(InstanceKlass* ik) {
  if (!SystemDictionaryShared::is_builtin_loader(ik->class_loader_data()) ||
      ik->is_hidden()) {
    return;
  }
  if (LambdaFormInvokers::may_be_regenerated_class(ik->name())) {
    return;
  }
  if (ik->name()->equals("jdk/internal/module/SystemModules$all")) {
    // This class is regenerated during JDK build process, so the classlist
    // may not match the version that's in the real jdk image.
    return;
  }

  if (!has_id(ik)) { // do not resolve CP for classes loaded by custom loaders.
    return;
  }

  ResourceMark rm;
  ConstantPool* cp = ik->constants();
  GrowableArray<bool> list(cp->length(), cp->length(), false);
  bool print = false;

  for (int cp_index = 1; cp_index < cp->length(); cp_index++) { // Index 0 is unused
    switch (cp->tag_at(cp_index).value()) {
    case JVM_CONSTANT_Class:
      {
        Klass* k = cp->resolved_klass_at(cp_index);
        if (k->is_instance_klass()) {
          list.at_put(cp_index, true);
          print = true;
        }
      }
      break;
    }
  }

  if (cp->cache() != nullptr) {
    Array<ResolvedIndyEntry>* indy_entries = cp->cache()->resolved_indy_entries();
    if (indy_entries != nullptr) {
      for (int i = 0; i < indy_entries->length(); i++) {
        ResolvedIndyEntry* rie = indy_entries->adr_at(i);
        int cp_index = rie->constant_pool_index();
        if (rie->is_resolved()) {
          list.at_put(cp_index, true);
          print = true;
        }
      }
    }

    Array<ResolvedFieldEntry>* field_entries = cp->cache()->resolved_field_entries();
    if (field_entries != nullptr) {
      for (int i = 0; i < field_entries->length(); i++) {
        ResolvedFieldEntry* rfe = field_entries->adr_at(i);
        if (rfe->is_resolved(Bytecodes::_getfield) ||
            rfe->is_resolved(Bytecodes::_putfield)) {
          list.at_put(rfe->constant_pool_index(), true);
          print = true;
        }
      }
    }

    Array<ResolvedMethodEntry>* method_entries = cp->cache()->resolved_method_entries();
    if (method_entries != nullptr) {
      for (int i = 0; i < method_entries->length(); i++) {
        ResolvedMethodEntry* rme = method_entries->adr_at(i);
        if (rme->is_resolved(Bytecodes::_invokevirtual) ||
            rme->is_resolved(Bytecodes::_invokespecial) ||
            rme->is_resolved(Bytecodes::_invokeinterface) ||
            rme->is_resolved(Bytecodes::_invokestatic) ||
            rme->is_resolved(Bytecodes::_invokehandle)) {
          list.at_put(rme->constant_pool_index(), true);
          print = true;
        }
      }
    }
  }

  if (print) {
    outputStream* stream = _classlist_file;
    stream->print("@cp %s", ik->name()->as_C_string());
    for (int i = 0; i < list.length(); i++) {
      if (list.at(i)) {
        constantTag cp_tag = cp->tag_at(i).value();
        assert(cp_tag.value() == JVM_CONSTANT_Class ||
               cp_tag.value() == JVM_CONSTANT_Fieldref ||
               cp_tag.value() == JVM_CONSTANT_Methodref||
               cp_tag.value() == JVM_CONSTANT_InterfaceMethodref ||
               cp_tag.value() == JVM_CONSTANT_InvokeDynamic, "sanity");
        stream->print(" %d", i);
      }
    }
    stream->cr();
  }
}

void ClassListWriter::write_loader_negative_lookup_cache_for(oop loader, const char* loader_type) {
  TempNewSymbol method = SymbolTable::new_symbol("negativeLookupCacheContents");
  TempNewSymbol signature = SymbolTable::new_symbol("()Ljava/lang/String;");

  EXCEPTION_MARK;
  HandleMark hm(THREAD);

  JavaValue result(T_OBJECT);
  JavaCalls::call_virtual(&result,
                          Handle(THREAD, loader),
                          loader->klass(),
                          method,
                          signature,
                          CHECK);

  if (HAS_PENDING_EXCEPTION) {
    log_warning(cds)("Error during BuiltinClassLoader::negativeLookupCacheContents() call for %s loader", loader_type);
    CLEAR_PENDING_EXCEPTION;
    return;
  } else if (result.get_oop() == nullptr) {
    return;
  }

  ResourceMark rm;
  const char* cache_contents = java_lang_String::as_utf8_string(result.get_oop());
  log_debug(cds)("%s loader negative cache: %s", loader_type, cache_contents);

  outputStream* stream = _classlist_file;
  const size_t buffer_size = strlen(ClassListParser::LOADER_NEGATIVE_CACHE_TAG) + 1 /* for space*/
                    + strlen(loader_type) + 1 /* for space */ + strlen(cache_contents) + 1 /* for null character */;
  char* buffer = NEW_C_HEAP_ARRAY(char, buffer_size, mtInternal);
  _classlist_file->set_scratch_buffer(buffer, buffer_size);
  MutexLocker lock2(ClassListFile_lock, Mutex::_no_safepoint_check_flag);
  stream->print("%s %s %s", ClassListParser::LOADER_NEGATIVE_CACHE_TAG, loader_type, cache_contents);
  stream->cr();
  _classlist_file->set_scratch_buffer(nullptr, 0);
}

void ClassListWriter::write_loader_negative_lookup_cache() {
  if (!is_enabled()) {
    return;
  }

  write_loader_negative_lookup_cache_for(SystemDictionary::java_platform_loader(), "platform");
  write_loader_negative_lookup_cache_for(SystemDictionary::java_system_loader(), "app");
}
