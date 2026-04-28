
#ifndef SHARE_CDS_CUSTOM_LOADER_SUPPORT_HPP
#define SHARE_CDS_CUSTOM_LOADER_SUPPORT_HPP

#include "classfile/compactHashtable.hpp"
#include "oops/instanceKlass.hpp"
#include "utilities/growableArray.hpp"
#include "utilities/resizableHashTable.hpp"

class ArchivedCustomLoaderClassTable {
private:
  Symbol* _loader_id;
  Array<InstanceKlass*>* _class_list;
public:
  void init(Symbol* aot_id, Array<InstanceKlass*>* class_list) {
    _loader_id = aot_id;
    _class_list = class_list;
  }
  Symbol* loader_id() const { return _loader_id; }
  address* loader_id_addr() const { return (address*)&_loader_id; }
  Array<InstanceKlass*>* class_list() const { return _class_list; }
  address* class_list_addr() const { return (address*)&_class_list; }

  void mark_pointers() {
    ArchivePtrMarker::mark_pointer(loader_id_addr());
    ArchivePtrMarker::mark_pointer(class_list_addr());
  }
};

inline bool custom_loader_class_list_equals(ArchivedCustomLoaderClassTable* table, Symbol* loader_id, int len_unused) {
  return table->loader_id()->equals(loader_id);
}

class ArchivedCustomLoaderClassTableMap : public OffsetCompactHashtable<Symbol*, ArchivedCustomLoaderClassTable*, custom_loader_class_list_equals>
{
public:
  ArchivedCustomLoaderClassTable* get_class_list(Symbol* aot_id) {
    unsigned int hash = Symbol::symbol_hash(aot_id);
    return lookup(aot_id, hash, 0 /* ignored */);
  }
};

typedef GrowableArrayCHeap<InstanceKlass*, mtClassShared> ClassList;

class ClassLoaderIdToClassTableMap : public ResizeableHashTable<Symbol*, ClassList*, AnyObj::C_HEAP, mtClass>
{
  using ResizeableHashTableBase = ResizeableHashTable<Symbol*, ClassList*, AnyObj::C_HEAP, mtClass>;
private:
  class CopyClassTableToArchive : StackObj {
  private:
    CompactHashtableWriter* _writer;
    ArchiveBuilder* _builder;
  public:
    CopyClassTableToArchive(CompactHashtableWriter* writer) : _writer(writer),
                                                              _builder(ArchiveBuilder::current())
    {}

    bool do_entry(Symbol* loader_id, ClassList* table) {
      ArchivedCustomLoaderClassTable* tableForLoader = (ArchivedCustomLoaderClassTable*)ArchiveBuilder::ro_region_alloc(sizeof(ArchivedCustomLoaderClassTable));
      assert(_builder->has_been_archived(loader_id), "must be");
      Symbol* buffered_loader_id = _builder->get_buffered_addr(loader_id);
      tableForLoader->init(buffered_loader_id, ArchiveUtils::archive_array(table));
      tableForLoader->mark_pointers();
      unsigned int hash = Symbol::symbol_hash(loader_id);
      _writer->add(hash, AOTCompressedPointers::encode_not_null((address)tableForLoader));
      return true;
    }
  };

public:
  ClassLoaderIdToClassTableMap(unsigned size, unsigned max_size) : ResizeableHashTableBase(size, max_size) {}

  void add_class(Symbol* loader_id, InstanceKlass* ik) {
    assert(loader_id != nullptr, "sanity check");
    ClassList** class_list_ptr = get(loader_id);
    ClassList* class_list = nullptr;
    if (class_list_ptr != nullptr) {
      class_list = *class_list_ptr;
    } else {
      class_list = new ClassList(1000);
      put(loader_id, class_list);
    }
    class_list->append(ik);
  }

  void write_to_archive(ArchivedCustomLoaderClassTableMap* archived_map, const char* map_name) {
    CompactHashtableStats stats;
    CompactHashtableWriter writer(number_of_entries(), &stats);
    CopyClassTableToArchive archiver(&writer);
    iterate(&archiver);
    writer.dump(archived_map, map_name);
  }
};

#endif // SHARE_CDS_CUSTOM_LOADER_SUPPORT_HPP
