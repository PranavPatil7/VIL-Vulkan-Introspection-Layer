/*
 * from backward.hpp
 * Copyright 2013 Google Inc. All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <backward/common.hpp>
#include <backward/resolve.hpp>
#include <mutex>
#include <fstream>

namespace backward {
namespace details {

template <typename K, typename V> struct hashtable {
  typedef std::unordered_map<K, V> type;
};

#if defined(BACKWARD_SYSTEM_WINDOWS)
const char kBackwardPathDelimiter[] = ";";
#else
const char kBackwardPathDelimiter[] = ":";
#endif

template <typename T> struct rm_ptr { typedef T type; };

template <typename T> struct rm_ptr<T *> { typedef T type; };

template <typename T> struct rm_ptr<const T *> { typedef const T type; };

template <typename R, typename T, R (*F)(T)> struct deleter {
  template <typename U> void operator()(U &ptr) const { (*F)(ptr); }
};

template <typename T> struct default_delete {
  void operator()(T &ptr) const { delete ptr; }
};

template <typename T, typename Deleter = deleter<void, void *, &::free>>
class handle {
  struct dummy;
  T _val;
  bool _empty;

#ifdef BACKWARD_ATLEAST_CXX11
  handle(const handle &) = delete;
  handle &operator=(const handle &) = delete;
#endif

public:
  ~handle() {
    if (!_empty) {
      Deleter()(_val);
    }
  }

  explicit handle() : _val(), _empty(true) {}
  explicit handle(T val) : _val(val), _empty(false) {
    if (!_val)
      _empty = true;
  }

#ifdef BACKWARD_ATLEAST_CXX11
  handle(handle &&from) : _empty(true) { swap(from); }
  handle &operator=(handle &&from) {
    swap(from);
    return *this;
  }
#else
  explicit handle(const handle &from) : _empty(true) {
    // some sort of poor man's move semantic.
    swap(const_cast<handle &>(from));
  }
  handle &operator=(const handle &from) {
    // some sort of poor man's move semantic.
    swap(const_cast<handle &>(from));
    return *this;
  }
#endif

  void reset(T new_val) {
    handle tmp(new_val);
    swap(tmp);
  }

  void update(T new_val) {
    _val = new_val;
    _empty = !static_cast<bool>(new_val);
  }

  operator const dummy *() const {
    if (_empty) {
      return nullptr;
    }
    return reinterpret_cast<const dummy *>(_val);
  }
  T get() { return _val; }
  T release() {
    _empty = true;
    return _val;
  }
  void swap(handle &b) {
    using std::swap;
    swap(b._val, _val);     // can throw, we are safe here.
    swap(b._empty, _empty); // should not throw: if you cannot swap two
    // bools without throwing... It's a lost cause anyway!
  }

  T &operator->() { return _val; }
  const T &operator->() const { return _val; }

  typedef typename rm_ptr<T>::type &ref_t;
  typedef const typename rm_ptr<T>::type &const_ref_t;
  ref_t operator*() { return *_val; }
  const_ref_t operator*() const { return *_val; }
  ref_t operator[](size_t idx) { return _val[idx]; }

  // Watch out, we've got a badass over here
  T *operator&() {
    _empty = false;
    return &_val;
  }
};

// Default demangler implementation (do nothing).
template <typename TAG> struct demangler_impl {
  static std::string demangle(const char *funcname) { return funcname; }
};

#if defined(BACKWARD_SYSTEM_LINUX) || defined(BACKWARD_SYSTEM_DARWIN)

template <> struct demangler_impl<system_tag::current_tag> {
  demangler_impl() : _demangle_buffer_length(0) {}

  std::string demangle(const char *funcname) {
    using namespace details;
    char *result = abi::__cxa_demangle(funcname, _demangle_buffer.get(),
                                       &_demangle_buffer_length, nullptr);
    if (result) {
      _demangle_buffer.update(result);
      return result;
    }
    return funcname;
  }

private:
  details::handle<char *> _demangle_buffer;
  size_t _demangle_buffer_length;
};

#endif // BACKWARD_SYSTEM_LINUX || BACKWARD_SYSTEM_DARWIN

struct demangler : public demangler_impl<system_tag::current_tag> {};

// Split a string on the platform's PATH delimiter.  Example: if delimiter
// is ":" then:
//   ""              --> []
//   ":"             --> ["",""]
//   "::"            --> ["","",""]
//   "/a/b/c"        --> ["/a/b/c"]
//   "/a/b/c:/d/e/f" --> ["/a/b/c","/d/e/f"]
//   etc.
inline std::vector<std::string> split_source_prefixes(const std::string &s) {
  std::vector<std::string> out;
  size_t last = 0;
  size_t next = 0;
  size_t delimiter_size = sizeof(kBackwardPathDelimiter) - 1;
  while ((next = s.find(kBackwardPathDelimiter, last)) != std::string::npos) {
    out.push_back(s.substr(last, next - last));
    last = next + delimiter_size;
  }
  if (last <= s.length()) {
    out.push_back(s.substr(last));
  }
  return out;
}

} // namespace details

/*************** A TRACE ***************/

struct Trace {
  void *addr;
  size_t idx;

  Trace() : addr(nullptr), idx(0) {}

  explicit Trace(void *_addr, size_t _idx) : addr(_addr), idx(_idx) {}
};

struct ResolvedTrace : public Trace {
  using SourceLoc = backward::SourceLoc;

  // In which binary object this trace is located.
  std::string object_filename;

  // The function in the object that contain the trace. This is not the same
  // as source.function which can be an function inlined in object_function.
  std::string object_function;

  // The source location of this trace. It is possible for filename to be
  // empty and for line/col to be invalid (value 0) if this information
  // couldn't be deduced, for example if there is no debug information in the
  // binary object.
  SourceLoc source;

  // An optionals list of "inliners". All the successive sources location
  // from where the source location of the trace (the attribute right above)
  // is inlined. It is especially useful when you compiled with optimization.
  typedef std::vector<SourceLoc> source_locs_t;
  source_locs_t inliners;

  ResolvedTrace() : Trace() {}
  ResolvedTrace(const Trace &mini_trace) : Trace(mini_trace) {}
};

/*************** TRACE RESOLVER ***************/
class TraceResolverImplBase {
public:
  virtual ~TraceResolverImplBase() {}

  virtual void load_addresses(void *const*addresses, int address_count) {
    (void)addresses;
    (void)address_count;
  }

  template <class ST> void load_stacktrace(ST &st) {
    load_addresses(st.begin(), (int)st.size());
  }

  virtual ResolvedTrace resolve(ResolvedTrace t) { return t; }

protected:
  std::string demangle(const char *funcname) {
    return _demangler.demangle(funcname);
  }

private:
  details::demangler _demangler;
};

template <typename TAG> class TraceResolverImpl;

#ifdef BACKWARD_SYSTEM_UNKNOWN

template <> class TraceResolverImpl<system_tag::unknown_tag>
    : public TraceResolverImplBase {};

#endif

#ifdef BACKWARD_SYSTEM_LINUX

class TraceResolverLinuxBase : public TraceResolverImplBase {
public:
  TraceResolverLinuxBase()
      : argv0_(get_argv0()), exec_path_(read_symlink("/proc/self/exe")) {}
  std::string resolve_exec_path(Dl_info &symbol_info) const {
    // mutates symbol_info.dli_fname to be filename to open and returns filename
    // to display
    if (symbol_info.dli_fname == argv0_) {
      // dladdr returns argv[0] in dli_fname for symbols contained in
      // the main executable, which is not a valid path if the
      // executable was found by a search of the PATH environment
      // variable; In that case, we actually open /proc/self/exe, which
      // is always the actual executable (even if it was deleted/replaced!)
      // but display the path that /proc/self/exe links to.
      // However, this right away reduces probability of successful symbol
      // resolution, because libbfd may try to find *.debug files in the
      // same dir, in case symbols are stripped. As a result, it may try
      // to find a file /proc/self/<exe_name>.debug, which obviously does
      // not exist. /proc/self/exe is a last resort. First load attempt
      // should go for the original executable file path.
      symbol_info.dli_fname = "/proc/self/exe";
      return exec_path_;
    } else {
      return symbol_info.dli_fname;
    }
  }

private:
  std::string argv0_;
  std::string exec_path_;

  static std::string get_argv0() {
    std::string argv0;
    std::ifstream ifs("/proc/self/cmdline");
    std::getline(ifs, argv0, '\0');
    return argv0;
  }

  static std::string read_symlink(std::string const &symlink_path) {
    std::string path;
    path.resize(100);

    while (true) {
      ssize_t len =
          ::readlink(symlink_path.c_str(), &*path.begin(), path.size());
      if (len < 0) {
        return "";
      }
      if (static_cast<size_t>(len) == path.size()) {
        path.resize(path.size() * 2);
      } else {
        path.resize(static_cast<std::string::size_type>(len));
        break;
      }
    }

    return path;
  }
};

template <typename STACKTRACE_TAG> class TraceResolverLinuxImpl;

#if BACKWARD_HAS_BACKTRACE_SYMBOL == 1

template <>
class TraceResolverLinuxImpl<trace_resolver_tag::backtrace_symbol>
    : public TraceResolverLinuxBase {
public:
  void load_addresses(void *const*addresses, int address_count) override {
    if (address_count == 0) {
      return;
    }
    _symbols.reset(backtrace_symbols(addresses, address_count));
  }

  ResolvedTrace resolve(ResolvedTrace trace) override {
    char *filename = _symbols[trace.idx];
    char *funcname = filename;
    while (*funcname && *funcname != '(') {
      funcname += 1;
    }
    trace.object_filename.assign(filename,
                                 funcname); // ok even if funcname is the ending
                                            // \0 (then we assign entire string)

    if (*funcname) { // if it's not end of string (e.g. from last frame ip==0)
      funcname += 1;
      char *funcname_end = funcname;
      while (*funcname_end && *funcname_end != ')' && *funcname_end != '+') {
        funcname_end += 1;
      }
      *funcname_end = '\0';
      trace.object_function = this->demangle(funcname);
      trace.source.function = trace.object_function; // we cannot do better.
    }
    return trace;
  }

private:
  details::handle<char **> _symbols;
};

#endif // BACKWARD_HAS_BACKTRACE_SYMBOL == 1

#if BACKWARD_HAS_BFD == 1

