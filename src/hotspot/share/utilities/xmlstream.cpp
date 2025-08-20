/*
 * Copyright (c) 2002, 2025, Oracle and/or its affiliates. All rights reserved.
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

#include "code/nmethod.hpp"
#include "memory/allocation.hpp"
#include "memory/allocation.inline.hpp"
#include "memory/resourceArea.hpp"
#include "oops/method.inline.hpp"
#include "oops/methodData.hpp"
#include "oops/oop.inline.hpp"
#include "runtime/deoptimization.hpp"
#include "runtime/handles.inline.hpp"
#include "runtime/osThread.hpp"
#include "runtime/vmOperations.hpp"
#include "runtime/vmThread.hpp"
#include "utilities/vmError.hpp"
#include "utilities/xmlstream.hpp"

// The XML stream is the contents of the LogFile (default hotspot_%p.log).
// It is a superset of whatever might be displayed on the tty.
// You can get to it by calls of the form xtty->...
// Normal calls to tty->... just embed plain text among any markup
// produced via the xtty API.
// The xtty has sub-streams called xtty->text() and xtty->log_long().
// These are ordinary output streams for writing unstructured text.
// The format of this log file is both unstructured and constrained.
//
// Apart from possible race conditions, every line in the log file
// is either an XML element (<tag ...>, or </tag>, or <tag .../>)
// or is unstructured text.
//
// On any given line, if the first character is '<', then the last
// character is '>' and the line consists of a single XML element,
// which uses single quote '\'' to delimit any attribute values.
// (The double-quote character '"' never appears, ever.)
//
// All other lines consist of unstructured text which is completely
// free of the following characters: '<', '>', '&', '\'', '"'.  If
// those characters are written to the tty (or to any other text
// stream underlying the xtty), those characters, and no other
// characters, are written as XML entities: "&lt;", "&gt;", "&amp;",
// "&apos", "&quot".  There is no other use of the character '&'.
//
// The net effect is that you may select a range of tools to process
// the marked-up logs, including XML parsers and simple line-oriented
// Java or Unix tools.  The main concession you have to make to XML
// is to convert the above five XML entities to single ASCII chars,
// as you process attribute strings or unstructured text.
//
// It would be wise to ignore any XML tags that you do not recognize.
// This can be done with grep, if you choose, because the log file
// is line-structured.
//
// The log file collects the output from many contributing threads.
// You should expect that an element of the form <writer thread='NNN'>
// could appear almost anywhere, as the lines interleave.
// It is straightforward to write a script to tease the log file
// into thread-specific substreams.

// Do not assert this condition if there's already another error reported.
#define assert_if_no_error(cond, msg) \
  vmassert((cond) || VMError::is_error_reported(), msg)
bool xmlStream::inside_attrs_or_error() {
  return inside_attrs() || VMError::is_error_reported();
}

void xmlStream::initialize(outputStream* out) {
  _out = out;
  _last_flush = 0;
  _markup_state = BODY;
  _text_init._outer_xmlStream = this;
  _text = &_text_init;
  _log_only = _text;

#ifdef ASSERT
  _element_depth = 0;
  int   init_len = 100;
  char* init_buf = NEW_C_HEAP_ARRAY(char, init_len, mtInternal);
  _element_close_stack_low  = init_buf;
  _element_close_stack_high = init_buf + init_len;
  _element_close_stack_ptr  = init_buf + init_len - 1;
  _element_close_stack_ptr[0] = '\0';
#endif

  // Make sure each log uses the same base for time stamps.
  if (is_open()) {
    _out->time_stamp().update_to(1);
  }
}

#ifdef ASSERT
xmlStream::~xmlStream() {
  FREE_C_HEAP_ARRAY(char, _element_close_stack_low);
}
#endif

// Pass the given chars directly to _out.
void xmlStream::write(const char* s, size_t len) {
  if (!is_open())  return;

  out()->write(s, len);
  update_position(s, len);
}


// Pass the given chars directly to _out, except that
// we watch for special "<&>" chars.
// This is suitable for either attribute text or for body text.
// We don't fool with "<![CDATA[" quotes, just single-character entities.
// This makes it easier for dumb tools to parse the output.
void xmlStream::write_text(const char* s, size_t len) {
  if (!is_open())  return;

  size_t written = 0;
  // All normally printed material goes inside XML quotes.
  // This leaves the output free to include markup also.
  // Scan the string looking for inadvertent "<&>" chars
  for (size_t i = 0; i < len; i++) {
    char ch = s[i];
    // Escape special chars.
    const char* esc = nullptr;
    switch (ch) {
      // These are important only in attrs, but we do them always:
    case '\'': esc = "&apos;"; break;
    case '"':  esc = "&quot;"; break;
    case '<':  esc = "&lt;";   break;
    case '&':  esc = "&amp;";  break;
      // This is a freebie.
    case '>':  esc = "&gt;";   break;
    }
    if (esc != nullptr) {
      if (written < i) {
        out()->write(&s[written], i - written);
        written = i;
      }
      out()->print_raw(esc);
      written++;
    }
  }

  // Print the clean remainder.  Usually, it is all of s.
  if (written < len) {
    out()->write(&s[written], len - written);
  }
}

// ------------------------------------------------------------------
// Outputs XML text, with special characters quoted.
void xmlStream::text(const char* format, ...) {
  va_list ap;
  va_start(ap, format);
  va_text(format, ap);
  va_end(ap);
}

// ------------------------------------------------------------------
// Outputs XML attribute, with quotes and special characters quoted.
void xmlStream::attr(const char* attr, const char* format, ...) {
  va_list ap;
  va_start(ap, format);
  va_attr(attr, format, ap);
  va_end(ap);
}

#define BUFLEN 2*K   /* max size of output of individual print methods */

