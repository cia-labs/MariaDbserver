/* Copyright (c) 2016 MariaDB corporation

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA */

#ifndef UNIQUE_INCLUDED
#define UNIQUE_INCLUDED

#include "filesort.h"

/*

  Descriptor class storing information about the keys that would be
  inserted in the Unique tree. This is an abstract class which is
  extended by other class to support descriptors for keys with fixed and
  variable size.
*/
class Descriptor : public Sql_alloc
{
protected:
  uint key_length;
  enum attributes
  {
    FIXED_SIZED_KEYS= 0,
    VARIABLE_SIZED_KEYS_WITH_ORIGINAL_VALUES
  };
  uint flags;

public:
  virtual ~Descriptor() {};
  virtual uint get_length_of_key(uchar *ptr) = 0;
  bool is_variable_sized()
  {
    return flags & (1 << VARIABLE_SIZED_KEYS_WITH_ORIGINAL_VALUES);
  }
  virtual int compare_keys(uchar *a, uchar *b) = 0;
  virtual int compare_keys_for_single_arg(uchar *a, uchar *b) = 0;
  virtual bool setup(THD *thd, Item_sum *item,
                     uint non_const_args, uint arg_count) { return false; }
  virtual bool setup(THD *thd, Field *field) { return false; }
  virtual uchar *get_packed_rec_ptr() { return NULL; }
  virtual uint make_packed_record(bool exclude_nulls) { return 0; }
  virtual Sort_keys *get_keys() { return NULL; }
  SORT_FIELD *get_sortorder() { return NULL; }
};


/*
  Descriptor for fixed size keys
*/
class Fixed_size_keys_descriptor : public Descriptor
{
public:
  Fixed_size_keys_descriptor(uint length);
  ~Fixed_size_keys_descriptor() {}
  uint get_length_of_key(uchar *ptr) override { return key_length; }
  int compare_keys(uchar *a, uchar *b) override { return 0; }
  int compare_keys_for_single_arg(uchar *a, uchar *b) override { return 0; }
};


/*
  Descriptor for variable size keys
*/
class Variable_size_keys_descriptor : public Descriptor
{
  /*
    Packed record ptr for a record of the table, the packed value in this
    record is added to the unique tree
  */
  uchar* packed_rec_ptr;

  String tmp_buffer;

  /*
    Array of SORT_FIELD structure storing the information about the key parts
    in the sort key of the Unique tree
    @see Unique::setup()
  */
  SORT_FIELD *sortorder;

  /*
    Structure storing information about usage of keys
  */
  Sort_keys *sort_keys;

public:
  Variable_size_keys_descriptor(uint length);
  ~Variable_size_keys_descriptor();

  uchar *get_packed_rec_ptr() { return packed_rec_ptr; }
  Sort_keys *get_keys() { return sort_keys; }
  SORT_FIELD *get_sortorder() { return sortorder; }

  uint make_packed_record(bool exclude_nulls);
  uint get_length_of_key(uchar *ptr) override
  {
    return read_packed_length(ptr);
  }
  int compare_keys(uchar *a, uchar *b) override;
  int compare_keys_for_single_arg(uchar *a, uchar *b);

  // Fill structures like sort_keys, sortorder
  bool setup(THD *thd, Item_sum *item,
            uint non_const_args, uint arg_count);
  bool setup(THD *thd, Field *field);
  // returns the length of the key along with the length bytes for the key
  static uint read_packed_length(uchar *p)
  {
    return size_of_length_field + uint4korr(p);
  }
  void store_packed_length(uchar *p, uint sz)
  {
    int4store(p, sz - size_of_length_field);
  }

  static const uint size_of_length_field= 4;
};


/*
   Unique -- An abstract class for unique (removing duplicates).
*/

class Unique : public Sql_alloc {

protected:
  Descriptor *m_descriptor;
public:

  virtual void reset() = 0;
  virtual bool unique_add(void *ptr) = 0;
  virtual ~Unique() {};

  virtual void close_for_expansion() = 0;

  virtual bool get(TABLE *table) = 0;
  virtual bool walk(TABLE *table, tree_walk_action action,
                    void *walk_action_arg)= 0;

  virtual SORT_INFO *get_sort() = 0;

  virtual ulong get_n_elements() = 0;
  virtual size_t get_max_in_memory_size() const = 0;
  virtual bool is_in_memory() = 0;

  // This will be renamed:
  virtual ulong elements_in_tree() = 0;
  Descriptor *get_descriptor() { return m_descriptor; }
};

/*
   Unique_impl -- class for unique (removing of duplicates).
   Puts all values to the TREE. If the tree becomes too big,
   it's dumped to the file. User can request sorted values, or
   just iterate through them. In the last case tree merging is performed in
   memory simultaneously with iteration, so it should be ~2-3x faster.
*/