template <>
class TraceResolverLinuxImpl<trace_resolver_tag::libbfd>
    : public TraceResolverLinuxBase {
public:
  TraceResolverLinuxImpl() : _bfd_loaded(false) {}

  ResolvedTrace resolve(ResolvedTrace trace) override {
    Dl_info symbol_info;

    // trace.addr is a virtual address in memory pointing to some code.
    // Let's try to find from which loaded object it comes from.
    // The loaded object can be yourself btw.
    if (!dladdr(trace.addr, &symbol_info)) {
      return trace; // dat broken trace...
    }

    // Now we get in symbol_info:
    // .dli_fname:
    //		pathname of the shared object that contains the address.
    // .dli_fbase:
    //		where the object is loaded in memory.
    // .dli_sname:
    //		the name of the nearest symbol to trace.addr, we expect a
    //		function name.
    // .dli_saddr:
    //		the exact address corresponding to .dli_sname.

    if (symbol_info.dli_sname) {
      trace.object_function = demangle(symbol_info.dli_sname);
    }

    if (!symbol_info.dli_fname) {
      return trace;
    }

    trace.object_filename = resolve_exec_path(symbol_info);
    bfd_fileobject *fobj;
    // Before rushing to resolution need to ensure the executable
    // file still can be used. For that compare inode numbers of
    // what is stored by the executable's file path, and in the
    // dli_fname, which not necessarily equals to the executable.
    // It can be a shared library, or /proc/self/exe, and in the
    // latter case has drawbacks. See the exec path resolution for
    // details. In short - the dli object should be used only as
    // the last resort.
    // If inode numbers are equal, it is known dli_fname and the
    // executable file are the same. This is guaranteed by Linux,
    // because if the executable file is changed/deleted, it will
    // be done in a new inode. The old file will be preserved in
    // /proc/self/exe, and may even have inode 0. The latter can
    // happen if the inode was actually reused, and the file was
    // kept only in the main memory.
    //
    struct stat obj_stat;
    struct stat dli_stat;
    if (stat(trace.object_filename.c_str(), &obj_stat) == 0 &&
        stat(symbol_info.dli_fname, &dli_stat) == 0 &&
        obj_stat.st_ino == dli_stat.st_ino) {
      // The executable file, and the shared object containing the
      // address are the same file. Safe to use the original path.
      // this is preferable. Libbfd will search for stripped debug
      // symbols in the same directory.
      fobj = load_object_with_bfd(trace.object_filename);
    } else{
      // The original object file was *deleted*! The only hope is
      // that the debug symbols are either inside the shared
      // object file, or are in the same directory, and this is
      // not /proc/self/exe.
      fobj = nullptr;
    }
    if (fobj == nullptr || !fobj->handle) {
      fobj = load_object_with_bfd(symbol_info.dli_fname);
      if (!fobj->handle) {
        return trace;
      }
    }

    find_sym_result *details_selected; // to be filled.

    // trace.addr is the next instruction to be executed after returning
    // from the nested stack frame. In C++ this usually relate to the next
    // statement right after the function call that leaded to a new stack
    // frame. This is not usually what you want to see when printing out a
    // stacktrace...
    find_sym_result details_call_site =
        find_symbol_details(fobj, trace.addr, symbol_info.dli_fbase);
    details_selected = &details_call_site;

#if BACKWARD_HAS_UNWIND == 0
    // ...this is why we also try to resolve the symbol that is right
    // before the return address. If we are lucky enough, we will get the
    // line of the function that was called. But if the code is optimized,
    // we might get something absolutely not related since the compiler
    // can reschedule the return address with inline functions and
    // tail-call optimisation (among other things that I don't even know
    // or cannot even dream about with my tiny limited brain).
    find_sym_result details_adjusted_call_site = find_symbol_details(
        fobj, (void *)(uintptr_t(trace.addr) - 1), symbol_info.dli_fbase);

    // In debug mode, we should always get the right thing(TM).
    if (details_call_site.found && details_adjusted_call_site.found) {
      // Ok, we assume that details_adjusted_call_site is a better estimation.
      details_selected = &details_adjusted_call_site;
      trace.addr = (void *)(uintptr_t(trace.addr) - 1);
    }

    if (details_selected == &details_call_site && details_call_site.found) {
      // we have to re-resolve the symbol in order to reset some
      // internal state in BFD... so we can call backtrace_inliners
      // thereafter...
      details_call_site =
          find_symbol_details(fobj, trace.addr, symbol_info.dli_fbase);
    }
#endif // BACKWARD_HAS_UNWIND

    if (details_selected->found) {
      if (details_selected->filename) {
        trace.source.filename = details_selected->filename;
      }
      trace.source.line = details_selected->line;

      if (details_selected->funcname) {
        // this time we get the name of the function where the code is
        // located, instead of the function were the address is
        // located. In short, if the code was inlined, we get the
        // function correspoding to the code. Else we already got in
        // trace.function.
        trace.source.function = demangle(details_selected->funcname);

        if (!symbol_info.dli_sname) {
          // for the case dladdr failed to find the symbol name of
          // the function, we might as well try to put something
          // here.
          trace.object_function = trace.source.function;
        }
      }

      // Maybe the source of the trace got inlined inside the function
      // (trace.source.function). Let's see if we can get all the inlined
      // calls along the way up to the initial call site.
      trace.inliners = backtrace_inliners(fobj, *details_selected);

#if 0
			if (trace.inliners.size() == 0) {
				// Maybe the trace was not inlined... or maybe it was and we
				// are lacking the debug information. Let's try to make the
				// world better and see if we can get the line number of the
				// function (trace.source.function) now.
				//
				// We will get the location of where the function start (to be
				// exact: the first instruction that really start the
				// function), not where the name of the function is defined.
				// This can be quite far away from the name of the function
				// btw.
				//
				// If the source of the function is the same as the source of
				// the trace, we cannot say if the trace was really inlined or
				// not.  However, if the filename of the source is different
				// between the function and the trace... we can declare it as
				// an inliner.  This is not 100% accurate, but better than
				// nothing.

				if (symbol_info.dli_saddr) {
					find_sym_result details = find_symbol_details(fobj,
							symbol_info.dli_saddr,
							symbol_info.dli_fbase);

					if (details.found) {
						ResolvedTrace::SourceLoc diy_inliner;
						diy_inliner.line = details.line;
						if (details.filename) {
							diy_inliner.filename = details.filename;
						}
						if (details.funcname) {
							diy_inliner.function = demangle(details.funcname);
						} else {
							diy_inliner.function = trace.source.function;
						}
						if (diy_inliner != trace.source) {
							trace.inliners.push_back(diy_inliner);
						}
					}
				}
			}
#endif
    }

    return trace;
  }

private:
  bool _bfd_loaded;

  typedef details::handle<bfd *,
                          details::deleter<bfd_boolean, bfd *, &bfd_close>>
      bfd_handle_t;

  typedef details::handle<asymbol **> bfd_symtab_t;

  struct bfd_fileobject {
    bfd_handle_t handle;
    bfd_vma base_addr;
    bfd_symtab_t symtab;
    bfd_symtab_t dynamic_symtab;
  };

  typedef details::hashtable<std::string, bfd_fileobject>::type fobj_bfd_map_t;
  fobj_bfd_map_t _fobj_bfd_map;

  bfd_fileobject *load_object_with_bfd(const std::string &filename_object) {
    using namespace details;

    if (!_bfd_loaded) {
      using namespace details;
      bfd_init();
      _bfd_loaded = true;
    }

    fobj_bfd_map_t::iterator it = _fobj_bfd_map.find(filename_object);
    if (it != _fobj_bfd_map.end()) {
      return &it->second;
    }

    // this new object is empty for now.
    bfd_fileobject *r = &_fobj_bfd_map[filename_object];

    // we do the work temporary in this one;
    bfd_handle_t bfd_handle;

    int fd = open(filename_object.c_str(), O_RDONLY);
    bfd_handle.reset(bfd_fdopenr(filename_object.c_str(), "default", fd));
    if (!bfd_handle) {
      close(fd);
      return r;
    }

    if (!bfd_check_format(bfd_handle.get(), bfd_object)) {
      return r; // not an object? You lose.
    }

    if ((bfd_get_file_flags(bfd_handle.get()) & HAS_SYMS) == 0) {
      return r; // that's what happen when you forget to compile in debug.
    }

    ssize_t symtab_storage_size = bfd_get_symtab_upper_bound(bfd_handle.get());

    ssize_t dyn_symtab_storage_size =
        bfd_get_dynamic_symtab_upper_bound(bfd_handle.get());

    if (symtab_storage_size <= 0 && dyn_symtab_storage_size <= 0) {
      return r; // weird, is the file is corrupted?
    }

    bfd_symtab_t symtab, dynamic_symtab;
    ssize_t symcount = 0, dyn_symcount = 0;

    if (symtab_storage_size > 0) {
      symtab.reset(static_cast<bfd_symbol **>(
          malloc(static_cast<size_t>(symtab_storage_size))));
      symcount = bfd_canonicalize_symtab(bfd_handle.get(), symtab.get());
    }

    if (dyn_symtab_storage_size > 0) {
      dynamic_symtab.reset(static_cast<bfd_symbol **>(
          malloc(static_cast<size_t>(dyn_symtab_storage_size))));
      dyn_symcount = bfd_canonicalize_dynamic_symtab(bfd_handle.get(),
                                                     dynamic_symtab.get());
    }

    if (symcount <= 0 && dyn_symcount <= 0) {
      return r; // damned, that's a stripped file that you got there!
    }

    r->handle = move(bfd_handle);
    r->symtab = move(symtab);
    r->dynamic_symtab = move(dynamic_symtab);
    return r;
  }

  struct find_sym_result {
    bool found;
    const char *filename;
    const char *funcname;
    unsigned int line;
  };

  struct find_sym_context {
    TraceResolverLinuxImpl *self;
    bfd_fileobject *fobj;
    void *addr;
    void *base_addr;
    find_sym_result result;
  };

  find_sym_result find_symbol_details(bfd_fileobject *fobj, void *addr,
                                      void *base_addr) {
    find_sym_context context;
    context.self = this;
    context.fobj = fobj;
    context.addr = addr;
    context.base_addr = base_addr;
    context.result.found = false;
    bfd_map_over_sections(fobj->handle.get(), &find_in_section_trampoline,
                          static_cast<void *>(&context));
    return context.result;
  }

  static void find_in_section_trampoline(bfd *, asection *section, void *data) {
    find_sym_context *context = static_cast<find_sym_context *>(data);
    context->self->find_in_section(
        reinterpret_cast<bfd_vma>(context->addr),
        reinterpret_cast<bfd_vma>(context->base_addr), context->fobj, section,
        context->result);
  }

  void find_in_section(bfd_vma addr, bfd_vma base_addr, bfd_fileobject *fobj,
                       asection *section, find_sym_result &result) {
    if (result.found)
      return;

#ifdef bfd_get_section_flags
    if ((bfd_get_section_flags(fobj->handle.get(), section) & SEC_ALLOC) == 0)
#else
    if ((bfd_section_flags(section) & SEC_ALLOC) == 0)
#endif
      return; // a debug section is never loaded automatically.

#ifdef bfd_get_section_vma
    bfd_vma sec_addr = bfd_get_section_vma(fobj->handle.get(), section);
#else
    bfd_vma sec_addr = bfd_section_vma(section);
#endif
#ifdef bfd_get_section_size
    bfd_size_type size = bfd_get_section_size(section);
#else
    bfd_size_type size = bfd_section_size(section);
#endif

    // are we in the boundaries of the section?
    if (addr < sec_addr || addr >= sec_addr + size) {
      addr -= base_addr; // oups, a relocated object, lets try again...
      if (addr < sec_addr || addr >= sec_addr + size) {
        return;
      }
    }

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wzero-as-null-pointer-constant"
#endif
    if (!result.found && fobj->symtab) {
      result.found = bfd_find_nearest_line(
          fobj->handle.get(), section, fobj->symtab.get(), addr - sec_addr,
          &result.filename, &result.funcname, &result.line);
    }

    if (!result.found && fobj->dynamic_symtab) {
      result.found = bfd_find_nearest_line(
          fobj->handle.get(), section, fobj->dynamic_symtab.get(),
          addr - sec_addr, &result.filename, &result.funcname, &result.line);
    }
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
  }

  ResolvedTrace::source_locs_t
  backtrace_inliners(bfd_fileobject *fobj, find_sym_result previous_result) {
    // This function can be called ONLY after a SUCCESSFUL call to
    // find_symbol_details. The state is global to the bfd_handle.
    ResolvedTrace::source_locs_t results;
    while (previous_result.found) {
      find_sym_result result;
      result.found = bfd_find_inliner_info(fobj->handle.get(), &result.filename,
                                           &result.funcname, &result.line);

      if (result
              .found) /* and not (
                            cstrings_eq(previous_result.filename,
                         result.filename) and
                         cstrings_eq(previous_result.funcname, result.funcname)
                            and result.line == previous_result.line
                            )) */
      {
        ResolvedTrace::SourceLoc src_loc;
        src_loc.line = result.line;
        if (result.filename) {
          src_loc.filename = result.filename;
        }
        if (result.funcname) {
          src_loc.function = demangle(result.funcname);
        }
        results.push_back(src_loc);
      }
      previous_result = result;
    }
    return results;
  }

  bool cstrings_eq(const char *a, const char *b) {
    if (!a || !b) {
      return false;
    }
    return strcmp(a, b) == 0;
  }
};
#endif // BACKWARD_HAS_BFD == 1