// ------------------------------------------------------------------
void xmlStream::va_tag(bool push, const char* format, va_list ap) {
  assert_if_no_error(!inside_attrs(), "cannot print tag inside attrs");
  char buffer[BUFLEN];
  size_t len;
  const char* kind = do_vsnprintf(buffer, BUFLEN, format, ap, false, len);
  see_tag(kind, push);
  // make sure all opening and/or closing tags begin in the first column
  if (out()->position() > 0)  print_raw("\n");
  print_raw("<");
  write(kind, len);
  _markup_state = (push ? HEAD : ELEM);
}

#ifdef ASSERT
/// Debugging goo to make sure element tags nest properly.

// ------------------------------------------------------------------
void xmlStream::see_tag(const char* tag, bool push) {
  assert_if_no_error(!inside_attrs(), "cannot start new element inside attrs");
  if (!push)  return;

  // tag goes up until either null or space:
  const char* tag_end = strchr(tag, ' ');
  size_t tag_len = (tag_end == nullptr) ? strlen(tag) : tag_end - tag;
  assert(tag_len > 0, "tag must not be empty");
  // push the tag onto the stack, pulling down the pointer
  char* old_ptr  = _element_close_stack_ptr;
  char* old_low  = _element_close_stack_low;
  char* push_ptr = old_ptr - (tag_len+1);
  if (push_ptr < old_low) {
    int old_len = pointer_delta_as_int(_element_close_stack_high, old_ptr);
    int new_len = old_len * 2;
    if (new_len < 100)  new_len = 100;
    char* new_low  = NEW_C_HEAP_ARRAY(char, new_len, mtInternal);
    char* new_high = new_low + new_len;
    char* new_ptr  = new_high - old_len;
    memcpy(new_ptr, old_ptr, old_len);
    _element_close_stack_high = new_high;
    _element_close_stack_low  = new_low;
    _element_close_stack_ptr  = new_ptr;
    FREE_C_HEAP_ARRAY(char, old_low);
    push_ptr = new_ptr - (tag_len+1);
  }
  assert(push_ptr >= _element_close_stack_low, "in range");
  memcpy(push_ptr, tag, tag_len);
  push_ptr[tag_len] = 0;
  _element_close_stack_ptr = push_ptr;
  _element_depth += 1;
}

// ------------------------------------------------------------------
void xmlStream::pop_tag(const char* tag) {
  assert_if_no_error(!inside_attrs(), "cannot close element inside attrs");
  assert(_element_depth > 0, "must be in an element to close");
  assert(*tag != 0, "tag must not be empty");
  char* cur_tag = _element_close_stack_ptr;
  bool  bad_tag = false;
  while (*cur_tag != 0 && strcmp(cur_tag, tag) != 0) {
    this->print_cr("</%s> <!-- missing closing tag -->", cur_tag);
    _element_close_stack_ptr = (cur_tag += strlen(cur_tag) + 1);
    _element_depth -= 1;
    bad_tag = true;
  }
  if (*cur_tag == 0) {
    bad_tag = true;
  } else {
    // Pop the stack, by skipping over the tag and its null.
    _element_close_stack_ptr = cur_tag + strlen(cur_tag) + 1;
    _element_depth -= 1;
  }
  if (bad_tag && !VMThread::should_terminate() && !VM_Exit::vm_exited() &&
      !VMError::is_error_reported())
  {
    assert(false, "bad tag in log");
  }
}
#endif


