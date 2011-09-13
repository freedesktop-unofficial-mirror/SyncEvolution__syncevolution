/*
 * Copyright (C) 2011 Intel Corporation
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) version 3.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301  USA
 */

#ifndef INCL_GLIB_SUPPORT
# define INCL_GLIB_SUPPORT

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <syncevo/util.h>
#include <syncevo/eds_abi_wrapper.h>

#ifdef HAVE_GLIB
# include <glib-object.h>
# include <gio/gio.h>
#else
typedef void *GMainLoop;
#endif

#include <boost/shared_ptr.hpp>
#include <boost/intrusive_ptr.hpp>
#include <boost/utility.hpp>
#include <boost/foreach.hpp>

#include <iterator>

#include <syncevo/declarations.h>
SE_BEGIN_CXX

enum {
    GLIB_SELECT_NONE = 0,
    GLIB_SELECT_READ = 1,
    GLIB_SELECT_WRITE = 2
};

enum GLibSelectResult {
    GLIB_SELECT_TIMEOUT,      /**< returned because not ready after given amount of time */
    GLIB_SELECT_READY,        /**< fd is ready */
    GLIB_SELECT_QUIT          /**< something else caused the loop to quit, return to caller immediately */
};

/**
 * Waits for one particular file descriptor to become ready for reading
 * and/or writing. Keeps the given loop running while waiting.
 *
 * @param  loop       loop to keep running; must not be NULL
 * @param  fd         file descriptor to watch, -1 for none
 * @param  direction  read, write, both, or none (then fd is ignored)
 * @param  timeout    timeout in seconds + nanoseconds from now, NULL for no timeout, empty value for immediate return
 * @return see GLibSelectResult
 */
GLibSelectResult GLibSelect(GMainLoop *loop, int fd, int direction, Timespec *timeout);

#ifdef HAVE_GLIB

SE_END_CXX
/** static functions for handling (un)referencing of GObjects */
class GObjectRef {
    public:
        // Regular GObject
        static void ref(GObject *obj) { g_object_ref(obj); }
        static void unref(GObject *obj) { g_object_unref(obj); }
        // GMainLoop
        static void ref(GMainLoop *obj) { g_main_loop_ref(obj); }
        static void unref(GMainLoop *obj) { g_main_loop_unref(obj); }
#ifdef ENABLE_EBOOK
        // EBookQuery
        static void ref(EBookQuery *query) { e_book_query_ref(query); }
        static void unref(EBookQuery *query) { e_book_query_unref(query); }
#endif
};
SE_BEGIN_CXX

/**
 * Defines a shared pointer for a GObject-based type, with intrusive
 * reference counting. Use *outside* of SyncEvolution namespace
 * (i.e. outside of SE_BEGIN/END_CXX. This is necessary because some
 * functions must be put into the boost namespace. The type itself is
 * *inside* the SyncEvolution namespace.
 *
 * Example:
 * SE_GOBJECT_TYPE(GFile)
 * SE_BEGIN_CXX
 * {
 *   // reference normally increased during construction,
 *   // steal() avoids that
 *   GFileCXX filecxx = GFileCXX::steal(g_file_new_for_path("foo"));
 *   GFile *filec = filecxx.get(); // does not increase reference count
 *   // file freed here as filecxx gets destroyed
 * }
 * SE_END_CXX
 *
 * If the GObject-based type uses custom ref/unref functions, then use 
 * SE_GOBJECT_BASED_TYPE(_x, _base) macro instead and indicate the base.
 * Make sure corresponding ref()/unref() functions are defined in the
 * GObjectRef class.
 *
 * Example:
 * SE_GOBJECT_BASED_TYPE(EBookQuery, EBookQuery)
 */
#define SE_GOBJECT_BASED_TYPE(_x, _base) \
    void inline intrusive_ptr_add_ref(_x *ptr) { GObjectRef::ref((_base *)ptr); } \
    void inline intrusive_ptr_release(_x *ptr) { GObjectRef::unref((_base *)ptr); } \
    SE_BEGIN_CXX \
    class _x ## CXX : public boost::intrusive_ptr<_x> { \
    public: \
         _x ## CXX(_x *ptr, bool add_ref = true) : boost::intrusive_ptr<_x>(ptr, add_ref) {} \
         _x ## CXX() {} \
         _x ## CXX(const _x ## CXX &other) : boost::intrusive_ptr<_x>(other) {} \
         operator _x * () { return boost::intrusive_ptr<_x>::get(); } \