#if BACKWARD_HAS_DW == 1

template <>
class TraceResolverLinuxImpl<trace_resolver_tag::libdw>
    : public TraceResolverLinuxBase {
public:
  TraceResolverLinuxImpl() : _dwfl_handle_initialized(false) {}

  ResolvedTrace resolve(ResolvedTrace trace) override {
    using namespace details;

    Dwarf_Addr trace_addr = (Dwarf_Addr)trace.addr;

    if (!_dwfl_handle_initialized) {
      // initialize dwfl...
      _dwfl_cb.reset(new Dwfl_Callbacks);
      _dwfl_cb->find_elf = &dwfl_linux_proc_find_elf;
      _dwfl_cb->find_debuginfo = &dwfl_standard_find_debuginfo;
      _dwfl_cb->debuginfo_path = 0;

      _dwfl_handle.reset(dwfl_begin(_dwfl_cb.get()));
      _dwfl_handle_initialized = true;

      if (!_dwfl_handle) {
        return trace;
      }

      // ...from the current process.
      dwfl_report_begin(_dwfl_handle.get());
      int r = dwfl_linux_proc_report(_dwfl_handle.get(), getpid());
      dwfl_report_end(_dwfl_handle.get(), NULL, NULL);
      if (r < 0) {
        return trace;
      }
    }

    if (!_dwfl_handle) {
      return trace;
    }

    // find the module (binary object) that contains the trace's address.
    // This is not using any debug information, but the addresses ranges of
    // all the currently loaded binary object.
    Dwfl_Module *mod = dwfl_addrmodule(_dwfl_handle.get(), trace_addr);
    if (mod) {
      // now that we found it, lets get the name of it, this will be the
      // full path to the running binary or one of the loaded library.
      const char *module_name = dwfl_module_info(mod, 0, 0, 0, 0, 0, 0, 0);
      if (module_name) {
        trace.object_filename = module_name;
      }
      // We also look after the name of the symbol, equal or before this
      // address. This is found by walking the symtab. We should get the
      // symbol corresponding to the function (mangled) containing the
      // address. If the code corresponding to the address was inlined,
      // this is the name of the out-most inliner function.
      const char *sym_name = dwfl_module_addrname(mod, trace_addr);
      if (sym_name) {
        trace.object_function = demangle(sym_name);
      }
    }

    // now let's get serious, and find out the source location (file and
    // line number) of the address.

    // This function will look in .debug_aranges for the address and map it
    // to the location of the compilation unit DIE in .debug_info and
    // return it.
    Dwarf_Addr mod_bias = 0;
    Dwarf_Die *cudie = dwfl_module_addrdie(mod, trace_addr, &mod_bias);

#if 1
    if (!cudie) {
      // Sadly clang does not generate the section .debug_aranges, thus
      // dwfl_module_addrdie will fail early. Clang doesn't either set
      // the lowpc/highpc/range info for every compilation unit.
      //
      // So in order to save the world:
      // for every compilation unit, we will iterate over every single
      // DIEs. Normally functions should have a lowpc/highpc/range, which
      // we will use to infer the compilation unit.

      // note that this is probably badly inefficient.
      while ((cudie = dwfl_module_nextcu(mod, cudie, &mod_bias))) {
        Dwarf_Die die_mem;
        Dwarf_Die *fundie =
            find_fundie_by_pc(cudie, trace_addr - mod_bias, &die_mem);
        if (fundie) {
          break;
        }
      }
    }
#endif

//#define BACKWARD_I_DO_NOT_RECOMMEND_TO_ENABLE_THIS_HORRIBLE_PIECE_OF_CODE
#ifdef BACKWARD_I_DO_NOT_RECOMMEND_TO_ENABLE_THIS_HORRIBLE_PIECE_OF_CODE
    if (!cudie) {
      // If it's still not enough, lets dive deeper in the shit, and try
      // to save the world again: for every compilation unit, we will
      // load the corresponding .debug_line section, and see if we can
      // find our address in it.

      Dwarf_Addr cfi_bias;
      Dwarf_CFI *cfi_cache = dwfl_module_eh_cfi(mod, &cfi_bias);

      Dwarf_Addr bias;
      while ((cudie = dwfl_module_nextcu(mod, cudie, &bias))) {
        if (dwarf_getsrc_die(cudie, trace_addr - bias)) {

          // ...but if we get a match, it might be a false positive
          // because our (address - bias) might as well be valid in a
          // different compilation unit. So we throw our last card on
          // the table and lookup for the address into the .eh_frame
          // section.

          handle<Dwarf_Frame *> frame;
          dwarf_cfi_addrframe(cfi_cache, trace_addr - cfi_bias, &frame);
          if (frame) {
            break;
          }
        }
      }
    }
#endif

    if (!cudie) {
      return trace; // this time we lost the game :/
    }

    // Now that we have a compilation unit DIE, this function will be able
    // to load the corresponding section in .debug_line (if not already
    // loaded) and hopefully find the source location mapped to our
    // address.
    Dwarf_Line *srcloc = dwarf_getsrc_die(cudie, trace_addr - mod_bias);

    if (srcloc) {
      const char *srcfile = dwarf_linesrc(srcloc, 0, 0);
      if (srcfile) {
        trace.source.filename = srcfile;
      }
      int line = 0, col = 0;
      dwarf_lineno(srcloc, &line);
      dwarf_linecol(srcloc, &col);
      trace.source.line = line;
      trace.source.col = col;
    }

    deep_first_search_by_pc(cudie, trace_addr - mod_bias,
                            inliners_search_cb(trace));
    if (trace.source.function.size() == 0) {
      // fallback.
      trace.source.function = trace.object_function;
    }

    return trace;
  }

private:
  typedef details::handle<Dwfl *, details::deleter<void, Dwfl *, &dwfl_end>>
      dwfl_handle_t;
  details::handle<Dwfl_Callbacks *, details::default_delete<Dwfl_Callbacks *>>
      _dwfl_cb;
  dwfl_handle_t _dwfl_handle;
  bool _dwfl_handle_initialized;

  // defined here because in C++98, template function cannot take locally
  // defined types... grrr.
  struct inliners_search_cb {
    void operator()(Dwarf_Die *die) {
      switch (dwarf_tag(die)) {
        const char *name;
      case DW_TAG_subprogram:
        if ((name = dwarf_diename(die))) {
          trace.source.function = name;
        }
        break;

      case DW_TAG_inlined_subroutine:
        ResolvedTrace::SourceLoc sloc;
        Dwarf_Attribute attr_mem;

        if ((name = dwarf_diename(die))) {
          sloc.function = name;
        }
        if ((name = die_call_file(die))) {
          sloc.filename = name;
        }

        Dwarf_Word line = 0, col = 0;
        dwarf_formudata(dwarf_attr(die, DW_AT_call_line, &attr_mem), &line);
        dwarf_formudata(dwarf_attr(die, DW_AT_call_column, &attr_mem), &col);
        sloc.line = (unsigned)line;
        sloc.col = (unsigned)col;

        trace.inliners.push_back(sloc);
        break;
      };
    }
    ResolvedTrace &trace;
    inliners_search_cb(ResolvedTrace &t) : trace(t) {}
  };

  static bool die_has_pc(Dwarf_Die *die, Dwarf_Addr pc) {
    Dwarf_Addr low, high;

    // continuous range
    if (dwarf_hasattr(die, DW_AT_low_pc) && dwarf_hasattr(die, DW_AT_high_pc)) {
      if (dwarf_lowpc(die, &low) != 0) {
        return false;
      }
      if (dwarf_highpc(die, &high) != 0) {
        Dwarf_Attribute attr_mem;
        Dwarf_Attribute *attr = dwarf_attr(die, DW_AT_high_pc, &attr_mem);
        Dwarf_Word value;
        if (dwarf_formudata(attr, &value) != 0) {
          return false;
        }
        high = low + value;
      }
      return pc >= low && pc < high;
    }

    // non-continuous range.
    Dwarf_Addr base;
    ptrdiff_t offset = 0;
    while ((offset = dwarf_ranges(die, offset, &base, &low, &high)) > 0) {
      if (pc >= low && pc < high) {
        return true;
      }
    }
    return false;
  }

  static Dwarf_Die *find_fundie_by_pc(Dwarf_Die *parent_die, Dwarf_Addr pc,
                                      Dwarf_Die *result) {
    if (dwarf_child(parent_die, result) != 0) {
      return 0;
    }

    Dwarf_Die *die = result;
    do {
      switch (dwarf_tag(die)) {
      case DW_TAG_subprogram:
      case DW_TAG_inlined_subroutine:
        if (die_has_pc(die, pc)) {
          return result;
        }
      };
      bool declaration = false;
      Dwarf_Attribute attr_mem;
      dwarf_formflag(dwarf_attr(die, DW_AT_declaration, &attr_mem),
                     &declaration);
      if (!declaration) {
        // let's be curious and look deeper in the tree,
        // function are not necessarily at the first level, but
        // might be nested inside a namespace, structure etc.
        Dwarf_Die die_mem;
        Dwarf_Die *indie = find_fundie_by_pc(die, pc, &die_mem);
        if (indie) {
          *result = die_mem;
          return result;
        }
      }
    } while (dwarf_siblingof(die, result) == 0);
    return 0;
  }

  template <typename CB>
  static bool deep_first_search_by_pc(Dwarf_Die *parent_die, Dwarf_Addr pc,
                                      CB cb) {
    Dwarf_Die die_mem;
    if (dwarf_child(parent_die, &die_mem) != 0) {
      return false;
    }

    bool branch_has_pc = false;
    Dwarf_Die *die = &die_mem;
    do {
      bool declaration = false;
      Dwarf_Attribute attr_mem;
      dwarf_formflag(dwarf_attr(die, DW_AT_declaration, &attr_mem),
                     &declaration);
      if (!declaration) {
        // let's be curious and look deeper in the tree, function are
        // not necessarily at the first level, but might be nested
        // inside a namespace, structure, a function, an inlined
        // function etc.
        branch_has_pc = deep_first_search_by_pc(die, pc, cb);
      }
      if (!branch_has_pc) {
        branch_has_pc = die_has_pc(die, pc);
      }
      if (branch_has_pc) {
        cb(die);
      }
    } while (dwarf_siblingof(die, &die_mem) == 0);
    return branch_has_pc;
  }

  static const char *die_call_file(Dwarf_Die *die) {
    Dwarf_Attribute attr_mem;
    Dwarf_Word file_idx = 0;

    dwarf_formudata(dwarf_attr(die, DW_AT_call_file, &attr_mem), &file_idx);

    if (file_idx == 0) {
      return 0;
    }

    Dwarf_Die die_mem;
    Dwarf_Die *cudie = dwarf_diecu(die, &die_mem, 0, 0);
    if (!cudie) {
      return 0;
    }

    Dwarf_Files *files = 0;
    size_t nfiles;
    dwarf_getsrcfiles(cudie, &files, &nfiles);
    if (!files) {
      return 0;
    }

    return dwarf_filesrc(files, file_idx, 0, 0);
  }
};
#endif // BACKWARD_HAS_DW == 1

