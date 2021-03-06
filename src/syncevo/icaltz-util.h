/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */
/* 
 * Authors : 
 *  Chenthill Palanisamy <pchenthill@novell.com>
 *
 * Copyright 2007, Novell, Inc.
 *
 * This program is free software; you can redistribute it and/or 
 * modify it under the terms of version 2 of the GNU Lesser General Public 
 * License as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor, 
 * Boston, MA 02110-1301, USA.
 */

#ifndef ICALTZUTIL_H
#define ICALTZUTIL_H

#include <libical/ical.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(sun) && defined(__SVR4)
#define ZONES_TAB_SYSTEM_FILENAME "tab/zone_sun.tab"
#else
#define ZONES_TAB_SYSTEM_FILENAME "zone.tab"
#endif

#if !defined(EVOLUTION_COMPATIBILITY) || !defined(ENABLE_ICAL)
const char *icaltzutil_get_zone_directory (void);
extern const char *ical_tzid_prefix;
#endif
icalcomponent *icaltzutil_fetch_timezone (const char *location);

#ifdef __cplusplus
}
#endif

#endif 