\
         static  _x ## CXX steal(_x *ptr) { return _x ## CXX(ptr, false); } \
    }; \
    SE_END_CXX \

#define SE_GOBJECT_TYPE(_x) SE_GOBJECT_BASED_TYPE(_x, GObject)

SE_END_CXX

SE_GOBJECT_TYPE(GFile)
SE_GOBJECT_TYPE(GFileMonitor)

SE_BEGIN_CXX

/**
 * Wrapper around g_file_monitor_file().
 * Not copyable because monitor is tied to specific callback
 * via memory address.
 */
class GLibNotify : public boost::noncopyable
{
 public:
    typedef boost::function<void (GFile *, GFile *, GFileMonitorEvent)> callback_t;

    GLibNotify(const char *file, 
               const callback_t &callback);
 private:
    GFileMonitorCXX m_monitor;
    callback_t m_callback;
};

/**
 * always throws an exception, including information from GError if available:
 * <action>: <error message>|failure
 *
 * Takes ownership of the error and frees it.
 *
 * Deprecated. Better use GErrorCXX.
 */
void GLibErrorException(const std::string &action, GError *error);


/**
 * Wraps GError. Where a GError** is expected, simply pass
 * a GErrorCXX instance.
 */
struct GErrorCXX {
    GError *m_gerror;

    /** empty error, NULL pointer */
    GErrorCXX() : m_gerror(NULL) {}

    /** copies error content */
    GErrorCXX(const GErrorCXX &other) : m_gerror(g_error_copy(other.m_gerror)) {}
    GErrorCXX &operator =(const GErrorCXX &other) {
        if (this != &other) {
            if (m_gerror) {
                g_clear_error(&m_gerror);
            }
            if (other.m_gerror) {
                m_gerror = g_error_copy(other.m_gerror);
            }
        }
        return *this;
    }
    GErrorCXX &operator =(const GError* err) {
       if (m_gerror) {
           g_clear_error(&m_gerror);
       }
       if (err) {
           m_gerror = g_error_copy(err);
       }
       return *this;
    }

    /** For convenient access to GError members (message, domain, ...) */
    GError*	operator-> () const { return m_gerror; }

    /** error description, with fallback if not set (not expected, so not localized) */
    operator const char * () { return m_gerror ? m_gerror->message : "<<no error>>"; }

    /** Check if a gError was set */
    bool isNull() const { return m_gerror == NULL; }

    /** clear error */
    ~GErrorCXX() { g_clear_error(&m_gerror); }

    /** clear error if any is set */
    void clear() { g_clear_error(&m_gerror); }

    /**
     * Use this when passing GErrorCXX instance to C functions which need to set it.
     * Make sure the pointer isn't set yet (new GErrorCXX instance, reset if
     * an error was encountered before) or the GNOME functions will complain
     * when overwriting the existing error.
     */
    operator GError ** () { return &m_gerror; }

    /**
     * Use this when passing GErrorCXX instance to C functions to read it.
     */
    operator GError * () { return m_gerror; }

    /**
     * always throws an exception, including information from GError if available:
     * <action>: <error message>|failure
     */
    void throwError(const std::string &action);
};

template<class T> void NoopDestructor(T *) {}
template<class T> void GObjectDestructor(T *obj) { g_object_unref(obj); }

/**
 * Wraps a G[S]List of pointers to a specific type.
 * Can be used with boost::FOREACH and provides forward iterators
 * (two-way iterators and reverse iterators also possible, but not implemented).
 * Frees the list and optionally (not turned on by default) also frees
 * the data contained in it, using the provided destructor class.
 * Use GObjectDestructor for GObject instances.
 *
 * @param T    the type of the instances pointed to inside the list
 * @param L    GList or GSList
 * @param D    destructor function freeing a T instance
 */