#if BACKWARD_HAS_DWARF == 1

template <>
class TraceResolverLinuxImpl<trace_resolver_tag::libdwarf>
    : public TraceResolverLinuxBase {
public:
  TraceResolverLinuxImpl() : _dwarf_loaded(false) {}

  ResolvedTrace resolve(ResolvedTrace trace) override {
    // trace.addr is a virtual address in memory pointing to some code.
    // Let's try to find from which loaded object it comes from.
    // The loaded object can be yourself btw.

    Dl_info symbol_info;
    int dladdr_result = 0;
#if defined(__GLIBC__)
    link_map *link_map;
    // We request the link map so we can get information about offsets
    dladdr_result =
        dladdr1(trace.addr, &symbol_info, reinterpret_cast<void **>(&link_map),
                RTLD_DL_LINKMAP);
#else
    // Android doesn't have dladdr1. Don't use the linker map.
    dladdr_result = dladdr(trace.addr, &symbol_info);
#endif
    if (!dladdr_result) {
      return trace; // dat broken trace...
    }

    // Now we get in symbol_info:
    // .dli_fname:
    //      pathname of the shared object that contains the address.
    // .dli_fbase:
    //      where the object is loaded in memory.
    // .dli_sname:
    //      the name of the nearest symbol to trace.addr, we expect a
    //      function name.
    // .dli_saddr:
    //      the exact address corresponding to .dli_sname.
    //
    // And in link_map:
    // .l_addr:
    //      difference between the address in the ELF file and the address
    //      in memory
    // l_name:
    //      absolute pathname where the object was found

    if (symbol_info.dli_sname) {
      trace.object_function = demangle(symbol_info.dli_sname);
    }

    if (!symbol_info.dli_fname) {
      return trace;
    }

    trace.object_filename = resolve_exec_path(symbol_info);
    dwarf_fileobject &fobj = load_object_with_dwarf(symbol_info.dli_fname);
    if (!fobj.dwarf_handle) {
      return trace; // sad, we couldn't load the object :(
    }

#if defined(__GLIBC__)
    // Convert the address to a module relative one by looking at
    // the module's loading address in the link map
    Dwarf_Addr address = reinterpret_cast<uintptr_t>(trace.addr) -
                         reinterpret_cast<uintptr_t>(link_map->l_addr);
#else
    Dwarf_Addr address = reinterpret_cast<uintptr_t>(trace.addr);
#endif

    if (trace.object_function.empty()) {
      symbol_cache_t::iterator it = fobj.symbol_cache.lower_bound(address);

      if (it != fobj.symbol_cache.end()) {
        if (it->first != address) {
          if (it != fobj.symbol_cache.begin()) {
            --it;
          }
        }
        trace.object_function = demangle(it->second.c_str());
      }
    }

    // Get the Compilation Unit DIE for the address
    Dwarf_Die die = find_die(fobj, address);

    if (!die) {
      return trace; // this time we lost the game :/
    }

    // libdwarf doesn't give us direct access to its objects, it always
    // allocates a copy for the caller. We keep that copy alive in a cache
    // and we deallocate it later when it's no longer required.
    die_cache_entry &die_object = get_die_cache(fobj, die);
    if (die_object.isEmpty())
      return trace; // We have no line section for this DIE

    die_linemap_t::iterator it = die_object.line_section.lower_bound(address);

    if (it != die_object.line_section.end()) {
      if (it->first != address) {
        if (it == die_object.line_section.begin()) {
          // If we are on the first item of the line section
          // but the address does not match it means that
          // the address is below the range of the DIE. Give up.
          return trace;
        } else {
          --it;
        }
      }
    } else {
      return trace; // We didn't find the address.
    }

    // Get the Dwarf_Line that the address points to and call libdwarf
    // to get source file, line and column info.
    Dwarf_Line line = die_object.line_buffer[it->second];
    Dwarf_Error error = DW_DLE_NE;

    char *filename;
    if (dwarf_linesrc(line, &filename, &error) == DW_DLV_OK) {
      trace.source.filename = std::string(filename);
      dwarf_dealloc(fobj.dwarf_handle.get(), filename, DW_DLA_STRING);
    }

    Dwarf_Unsigned number = 0;
    if (dwarf_lineno(line, &number, &error) == DW_DLV_OK) {
      trace.source.line = number;
    } else {
      trace.source.line = 0;
    }

    if (dwarf_lineoff_b(line, &number, &error) == DW_DLV_OK) {
      trace.source.col = number;
    } else {
      trace.source.col = 0;
    }

    std::vector<std::string> namespace_stack;
    deep_first_search_by_pc(fobj, die, address, namespace_stack,
                            inliners_search_cb(trace, fobj, die));

    dwarf_dealloc(fobj.dwarf_handle.get(), die, DW_DLA_DIE);

    return trace;
  }

public:
  static int close_dwarf(Dwarf_Debug dwarf) {
    return dwarf_finish(dwarf, NULL);
  }

private:
  bool _dwarf_loaded;

  typedef details::handle<int, details::deleter<int, int, &::close>>
      dwarf_file_t;

  typedef details::handle<Elf *, details::deleter<int, Elf *, &elf_end>>
      dwarf_elf_t;

  typedef details::handle<Dwarf_Debug,
                          details::deleter<int, Dwarf_Debug, &close_dwarf>>
      dwarf_handle_t;

  typedef std::map<Dwarf_Addr, int> die_linemap_t;

  typedef std::map<Dwarf_Off, Dwarf_Off> die_specmap_t;

  struct die_cache_entry {
    die_specmap_t spec_section;
    die_linemap_t line_section;
    Dwarf_Line *line_buffer;
    Dwarf_Signed line_count;
    Dwarf_Line_Context line_context;

    inline bool isEmpty() {
      return line_buffer == NULL || line_count == 0 || line_context == NULL ||
             line_section.empty();
    }

    die_cache_entry() : line_buffer(0), line_count(0), line_context(0) {}

    ~die_cache_entry() {
      if (line_context) {
        dwarf_srclines_dealloc_b(line_context);
      }
    }
  };

  typedef std::map<Dwarf_Off, die_cache_entry> die_cache_t;

  typedef std::map<uintptr_t, std::string> symbol_cache_t;

  struct dwarf_fileobject {
    dwarf_file_t file_handle;
    dwarf_elf_t elf_handle;
    dwarf_handle_t dwarf_handle;
    symbol_cache_t symbol_cache;

    // Die cache
    die_cache_t die_cache;
    die_cache_entry *current_cu;
  };

  typedef details::hashtable<std::string, dwarf_fileobject>::type
      fobj_dwarf_map_t;
  fobj_dwarf_map_t _fobj_dwarf_map;

  static bool cstrings_eq(const char *a, const char *b) {
    if (!a || !b) {
      return false;
    }
    return strcmp(a, b) == 0;
  }

  dwarf_fileobject &load_object_with_dwarf(const std::string &filename_object) {

    if (!_dwarf_loaded) {
      // Set the ELF library operating version
      // If that fails there's nothing we can do
      _dwarf_loaded = elf_version(EV_CURRENT) != EV_NONE;
    }

    fobj_dwarf_map_t::iterator it = _fobj_dwarf_map.find(filename_object);
    if (it != _fobj_dwarf_map.end()) {
      return it->second;
    }

    // this new object is empty for now
    dwarf_fileobject &r = _fobj_dwarf_map[filename_object];

    dwarf_file_t file_handle;
    file_handle.reset(open(filename_object.c_str(), O_RDONLY));
    if (file_handle.get() < 0) {
      return r;
    }

    // Try to get an ELF handle. We need to read the ELF sections
    // because we want to see if there is a .gnu_debuglink section
    // that points to a split debug file
    dwarf_elf_t elf_handle;
    elf_handle.reset(elf_begin(file_handle.get(), ELF_C_READ, NULL));
    if (!elf_handle) {
      return r;
    }

    const char *e_ident = elf_getident(elf_handle.get(), 0);
    if (!e_ident) {
      return r;
    }

    // Get the number of sections
    // We use the new APIs as elf_getshnum is deprecated
    size_t shdrnum = 0;
    if (elf_getshdrnum(elf_handle.get(), &shdrnum) == -1) {
      return r;
    }

    // Get the index to the string section
    size_t shdrstrndx = 0;
    if (elf_getshdrstrndx(elf_handle.get(), &shdrstrndx) == -1) {
      return r;
    }

    std::string debuglink;
    // Iterate through the ELF sections to try to get a gnu_debuglink
    // note and also to cache the symbol table.
    // We go the preprocessor way to avoid having to create templated
    // classes or using gelf (which might throw a compiler error if 64 bit
    // is not supported
#define ELF_GET_DATA(ARCH)                                                     \
  Elf_Scn *elf_section = 0;                                                    \
  Elf_Data *elf_data = 0;                                                      \
  Elf##ARCH##_Shdr *section_header = 0;                                        \
  Elf_Scn *symbol_section = 0;                                                 \
  size_t symbol_count = 0;                                                     \
  size_t symbol_strings = 0;                                                   \
  Elf##ARCH##_Sym *symbol = 0;                                                 \
  const char *section_name = 0;                                                \
                                                                               \
  while ((elf_section = elf_nextscn(elf_handle.get(), elf_section)) != NULL) { \
    section_header = elf##ARCH##_getshdr(elf_section);                         \
    if (section_header == NULL) {                                              \
      return r;                                                                \
    }                                                                          \
                                                                               \
    if ((section_name = elf_strptr(elf_handle.get(), shdrstrndx,               \
                                   section_header->sh_name)) == NULL) {        \
      return r;                                                                \
    }                                                                          \
                                                                               \
    if (cstrings_eq(section_name, ".gnu_debuglink")) {                         \
      elf_data = elf_getdata(elf_section, NULL);                               \
      if (elf_data && elf_data->d_size > 0) {                                  \
        debuglink =                                                            \
            std::string(reinterpret_cast<const char *>(elf_data->d_buf));      \
      }                                                                        \
    }                                                                          \
                                                                               \
    switch (section_header->sh_type) {                                         \
    case SHT_SYMTAB:                                                           \
      symbol_section = elf_section;                                            \
      symbol_count = section_header->sh_size / section_header->sh_entsize;     \
      symbol_strings = section_header->sh_link;                                \
      break;                                                                   \
                                                                               \
    /* We use .dynsyms as a last resort, we prefer .symtab */                  \
    case SHT_DYNSYM:                                                           \
      if (!symbol_section) {                                                   \
        symbol_section = elf_section;                                          \
        symbol_count = section_header->sh_size / section_header->sh_entsize;   \
        symbol_strings = section_header->sh_link;                              \
      }                                                                        \
      break;                                                                   \
    }                                                                          \
  }                                                                            \
                                                                               \
  if (symbol_section && symbol_count && symbol_strings) {                      \
    elf_data = elf_getdata(symbol_section, NULL);                              \
    symbol = reinterpret_cast<Elf##ARCH##_Sym *>(elf_data->d_buf);             \
    for (size_t i = 0; i < symbol_count; ++i) {                                \
      int type = ELF##ARCH##_ST_TYPE(symbol->st_info);                         \
      if (type == STT_FUNC && symbol->st_value > 0) {                          \
        r.symbol_cache[symbol->st_value] = std::string(                        \
            elf_strptr(elf_handle.get(), symbol_strings, symbol->st_name));    \
      }                                                                        \
      ++symbol;                                                                \
    }                                                                          \
  }

    if (e_ident[EI_CLASS] == ELFCLASS32) {
      ELF_GET_DATA(32)
    } else if (e_ident[EI_CLASS] == ELFCLASS64) {
      // libelf might have been built without 64 bit support
#if __LIBELF64
      ELF_GET_DATA(64)
#endif
    }

    if (!debuglink.empty()) {
      // We have a debuglink section! Open an elf instance on that
      // file instead. If we can't open the file, then return
      // the elf handle we had already opened.
      dwarf_file_t debuglink_file;
      debuglink_file.reset(open(debuglink.c_str(), O_RDONLY));
      if (debuglink_file.get() > 0) {
        dwarf_elf_t debuglink_elf;
        debuglink_elf.reset(elf_begin(debuglink_file.get(), ELF_C_READ, NULL));

        // If we have a valid elf handle, return the new elf handle
        // and file handle and discard the original ones
        if (debuglink_elf) {
          elf_handle = move(debuglink_elf);
          file_handle = move(debuglink_file);
        }
      }
    }

    // Ok, we have a valid ELF handle, let's try to get debug symbols
    Dwarf_Debug dwarf_debug;
    Dwarf_Error error = DW_DLE_NE;
    dwarf_handle_t dwarf_handle;

    int dwarf_result = dwarf_elf_init(elf_handle.get(), DW_DLC_READ, NULL, NULL,
                                      &dwarf_debug, &error);

    // We don't do any special handling for DW_DLV_NO_ENTRY specially.
    // If we get an error, or the file doesn't have debug information
    // we just return.
    if (dwarf_result != DW_DLV_OK) {
      return r;
    }

    dwarf_handle.reset(dwarf_debug);

    r.file_handle = move(file_handle);
    r.elf_handle = move(elf_handle);
    r.dwarf_handle = move(dwarf_handle);

    return r;
  }

  die_cache_entry &get_die_cache(dwarf_fileobject &fobj, Dwarf_Die die) {
    Dwarf_Error error = DW_DLE_NE;

    // Get the die offset, we use it as the cache key
    Dwarf_Off die_offset;
    if (dwarf_dieoffset(die, &die_offset, &error) != DW_DLV_OK) {
      die_offset = 0;
    }

    die_cache_t::iterator it = fobj.die_cache.find(die_offset);

    if (it != fobj.die_cache.end()) {
      fobj.current_cu = &it->second;
      return it->second;
    }

    die_cache_entry &de = fobj.die_cache[die_offset];
    fobj.current_cu = &de;

    Dwarf_Addr line_addr;
    Dwarf_Small table_count;

    // The addresses in the line section are not fully sorted (they might
    // be sorted by block of code belonging to the same file), which makes
    // it necessary to do so before searching is possible.
    //
    // As libdwarf allocates a copy of everything, let's get the contents
    // of the line section and keep it around. We also create a map of
    // program counter to line table indices so we can search by address
    // and get the line buffer index.
    //
    // To make things more difficult, the same address can span more than
    // one line, so we need to keep the index pointing to the first line
    // by using insert instead of the map's [ operator.

    // Get the line context for the DIE
    if (dwarf_srclines_b(die, 0, &table_count, &de.line_context, &error) ==
        DW_DLV_OK) {
      // Get the source lines for this line context, to be deallocated
      // later
      if (dwarf_srclines_from_linecontext(de.line_context, &de.line_buffer,
                                          &de.line_count,
                                          &error) == DW_DLV_OK) {

        // Add all the addresses to our map
        for (int i = 0; i < de.line_count; i++) {
          if (dwarf_lineaddr(de.line_buffer[i], &line_addr, &error) !=
              DW_DLV_OK) {
            line_addr = 0;
          }
          de.line_section.insert(std::pair<Dwarf_Addr, int>(line_addr, i));
        }
      }
    }

    // For each CU, cache the function DIEs that contain the
    // DW_AT_specification attribute. When building with -g3 the function
    // DIEs are separated in declaration and specification, with the
    // declaration containing only the name and parameters and the
    // specification the low/high pc and other compiler attributes.
    //
    // We cache those specifications so we don't skip over the declarations,
    // because they have no pc, and we can do namespace resolution for
    // DWARF function names.
    Dwarf_Debug dwarf = fobj.dwarf_handle.get();
    Dwarf_Die current_die = 0;
    if (dwarf_child(die, &current_die, &error) == DW_DLV_OK) {
      for (;;) {
        Dwarf_Die sibling_die = 0;

        Dwarf_Half tag_value;
        dwarf_tag(current_die, &tag_value, &error);

        if (tag_value == DW_TAG_subprogram ||
            tag_value == DW_TAG_inlined_subroutine) {

          Dwarf_Bool has_attr = 0;
          if (dwarf_hasattr(current_die, DW_AT_specification, &has_attr,
                            &error) == DW_DLV_OK) {
            if (has_attr) {
              Dwarf_Attribute attr_mem;
              if (dwarf_attr(current_die, DW_AT_specification, &attr_mem,
                             &error) == DW_DLV_OK) {
                Dwarf_Off spec_offset = 0;
                if (dwarf_formref(attr_mem, &spec_offset, &error) ==
                    DW_DLV_OK) {
                  Dwarf_Off spec_die_offset;
                  if (dwarf_dieoffset(current_die, &spec_die_offset, &error) ==
                      DW_DLV_OK) {
                    de.spec_section[spec_offset] = spec_die_offset;
                  }
                }
              }
              dwarf_dealloc(dwarf, attr_mem, DW_DLA_ATTR);
            }
          }
        }

        int result = dwarf_siblingof(dwarf, current_die, &sibling_die, &error);
        if (result == DW_DLV_ERROR) {
          break;
        } else if (result == DW_DLV_NO_ENTRY) {
          break;
        }

        if (current_die != die) {
          dwarf_dealloc(dwarf, current_die, DW_DLA_DIE);
          current_die = 0;
        }

        current_die = sibling_die;
      }
    }
    return de;
  }

  static Dwarf_Die get_referenced_die(Dwarf_Debug dwarf, Dwarf_Die die,
                                      Dwarf_Half attr, bool global) {
    Dwarf_Error error = DW_DLE_NE;
    Dwarf_Attribute attr_mem;

    Dwarf_Die found_die = NULL;
    if (dwarf_attr(die, attr, &attr_mem, &error) == DW_DLV_OK) {
      Dwarf_Off offset;
      int result = 0;
      if (global) {
        result = dwarf_global_formref(attr_mem, &offset, &error);
      } else {
        result = dwarf_formref(attr_mem, &offset, &error);
      }

      if (result == DW_DLV_OK) {
        if (dwarf_offdie(dwarf, offset, &found_die, &error) != DW_DLV_OK) {
          found_die = NULL;
        }
      }
      dwarf_dealloc(dwarf, attr_mem, DW_DLA_ATTR);
    }
    return found_die;
  }

  static std::string get_referenced_die_name(Dwarf_Debug dwarf, Dwarf_Die die,
                                             Dwarf_Half attr, bool global) {
    Dwarf_Error error = DW_DLE_NE;
    std::string value;

    Dwarf_Die found_die = get_referenced_die(dwarf, die, attr, global);

    if (found_die) {
      char *name;
      if (dwarf_diename(found_die, &name, &error) == DW_DLV_OK) {
        if (name) {
          value = std::string(name);
        }
        dwarf_dealloc(dwarf, name, DW_DLA_STRING);
      }
      dwarf_dealloc(dwarf, found_die, DW_DLA_DIE);
    }

    return value;
  }

  // Returns a spec DIE linked to the passed one. The caller should
  // deallocate the DIE
  static Dwarf_Die get_spec_die(dwarf_fileobject &fobj, Dwarf_Die die) {
    Dwarf_Debug dwarf = fobj.dwarf_handle.get();
    Dwarf_Error error = DW_DLE_NE;
    Dwarf_Off die_offset;
    if (fobj.current_cu &&
        dwarf_die_CU_offset(die, &die_offset, &error) == DW_DLV_OK) {
      die_specmap_t::iterator it =
          fobj.current_cu->spec_section.find(die_offset);

      // If we have a DIE that completes the current one, check if
      // that one has the pc we are looking for
      if (it != fobj.current_cu->spec_section.end()) {
        Dwarf_Die spec_die = 0;
        if (dwarf_offdie(dwarf, it->second, &spec_die, &error) == DW_DLV_OK) {
          return spec_die;
        }
      }
    }

    // Maybe we have an abstract origin DIE with the function information?
    return get_referenced_die(fobj.dwarf_handle.get(), die,
                              DW_AT_abstract_origin, true);
  }

  static bool die_has_pc(dwarf_fileobject &fobj, Dwarf_Die die, Dwarf_Addr pc) {
    Dwarf_Addr low_pc = 0, high_pc = 0;
    Dwarf_Half high_pc_form = 0;
    Dwarf_Form_Class return_class;
    Dwarf_Error error = DW_DLE_NE;
    Dwarf_Debug dwarf = fobj.dwarf_handle.get();
    bool has_lowpc = false;
    bool has_highpc = false;
    bool has_ranges = false;

    if (dwarf_lowpc(die, &low_pc, &error) == DW_DLV_OK) {
      // If we have a low_pc check if there is a high pc.
      // If we don't have a high pc this might mean we have a base
      // address for the ranges list or just an address.
      has_lowpc = true;

      if (dwarf_highpc_b(die, &high_pc, &high_pc_form, &return_class, &error) ==
          DW_DLV_OK) {
        // We do have a high pc. In DWARF 4+ this is an offset from the
        // low pc, but in earlier versions it's an absolute address.

        has_highpc = true;
        // In DWARF 2/3 this would be a DW_FORM_CLASS_ADDRESS
        if (return_class == DW_FORM_CLASS_CONSTANT) {
          high_pc = low_pc + high_pc;
        }

        // We have low and high pc, check if our address
        // is in that range
        return pc >= low_pc && pc < high_pc;
      }
    } else {
      // Reset the low_pc, in case dwarf_lowpc failing set it to some
      // undefined value.
      low_pc = 0;
    }

    // Check if DW_AT_ranges is present and search for the PC in the
    // returned ranges list. We always add the low_pc, as it not set it will
    // be 0, in case we had a DW_AT_low_pc and DW_AT_ranges pair
    bool result = false;

    Dwarf_Attribute attr;
    if (dwarf_attr(die, DW_AT_ranges, &attr, &error) == DW_DLV_OK) {

      Dwarf_Off offset;
      if (dwarf_global_formref(attr, &offset, &error) == DW_DLV_OK) {
        Dwarf_Ranges *ranges;
        Dwarf_Signed ranges_count = 0;
        Dwarf_Unsigned byte_count = 0;

        if (dwarf_get_ranges_a(dwarf, offset, die, &ranges, &ranges_count,
                               &byte_count, &error) == DW_DLV_OK) {
          has_ranges = ranges_count != 0;
          for (int i = 0; i < ranges_count; i++) {
            if (ranges[i].dwr_addr1 != 0 &&
                pc >= ranges[i].dwr_addr1 + low_pc &&
                pc < ranges[i].dwr_addr2 + low_pc) {
              result = true;
              break;
            }
          }
          dwarf_ranges_dealloc(dwarf, ranges, ranges_count);
        }
      }
    }

    // Last attempt. We might have a single address set as low_pc.
    if (!result && low_pc != 0 && pc == low_pc) {
      result = true;
    }

    // If we don't have lowpc, highpc and ranges maybe this DIE is a
    // declaration that relies on a DW_AT_specification DIE that happens
    // later. Use the specification cache we filled when we loaded this CU.
    if (!result && (!has_lowpc && !has_highpc && !has_ranges)) {
      Dwarf_Die spec_die = get_spec_die(fobj, die);
      if (spec_die) {
        result = die_has_pc(fobj, spec_die, pc);
        dwarf_dealloc(dwarf, spec_die, DW_DLA_DIE);
      }
    }

    return result;
  }

  static void get_type(Dwarf_Debug dwarf, Dwarf_Die die, std::string &type) {
    Dwarf_Error error = DW_DLE_NE;

    Dwarf_Die child = 0;
    if (dwarf_child(die, &child, &error) == DW_DLV_OK) {
      get_type(dwarf, child, type);
    }

    if (child) {
      type.insert(0, "::");
      dwarf_dealloc(dwarf, child, DW_DLA_DIE);
    }

    char *name;
    if (dwarf_diename(die, &name, &error) == DW_DLV_OK) {
      type.insert(0, std::string(name));
      dwarf_dealloc(dwarf, name, DW_DLA_STRING);
    } else {
      type.insert(0, "<unknown>");
    }
  }

  static std::string get_type_by_signature(Dwarf_Debug dwarf, Dwarf_Die die) {
    Dwarf_Error error = DW_DLE_NE;

    Dwarf_Sig8 signature;
    Dwarf_Bool has_attr = 0;
    if (dwarf_hasattr(die, DW_AT_signature, &has_attr, &error) == DW_DLV_OK) {
      if (has_attr) {
        Dwarf_Attribute attr_mem;
        if (dwarf_attr(die, DW_AT_signature, &attr_mem, &error) == DW_DLV_OK) {
          if (dwarf_formsig8(attr_mem, &signature, &error) != DW_DLV_OK) {
            return std::string("<no type signature>");
          }
        }
        dwarf_dealloc(dwarf, attr_mem, DW_DLA_ATTR);
      }
    }

    Dwarf_Unsigned next_cu_header;
    Dwarf_Sig8 tu_signature;
    std::string result;
    bool found = false;

    while (dwarf_next_cu_header_d(dwarf, 0, 0, 0, 0, 0, 0, 0, &tu_signature, 0,
                                  &next_cu_header, 0, &error) == DW_DLV_OK) {

      if (strncmp(signature.signature, tu_signature.signature, 8) == 0) {
        Dwarf_Die type_cu_die = 0;
        if (dwarf_siblingof_b(dwarf, 0, 0, &type_cu_die, &error) == DW_DLV_OK) {
          Dwarf_Die child_die = 0;
          if (dwarf_child(type_cu_die, &child_die, &error) == DW_DLV_OK) {
            get_type(dwarf, child_die, result);
            found = !result.empty();
            dwarf_dealloc(dwarf, child_die, DW_DLA_DIE);
          }
          dwarf_dealloc(dwarf, type_cu_die, DW_DLA_DIE);
        }
      }
    }

    if (found) {
      while (dwarf_next_cu_header_d(dwarf, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                    &next_cu_header, 0, &error) == DW_DLV_OK) {
        // Reset the cu header state. Unfortunately, libdwarf's
        // next_cu_header API keeps its own iterator per Dwarf_Debug
        // that can't be reset. We need to keep fetching elements until
        // the end.
      }
    } else {
      // If we couldn't resolve the type just print out the signature
      std::ostringstream string_stream;
      string_stream << "<0x" << std::hex << std::setfill('0');
      for (int i = 0; i < 8; ++i) {
        string_stream << std::setw(2) << std::hex
                      << (int)(unsigned char)(signature.signature[i]);
      }
      string_stream << ">";
      result = string_stream.str();
    }
    return result;
  }

  struct type_context_t {
    bool is_const;
    bool is_typedef;
    bool has_type;
    bool has_name;
    std::string text;

    type_context_t()
        : is_const(false), is_typedef(false), has_type(false), has_name(false) {
    }
  };

  // Types are resolved from right to left: we get the variable name first
  // and then all specifiers (like const or pointer) in a chain of DW_AT_type
  // DIEs. Call this function recursively until we get a complete type
  // string.
  static void set_parameter_string(dwarf_fileobject &fobj, Dwarf_Die die,
                                   type_context_t &context) {
    char *name;
    Dwarf_Error error = DW_DLE_NE;

    // typedefs contain also the base type, so we skip it and only
    // print the typedef name
    if (!context.is_typedef) {
      if (dwarf_diename(die, &name, &error) == DW_DLV_OK) {
        if (!context.text.empty()) {
          context.text.insert(0, " ");
        }
        context.text.insert(0, std::string(name));
        dwarf_dealloc(fobj.dwarf_handle.get(), name, DW_DLA_STRING);
      }
    } else {
      context.is_typedef = false;
      context.has_type = true;
      if (context.is_const) {
        context.text.insert(0, "const ");
        context.is_const = false;
      }
    }

    bool next_type_is_const = false;
    bool is_keyword = true;

    Dwarf_Half tag = 0;
    Dwarf_Bool has_attr = 0;
    if (dwarf_tag(die, &tag, &error) == DW_DLV_OK) {
      switch (tag) {
      case DW_TAG_structure_type:
      case DW_TAG_union_type:
      case DW_TAG_class_type:
      case DW_TAG_enumeration_type:
        context.has_type = true;
        if (dwarf_hasattr(die, DW_AT_signature, &has_attr, &error) ==
            DW_DLV_OK) {
          // If we have a signature it means the type is defined
          // in .debug_types, so we need to load the DIE pointed
          // at by the signature and resolve it
          if (has_attr) {
            std::string type =
                get_type_by_signature(fobj.dwarf_handle.get(), die);
            if (context.is_const)
              type.insert(0, "const ");

            if (!context.text.empty())
              context.text.insert(0, " ");
            context.text.insert(0, type);
          }

          // Treat enums like typedefs, and skip printing its
          // base type
          context.is_typedef = (tag == DW_TAG_enumeration_type);
        }
        break;
      case DW_TAG_const_type:
        next_type_is_const = true;
        break;
      case DW_TAG_pointer_type:
        context.text.insert(0, "*");
        break;
      case DW_TAG_reference_type:
        context.text.insert(0, "&");
        break;
      case DW_TAG_restrict_type:
        context.text.insert(0, "restrict ");
        break;
      case DW_TAG_rvalue_reference_type:
        context.text.insert(0, "&&");
        break;
      case DW_TAG_volatile_type:
        context.text.insert(0, "volatile ");
        break;
      case DW_TAG_typedef:
        // Propagate the const-ness to the next type
        // as typedefs are linked to its base type
        next_type_is_const = context.is_const;
        context.is_typedef = true;
        context.has_type = true;
        break;
      case DW_TAG_base_type:
        context.has_type = true;
        break;
      case DW_TAG_formal_parameter:
        context.has_name = true;
        break;
      default:
        is_keyword = false;
        break;
      }
    }

    if (!is_keyword && context.is_const) {
      context.text.insert(0, "const ");
    }

    context.is_const = next_type_is_const;

    Dwarf_Die ref =
        get_referenced_die(fobj.dwarf_handle.get(), die, DW_AT_type, true);
    if (ref) {
      set_parameter_string(fobj, ref, context);
      dwarf_dealloc(fobj.dwarf_handle.get(), ref, DW_DLA_DIE);
    }

    if (!context.has_type && context.has_name) {
      context.text.insert(0, "void ");
      context.has_type = true;
    }
  }

  // Resolve the function return type and parameters
  static void set_function_parameters(std::string &function_name,
                                      std::vector<std::string> &ns,
                                      dwarf_fileobject &fobj, Dwarf_Die die) {
    Dwarf_Debug dwarf = fobj.dwarf_handle.get();
    Dwarf_Error error = DW_DLE_NE;
    Dwarf_Die current_die = 0;
    std::string parameters;
    bool has_spec = true;
    // Check if we have a spec DIE. If we do we use it as it contains
    // more information, like parameter names.
    Dwarf_Die spec_die = get_spec_die(fobj, die);
    if (!spec_die) {
      has_spec = false;
      spec_die = die;
    }

    std::vector<std::string>::const_iterator it = ns.begin();
    std::string ns_name;
    for (it = ns.begin(); it < ns.end(); ++it) {
      ns_name.append(*it).append("::");
    }

    if (!ns_name.empty()) {
      function_name.insert(0, ns_name);
    }

    // See if we have a function return type. It can be either on the
    // current die or in its spec one (usually true for inlined functions)
    std::string return_type =
        get_referenced_die_name(dwarf, die, DW_AT_type, true);
    if (return_type.empty()) {
      return_type = get_referenced_die_name(dwarf, spec_die, DW_AT_type, true);
    }
    if (!return_type.empty()) {
      return_type.append(" ");
      function_name.insert(0, return_type);
    }

    if (dwarf_child(spec_die, &current_die, &error) == DW_DLV_OK) {
      for (;;) {
        Dwarf_Die sibling_die = 0;

        Dwarf_Half tag_value;
        dwarf_tag(current_die, &tag_value, &error);

        if (tag_value == DW_TAG_formal_parameter) {
          // Ignore artificial (ie, compiler generated) parameters
          bool is_artificial = false;
          Dwarf_Attribute attr_mem;
          if (dwarf_attr(current_die, DW_AT_artificial, &attr_mem, &error) ==
              DW_DLV_OK) {
            Dwarf_Bool flag = 0;
            if (dwarf_formflag(attr_mem, &flag, &error) == DW_DLV_OK) {
              is_artificial = flag != 0;
            }
            dwarf_dealloc(dwarf, attr_mem, DW_DLA_ATTR);
          }

          if (!is_artificial) {
            type_context_t context;
            set_parameter_string(fobj, current_die, context);

            if (parameters.empty()) {
              parameters.append("(");
            } else {
              parameters.append(", ");
            }
            parameters.append(context.text);
          }
        }

        int result = dwarf_siblingof(dwarf, current_die, &sibling_die, &error);
        if (result == DW_DLV_ERROR) {
          break;
        } else if (result == DW_DLV_NO_ENTRY) {
          break;
        }

        if (current_die != die) {
          dwarf_dealloc(dwarf, current_die, DW_DLA_DIE);
          current_die = 0;
        }

        current_die = sibling_die;
      }
    }
    if (parameters.empty())
      parameters = "(";
    parameters.append(")");

    // If we got a spec DIE we need to deallocate it
    if (has_spec)
      dwarf_dealloc(dwarf, spec_die, DW_DLA_DIE);

    function_name.append(parameters);
  }

  // defined here because in C++98, template function cannot take locally
  // defined types... grrr.
  struct inliners_search_cb {
    void operator()(Dwarf_Die die, std::vector<std::string> &ns) {
      Dwarf_Error error = DW_DLE_NE;
      Dwarf_Half tag_value;
      Dwarf_Attribute attr_mem;
      Dwarf_Debug dwarf = fobj.dwarf_handle.get();

      dwarf_tag(die, &tag_value, &error);

      switch (tag_value) {
        char *name;
      case DW_TAG_subprogram:
        if (!trace.source.function.empty())
          break;
        if (dwarf_diename(die, &name, &error) == DW_DLV_OK) {
          trace.source.function = std::string(name);
          dwarf_dealloc(dwarf, name, DW_DLA_STRING);
        } else {
          // We don't have a function name in this DIE.
          // Check if there is a referenced non-defining
          // declaration.
          trace.source.function =
              get_referenced_die_name(dwarf, die, DW_AT_abstract_origin, true);
          if (trace.source.function.empty()) {
            trace.source.function =
                get_referenced_die_name(dwarf, die, DW_AT_specification, true);
          }
        }

        // Append the function parameters, if available
        set_function_parameters(trace.source.function, ns, fobj, die);

        // If the object function name is empty, it's possible that
        // there is no dynamic symbol table (maybe the executable
        // was stripped or not built with -rdynamic). See if we have
        // a DWARF linkage name to use instead. We try both
        // linkage_name and MIPS_linkage_name because the MIPS tag
        // was the unofficial one until it was adopted in DWARF4.
        // Old gcc versions generate MIPS_linkage_name
        if (trace.object_function.empty()) {
          details::demangler demangler;

          if (dwarf_attr(die, DW_AT_linkage_name, &attr_mem, &error) !=
              DW_DLV_OK) {
            if (dwarf_attr(die, DW_AT_MIPS_linkage_name, &attr_mem, &error) !=
                DW_DLV_OK) {
              break;
            }
          }

          char *linkage;
          if (dwarf_formstring(attr_mem, &linkage, &error) == DW_DLV_OK) {
            trace.object_function = demangler.demangle(linkage);
            dwarf_dealloc(dwarf, linkage, DW_DLA_STRING);
          }
          dwarf_dealloc(dwarf, attr_mem, DW_DLA_ATTR);
        }
        break;

      case DW_TAG_inlined_subroutine:
        ResolvedTrace::SourceLoc sloc;

        if (dwarf_diename(die, &name, &error) == DW_DLV_OK) {
          sloc.function = std::string(name);
          dwarf_dealloc(dwarf, name, DW_DLA_STRING);
        } else {
          // We don't have a name for this inlined DIE, it could
          // be that there is an abstract origin instead.
          // Get the DW_AT_abstract_origin value, which is a
          // reference to the source DIE and try to get its name
          sloc.function =
              get_referenced_die_name(dwarf, die, DW_AT_abstract_origin, true);
        }

        set_function_parameters(sloc.function, ns, fobj, die);

        std::string file = die_call_file(dwarf, die, cu_die);
        if (!file.empty())
          sloc.filename = file;

        Dwarf_Unsigned number = 0;
        if (dwarf_attr(die, DW_AT_call_line, &attr_mem, &error) == DW_DLV_OK) {
          if (dwarf_formudata(attr_mem, &number, &error) == DW_DLV_OK) {
            sloc.line = number;
          }
          dwarf_dealloc(dwarf, attr_mem, DW_DLA_ATTR);
        }

        if (dwarf_attr(die, DW_AT_call_column, &attr_mem, &error) ==
            DW_DLV_OK) {
          if (dwarf_formudata(attr_mem, &number, &error) == DW_DLV_OK) {
            sloc.col = number;
          }
          dwarf_dealloc(dwarf, attr_mem, DW_DLA_ATTR);
        }

        trace.inliners.push_back(sloc);
        break;
      };
    }
    ResolvedTrace &trace;
    dwarf_fileobject &fobj;
    Dwarf_Die cu_die;
    inliners_search_cb(ResolvedTrace &t, dwarf_fileobject &f, Dwarf_Die c)
        : trace(t), fobj(f), cu_die(c) {}
  };

  static Dwarf_Die find_fundie_by_pc(dwarf_fileobject &fobj,
                                     Dwarf_Die parent_die, Dwarf_Addr pc,
                                     Dwarf_Die result) {
    Dwarf_Die current_die = 0;
    Dwarf_Error error = DW_DLE_NE;
    Dwarf_Debug dwarf = fobj.dwarf_handle.get();

    if (dwarf_child(parent_die, &current_die, &error) != DW_DLV_OK) {
      return NULL;
    }

    for (;;) {
      Dwarf_Die sibling_die = 0;
      Dwarf_Half tag_value;
      dwarf_tag(current_die, &tag_value, &error);

      switch (tag_value) {
      case DW_TAG_subprogram:
      case DW_TAG_inlined_subroutine:
        if (die_has_pc(fobj, current_die, pc)) {
          return current_die;
        }
      };
      bool declaration = false;
      Dwarf_Attribute attr_mem;
      if (dwarf_attr(current_die, DW_AT_declaration, &attr_mem, &error) ==
          DW_DLV_OK) {
        Dwarf_Bool flag = 0;
        if (dwarf_formflag(attr_mem, &flag, &error) == DW_DLV_OK) {
          declaration = flag != 0;
        }
        dwarf_dealloc(dwarf, attr_mem, DW_DLA_ATTR);
      }

      if (!declaration) {
        // let's be curious and look deeper in the tree, functions are
        // not necessarily at the first level, but might be nested
        // inside a namespace, structure, a function, an inlined
        // function etc.
        Dwarf_Die die_mem = 0;
        Dwarf_Die indie = find_fundie_by_pc(fobj, current_die, pc, die_mem);
        if (indie) {
          result = die_mem;
          return result;
        }
      }

      int res = dwarf_siblingof(dwarf, current_die, &sibling_die, &error);
      if (res == DW_DLV_ERROR) {
        return NULL;
      } else if (res == DW_DLV_NO_ENTRY) {
        break;
      }

      if (current_die != parent_die) {
        dwarf_dealloc(dwarf, current_die, DW_DLA_DIE);
        current_die = 0;
      }

      current_die = sibling_die;
    }
    return NULL;
  }

  template <typename CB>
  static bool deep_first_search_by_pc(dwarf_fileobject &fobj,
                                      Dwarf_Die parent_die, Dwarf_Addr pc,
                                      std::vector<std::string> &ns, CB cb) {
    Dwarf_Die current_die = 0;
    Dwarf_Debug dwarf = fobj.dwarf_handle.get();
    Dwarf_Error error = DW_DLE_NE;

    if (dwarf_child(parent_die, &current_die, &error) != DW_DLV_OK) {
      return false;
    }

    bool branch_has_pc = false;
    bool has_namespace = false;
    for (;;) {
      Dwarf_Die sibling_die = 0;

      Dwarf_Half tag;
      if (dwarf_tag(current_die, &tag, &error) == DW_DLV_OK) {
        if (tag == DW_TAG_namespace || tag == DW_TAG_class_type) {
          char *ns_name = NULL;
          if (dwarf_diename(current_die, &ns_name, &error) == DW_DLV_OK) {
            if (ns_name) {
              ns.push_back(std::string(ns_name));
            } else {
              ns.push_back("<unknown>");
            }
            dwarf_dealloc(dwarf, ns_name, DW_DLA_STRING);
          } else {
            ns.push_back("<unknown>");
          }
          has_namespace = true;
        }
      }

      bool declaration = false;
      Dwarf_Attribute attr_mem;
      if (tag != DW_TAG_class_type &&
          dwarf_attr(current_die, DW_AT_declaration, &attr_mem, &error) ==
              DW_DLV_OK) {
        Dwarf_Bool flag = 0;
        if (dwarf_formflag(attr_mem, &flag, &error) == DW_DLV_OK) {
          declaration = flag != 0;
        }
        dwarf_dealloc(dwarf, attr_mem, DW_DLA_ATTR);
      }

      if (!declaration) {
        // let's be curious and look deeper in the tree, function are
        // not necessarily at the first level, but might be nested
        // inside a namespace, structure, a function, an inlined
        // function etc.
        branch_has_pc = deep_first_search_by_pc(fobj, current_die, pc, ns, cb);
      }

      if (!branch_has_pc) {
        branch_has_pc = die_has_pc(fobj, current_die, pc);
      }

      if (branch_has_pc) {
        cb(current_die, ns);
      }

      int result = dwarf_siblingof(dwarf, current_die, &sibling_die, &error);
      if (result == DW_DLV_ERROR) {
        return false;
      } else if (result == DW_DLV_NO_ENTRY) {
        break;
      }

      if (current_die != parent_die) {
        dwarf_dealloc(dwarf, current_die, DW_DLA_DIE);
        current_die = 0;
      }

      if (has_namespace) {
        has_namespace = false;
        ns.pop_back();
      }
      current_die = sibling_die;
    }

    if (has_namespace) {
      ns.pop_back();
    }
    return branch_has_pc;
  }

  static std::string die_call_file(Dwarf_Debug dwarf, Dwarf_Die die,
                                   Dwarf_Die cu_die) {
    Dwarf_Attribute attr_mem;
    Dwarf_Error error = DW_DLE_NE;
    Dwarf_Unsigned file_index;

    std::string file;

    if (dwarf_attr(die, DW_AT_call_file, &attr_mem, &error) == DW_DLV_OK) {
      if (dwarf_formudata(attr_mem, &file_index, &error) != DW_DLV_OK) {
        file_index = 0;
      }
      dwarf_dealloc(dwarf, attr_mem, DW_DLA_ATTR);

      if (file_index == 0) {
        return file;
      }

      char **srcfiles = 0;
      Dwarf_Signed file_count = 0;
      if (dwarf_srcfiles(cu_die, &srcfiles, &file_count, &error) == DW_DLV_OK) {
        if (file_count > 0 && file_index <= static_cast<Dwarf_Unsigned>(file_count)) {
          file = std::string(srcfiles[file_index - 1]);
	}

        // Deallocate all strings!
        for (int i = 0; i < file_count; ++i) {
          dwarf_dealloc(dwarf, srcfiles[i], DW_DLA_STRING);
        }
        dwarf_dealloc(dwarf, srcfiles, DW_DLA_LIST);
      }
    }
    return file;
  }

  Dwarf_Die find_die(dwarf_fileobject &fobj, Dwarf_Addr addr) {
    // Let's get to work! First see if we have a debug_aranges section so
    // we can speed up the search

    Dwarf_Debug dwarf = fobj.dwarf_handle.get();
    Dwarf_Error error = DW_DLE_NE;
    Dwarf_Arange *aranges;
    Dwarf_Signed arange_count;

    Dwarf_Die returnDie;
    bool found = false;
    if (dwarf_get_aranges(dwarf, &aranges, &arange_count, &error) !=
        DW_DLV_OK) {
      aranges = NULL;
    }

    if (aranges) {
      // We have aranges. Get the one where our address is.
      Dwarf_Arange arange;
      if (dwarf_get_arange(aranges, arange_count, addr, &arange, &error) ==
          DW_DLV_OK) {

        // We found our address. Get the compilation-unit DIE offset
        // represented by the given address range.
        Dwarf_Off cu_die_offset;
        if (dwarf_get_cu_die_offset(arange, &cu_die_offset, &error) ==
            DW_DLV_OK) {
          // Get the DIE at the offset returned by the aranges search.
          // We set is_info to 1 to specify that the offset is from
          // the .debug_info section (and not .debug_types)
          int dwarf_result =
              dwarf_offdie_b(dwarf, cu_die_offset, 1, &returnDie, &error);

          found = dwarf_result == DW_DLV_OK;
        }
        dwarf_dealloc(dwarf, arange, DW_DLA_ARANGE);
      }
    }

    if (found)
      return returnDie; // The caller is responsible for freeing the die

    // The search for aranges failed. Try to find our address by scanning
    // all compilation units.
    Dwarf_Unsigned next_cu_header;
    Dwarf_Half tag = 0;
    returnDie = 0;

    while (!found &&
           dwarf_next_cu_header_d(dwarf, 1, 0, 0, 0, 0, 0, 0, 0, 0,
                                  &next_cu_header, 0, &error) == DW_DLV_OK) {

      if (returnDie)
        dwarf_dealloc(dwarf, returnDie, DW_DLA_DIE);

      if (dwarf_siblingof(dwarf, 0, &returnDie, &error) == DW_DLV_OK) {
        if ((dwarf_tag(returnDie, &tag, &error) == DW_DLV_OK) &&
            tag == DW_TAG_compile_unit) {
          if (die_has_pc(fobj, returnDie, addr)) {
            found = true;
          }
        }
      }
    }

    if (found) {
      while (dwarf_next_cu_header_d(dwarf, 1, 0, 0, 0, 0, 0, 0, 0, 0,
                                    &next_cu_header, 0, &error) == DW_DLV_OK) {
        // Reset the cu header state. Libdwarf's next_cu_header API
        // keeps its own iterator per Dwarf_Debug that can't be reset.
        // We need to keep fetching elements until the end.
      }
    }

    if (found)
      return returnDie;

    // We couldn't find any compilation units with ranges or a high/low pc.
    // Try again by looking at all DIEs in all compilation units.
    Dwarf_Die cudie;
    while (dwarf_next_cu_header_d(dwarf, 1, 0, 0, 0, 0, 0, 0, 0, 0,
                                  &next_cu_header, 0, &error) == DW_DLV_OK) {
      if (dwarf_siblingof(dwarf, 0, &cudie, &error) == DW_DLV_OK) {
        Dwarf_Die die_mem = 0;
        Dwarf_Die resultDie = find_fundie_by_pc(fobj, cudie, addr, die_mem);

        if (resultDie) {
          found = true;
          break;
        }
      }
    }

    if (found) {
      while (dwarf_next_cu_header_d(dwarf, 1, 0, 0, 0, 0, 0, 0, 0, 0,
                                    &next_cu_header, 0, &error) == DW_DLV_OK) {
        // Reset the cu header state. Libdwarf's next_cu_header API
        // keeps its own iterator per Dwarf_Debug that can't be reset.
        // We need to keep fetching elements until the end.
      }
    }

    if (found)
      return cudie;

    // We failed.
    return NULL;
  }
};
#endif // BACKWARD_HAS_DWARF == 1

