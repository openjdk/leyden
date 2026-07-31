/*
 * Copyright (c) 2021, 2026, Oracle and/or its affiliates. All rights reserved.
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

#include "cds/aotClassLocation.hpp"
#include "cds/cdsConfig.hpp"
#include "cds/cdsProtectionDomain.hpp"
#include "classfile/classLoader.hpp"
#include "classfile/classLoaderData.inline.hpp"
#include "classfile/javaClasses.hpp"
#include "classfile/moduleEntry.hpp"
#include "classfile/systemDictionaryShared.hpp"
#include "classfile/vmClasses.hpp"
#include "classfile/vmSymbols.hpp"
#include "memory/oopFactory.hpp"
#include "memory/resourceArea.hpp"
#include "memory/universe.hpp"
#include "oops/instanceKlass.hpp"
#include "oops/oopCast.inline.hpp"
#include "oops/refArrayOop.hpp"
#include "oops/symbol.hpp"
#include "runtime/javaCalls.hpp"

OopHandle CDSProtectionDomain::_shared_protection_domains;
OopHandle CDSProtectionDomain::_shared_jar_urls;
OopHandle CDSProtectionDomain::_shared_jar_manifests;

static Handle to_file_URL(const char* path, TRAPS) {
  JavaValue result(T_OBJECT);
  Handle path_string = java_lang_String::create_from_str(path, CHECK_NH);
  JavaCalls::call_static(&result,
                         vmClasses::jdk_internal_loader_ClassLoaders_klass(),
                         vmSymbols::toFileURL_name(),
                         vmSymbols::toFileURL_signature(),
                         path_string, CHECK_NH);
  return Handle(THREAD, result.get_oop());
}

static Handle create_jar_manifest(const char* manifest_chars, size_t size, TRAPS) {
  typeArrayOop buf = oopFactory::new_byteArray((int)size, CHECK_NH);
  typeArrayHandle bufhandle(THREAD, buf);
  ArrayAccess<>::arraycopy_from_native(reinterpret_cast<const jbyte*>(manifest_chars),
                                         buf, typeArrayOopDesc::element_offset<jbyte>(0), size);
  Handle bais = JavaCalls::construct_new_instance(vmClasses::ByteArrayInputStream_klass(),
                      vmSymbols::byte_array_void_signature(),
                      bufhandle, CHECK_NH);
  // manifest = new Manifest(ByteArrayInputStream)
  Handle manifest = JavaCalls::construct_new_instance(vmClasses::Jar_Manifest_klass(),
                      vmSymbols::input_stream_void_signature(),
                      bais, CHECK_NH);
  return manifest;
}

static Handle get_package_name(Symbol* class_name, TRAPS) {
  ResourceMark rm(THREAD);
  Handle pkgname_string;
  TempNewSymbol pkg = ClassLoader::package_from_class_name(class_name);
  if (pkg != nullptr) { // Package prefix found
    const char* pkgname = pkg->as_klass_external_name();
    pkgname_string = java_lang_String::create_from_str(pkgname,
                                                       CHECK_(pkgname_string));
  }
  return pkgname_string;
}

// Define Package for shared app classes from JAR file and also checks for
// package sealing (all done in Java code)
// See http://docs.oracle.com/javase/tutorial/deployment/jar/sealman.html
static void define_shared_package(Symbol* class_name, Handle class_loader, Handle manifest, Handle url, TRAPS) {
  //assert(SystemDictionary::is_system_class_loader(class_loader()), "unexpected class loader");
  // get_package_name() returns a null handle if the class is in unnamed package
  Handle pkgname_string = get_package_name(class_name, CHECK);
  if (pkgname_string.not_null()) {
    Klass* app_classLoader_klass = vmClasses::jdk_internal_loader_ClassLoaders_AppClassLoader_klass();
    JavaValue result(T_OBJECT);
    JavaCallArguments args(3);
    args.set_receiver(class_loader);
    args.push_oop(pkgname_string);
    args.push_oop(manifest);
    args.push_oop(url);
    JavaCalls::call_virtual(&result, app_classLoader_klass,
                            vmSymbols::defineOrCheckPackage_name(),
                            vmSymbols::defineOrCheckPackage_signature(),
                            &args,
                            CHECK);
  }
}

// Get the ProtectionDomain associated with the CodeSource from the classloader.
static Handle get_protection_domain_from_classloader(Handle class_loader, Handle url, TRAPS) {
  // CodeSource cs = new CodeSource(url, null);
  Handle cs = JavaCalls::construct_new_instance(vmClasses::CodeSource_klass(),
                  vmSymbols::url_code_signer_array_void_signature(),
                  url, Handle(), CHECK_NH);

  // protection_domain = SecureClassLoader.getProtectionDomain(cs);
  Klass* secureClassLoader_klass = vmClasses::SecureClassLoader_klass();
  JavaValue obj_result(T_OBJECT);
  JavaCalls::call_virtual(&obj_result, class_loader, secureClassLoader_klass,
                          vmSymbols::getProtectionDomain_name(),
                          vmSymbols::getProtectionDomain_signature(),
                          cs, CHECK_NH);
  return Handle(THREAD, obj_result.get_oop());
}

// Returns the ProtectionDomain associated with the moduleEntry.
static Handle get_pd_from_mod_entry(Handle class_loader, ModuleEntry* mod, TRAPS) {
  ClassLoaderData *loader_data = mod->loader_data();
  if (mod->shared_protection_domain() == nullptr) {
    Symbol* location = mod->location();
    if (location != nullptr) {
      Handle location_string = java_lang_String::create_from_symbol(
                                     location, CHECK_NH);
      Handle url;
      JavaValue result(T_OBJECT);
      if (location->starts_with("jrt:/")) {
        url = JavaCalls::construct_new_instance(vmClasses::URL_klass(),
                                                vmSymbols::string_void_signature(),
                                                location_string, CHECK_NH);
      } else {
        Klass* classLoaders_klass =
          vmClasses::jdk_internal_loader_ClassLoaders_klass();
        JavaCalls::call_static(&result, classLoaders_klass, vmSymbols::toFileURL_name(),
                               vmSymbols::toFileURL_signature(),
                               location_string, CHECK_NH);
        url = Handle(THREAD, result.get_oop());
      }

      Handle pd = get_protection_domain_from_classloader(class_loader, url,
                                                         CHECK_NH);
      mod->set_shared_protection_domain(loader_data, pd);
    }
  }

  Handle protection_domain(THREAD, mod->shared_protection_domain());
  assert(protection_domain.not_null(), "sanity");
  return protection_domain;
}

void CDSProtectionDomain::initialize(TRAPS) {
  int size = AOTClassLocationConfig::runtime()->length();
  if (size > 0) {
    allocate_shared_data_arrays(size, CHECK);
  }
}

void CDSProtectionDomain::exercise_runtime_cds_code(const char* dummy_manifest, const char* dummy_jar, TRAPS) {
  create_jar_manifest(dummy_manifest, strlen(dummy_manifest), CHECK);
  to_file_URL(dummy_jar, CHECK);
}

// Initializes the java.lang.Package and java.security.ProtectionDomain objects associated with
// the given InstanceKlass.
// Returns the ProtectionDomain for the InstanceKlass.
Handle CDSProtectionDomain::init_security_info(Handle class_loader, InstanceKlass* ik, PackageEntry* pkg_entry, TRAPS) {
  int index = ik->shared_classpath_index();
  assert(index >= 0, "Sanity");
  Symbol* class_name = ik->name();
  AOTClassLocation* cl = AOTClassLocationConfig::runtime()->class_locations()->at(index);
  if (cl->is_modules_image()) {
    // For shared app/platform classes originated from the run-time image:
    //   The ProtectionDomains are cached in the corresponding ModuleEntries
    //   for fast access by the VM.
    // all packages from module image are already created during VM bootstrap in
    // Modules::define_module().
    assert(pkg_entry != nullptr, "archived class in module image cannot be from unnamed package");
    ModuleEntry* mod_entry = pkg_entry->module();
    return get_pd_from_mod_entry(class_loader, mod_entry, THREAD);
  } else {
    // For shared app/platform classes originated from JAR files on the class path:
    //   Each of the 3 CDSProtectionDomain::_shared_xxx arrays has the same length
    //   as the shared classpath table in the shared archive.
    //
    //   If a shared InstanceKlass k is loaded from the class path, let
    //
    //     index = k->shared_classpath_index();
    //
    //   AOTClassLocationConfig::_runtime_instance->_array->at(index) identifies the JAR file that contains k.
    //
    //   k's protection domain is:
    //
    //     ProtectionDomain pd = _shared_protection_domains[index];
    //
    //   and k's Package is initialized using
    //
    //     manifest = _shared_jar_manifests[index];
    //     url = _shared_jar_urls[index];
    //     define_shared_package(class_name, class_loader, manifest, url, CHECK_NH);
    //
    //   Note that if an element of these 3 _shared_xxx arrays is null, it will be initialized by
    //   the corresponding CDSProtectionDomain::get_shared_xxx() function.
    Handle manifest = get_shared_jar_manifest(index, CHECK_NH);
    Handle url = get_shared_jar_url(index, CHECK_NH);
    int index_offset = index - AOTClassLocationConfig::runtime()->app_cp_start_index();
    if (index_offset < PackageEntry::max_index_for_defined_in_class_path()) {
      if (pkg_entry == nullptr || !pkg_entry->is_defined_by_cds_in_class_path(index_offset)) {
        // define_shared_package only needs to be called once for each package in a jar specified
        // in the shared class path.
        define_shared_package(class_name, class_loader, manifest, url, CHECK_NH);
        if (pkg_entry != nullptr) {
          pkg_entry->set_defined_by_cds_in_class_path(index_offset);
        }
      }
    } else {
      define_shared_package(class_name, class_loader, manifest, url, CHECK_NH);
    }
    return get_shared_protection_domain(class_loader, index, url, THREAD);
  }
}

PackageEntry* CDSProtectionDomain::get_package_entry_from_class(InstanceKlass* ik, Handle class_loader) {
  PackageEntry* pkg_entry = ik->package();
  if (CDSConfig::is_using_full_module_graph() && ik->in_aot_cache() && pkg_entry != nullptr) {
    assert(AOTMetaspace::in_aot_cache(pkg_entry), "must be");
    assert(!ik->defined_by_other_loaders(), "unexpected archived package entry for an unregistered class");
    return pkg_entry;
  }
  TempNewSymbol pkg_name = ClassLoader::package_from_class_name(ik->name());
  if (pkg_name != nullptr) {
    pkg_entry = ClassLoaderData::class_loader_data(class_loader())->packages()->lookup_only(pkg_name);
  } else {
    pkg_entry = nullptr;
  }
  return pkg_entry;
}


Handle CDSProtectionDomain::get_shared_jar_manifest(int shared_path_index, TRAPS) {
  Handle manifest;
  if (shared_jar_manifest(shared_path_index) == nullptr) {
    const AOTClassLocation* cl = AOTClassLocationConfig::runtime()->class_location_at(shared_path_index);
    size_t size = cl->manifest_length();
    if (size == 0) {
      return Handle();
    }

    // ByteArrayInputStream bais = new ByteArrayInputStream(buf);
    const char* src = cl->manifest();
    assert(src != nullptr, "No Manifest data");
    manifest = create_jar_manifest(src, size, CHECK_NH);
    atomic_set_shared_jar_manifest(shared_path_index, manifest());
  }
  manifest = Handle(THREAD, shared_jar_manifest(shared_path_index));
  assert(manifest.not_null(), "sanity");
  return manifest;
}

Handle CDSProtectionDomain::get_shared_jar_url(int shared_path_index, TRAPS) {
  Handle url_h;
  if (shared_jar_url(shared_path_index) == nullptr) {
    const char* path = AOTClassLocationConfig::runtime()->class_location_at(shared_path_index)->path();
    url_h = to_file_URL(path, CHECK_NH);
    atomic_set_shared_jar_url(shared_path_index, url_h());
  }

  url_h = Handle(THREAD, shared_jar_url(shared_path_index));
  assert(url_h.not_null(), "sanity");
  return url_h;
}



// Returns the ProtectionDomain associated with the JAR file identified by the url.
Handle CDSProtectionDomain::get_shared_protection_domain(Handle class_loader,
                                                            int shared_path_index,
                                                            Handle url,
                                                            TRAPS) {
  Handle protection_domain;
  if (shared_protection_domain(shared_path_index) == nullptr) {
    Handle pd = get_protection_domain_from_classloader(class_loader, url, CHECK_NH);
    atomic_set_shared_protection_domain(shared_path_index, pd());
  }

  // Acquire from the cache because if another thread beats the current one to
  // set the shared protection_domain and the atomic_set fails, the current thread
  // needs to get the updated protection_domain from the cache.
  protection_domain = Handle(THREAD, shared_protection_domain(shared_path_index));
  assert(protection_domain.not_null(), "sanity");
  return protection_domain;
}

void CDSProtectionDomain::atomic_set_array_index(OopHandle array, int index, oop o) {
  // Benign race condition:  array.obj_at(index) may already be filled in.
  // The important thing here is that all threads pick up the same result.
  // It doesn't matter which racing thread wins, as long as only one
  // result is used by all threads, and all future queries.
  oop_cast<refArrayOop>(array.resolve())->replace_if_null(index, o);
}

oop CDSProtectionDomain::shared_protection_domain(int index) {
  return oop_cast<refArrayOop>(_shared_protection_domains.resolve())->obj_at(index);
}

void CDSProtectionDomain::allocate_shared_protection_domain_array(int size, TRAPS) {
  if (_shared_protection_domains.resolve() == nullptr) {
    oop spd = oopFactory::new_refArray(vmClasses::ProtectionDomain_klass(), size, CHECK);
    _shared_protection_domains = OopHandle(Universe::vm_global(), spd);
  }
}

oop CDSProtectionDomain::shared_jar_url(int index) {
  return oop_cast<refArrayOop>(_shared_jar_urls.resolve())->obj_at(index);
}

void CDSProtectionDomain::allocate_shared_jar_url_array(int size, TRAPS) {
  if (_shared_jar_urls.resolve() == nullptr) {
    oop sju = oopFactory::new_refArray(vmClasses::URL_klass(), size, CHECK);
    _shared_jar_urls = OopHandle(Universe::vm_global(), sju);
  }
}

oop CDSProtectionDomain::shared_jar_manifest(int index) {
  return oop_cast<refArrayOop>(_shared_jar_manifests.resolve())->obj_at(index);
}

void CDSProtectionDomain::allocate_shared_jar_manifest_array(int size, TRAPS) {
  if (_shared_jar_manifests.resolve() == nullptr) {
    oop sjm = oopFactory::new_refArray(vmClasses::Jar_Manifest_klass(), size, CHECK);
    _shared_jar_manifests = OopHandle(Universe::vm_global(), sjm);
  }
}

KlassMirrorDataCache::KlassMirrorDataCache(Handle class_loader, Array<AOTClassLocation*>* class_locations, TRAPS) {
  int size = class_locations->length();
  oop manifest_array = oopFactory::new_refArray(vmClasses::Jar_Manifest_klass(), size, CHECK);
  _jar_manifests = Handle(THREAD, manifest_array);
  oop jar_array = oopFactory::new_refArray(vmClasses::URL_klass(), size, CHECK);
  _jar_urls = Handle(THREAD, jar_array);
  oop pd_array = oopFactory::new_refArray(vmClasses::ProtectionDomain_klass(), size, CHECK);
  _protection_domains = Handle(THREAD, pd_array);
  _class_loader = class_loader;
  _locations = class_locations;
}

Handle KlassMirrorDataCache::get_jar_manifest(int index, TRAPS) {
  Handle manifest_h;
  oop manifest_oop = jar_manifest_at(index);
  if (manifest_oop == nullptr) {
    size_t size = _locations->at(index)->manifest_length();
    if (size == 0) {
      return Handle();
    }
    // ByteArrayInputStream bais = new ByteArrayInputStream(buf);
    const char* src = _locations->at(index)->manifest();
    assert(src != nullptr, "No Manifest data");
    manifest_h = create_jar_manifest(src, size, CHECK_NH);
    set_jar_manifest_at(index, manifest_h());
  } else {
    manifest_h = Handle(THREAD, manifest_oop);
  }
  return manifest_h;
}

Handle KlassMirrorDataCache::get_jar_url(int index, TRAPS) {
  Handle url_h;
  oop url_oop = jar_url_at(index);
  if (url_oop == nullptr) {
    const char* path = _locations->at(index)->path();
    url_h = to_file_URL(path, CHECK_NH);
    set_jar_url_at(index, url_h());
  } else {
    url_h = Handle(THREAD, url_oop);
  }
  return url_h;
}

// Returns the ProtectionDomain associated with the JAR file identified by the url.
Handle KlassMirrorDataCache::get_protection_domain(int index, TRAPS) {
  Handle pd_h;
  oop pd_oop = protection_domain_at(index);
  if (pd_oop == nullptr) {
    Handle url_h = get_jar_url(index, CHECK_NH);
    assert(url_h.not_null(), "sanity check");
    pd_h = get_protection_domain_from_classloader(_class_loader, url_h, CHECK_NH);
    set_protection_domain_at(index, pd_h());
  } else {
    // Acquire from the cache because if another thread beats the current one to
    // set the shared protection_domain and the atomic_set fails, the current thread
    // needs to get the updated protection_domain from the cache.
    pd_h = Handle(THREAD, pd_oop);
  }
  return pd_h;
}

PackageEntry* KlassMirrorData::get_package_entry(InstanceKlass* ik, KlassMirrorDataCache& mirror_data_cache, TRAPS) {
  PackageEntry* pkg_entry = nullptr;
  TempNewSymbol pkg_name = ClassLoader::package_from_class_name(ik->name());
  Handle loader = mirror_data_cache.class_loader();
  if (pkg_name != nullptr) {
    int index = ik->shared_classpath_index();
    pkg_entry = ClassLoaderData::class_loader_data(loader())->packages()->lookup_only(pkg_name);
#ifdef ASSERT
    AOTClassLocation* cl = mirror_data_cache.locations()->at(index);
    if (cl->is_modules_image()) {
      // All packages from module image are already created during VM bootstrap in
      // Modules::define_module().
      assert(pkg_entry != nullptr, "archived class in module image cannot be from unnamed package");
    }
#endif
    // If the packageEntry is not yet created, then create the Package object
    if (pkg_entry == nullptr && loader.not_null()) {
      Handle url = mirror_data_cache.get_jar_url(index, CHECK_NULL);
      Handle manifest = mirror_data_cache.get_jar_manifest(index, CHECK_NULL);
      define_shared_package(ik->name(), loader, manifest, url, CHECK_NULL);
    }
  }
  return pkg_entry;
}

Handle KlassMirrorData::get_protection_domain(InstanceKlass* ik, PackageEntry* pkg_entry, KlassMirrorDataCache& mirror_data_cache, TRAPS) {
    int index = ik->shared_classpath_index();
    AOTClassLocation* cl = mirror_data_cache.locations()->at(ik->shared_classpath_index());
    if (cl->is_modules_image()) {
      // For shared app/platform classes originated from the run-time image:
      //   The ProtectionDomains are cached in the corresponding ModuleEntries
      //   for fast access by the VM.
      assert(pkg_entry != nullptr, "archived class in module image cannot be from unnamed package");
      ModuleEntry* mod_entry = pkg_entry->module();
      return get_pd_from_mod_entry(mirror_data_cache.class_loader(), mod_entry, THREAD);
    } else {
      return mirror_data_cache.get_protection_domain(index, THREAD);
    }
}