// ------------------------------------------------------------------
// First word in formatted string is element kind, and any subsequent
// words must be XML attributes.  Outputs "<kind .../>".
void xmlStream::elem(const char* format, ...) {
  va_list ap;
  va_start(ap, format);
  va_elem(format, ap);
  va_end(ap);
}

// ------------------------------------------------------------------
void xmlStream::va_elem(const char* format, va_list ap) {
  va_begin_elem(format, ap);
  end_elem();
}


// ------------------------------------------------------------------
// First word in formatted string is element kind, and any subsequent
// words must be XML attributes.  Outputs "<kind ...", not including "/>".
void xmlStream::begin_elem(const char* format, ...) {
  va_list ap;
  va_start(ap, format);
  va_tag(false, format, ap);
  va_end(ap);
}

// ------------------------------------------------------------------
void xmlStream::va_begin_elem(const char* format, va_list ap) {
  va_tag(false, format, ap);
}

// ------------------------------------------------------------------
// Outputs "/>".
void xmlStream::end_elem() {
  assert(_markup_state == ELEM, "misplaced end_elem");
  print_raw("/>\n");
  _markup_state = BODY;
}

// ------------------------------------------------------------------
// Outputs formatted text, followed by "/>".
void xmlStream::end_elem(const char* format, ...) {
  va_list ap;
  va_start(ap, format);
  out()->vprint(format, ap);
  va_end(ap);
  end_elem();
}


// ------------------------------------------------------------------
// First word in formatted string is element kind, and any subsequent
// words must be XML attributes.  Outputs "<kind ...>".
void xmlStream::head(const char* format, ...) {
  va_list ap;
  va_start(ap, format);
  va_head(format, ap);
  va_end(ap);
}

// ------------------------------------------------------------------
void xmlStream::va_head(const char* format, va_list ap) {
  va_begin_head(format, ap);
  end_head();
}

// ------------------------------------------------------------------
// First word in formatted string is element kind, and any subsequent
// words must be XML attributes.  Outputs "<kind ...", not including ">".
void xmlStream::begin_head(const char* format, ...) {
  va_list ap;
  va_start(ap, format);
  va_tag(true, format, ap);
  va_end(ap);
}

// ------------------------------------------------------------------
void xmlStream::va_begin_head(const char* format, va_list ap) {
  va_tag(true, format, ap);
}

// ------------------------------------------------------------------
// Outputs ">".
void xmlStream::end_head() {
  assert(_markup_state == HEAD, "misplaced end_head");
  print_raw(">\n");
  _markup_state = BODY;
}


// ------------------------------------------------------------------
// Outputs formatted text, followed by ">".
void xmlStream::end_head(const char* format, ...) {
  va_list ap;
  va_start(ap, format);
  out()->vprint(format, ap);
  va_end(ap);
  end_head();
}


// ------------------------------------------------------------------
// Outputs "</kind>".
void xmlStream::tail(const char* kind) {
  pop_tag(kind);
  // make sure all opening and/or closing tags begin in the first column
  if (out()->position() > 0)  print_raw("\n");
  print_raw("</");
  print_raw(kind);
  print_raw(">\n");
}

// ------------------------------------------------------------------
// Outputs "<kind_done ... stamp='D.DD'/> </kind>".
void xmlStream::done(const char* format, ...) {
  va_list ap;
  va_start(ap, format);
  va_done(format, ap);
  va_end(ap);
}

// ------------------------------------------------------------------
// Outputs "<kind_done stamp='D.DD'/> </kind>".
// Because done_raw() doesn't need to format strings, it's simpler than
// done(), and can be called safely by fatal error handler.
void xmlStream::done_raw(const char* kind) {
  print_raw("<");
  print_raw(kind);
  print_raw("_done stamp='");
  out()->stamp();
  print_raw_cr("'/>");
  print_raw("</");
  print_raw(kind);
  print_raw_cr(">");
}