template <>
class TraceResolverImpl<system_tag::linux_tag>
    : public TraceResolverLinuxImpl<trace_resolver_tag::current> {};

#endif // BACKWARD_SYSTEM_LINUX

#ifdef BACKWARD_SYSTEM_DARWIN

template <typename STACKTRACE_TAG> class TraceResolverDarwinImpl;

template <>
class TraceResolverDarwinImpl<trace_resolver_tag::backtrace_symbol>
    : public TraceResolverImplBase {
public:
  void load_addresses(void *const*addresses, int address_count) override {
    if (address_count == 0) {
      return;
    }
    _symbols.reset(backtrace_symbols(addresses, address_count));
  }

  ResolvedTrace resolve(ResolvedTrace trace) override {
    // parse:
    // <n>  <file>  <addr>  <mangled-name> + <offset>
    char *filename = _symbols[trace.idx];

    // skip "<n>  "
    while (*filename && *filename != ' ')
      filename++;
    while (*filename == ' ')
      filename++;

    // find start of <mangled-name> from end (<file> may contain a space)
    char *p = filename + strlen(filename) - 1;
    // skip to start of " + <offset>"
    while (p > filename && *p != ' ')
      p--;
    while (p > filename && *p == ' ')
      p--;
    while (p > filename && *p != ' ')
      p--;
    while (p > filename && *p == ' ')
      p--;
    char *funcname_end = p + 1;

    // skip to start of "<manged-name>"
    while (p > filename && *p != ' ')
      p--;
    char *funcname = p + 1;

    // skip to start of "  <addr>  "
    while (p > filename && *p == ' ')
      p--;
    while (p > filename && *p != ' ')
      p--;
    while (p > filename && *p == ' ')
      p--;

    // skip "<file>", handling the case where it contains a
    char *filename_end = p + 1;
    if (p == filename) {
      // something went wrong, give up
      filename_end = filename + strlen(filename);
      funcname = filename_end;
    }
    trace.object_filename.assign(
        filename, filename_end); // ok even if filename_end is the ending \0
                                 // (then we assign entire string)

    if (*funcname) { // if it's not end of string
      *funcname_end = '\0';

      trace.object_function = this->demangle(funcname);
      trace.object_function += " ";
      trace.object_function += (funcname_end + 1);
      trace.source.function = trace.object_function; // we cannot do better.
    }
    return trace;
  }

private:
  details::handle<char **> _symbols;
};

