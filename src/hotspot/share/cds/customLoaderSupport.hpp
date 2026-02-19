
#ifndef SHARE_CDS_CUSTOM_LOADER_SUPPORT_HPP
#define SHARE_CDS_CUSTOM_LOADER_SUPPORT_HPP

#include "classfile/compactHashtable.hpp"
#include "oops/instanceKlass.hpp"
#include "utilities/growableArray.hpp"
#include "utilities/resizableHashTable.hpp"

class CustomLoaderClassTable {
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
};

inline bool custom_loader_class_list_equals(CustomLoaderClassTable* table, Symbol* loader_id, int len_unused) {
  return table->loader_id()->equals(loader_id);
}

class ArchivedCustomLoaderClassTableMap : public OffsetCompactHashtable<Symbol*, CustomLoaderClassTable*, custom_loader_class_list_equals>
{
public:
  CustomLoaderClassTable* get_class_list(Symbol* aot_id) {
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
      CustomLoaderClassTable* tableForLoader = (CustomLoaderClassTable*)ArchiveBuilder::ro_region_alloc(sizeof(CustomLoaderClassTable));
      assert(_builder->has_been_archived(loader_id), "must be");
      Symbol* buffered_sym = _builder->get_buffered_addr(loader_id);
      tableForLoader->init(buffered_sym, ArchiveUtils::archive_array(table));
      ArchivePtrMarker::mark_pointer(tableForLoader->loader_id_addr());
      ArchivePtrMarker::mark_pointer(tableForLoader->class_list_addr());
      unsigned int hash = Symbol::symbol_hash(loader_id);
      u4 delta = _builder->buffer_to_offset_u4((address)tableForLoader);
      _writer->add(hash, delta);
      return true;
    }
  };

public:
  ClassLoaderIdToClassTableMap(unsigned size, unsigned max_size) : ResizeableHashTableBase(size, max_size) {}

  void write_to_archive(ArchivedCustomLoaderClassTableMap* archived_map, const char* map_name) {
    CompactHashtableStats stats;
    CompactHashtableWriter writer(number_of_entries(), &stats);
    CopyClassTableToArchive archiver(&writer);
    iterate(&archiver);
    writer.dump(archived_map, map_name);
  }
};

#endif // SHARE_CDS_CUSTOM_LOADER_SUPPORT_HPP
