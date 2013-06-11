/*
 * Copyright (C) 2005-2009 Patrick Ohly <patrick.ohly@gmx.de>
 * Copyright (C) 2009 Intel Corporation
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

#include <memory>
#include <map>
#include <sstream>
#include <list>
using namespace std;

#include "config.h"
#include "EvolutionSyncSource.h"

#ifdef ENABLE_EBOOK

#include <syncevo/SyncContext.h>
#include "EvolutionContactSource.h"
#include <syncevo/util.h>

#include <syncevo/Logging.h>

#include <boost/algorithm/string.hpp>
#include <boost/algorithm/string/join.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/foreach.hpp>

#include <syncevo/declarations.h>

void inline intrusive_ptr_add_ref(EBookQuery *ptr) { e_book_query_ref(ptr); }
void inline intrusive_ptr_release(EBookQuery *ptr) { e_book_query_unref(ptr); }
SE_BEGIN_CXX
typedef boost::intrusive_ptr<EBookQuery> EBookQueryCXX;
SE_END_CXX

SE_BEGIN_CXX

inline bool IsContactNotFound(const GError *gerror) {
    return gerror &&
#ifdef USE_EDS_CLIENT
        gerror->domain == E_BOOK_CLIENT_ERROR &&
        gerror->code == E_BOOK_CLIENT_ERROR_CONTACT_NOT_FOUND
#else
        gerror->domain == E_BOOK_ERROR &&
        gerror->code == E_BOOK_ERROR_CONTACT_NOT_FOUND
#endif
        ;
}


const EvolutionContactSource::extensions EvolutionContactSource::m_vcardExtensions;
const EvolutionContactSource::unique EvolutionContactSource::m_uniqueProperties;

EvolutionContactSource::EvolutionContactSource(const SyncSourceParams &params,
                                               EVCardFormat vcardFormat) :
    EvolutionSyncSource(params),
    m_vcardFormat(vcardFormat)
{
    SyncSourceLogging::init(InitList<std::string>("N_FIRST") + "N_MIDDLE" + "N_LAST",
                            " ",
                            m_operations);
}

EvolutionContactSource::~EvolutionContactSource()
{
    // Don't close while we have pending operations.  They might
    // complete after we got destroyed, causing them to use an invalid
    // "this" pointer. We also don't know how well EDS copes with
    // closing the address book while it has pending operations - EDS
    // maintainer mcrha wasn't sure.
    //
    // TODO: cancel the operations().
    finishItemChanges();
    close();
}

#ifdef USE_EDS_CLIENT
static EClient *newEBookClient(ESource *source,
                               GError **gerror)
{
    return E_CLIENT(e_book_client_new(source, gerror));
}
#endif

EvolutionSyncSource::Databases EvolutionContactSource::getDatabases()
{
    Databases result;
#ifdef USE_EDS_CLIENT
    getDatabasesFromRegistry(result,
                             E_SOURCE_EXTENSION_ADDRESS_BOOK,
                             e_source_registry_ref_default_address_book);
#else
    ESourceList *sources = NULL;
    if (!e_book_get_addressbooks(&sources, NULL)) {
        SyncContext::throwError("unable to access address books");
    }

    Databases secondary;
    for (GSList *g = e_source_list_peek_groups (sources); g; g = g->next) {
        ESourceGroup *group = E_SOURCE_GROUP (g->data);
        for (GSList *s = e_source_group_peek_sources (group); s; s = s->next) {
            ESource *source = E_SOURCE (s->data);
            eptr<char> uri(e_source_get_uri(source));
            string uristr;
            if (uri) {
                uristr = uri.get();
            }
            Database entry(e_source_peek_name(source),
                           uristr,
                           false);
            if (boost::starts_with(uristr, "couchdb://")) {
                // Append CouchDB address books at the end of the list,
                // otherwise preserving the order of address books.
                //
                // The reason is Moblin Bugzilla #7877 (aka CouchDB
                // feature request #479110): the initial release of
                // evolution-couchdb in Ubuntu 9.10 is unusable because
                // it does not support the REV property.
                //
                // Reordering the entries ensures that the CouchDB
                // address book is not used as the default database by
                // SyncEvolution, as it happened in Ubuntu 9.10.
                // Users can still pick it intentionally via
                // "evolutionsource".
                secondary.push_back(entry);
            } else {
                result.push_back(entry);
            }
        }
    }
    result.insert(result.end(), secondary.begin(), secondary.end());

    // No results? Try system address book (workaround for embedded Evolution Dataserver).
    if (!result.size()) {
        eptr<EBook, GObject> book;
        GErrorCXX gerror;
        const char *name;

        name = "<<system>>";
        book = e_book_new_system_addressbook (gerror);
        gerror.clear();
        if (!book) {
            name = "<<default>>";
            book = e_book_new_default_addressbook (gerror);
        }

        if (book) {
            const char *uri = e_book_get_uri (book);
            result.push_back(Database(name, uri, true));
        }
    } else {
        //  the first DB found is the default
        result[0].m_isDefault = true;
    }
#endif

    return result;
}

void EvolutionContactSource::open()
{
#ifdef USE_EDS_CLIENT
    m_addressbook.reset(E_BOOK_CLIENT(openESource(E_SOURCE_EXTENSION_ADDRESS_BOOK,
                                                  e_source_registry_ref_builtin_address_book,
                                                  newEBookClient).get()));
    const char *mode = getEnv("SYNCEVOLUTION_EDS_ACCESS_MODE", "");
    m_accessMode = boost::iequals(mode, "synchronous") ? SYNCHRONOUS :
        boost::iequals(mode, "batched") ? BATCHED :
        DEFAULT;
#else
    GErrorCXX gerror;
    bool created = false;
    bool onlyIfExists = false; // always try to create address book, because even if there is
                               // a source there's no guarantee that the actual database was
                               // created already; the original logic below for only setting
                               // this when explicitly requesting a new address book
                               // therefore failed in some cases
    ESourceList *tmp;
    if (!e_book_get_addressbooks(&tmp, gerror)) {
        throwError("unable to access address books", gerror);
    }
    ESourceListCXX sources(tmp, TRANSFER_REF);

    string id = getDatabaseID();
    ESource *source = findSource(sources, id);
    if (!source) {
        // might have been special "<<system>>" or "<<default>>", try that and
        // creating address book from file:// URI before giving up
        if (id.empty() || id == "<<system>>") {
            m_addressbook.set( e_book_new_system_addressbook (gerror), "system address book" );
        } else if (id.empty() || id == "<<default>>") {
            m_addressbook.set( e_book_new_default_addressbook (gerror), "default address book" );
        } else if (boost::starts_with(id, "file://")) {
            m_addressbook.set(e_book_new_from_uri(id.c_str(), gerror), "creating address book");
        } else {
            throwError(string(getName()) + ": no such address book: '" + id + "'");
        }
        created = true;
    } else {
        m_addressbook.set( e_book_new( source, gerror ), "address book" );
    }
 
    if (!e_book_open( m_addressbook, onlyIfExists, gerror) ) {
        if (created) {
            // opening newly created address books often fails, try again once more
            sleep(5);
            if (!e_book_open(m_addressbook, onlyIfExists, gerror)) {
                throwError("opening address book", gerror);
            }
        } else {
            throwError("opening address book", gerror);
        }
    }

    // users are not expected to configure an authentication method,
    // so pick one automatically if the user indicated that he wants authentication
    // by setting user or password
    std::string user = getUser(),
        passwd = getPassword();
    if (!user.empty() || !passwd.empty()) {
        GList *authmethod;
        if (!e_book_get_supported_auth_methods(m_addressbook, &authmethod, gerror)) {
            throwError("getting authentication methods", gerror);
        }
        while (authmethod) {
            const char *method = (const char *)authmethod->data;
            SE_LOG_DEBUG(getDisplayName(), "trying authentication method \"%s\", user %s, password %s",
                         method,
                         !user.empty() ? "configured" : "not configured",
                         !passwd.empty() ? "configured" : "not configured");
            if (e_book_authenticate_user(m_addressbook,
                                         user.c_str(),
                                         passwd.c_str(),
                                         method,
                                         gerror)) {
                SE_LOG_DEBUG(getDisplayName(), "authentication succeeded");
                break;
            } else {
                SE_LOG_ERROR(getDisplayName(), "authentication failed: %s", gerror->message);
            }
            authmethod = authmethod->next;
        }
    }

    g_signal_connect_after(m_addressbook,
                           "backend-died",
                           G_CALLBACK(SyncContext::fatalError),
                           (void *)"Evolution Data Server has died unexpectedly, contacts no longer available.");
#endif
}

bool EvolutionContactSource::isEmpty()
{
    // TODO: add more efficient implementation which does not
    // depend on actually pulling all items from EDS
    RevisionMap_t revisions;
    listAllItems(revisions);
    return revisions.empty();
}

#ifdef USE_EDS_CLIENT
class EBookClientViewSyncHandler {
    public:
        typedef boost::function<void (const GSList *list)> Process_t;

        EBookClientViewSyncHandler(const EBookClientViewCXX &view,
                                   const Process_t &process) :
            m_process(process),
            m_view(view)
        {}

        bool process(GErrorCXX &gerror) {
            // Listen for view signals
            m_view.connectSignal<void (EBookClientView *ebookview,
                                       const GSList *contacts)>("objects-added",
                                                                boost::bind(m_process, _2));
            m_view.connectSignal<void (EBookClientView *ebookview,
                                       const GError *error)>("complete",
                                                             boost::bind(&EBookClientViewSyncHandler::completed, this, _2));

            // Start the view
            e_book_client_view_start (m_view, m_error);
            if (m_error) {
                std::swap(gerror, m_error);
                return false;
            }

            // Async -> Sync
            m_loop.run();
            e_book_client_view_stop (m_view, NULL); 

            if (m_error) {
                std::swap(gerror, m_error);
                return false;
            } else {
                return true;
            }
        }
 
        void completed(const GError *error) {
            m_error = error;
            m_loop.quit();
        }

    public:
        // Event loop for Async -> Sync
        EvolutionAsync m_loop;

    private:
        // Process list callback
        boost::function<void (const GSList *list)> m_process;
        // View watched
        EBookClientViewCXX m_view;
        // Possible error while watching the view
        GErrorCXX m_error;
};

static void list_revisions(const GSList *contacts, EvolutionContactSource::RevisionMap_t *revisions)
{
    const GSList *l;

    for (l = contacts; l; l = l->next) {
        EContact *contact = E_CONTACT(l->data);
        if (!contact) {
            SE_THROW("contact entry without data");
        }
        pair<string, string> revmapping;
        const char *uid = (const char *)e_contact_get_const(contact,
                                                            E_CONTACT_UID);
        if (!uid || !uid[0]) {
            SE_THROW("contact entry without UID");
        }
        revmapping.first = uid;
        const char *rev = (const char *)e_contact_get_const(contact,
                                                            E_CONTACT_REV);
        if (!rev || !rev[0]) {
            SE_THROW(string("contact entry without REV: ") + revmapping.first);
        }
        revmapping.second = rev;
        revisions->insert(revmapping);
    }
}

#endif

void EvolutionContactSource::listAllItems(RevisionMap_t &revisions)
{
#ifdef USE_EDS_CLIENT
    GErrorCXX gerror;
    EBookClientView *view;

    EBookQueryCXX allItemsQuery(e_book_query_any_field_contains(""), TRANSFER_REF);
    PlainGStr sexp(e_book_query_to_string (allItemsQuery.get()));
    
    if (!e_book_client_get_view_sync(m_addressbook, sexp, &view, NULL, gerror)) {
        throwError( "getting the view" , gerror);
    }
    EBookClientViewCXX viewPtr = EBookClientViewCXX::steal(view);

    // Optimization: set fields_of_interest (UID / REV)
    GListCXX<const char, GSList> interesting_field_list;
    interesting_field_list.push_back(e_contact_field_name (E_CONTACT_UID));
    interesting_field_list.push_back(e_contact_field_name (E_CONTACT_REV));
    e_book_client_view_set_fields_of_interest (viewPtr, interesting_field_list, gerror);
    if (gerror) {
        SE_LOG_ERROR(getDisplayName(), "e_book_client_view_set_fields_of_interest: %s", (const char*)gerror);
        gerror.clear();
    }

    EBookClientViewSyncHandler handler(viewPtr, boost::bind(list_revisions, _1, &revisions));
    if (!handler.process(gerror)) {
        throwError("watching view", gerror);
    }
#else
    GErrorCXX gerror;
    eptr<EBookQuery> allItemsQuery(e_book_query_any_field_contains(""), "query");
    GList *nextItem;
    if (!e_book_get_contacts(m_addressbook, allItemsQuery, &nextItem, gerror)) {
        throwError( "reading all items", gerror );
    }
    eptr<GList> listptr(nextItem);
    while (nextItem) {
        EContact *contact = E_CONTACT(nextItem->data);
        if (!contact) {
            throwError("contact entry without data");
        }
        pair<string, string> revmapping;
        const char *uid = (const char *)e_contact_get_const(contact,
                                                            E_CONTACT_UID);
        if (!uid || !uid[0]) {
            throwError("contact entry without UID");
        }
        revmapping.first = uid;
        const char *rev = (const char *)e_contact_get_const(contact,
                                                            E_CONTACT_REV);
        if (!rev || !rev[0]) {
            throwError(string("contact entry without REV: ") + revmapping.first);
        }
        revmapping.second = rev;
        revisions.insert(revmapping);
        nextItem = nextItem->next;
    }
#endif
}

void EvolutionContactSource::close()
{
    m_addressbook.reset();
}

string EvolutionContactSource::getRevision(const string &luid)
{
    if (!needChanges()) {
        return "";
    }

    EContact *contact;
    GErrorCXX gerror;
    if (
#ifdef USE_EDS_CLIENT
        !e_book_client_get_contact_sync(m_addressbook,
                                        luid.c_str(),
                                        &contact,
                                        NULL,
                                        gerror)
#else
        !e_book_get_contact(m_addressbook,
                            luid.c_str(),
                            &contact,
                            gerror)
#endif
        ) {
        if (IsContactNotFound(gerror)) {
            throwError(STATUS_NOT_FOUND, string("retrieving item: ") + luid);
        } else {
            throwError(string("reading contact ") + luid,
                       gerror);
        }
    }
    eptr<EContact, GObject> contactptr(contact, "contact");
    const char *rev = (const char *)e_contact_get_const(contact,
                                                        E_CONTACT_REV);
    if (!rev || !rev[0]) {
        throwError(string("contact entry without REV: ") + luid);
    }
    return rev;
}

void EvolutionContactSource::readItem(const string &luid, std::string &item, bool raw)
{
    EContact *contact;
    GErrorCXX gerror;
    if (
#ifdef USE_EDS_CLIENT
        !e_book_client_get_contact_sync(m_addressbook,
                                        luid.c_str(),
                                        &contact,
                                        NULL,
                                        gerror)
#else
        !e_book_get_contact(m_addressbook,
                            luid.c_str(),
                            &contact,
                            gerror)
#endif
        ) {
        if (IsContactNotFound(gerror)) {
            throwError(STATUS_NOT_FOUND, string("reading contact: ") + luid);
        } else {
            throwError(string("reading contact ") + luid,
                       gerror);
        }
    }

    eptr<EContact, GObject> contactptr(contact, "contact");

    // Inline PHOTO data if exporting, leave VALUE=uri references unchanged
    // when processing inside engine (will be inlined by engine as needed).
    // The function for doing the inlining was added in EDS 3.4.
    // In compatibility mode, we must check the function pointer for non-NULL.
    // In direct call mode, the existence check is done by configure.
    if (raw
#ifdef EVOLUTION_COMPATIBILITY
        && e_contact_inline_local_photos
#endif
        ) {
#if defined(EVOLUTION_COMPATIBILITY) || defined(HAVE_E_CONTACT_INLINE_LOCAL_PHOTOS)
        if (!e_contact_inline_local_photos(contactptr, gerror)) {
            throwError(string("inlining PHOTO file data in ") + luid, gerror);
        }
#endif
    }

    eptr<char> vcardstr(e_vcard_to_string(&contactptr->parent,
                                          EVC_FORMAT_VCARD_30));
    if (!vcardstr) {
        throwError(string("failure extracting contact from Evolution " ) + luid);
    }

    item = vcardstr.get();
}

#ifdef USE_EDS_CLIENT
TrackingSyncSource::InsertItemResult EvolutionContactSource::checkBatchedInsert(const boost::shared_ptr<Pending> &pending)
{
    SE_LOG_DEBUG(pending->m_name, "checking operation: %s", pending->m_status == MODIFYING ? "waiting" : "inserted");
    if (pending->m_status == MODIFYING) {
        return TrackingSyncSource::InsertItemResult(boost::bind(&EvolutionContactSource::checkBatchedInsert, this, pending));
    }
    if (pending->m_gerror) {
        pending->m_gerror.throwError(pending->m_name);
    }
    string newrev = getRevision(pending->m_uid);
    return TrackingSyncSource::InsertItemResult(pending->m_uid, newrev, ITEM_OKAY);
}

void EvolutionContactSource::completedAdd(const boost::shared_ptr<PendingContainer_t> &batched, gboolean success, GSList *uids, const GError *gerror) throw()
{
    try {
        // The destructor ensures that the pending operations complete
        // before destructing the instance, so our "this" pointer is
        // always valid here.
        SE_LOG_DEBUG(getDisplayName(), "batch add of %d contacts completed", (int)batched->size());
        m_numRunningOperations--;
        PendingContainer_t::iterator it = (*batched).begin();
        GSList *uid = uids;
        while (it != (*batched).end() && uid) {
            SE_LOG_DEBUG((*it)->m_name, "completed: %s",
                         success ? "<<successfully>>" :
                         gerror ? gerror->message :
                         "<<unknown failure>>");
            if (success) {
                (*it)->m_uid = static_cast<gchar *>(uid->data);
                // Get revision when engine checks the item.
                (*it)->m_status = REVISION;
            } else {
                (*it)->m_status = DONE;
                (*it)->m_gerror = gerror;
            }
            ++it;
            uid = uid->next;
        }

        while (it != (*batched).end()) {
            // Should never happen.
            SE_LOG_DEBUG((*it)->m_name, "completed: missing uid?!");
            (*it)->m_status = DONE;
            ++it;
        }

        g_slist_free_full(uids, g_free);
    } catch (...) {
        Exception::handle(HANDLE_EXCEPTION_FATAL);
    }
}

void EvolutionContactSource::completedUpdate(const boost::shared_ptr<PendingContainer_t> &batched, gboolean success, const GError *gerror) throw()
{
    try {
        SE_LOG_DEBUG(getDisplayName(), "batch update of %d contacts completed", (int)batched->size());
        m_numRunningOperations--;
        PendingContainer_t::iterator it = (*batched).begin();
        while (it != (*batched).end()) {
            SE_LOG_DEBUG((*it)->m_name, "completed: %s",
                         success ? "<<successfully>>" :
                         gerror ? gerror->message :
                         "<<unknown failure>>");
            if (success) {
                (*it)->m_status = REVISION;
            } else {
                (*it)->m_status = DONE;
                (*it)->m_gerror = gerror;
            }
            ++it;
        }
    } catch (...) {
        Exception::handle(HANDLE_EXCEPTION_FATAL);
    }
}

void EvolutionContactSource::flushItemChanges()
{
    if (!m_batchedAdd.empty()) {
        SE_LOG_DEBUG(getDisplayName(), "batch add of %d contacts starting", (int)m_batchedAdd.size());
        m_numRunningOperations++;
        GListCXX<EContact, GSList> contacts;
        // Iterate backwards, push to front (cheaper for single-linked list) -> same order in the end.
        BOOST_REVERSE_FOREACH (const boost::shared_ptr<Pending> &pending, m_batchedAdd) {
            contacts.push_front(pending->m_contact.get());
        }
        // Transfer content without copying and then copy only the shared pointer.
        boost::shared_ptr<PendingContainer_t> batched(new PendingContainer_t);
        std::swap(*batched, m_batchedAdd);
        SYNCEVO_GLIB_CALL_ASYNC(e_book_client_add_contacts,
                                boost::bind(&EvolutionContactSource::completedAdd,
                                            this,
                                            batched,
                                            _1, _2, _3),
                                m_addressbook, contacts, NULL);
    }
    if (!m_batchedUpdate.empty()) {
        SE_LOG_DEBUG(getDisplayName(), "batch update of %d contacts starting", (int)m_batchedUpdate.size());
        m_numRunningOperations++;
        GListCXX<EContact, GSList> contacts;
        BOOST_REVERSE_FOREACH (const boost::shared_ptr<Pending> &pending, m_batchedUpdate) {
            contacts.push_front(pending->m_contact.get());
        }
        boost::shared_ptr<PendingContainer_t> batched(new PendingContainer_t);
        std::swap(*batched, m_batchedUpdate);
        SYNCEVO_GLIB_CALL_ASYNC(e_book_client_modify_contacts,
                                boost::bind(&EvolutionContactSource::completedUpdate,
                                            this,
                                            batched,
                                            _1, _2),
                                m_addressbook, contacts, NULL);
    }
}

void EvolutionContactSource::finishItemChanges()
{
    if (m_numRunningOperations) {
        SE_LOG_DEBUG(getDisplayName(), "waiting for %d pending operations to complete", m_numRunningOperations.get());
        while (m_numRunningOperations) {
            g_main_context_iteration(NULL, true);
        }
        SE_LOG_DEBUG(getDisplayName(), "pending operations completed");
    }
}

#endif

TrackingSyncSource::InsertItemResult
EvolutionContactSource::insertItem(const string &uid, const std::string &item, bool raw)
{
    EContactCXX contact(e_contact_new_from_vcard(item.c_str()), TRANSFER_REF);
    if (contact) {
        e_contact_set(contact, E_CONTACT_UID,
                      uid.empty() ?
                      NULL :
                      const_cast<char *>(uid.c_str()));
        GErrorCXX gerror;
#ifdef USE_EDS_CLIENT
        switch (m_accessMode) {
        case SYNCHRONOUS:
            if (uid.empty()) {
                gchar* newuid;
                if (!e_book_client_add_contact_sync(m_addressbook, contact, &newuid, NULL, gerror)) {
                    throwError("add new contact", gerror);
                }
                PlainGStr newuidPtr(newuid);
                string newrev = getRevision(newuid);
                return InsertItemResult(newuid, newrev, ITEM_OKAY);
            } else {
                if (!e_book_client_modify_contact_sync(m_addressbook, contact, NULL, gerror)) {
                    throwError("updating contact "+ uid, gerror);
                }
                string newrev = getRevision(uid);
                return InsertItemResult(uid, newrev, ITEM_OKAY);
            }
            break;
        case BATCHED:
        case DEFAULT:
            std::string name = StringPrintf("%s: %s operation #%d",
                                            getDisplayName().c_str(),
                                            uid.empty() ? "add" : ("insert " + uid).c_str(),
                                            m_asyncOpCounter++);
            SE_LOG_DEBUG(name, "queueing for batched %s", uid.empty() ? "add" : "update");
            boost::shared_ptr<Pending> pending(new Pending);
            pending->m_name = name;
            pending->m_contact = contact;
            pending->m_uid = uid;
            if (uid.empty()) {
                m_batchedAdd.push_back(pending);
            } else {
                m_batchedUpdate.push_back(pending);
            }
            // SyncSource is going to live longer than Synthesis
            // engine, so using "this" is safe here.
            return InsertItemResult(boost::bind(&EvolutionContactSource::checkBatchedInsert, this, pending));
            break;
        }
#else
        if (uid.empty() ?
            e_book_add_contact(m_addressbook, contact, gerror) :
            e_book_commit_contact(m_addressbook, contact, gerror)) {
            const char *newuid = (const char *)e_contact_get_const(contact, E_CONTACT_UID);
            if (!newuid) {
                throwError("no UID for contact");
            }
            string newrev = getRevision(newuid);
            return InsertItemResult(newuid, newrev, ITEM_OKAY);
        } else {
            throwError(uid.empty() ?
                       "storing new contact" :
                       string("updating contact ") + uid,
                       gerror);
        }
#endif
    } else {
        throwError(string("failure parsing vcard " ) + item);
    }
    // not reached!
    return InsertItemResult("", "", ITEM_OKAY);
}

void EvolutionContactSource::removeItem(const string &uid)
{
    GErrorCXX gerror;
    if (
#ifdef USE_EDS_CLIENT
        !e_book_client_remove_contact_by_uid_sync(m_addressbook, uid.c_str(), NULL, gerror)
#else
        !e_book_remove_contact(m_addressbook, uid.c_str(), gerror)
#endif
        ) {
        if (IsContactNotFound(gerror)) {
            throwError(STATUS_NOT_FOUND, string("deleting contact: ") + uid);
        } else {
            throwError( string( "deleting contact " ) + uid,
                        gerror);
        }
    }
}

std::string EvolutionContactSource::getDescription(const string &luid)
{
    try {
        EContact *contact;
        GErrorCXX gerror;
        if (
#ifdef USE_EDS_CLIENT
            !e_book_client_get_contact_sync(m_addressbook,
                                            luid.c_str(),
                                            &contact,
                                            NULL,
                                            gerror)
#else
            !e_book_get_contact(m_addressbook,
                                luid.c_str(),
                                &contact,
                                gerror)
#endif
            ) {
            throwError(string("reading contact ") + luid,
                       gerror);
        }
        eptr<EContact, GObject> contactptr(contact, "contact");
        const char *name = (const char *)e_contact_get_const(contact, E_CONTACT_FULL_NAME);
        if (name) {
            return name;
        }
        const char *fileas = (const char *)e_contact_get_const(contact, E_CONTACT_FILE_AS);
        if (fileas) {
            return fileas;
        }
        EContactName *names =
            (EContactName *)e_contact_get(contact, E_CONTACT_NAME);
        std::list<std::string> buffer;
        if (names) {
            try {
                if (names->given && names->given[0]) {
                    buffer.push_back(names->given);
                }
                if (names->additional && names->additional[0]) {
                    buffer.push_back(names->additional);
                }
                if (names->family && names->family[0]) {
                    buffer.push_back(names->family);
                }
            } catch (...) {
            }
            e_contact_name_free(names);
        }
        return boost::join(buffer, " ");
    } catch (...) {
        // Instead of failing we log the error and ask
        // the caller to log the UID. That way transient
        // errors or errors in the logging code don't
        // prevent syncs.
        handleException();
        return "";
    }
}

std::string EvolutionContactSource::getMimeType() const
{
    switch( m_vcardFormat ) {
     case EVC_FORMAT_VCARD_21:
        return "text/x-vcard";
        break;
     case EVC_FORMAT_VCARD_30:
     default:
        return "text/vcard";
        break;
    }
}

std::string EvolutionContactSource::getMimeVersion() const
{
    switch( m_vcardFormat ) {
     case EVC_FORMAT_VCARD_21:
        return "2.1";
        break;
     case EVC_FORMAT_VCARD_30:
     default:
        return "3.0";
        break;
    }
}

SE_END_CXX

#endif /* ENABLE_EBOOK */

#ifdef ENABLE_MODULES
# include "EvolutionContactSourceRegister.cpp"
#endif