template <>
class TraceResolverImpl<system_tag::darwin_tag>
    : public TraceResolverDarwinImpl<trace_resolver_tag::current> {};

#endif // BACKWARD_SYSTEM_DARWIN

#ifdef BACKWARD_SYSTEM_WINDOWS

// Load all symbol info
// Based on:
// https://stackoverflow.com/questions/6205981/windows-c-stack-trace-from-a-running-app/28276227#28276227

struct module_data {
  std::string image_name;
  std::string module_name;
  void *base_address;
  DWORD load_size;
};

class get_mod_info {
  HANDLE process;
  static const int buffer_length = 4096;

public:
  get_mod_info(HANDLE h) : process(h) {}

  module_data operator()(HMODULE module) {
    module_data ret;
    char temp[buffer_length];
    MODULEINFO mi;

    GetModuleInformation(process, module, &mi, sizeof(mi));
    ret.base_address = mi.lpBaseOfDll;
    ret.load_size = mi.SizeOfImage;

    GetModuleFileNameExA(process, module, temp, sizeof(temp));
    ret.image_name = temp;
    GetModuleBaseNameA(process, module, temp, sizeof(temp));
    ret.module_name = temp;
    std::vector<char> img(ret.image_name.begin(), ret.image_name.end());
    std::vector<char> mod(ret.module_name.begin(), ret.module_name.end());
    SymLoadModule64(process, 0, &img[0], &mod[0], (DWORD64)ret.base_address,
                    ret.load_size);
    return ret;
  }
};