// If you remove the PRAGMA, this fails to compile with clang-503.0.40.
PRAGMA_DIAG_PUSH
PRAGMA_FORMAT_NONLITERAL_IGNORED
// ------------------------------------------------------------------
void xmlStream::va_done(const char* format, va_list ap) {
  char buffer[200];
  size_t format_len = strlen(format);
  guarantee(format_len + 10 < sizeof(buffer), "bigger format buffer");
  const char* kind = format;
  const char* kind_end = strchr(kind, ' ');
  size_t kind_len;
  if (kind_end != nullptr) {
    kind_len = kind_end - kind;
    int n = os::snprintf(buffer, sizeof(buffer), "%.*s_done%s", (int)kind_len, kind, kind + kind_len);
    assert((size_t)n < sizeof(buffer), "Unexpected number of characters in string");
  } else {
    kind_len = format_len;
    int n = os::snprintf(buffer, sizeof(buffer), "%s_done", kind);
    assert((size_t)n < sizeof(buffer), "Unexpected number of characters in string");
  }
  // Output the trailing event with the timestamp.
  va_begin_elem(buffer, ap);
  stamp();
  end_elem();
  // Output the tail-tag of the enclosing element.
  buffer[kind_len] = 0;
  tail(buffer);
}
PRAGMA_DIAG_POP

// Output a timestamp attribute.
void xmlStream::stamp() {
  assert(inside_attrs_or_error(), "stamp must be an attribute");
  print_raw(" stamp='");
  out()->stamp();
  print_raw("'");
}


// ------------------------------------------------------------------
// Output a method attribute, in the form " method='pkg/cls name sig'".
// This is used only when there is no ciMethod available.
void xmlStream::method(Method* method, const char* pfx) {
  assert(inside_attrs_or_error(), "printing attributes");
  if (method == nullptr)  return;
  if (*pfx) {
    print(" %smethod='", pfx);
    method_text(method);
    print_raw("'");
    return;
  }
  print(" method='");
  method_text(method);
  print("' bytes='%d'", method->code_size());
  print(" count='%d'", method->invocation_count());
  if (AOTRecordTraining) {
    // print stuff about this method's compilation history
    print(" highest_comp_level='%d'", method->highest_comp_level());
    nmethod* nm = method->code();
    if (nm != nullptr)  print(" last_compile_id='%d'", nm->compile_id());
  }
  int bec = method->backedge_count();
  if (bec != 0)  print(" backedge_count='%d'", bec);
  print(" iicount='%d'", method->interpreter_invocation_count());
  int throwouts = method->interpreter_throwout_count();
  if (throwouts != 0)  print(" throwouts='%d'", throwouts);
  MethodData* mdo = method->method_data();
  if (mdo != nullptr) {
    uint cnt;
    cnt = mdo->decompile_count();
    if (cnt != 0)  print(" decompiles='%d'", cnt);
    for (uint reason = 0; reason < mdo->trap_reason_limit(); reason++) {
      cnt = mdo->trap_count(reason);
      if (cnt != 0)  print(" %s_traps='%d'", Deoptimization::trap_reason_name(reason), cnt);
    }
    cnt = mdo->overflow_trap_count();
    if (cnt != 0)  print(" overflow_traps='%d'", cnt);
    cnt = mdo->overflow_recompile_count();
    if (cnt != 0)  print(" overflow_recompiles='%d'", cnt);
  }
}

void xmlStream::method_text(Method* method) {
  ResourceMark rm;
  assert(inside_attrs_or_error(), "printing attributes");
  if (method == nullptr)  return;
  text()->print("%s", method->method_holder()->external_name());
  print_raw(" ");  // " " is easier for tools to parse than "::"
  method->name()->print_symbol_on(text());
  print_raw(" ");  // separator
  method->signature()->print_symbol_on(text());
}


// ------------------------------------------------------------------
// Output a klass attribute, in the form " klass='pkg/cls'".
// This is used only when there is no ciKlass available.
void xmlStream::klass(Klass* klass, const char* pfx) {
  assert(inside_attrs_or_error(), "printing attributes");
  if (klass == nullptr) return;
  print(" %sklass='", pfx);
  klass_text(klass);
  print_raw("'");
  if (klass->class_loader() != nullptr) {
    loader(klass->class_loader(), pfx);
  }
}

