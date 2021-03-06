#ifndef __TREE_MAP__
#define __TREE_MAP__

#include <stddef.h>
#include <new>

#include <muos/compiler.h>
#include <muos/decls.h>

/**
 * \brief   Classic AVL balanced tree implementation.
 *
 * Internal nodes of the tree are allocated dynamically by a slab.
 *
 * \class RawTreeMap tree-map.hpp kernel/tree-map.hpp
 */
class RawTreeMap
{
public:
    /**
     * Forward declaration
     */
    class InternalNode;

    /**
     * \brief   Key types
     */
    typedef void const * Key_t;

    /**
     * \brief   Value types
     */
    typedef void * Value_t;

    /**
     * \brief   Key comparison function
     *
     * Return -1 if <tt>left</tt> is less than <tt>right</tt>, 0 if
     * <tt>left</tt> is equal to <tt>right</tt>, and +1 if <tt>left</tt>
     * is greater than <tt>right</tt>.
     */
    typedef int (*CompareFunc) (
            Key_t left,
            Key_t right
            );

    typedef void (*ForeachFunc) (
            Key_t key,
            Value_t value,
            void * user_data
            );

    /**
     * \brief   Canned key-comparision function suitable for comparing
     *          keys that are virtual memory addresses.
     */
    static CompareFunc AddressCompareFunc;

    /**
     * \brief  Canned key-comparison function suitable for comparing
     *         keys that are signed integers.
     */
    static CompareFunc SignedIntCompareFunc;

public:
    void *  operator new    (size_t) throw (std::bad_alloc);
    void    operator delete (void *) throw ();

    /**
     * \brief   Make a tree instance
     */
    RawTreeMap (CompareFunc comparator) throw ();

    /**
     * \brief   Destroy a tree instance.
     */
    ~RawTreeMap () throw ();

    /**
     * \brief   Maps <tt>key</tt> to <tt>value</tt>.
     *
     * Returns any previous value that was mapped to <tt>key</tt>.
     */
    Value_t Insert (
            Key_t key,
            Value_t value
            );

    /**
     * \brief   Removes any mapping from <tt>key</tt> to a value.
     *
     * Returns the value (if any) that was mapped to <tt>key</tt>.
     */
    Value_t Remove (
            Key_t key
            );

    /**
     * \brief   Finds the value (if any) mapped to <tt>key</tt>
     */
    Value_t Lookup (
            Key_t key
            );

    /**
     * \brief   Returns number of entries in the map
     */
    unsigned int Size ();

    /**
     * \brief   Visit each key/value pair current stored in the map
     */
    void Foreach (
            ForeachFunc func,
            void * user_data
            );

private:
    /**
     * \brief   Number of elements mapped in this tree instance
     */
    unsigned int size;

    /**
     * \brief   Root of the internal hierarchical tree
     */
    InternalNode * root;

    /**
     * \brief   Comparison function used to sort keys
     */
    CompareFunc comparator;

private:
    void *  operator new[]      (size_t);
    void    operator delete[]   (void *);
};

/**
 * \brief   Type-safe classic AVL balanced tree implementation.
 *
 * This is a convenience wrapper around RawTreeMap to free user
 * code from using casts during all operations.
 *
 * \tparam K    Key type. Must be immutable and const.
 * \tparam V    Value type.
 *
 * \class TreeMap tree-map.hpp kernel/tree-map.hpp
 */
template<typename K, typename V>
    class TreeMap : public RawTreeMap
    {

    COMPILER_ASSERT(sizeof(K) == sizeof(Key_t));
    COMPILER_ASSERT(sizeof(V) == sizeof(Value_t));

    public:
        inline TreeMap (CompareFunc comparator) throw ()
            : RawTreeMap(comparator)
        {
        }

        inline ~TreeMap () throw ()
        {
        }

        inline V Insert (K key, V value)
        {
            return reinterpret_cast<V>(RawTreeMap::Insert(reinterpret_cast<Key_t>(key), reinterpret_cast<Value_t>(value)));
        }

        inline V Remove (K key)
        {
            return reinterpret_cast<V>(RawTreeMap::Remove(reinterpret_cast<Key_t>(key)));
        }

        inline V Lookup (K key)
        {
            return reinterpret_cast<V>(RawTreeMap::Lookup(reinterpret_cast<Key_t>(key)));
        }
    };

#endif /* __TREE_MAP__ */
