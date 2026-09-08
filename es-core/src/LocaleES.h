// SPDX-License-Identifier: MIT
// Copyright (C) 2026-present PortareOS (https://github.com/portare-ch)

#pragma once
#ifndef _LOCALE_H_
#define _LOCALE_H_

#include <string>

// The interface is English and only English. There is no gettext, no message
// catalogues and no runtime language switch.
//
// The wrappers stay: thousands of call sites read _("SOMETHING"), and keeping
// them is what makes the user-facing strings greppable. They are the identity.
// system.language is a separate thing and still means something: scrapers,
// LangParser and Genres all read it to pick metadata, not interface text.
#define _(A) std::string(A)
#define _U(x) x

// Not named ngettext/pgettext: glibc declares those itself, returning char*,
// and something pulls libintl.h in transitively.
const char* es_ngettext(const char* msgid, const char* msgid_plural, unsigned long int n);
const char* es_pgettext(const char* context, const char* msgid);

class EsLocale
{
public:
	// No right-to-left interface without translations to be right-to-left in.
	static const bool isRTL() { return false; }
};

#endif // _LOCALE_H_
