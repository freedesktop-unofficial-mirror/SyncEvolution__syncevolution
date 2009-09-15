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

#ifndef INCL_EVOLUTIONCONTACTSOURCE
#define INCL_EVOLUTIONCONTACTSOURCE

#include <config.h>
#include "EvolutionSyncSource.h"
#include "EvolutionSmartPtr.h"

#include <boost/noncopyable.hpp>

#ifdef ENABLE_EBOOK

#include <set>

/**
 * Implements access to Evolution address books.
 */
class EvolutionContactSource : public EvolutionSyncSource,
    public SyncSourceLogging,
    private boost::noncopyable
{
  public:
    EvolutionContactSource(const SyncSourceParams &params,
                           EVCardFormat vcardFormat = EVC_FORMAT_VCARD_30);
    virtual ~EvolutionContactSource() { close(); }

    //
    // implementation of SyncSource
    //
    virtual Databases getDatabases();
    virtual void open();
    virtual void close();
    virtual const char *getMimeType() const;
    virtual const char *getMimeVersion() const;
   
  protected:
    //
    // implementation of TrackingSyncSource callbacks
    //
    virtual void listAllItems(RevisionMap_t &revisions);
    virtual InsertItemResult insertItem(const string &uid, const string rev, const std::string &item, bool raw, bool restore =false);
    void readItem(const std::string &luid, std::string &item, bool raw);
    virtual void deleteItem(const string &uid, const string rev);

    // implementation of SyncSourceLogging callback
    virtual std::string getDescription(const string &luid);

    // need to override native format: it is always vCard 3.0
    void getSynthesisInfo(SynthesisInfo &info,
                          XMLConfigFragments &fragments)
    {
        TrackingSyncSource::getSynthesisInfo(info, fragments);
        info.m_profile = "\"vCard\", 2";
        info.m_native = "vCard30";
        info.m_incomingScript = "$VCARD_INCOMING_SCRIPT_EVOLUTION;";
    }

  private:
    /** extract REV string for contact, throw error if not found */
    std::string getRevision(const std::string &uid);

    /** valid after open(): the address book that this source references */
    eptr<EBook, GObject> m_addressbook;

    /** the format of vcards that new items are expected to have */
    const EVCardFormat m_vcardFormat;

    const char *RETURN_REV_CAP;
    const char *ATOMIC_MODIFICATION_CAP;
    //TODO work out a better way to define this flag
    const int E_BOOK_CHECK_REVISION;
    const int E_BOOK_SET_REVISION;

    bool m_returnRev;
    bool m_atomicModification;

    /**
     * a list of Evolution vcard properties which have to be encoded
     * as X-SYNCEVOLUTION-* when sending to server in 2.1 and decoded
     * back when receiving.
     */
    static const class extensions : public set<string> {
      public:
        extensions() : prefix("X-SYNCEVOLUTION-") {
            this->insert("FBURL");
            this->insert("CALURI");
        }

        const string prefix;
    } m_vcardExtensions;

    /**
     * a list of properties which SyncEvolution (in contrast to
     * the server) will only store once in each contact
     */
    static const class unique : public set<string> {
      public:
        unique () {
            insert("X-AIM");
            insert("X-GROUPWISE");
            insert("X-ICQ");
            insert("X-YAHOO");
            insert("X-EVOLUTION-ANNIVERSARY");
            insert("X-EVOLUTION-ASSISTANT");
            insert("X-EVOLUTION-BLOG-URL");
            insert("X-EVOLUTION-FILE-AS");
            insert("X-EVOLUTION-MANAGER");
            insert("X-EVOLUTION-SPOUSE");
            insert("X-EVOLUTION-VIDEO-URL");
            insert("X-MOZILLA-HTML");
            insert("FBURL");
            insert("CALURI");
        }
    } m_uniqueProperties;
};

#endif // ENABLE_EBOOK

#endif // INCL_EVOLUTIONCONTACTSOURCE