class Unique_impl : public Unique {
  DYNAMIC_ARRAY file_ptrs;
  /* Total number of elements that will be stored in-memory */
  ulong max_elements;
  size_t max_in_memory_size;
  IO_CACHE file;
  TREE tree;
 /* Number of elements filtered out due to min_dupl_count when storing results
    to table. See Unique::get */
  ulong filtered_out_elems;
  uint size;

  uint full_size;   /* Size of element + space needed to store the number of
                       duplicates found for the element. */
  uint min_dupl_count;   /* Minimum number of occurences of element required for
                            it to be written to record_pointers.
                            always 0 for unions, > 0 for intersections */
  bool with_counters;
  /*
    size in bytes used for storing keys in the Unique tree
  */
  size_t memory_used;
  ulong elements;
  SORT_INFO sort;

  bool merge(TABLE *table, uchar *buff, size_t size, bool without_last_merge);
  bool flush();

  // return the amount of unused memory in the Unique tree
  size_t space_left()
  {
    DBUG_ASSERT(max_in_memory_size >= memory_used);
    return max_in_memory_size - memory_used;
  }

  // Check if the Unique tree is full or not
  bool is_full(size_t record_size)
  {
    if (!tree.elements_in_tree)  // Atleast insert one element in the tree
      return false;
    return record_size > space_left();
  }

public:

  /*
    @brief
      Returns the number of elements in the unique instance

    @details
      If all the elements fit in the memeory, then this returns all the
      distinct elements.
  */
  ulong get_n_elements() override
  {
    return is_in_memory() ? elements_in_tree() : elements;
  }

  SORT_INFO *get_sort() override { return &sort; }

  Unique_impl(qsort_cmp2 comp_func, void *comp_func_fixed_arg,
         uint size_arg, size_t max_in_memory_size_arg,
         uint min_dupl_count_arg, Descriptor *desc);
  virtual ~Unique_impl();
  ulong elements_in_tree() { return tree.elements_in_tree; }

  bool unique_add(void *ptr) override
  {
    return unique_add(ptr, m_descriptor->get_length_of_key((uchar*)ptr));
  }

  /*
    @brief
      Add a record to the Unique tree
    @param
      ptr                      key value
      size                     length of the key
  */

  bool unique_add(void *ptr, uint size_arg)
  {
    DBUG_ENTER("unique_add");
    DBUG_PRINT("info", ("tree %u - %lu", tree.elements_in_tree, max_elements));
    TREE_ELEMENT *res;
    size_t rec_size= size_arg + sizeof(TREE_ELEMENT) + tree.size_of_element;

    if (!(tree.flag & TREE_ONLY_DUPS) && is_full(rec_size) && flush())
      DBUG_RETURN(1);
    uint count= tree.elements_in_tree;
    res= tree_insert(&tree, ptr, size_arg, tree.custom_arg);
    if (tree.elements_in_tree != count)
    {
      /*
        increment memory used only when a unique element is inserted
        in the tree
      */
      memory_used+= rec_size;
    }
    DBUG_RETURN(!res);
  }

  bool is_in_memory() { return (my_b_tell(&file) == 0); }
  void close_for_expansion() { tree.flag= TREE_ONLY_DUPS; }

  bool get(TABLE *table);
  
  /* Cost of searching for an element in the tree */
  inline static double get_search_cost(ulonglong tree_elems,
                                       double compare_factor)
  {
    return log((double) tree_elems) / (compare_factor * M_LN2);
  }  

  static double get_use_cost(uint *buffer, size_t nkeys, uint key_size,
                             size_t max_in_memory_size, double compare_factor,
                             bool intersect_fl, bool *in_memory);
  inline static int get_cost_calc_buff_size(size_t nkeys, uint key_size,
                                            size_t max_in_memory_size)
  {
    size_t max_elems_in_tree=
      max_in_memory_size / ALIGN_SIZE(sizeof(TREE_ELEMENT)+key_size);

    if (max_elems_in_tree == 0)
      max_elems_in_tree= 1;
    return (int) (sizeof(uint)*(1 + nkeys/max_elems_in_tree));
  }

  void reset() override;
  bool walk(TABLE *table, tree_walk_action action, void *walk_action_arg);

  uint get_size() const { return size; }
  uint get_full_size() const { return full_size; }
  size_t get_max_in_memory_size() const { return max_in_memory_size; }
  bool is_count_stored() { return with_counters; }
  IO_CACHE *get_file ()  { return &file; }
  virtual int write_record_to_file(uchar *key);

  // returns TRUE if the unique tree stores packed values
  bool is_variable_sized() { return m_descriptor->is_variable_sized(); }
  Descriptor* get_descriptor() { return m_descriptor; }

  friend int unique_write_to_file(uchar* key, element_count count, Unique_impl *unique);
  friend int unique_write_to_ptrs(uchar* key, element_count count, Unique_impl *unique);

  friend int unique_write_to_file_with_count(uchar* key, element_count count,
                                             Unique_impl *unique);
  friend int unique_intersect_write_to_ptrs(uchar* key, element_count count,
				            Unique_impl *unique);
};

#endif /* UNIQUE_INCLUDED */