void xmlStream::klass_text(Klass* klass) {
  assert(inside_attrs_or_error(), "printing attributes");
  if (klass == nullptr) return;
  //klass->print_short_name(log->out());
  klass->name()->print_symbol_on(out());
  if (klass->is_hidden()) {
    out()->print(" //hidden");
    // FIXME:  maybe hash the contents of its classfile
  }
}

void xmlStream::loader(oop cl, const char* pfx) {
  assert(inside_attrs_or_error(), "printing attributes");
  if (cl == nullptr) return;
  print(" %sloader='", pfx);
  loader_text(cl);
  print_raw("'");
}

void xmlStream::loader_text(oop cl) {
  assert(inside_attrs_or_error(), "printing attributes");
  if (cl == nullptr) return;
  oop id = java_lang_ClassLoader::nameAndId(cl);
  if (id != nullptr)  string_text(id);
}

void xmlStream::name(const Symbol* name, const char* pfx) {
  assert(inside_attrs_or_error(), "printing attributes");
  if (name == nullptr)  return;
  print(" %sname='", pfx);
  symbol_text(name);
  print_raw("'");
}

void xmlStream::signature(const Symbol* sig, const char* pfx) {
  assert(inside_attrs_or_error(), "printing attributes");
  if (sig == nullptr)  return;
  print(" %ssignature='", pfx);
  symbol_text(sig);
  print_raw("'");
}

void xmlStream::symbol_text(const Symbol* name) {
  assert(inside_attrs_or_error(), "printing attributes");
  if (name == nullptr)  return;
  //name->print_short_name(text());
  //name->print_symbol_on(text());  // this has odd escapes in it (\\x%04x)
  const char* base = (const char*) name->base();
  int len = name->utf8_length();
  log_only()->write(base, len);
}

void xmlStream::string_text(oop str) {
  assert(inside_attrs_or_error(), "printing attributes");
  if (str == nullptr)  return;
  if (!java_lang_String::is_instance(str)) {
    print("*** not a string*** ");
    str->print_value_on(log_only());
    return;
  }
  ResourceMark rm;
  log_only()->print_raw(java_lang_String::as_utf8_string(str));
}

void xmlStream::thread(Thread* t, const char* pfx) {
  assert(inside_attrs_or_error(), "printing attributes");
  intx tid = t == nullptr ? os::current_thread_id() : t->osthread()->thread_id();
  guarantee(tid == (t == nullptr ? Thread::current() : t)->osthread()->thread_id(), "");
  print(" %sthread=%zd", pfx, tid);
}

void xmlStream::object(const char* attr, Handle x) {
  assert(inside_attrs_or_error(), "printing attributes");
  if (x == nullptr)  return;
  print_raw(" ");
  print_raw(attr);
  print_raw("='");
  object_text(x);
  print_raw("'");
}

void xmlStream::object_text(Handle x) {
  assert(inside_attrs_or_error(), "printing attributes");
  if (x.is_null())  return;
  if (java_lang_ClassLoader::is_instance(x())) {
    print_raw("loader:");
    loader_text(x());
    return;
  }
  if (java_lang_String::is_instance(x())) {
    print_raw("string:");
    string_text(x());
    return;
  }
  x->print_value_on(log_only());
}


void xmlStream::object(const char* attr, Metadata* x) {
  assert(inside_attrs_or_error(), "printing attributes");
  if (x == nullptr)  return;
  print_raw(" ");
  print_raw(attr);
  print_raw("='");
  object_text(x);
  print_raw("'");
}

void xmlStream::object_text(Metadata* x) {
  assert(inside_attrs_or_error(), "printing attributes");
  if (x == nullptr)  return;
  //x->print_value_on(text());
  if (x->is_method())
    method_text((Method*)x);
  else if (x->is_klass())
    klass_text((Klass*)x);
  else
    ShouldNotReachHere(); // Add impl if this is reached.
}


void xmlStream::flush() {
  out()->flush();
  _last_flush = count();
}

void xmlTextStream::flush() {
  if (_outer_xmlStream == nullptr)  return;
  _outer_xmlStream->flush();
}

void xmlTextStream::write(const char* str, size_t len) {
  if (_outer_xmlStream == nullptr)  return;
  _outer_xmlStream->write_text(str, len);
  update_position(str, len);
}