template <> class TraceResolverImpl<system_tag::windows_tag>
    : public TraceResolverImplBase {
public:
  TraceResolverImpl() {

    HANDLE process = GetCurrentProcess();

    std::vector<module_data> modules;
    DWORD cbNeeded;
    std::vector<HMODULE> module_handles(1);
    SymInitialize(process, NULL, false);
    DWORD symOptions = SymGetOptions();
    symOptions |= SYMOPT_LOAD_LINES | SYMOPT_UNDNAME;
    SymSetOptions(symOptions);
    EnumProcessModules(process, &module_handles[0],
                       module_handles.size() * sizeof(HMODULE), &cbNeeded);
    module_handles.resize(cbNeeded / sizeof(HMODULE));
    EnumProcessModules(process, &module_handles[0],
                       module_handles.size() * sizeof(HMODULE), &cbNeeded);
    std::transform(module_handles.begin(), module_handles.end(),
                   std::back_inserter(modules), get_mod_info(process));
    void *base = modules[0].base_address;
    IMAGE_NT_HEADERS *h = ImageNtHeader(base);
    image_type = h->FileHeader.Machine;
  }

  static const int max_sym_len = 255;
  struct symbol_t {
    SYMBOL_INFO sym;
    char buffer[max_sym_len];
  } sym;

  DWORD64 displacement;

  ResolvedTrace resolve(ResolvedTrace t) override {
    HANDLE process = GetCurrentProcess();

    char name[256];

    memset(&sym, 0, sizeof(sym));
    sym.sym.SizeOfStruct = sizeof(SYMBOL_INFO);
    sym.sym.MaxNameLen = max_sym_len;

    if (!SymFromAddr(process, (ULONG64)t.addr, &displacement, &sym.sym)) {
      // TODO:  error handling everywhere
      char* lpMsgBuf;
      DWORD dw = GetLastError();

      FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER |
                        FORMAT_MESSAGE_FROM_SYSTEM |
                        FORMAT_MESSAGE_IGNORE_INSERTS,
                    NULL, dw, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                    (char*)&lpMsgBuf, 0, NULL);

      printf(lpMsgBuf);

      // abort();
    }
    UnDecorateSymbolName(sym.sym.Name, (PSTR)name, 256, UNDNAME_COMPLETE);

    DWORD offset = 0;
    IMAGEHLP_LINE line;
    if (SymGetLineFromAddr(process, (ULONG64)t.addr, &offset, &line)) {
      t.object_filename = line.FileName;
      t.source.filename = line.FileName;
      t.source.line = line.LineNumber;
      t.source.col = offset;
    }

    t.source.function = name;
    t.object_filename = "";
    t.object_function = name;

    return t;
  }

  DWORD machine_type() const { return image_type; }

private:
  DWORD image_type;
};

#endif

class TraceResolver : public TraceResolverImpl<system_tag::current_tag> {};

std::vector<SourceLoc> resolve(vil::LinAllocScope& alloc, vil::span<void*> addresses) {
	// TODO PERF: lazy init kinda sucks here. Make it global?
	static TraceResolver resolver;
	static std::mutex mutex;
	static std::unordered_map<void*, SourceLoc> locs;

	// TODO PERF: use allocator here
	// auto ret = alloc.alloc<SourceLoc>(addresses.size());
	(void) alloc;
	std::vector<SourceLoc> ret(addresses.size());

	std::lock_guard lock(mutex);

	resolver.load_addresses(addresses.data(), addresses.size());

	for(auto i = 0u; i < addresses.size(); ++i) {
		auto addr = addresses[i];
		auto it = locs.find(addr);
		if(it == locs.end()) {
			auto res = resolver.resolve(ResolvedTrace(Trace(addr, i)));
			it = locs.emplace(addr, res.source).first;
		}

		ret[i] = it->second;
	}

	return ret;
}


} // namespace backward
