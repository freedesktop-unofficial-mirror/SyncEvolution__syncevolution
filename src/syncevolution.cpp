/*
 * Copyright (C) 2005-2008 Patrick Ohly
 */

#include <config.h>
#include <stddef.h>
#include <iostream>
#include <memory>
using namespace std;

#include <libgen.h>
#ifdef HAVE_GLIB
#include <glib-object.h>
#endif

#include "SyncEvolutionCmdline.h"
#include "EvolutionSyncSource.h"
#include "EvolutionSyncClient.h"

#if defined(ENABLE_MAEMO) && defined (ENABLE_EBOOK)

#include <dlfcn.h>

// really override the symbol, even if redefined by EDSAbiWrapper
#undef e_contact_new_from_vcard
extern "C" EContact *e_contact_new_from_vcard(const char *vcard)
{
    static typeof(e_contact_new_from_vcard) *impl;

    if (!impl) {
        impl = (typeof(impl))dlsym(RTLD_NEXT, "e_contact_new_from_vcard");
    }

    // Old versions of EDS-DBus parse_changes_array() call
    // e_contact_new_from_vcard() with a pointer which starts
    // with a line break; Evolution is not happy with that and
    // refuses to parse it. This code forwards until it finds
    // the first non-whitespace, presumably the BEGIN:VCARD.
    while (*vcard && isspace(*vcard)) {
        vcard++;
    }

    return impl ? impl(vcard) : NULL;
}
#endif

int main( int argc, char **argv )
{
#ifdef ENABLE_MAEMO
    // EDS-DBus uses potentially long-running calls which may fail due
    // to the default 25s timeout. Some of these can be replaced by
    // their async version, but e_book_async_get_changes() still
    // triggered it.
    //
    // The workaround for this is to link the binary against a libdbus
    // which has the dbus-timeout.patch and thus let's users and
    // the application increase the default timeout.
    setenv("DBUS_DEFAULT_TIMEOUT", "600000", 0);
#endif
    
#if defined(HAVE_GLIB) && defined(HAVE_EDS)
    // this is required on Maemo and does not harm either on a normal
    // desktop system with Evolution
    g_type_init();
#endif

    setvbuf(stderr, NULL, _IONBF, 0);
    setvbuf(stdout, NULL, _IONBF, 0);

    // Expand PATH to cover the directory we were started from?
    // This might be needed to find normalize_vcard.
    char *exe = strdup(argv[0]);
    if (strchr(exe, '/') ) {
        char *dir = dirname(exe);
        string path;
        char *oldpath = getenv("PATH");
        if (oldpath) {
            path += oldpath;
            path += ":";
        }
        path += dir;
        setenv("PATH", path.c_str(), 1);
    }
    free(exe);

    try {
        EDSAbiWrapperInit();

        SyncEvolutionCmdline cmdline(argc, argv, cout, cerr);
        if (cmdline.parse() &&
            cmdline.run()) {
            return 0;
        } else {
            return 1;
        }
    } catch ( const std::exception &ex ) {
        SE_LOG_ERROR(NULL, NULL, "%s", ex.what());
    } catch (...) {
        SE_LOG_ERROR(NULL, NULL, "unknown error");
    }

    return 1;
}
