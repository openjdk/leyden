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

#include "cds/archiveBuilder.hpp"
#include "cppstdlib/type_traits.hpp"
#include "oops/method.hpp"
#include "oops/resolvedMethodEntry.hpp"

static_assert(std::is_trivially_copyable_v<ResolvedMethodEntry>);

// Detect inadvertently introduced trailing padding.
class ResolvedMethodEntryWithExtra : public ResolvedMethodEntry {
  u1 _extra_field;
};
static_assert(sizeof(ResolvedMethodEntryWithExtra) > sizeof(ResolvedMethodEntry));

bool ResolvedMethodEntry::check_no_old_or_obsolete_entry() {
  // return false if m refers to a non-deleted old or obsolete method
  if (_method != nullptr) {
    assert(_method->is_valid() && _method->is_method(), "m is a valid method");
    return !_method->is_old() && !_method->is_obsolete(); // old is always set for old and obsolete
  } else {
    return true;
  }
}

void ResolvedMethodEntry::reset_entry() {
  if (has_resolved_references_index()) {
    u2 saved_resolved_references_index = _entry_specific._resolved_references_index;
    *this = ResolvedMethodEntry(_cpool_index);
    set_resolved_references_index(saved_resolved_references_index);
  } else {
    *this = ResolvedMethodEntry(_cpool_index);
  }
}

#if INCLUDE_CDS
void ResolvedMethodEntry::remove_unshareable_info() {
  reset_entry();
}

void ResolvedMethodEntry::mark_and_relocate(ConstantPool* src_cp) {
  if (_method == nullptr) {
    assert(bytecode2() == Bytecodes::_invokevirtual, "");
  } else {
    ArchiveBuilder::current()->mark_and_relocate_to_buffered_addr(&_method);
  }
  if (bytecode1() == Bytecodes::_invokeinterface) {
    ArchiveBuilder::current()->mark_and_relocate_to_buffered_addr(&_entry_specific._interface_klass);
  }
#if 0
  // OLD CODE ... some of it may need to be salvaged.
  Bytecodes::Code invoke_code = bytecode_1();
  if (invoke_code != (Bytecodes::Code)0) {
    Metadata* f1 = f1_ord();
    if (f1 != nullptr) {
      ArchiveBuilder::current()->mark_and_relocate_to_buffered_addr(&_f1);
      switch (invoke_code) {
      case Bytecodes::_invokeinterface:
        assert(0, "not implemented");
        //assert(f1->is_klass(), "");
        //ArchiveBuilder::current()->mark_and_relocate_to_buffered_addr(&_f2); // f2 is interface method
        return false;
      case Bytecodes::_invokestatic:
        // For safety, we support invokestatic only for invoking methods in MethodHandle.
        // FIXME -- further restrict it to linkToStatic(), etc?
        assert(bytecode_2() == (Bytecodes::Code)0, "must be");
        assert(f1->is_method(), "");
        assert(f1_as_method()->method_holder()->name()->equals("java/lang/invoke/MethodHandle") ||
               f1_as_method()->method_holder()->name()->equals("java/lang/invoke/MethodHandleNatives"), "sanity");
        return true;
      case Bytecodes::_invokespecial:
        assert(f1->is_method(), "must be");
        // Also need to work on bytecode_2() below.
        break;
      case Bytecodes::_invokehandle:
        assert(bytecode_2() == (Bytecodes::Code)0, "must be");
        assert(f1->is_method(), "");
        return true;
      default:
        ShouldNotReachHere();
        break;
      }
    }
  }

  // TODO test case: can invokespecial and invokevirtual share the same CP?
  invoke_code = bytecode_2();
  if (invoke_code != (Bytecodes::Code)0) {
    assert(invoke_code == Bytecodes::_invokevirtual, "must be");
    if (is_vfinal()) {
      // f2 is vfinal method
      ArchiveBuilder::current()->mark_and_relocate_to_buffered_addr(&_f2); // f2 is final method
    } else {
      // f2 is vtable index, no need to mark
      if (DynamicDumpSharedSpaces) {
        // InstanceKlass::methods() is has been resorted, so we need to
        // update the vtable_index.
        int holder_index = src_cp->uncached_klass_ref_index_at(constant_pool_index());
        Klass* src_klass = src_cp->resolved_klass_at(holder_index);
        Method* src_m = src_klass->method_at_vtable(f2_as_index());
        if (!ArchiveBuilder::current()->is_in_mapped_static_archive(src_m->method_holder()) &&
            !ArchiveBuilder::current()->is_in_mapped_static_archive(src_m)) {
          Klass* buffered_klass = ArchiveBuilder::current()->get_buffered_addr(src_klass);
          Method* buffered_m = ArchiveBuilder::current()->get_buffered_addr(src_m);
          int vtable_index;
          if (src_m->method_holder()->is_interface()) { // default or miranda method
            assert(src_m->vtable_index() < 0, "must be");
            assert(buffered_klass->is_instance_klass(), "must be");
            vtable_index = InstanceKlass::cast(buffered_klass)->vtable_index_of_interface_method(buffered_m);
            assert(vtable_index >= 0, "must be");
          } else {
            vtable_index = buffered_m->vtable_index();
            assert(vtable_index >= 0, "must be");
          }
          if (_f2 != vtable_index) {
            log_trace(cds, resolve)("vtable_index changed %d => %d", (int)_f2, vtable_index);
            _f2 = vtable_index;
          }
        }
      }
    }
  }

#endif
}
#endif

void ResolvedMethodEntry::print_on(outputStream* st) const {
  st->print_cr("Method Entry:");

  if (method() != nullptr) {
    st->print_cr(" - Method: " INTPTR_FORMAT " %s", p2i(method()), method()->external_name());
  } else {
    st->print_cr("- Method: null");
  }
  // Some fields are mutually exclusive and are only used by certain invoke codes
  if (bytecode1() == Bytecodes::_invokeinterface && interface_klass() != nullptr) {
    st->print_cr(" - Klass: " INTPTR_FORMAT " %s", p2i(interface_klass()), interface_klass()->external_name());
  } else {
    st->print_cr("- Klass: null");
  }
  if (bytecode1() == Bytecodes::_invokehandle) {
    st->print_cr(" - Resolved References Index: %d", resolved_references_index());
  } else {
    st->print_cr(" - Resolved References Index: none");
  }
  if (bytecode2() == Bytecodes::_invokevirtual) {
#ifdef ASSERT
    if (_has_table_index) {
      st->print_cr(" - Table Index: %d", table_index());
    }
#else
    st->print_cr(" - Table Index: %d", table_index());
#endif
  } else {
    st->print_cr(" - Table Index: none");
  }
  st->print_cr(" - CP Index: %d", constant_pool_index());
  st->print_cr(" - TOS: %s", type2name(as_BasicType((TosState)tos_state())));
  st->print_cr(" - Number of Parameters: %d", number_of_parameters());
  st->print_cr(" - Is Virtual Final: %d", is_vfinal());
  st->print_cr(" - Is Final: %d", is_final());
  st->print_cr(" - Is Forced Virtual: %d", is_forced_virtual());
  st->print_cr(" - Has Appendix: %d", has_appendix());
  st->print_cr(" - Has Local Signature: %d", has_local_signature());
  st->print_cr(" - Bytecode 1: %s", Bytecodes::name((Bytecodes::Code)bytecode1()));
  st->print_cr(" - Bytecode 2: %s", Bytecodes::name((Bytecodes::Code)bytecode2()));
}