template< class T, class L, void (*D)(T*) = NoopDestructor<T> > struct GListCXX : boost::noncopyable {
    L *m_list;

    static void listFree(GSList *l) { g_slist_free(l); }
    static void listFree(GList *l) { g_list_free(l); }

    static GSList *listPrepend(GSList *list, T *entry) { return g_slist_prepend(list, (gpointer)entry); }
    static GList *listPrepend(GList *list, T *entry) { return g_list_prepend(list, (gpointer)entry); }

    static GSList *listAppend(GSList *list, T *entry) { return g_slist_append(list, (gpointer)entry); }
    static GList *listAppend(GList *list, T *entry) { return g_list_append(list, (gpointer)entry); }

 public:
    typedef T * value_type;

    /** empty error, NULL pointer */
    GListCXX() : m_list(NULL) {}

    GListCXX(L* l) : m_list(l) {}

    /** free list */
    ~GListCXX() { clear(); }

    /** clear error if any is set */
    void clear() {
#if 1
        BOOST_FOREACH(T *entry, *this) {
            D(entry);
        }
#else
        for (iterator it = begin();
             it != end();
             ++it) {
            D(*it);
        }
#endif
        listFree(m_list);
        m_list = NULL;
    }

    /**
     * Use this when passing GListCXX instance to C functions which need to set it.
     * Make sure the pointer isn't set yet (new GListCXX instance or cleared).
     */
    operator L ** () { return &m_list; }

    /**
     * Cast to plain G[S]List, for use in functions which do not modify the list.
     */
    operator L * () { return m_list; }

    class iterator : public std::iterator<std::forward_iterator_tag, T *> {
        L *m_entry;
    public:
        iterator(L *list) : m_entry(list) {}
        iterator(const iterator &other) : m_entry(other.m_entry) {}
        /**
         * boost::foreach needs a reference as return code here,
         * which forces us to do type casting on the address of the void * pointer,
         * then dereference the pointer. The reason is that typecasting the
         * pointer value directly yields an rvalue, which can't be used to initialize
         * the reference return value.
         */
        T * &operator -> () const { return *getEntryPtr(); }
        T * &operator * () const { return *getEntryPtr(); }
        iterator & operator ++ () { m_entry = m_entry->next; return *this; }
        iterator operator ++ (int) { return iterator(m_entry->next); }
        bool operator == (const iterator &other) { return m_entry == other.m_entry; }
        bool operator != (const iterator &other) { return m_entry != other.m_entry; }

    private:
        /**
         * Used above, necessary to hide the fact that we do type
         * casting tricks. Otherwise the compiler will complain about
         * *(T **)&m_entry->data with "dereferencing type-punned
         * pointer will break strict-aliasing rules".
         *
         * That warning is about breaking assumptions that the compiler
         * uses for optimizations. The hope is that those optimzations
         * aren't done here, and/or are disabled by using a function.
         */
        T** getEntryPtr() const { return (T **)&m_entry->data; }
    };
    iterator begin() { return iterator(m_list); }
    iterator end() { return iterator(NULL); }

    class const_iterator : public std::iterator<std::forward_iterator_tag, T *> {
        L *m_entry;
        T *m_value;

    public:
        const_iterator(L *list) : m_entry(list) {}
        const_iterator(const const_iterator &other) : m_entry(other.m_entry) {}
        T * &operator -> () const { return *getEntryPtr(); }
        T * &operator * () const { return *getEntryPtr(); }
        const_iterator & operator ++ () { m_entry = m_entry->next; return *this; }
        const_iterator operator ++ (int) { return iterator(m_entry->next); }
        bool operator == (const const_iterator &other) { return m_entry == other.m_entry; }
        bool operator != (const const_iterator &other) { return m_entry != other.m_entry; }

    private:
        T** getEntryPtr() const { return (T **)&m_entry->data; }
    };

    const_iterator begin() const { return const_iterator(m_list); }
    const_iterator end() const { return const_iterator(NULL); }

    void push_back(T *entry) { m_list = listAppend(m_list, entry); }
    void push_front(T *entry) { m_list = listPrepend(m_list, entry); }
};

/**
 * Wraps a C gchar array and takes care of freeing the memory.
 */
class PlainGStr : public boost::shared_ptr<gchar>
{
    public:
        PlainGStr() {}
        PlainGStr(gchar *str) : boost::shared_ptr<char>(str, g_free) {}
        PlainGStr(const PlainGStr &other) : boost::shared_ptr<gchar>(other) {}    
        operator const gchar *() const { return &**this; }
        const gchar *c_str() const { return &**this; }
};

/**
 * Wraps a pointer to one or more instances of T which need to be freed by g_free().
 * Like GListCXX above this can be used in functions which set the pointer to
 * memory that needs to be free by the caller.
 *
 * Example:
 * GMemPtr<gchar> uid;
 * e_cal_client_create_object_sync (...,
 *                                  uid, // gchar **uid
 *                                  ...);
 */
template<class T> class GMemPtr : boost::noncopyable {
    T *m_ptr;

 public:
    GMemPtr(T *ptr = NULL) : m_ptr(ptr) {}
    ~GMemPtr() { g_free((void *)m_ptr); }

    operator const T * () const { return m_ptr; }
    operator T ** () { return &m_ptr; }
};

#endif

SE_END_CXX

#endif // INCL_GLIB_SUPPORT

