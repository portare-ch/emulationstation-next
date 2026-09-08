// SPDX-License-Identifier: MIT
// Copyright (C) 2026-present PortareOS (https://github.com/portare-ch)

#include "LocaleES.h"

const char* es_ngettext(const char* msgid, const char* msgid_plural, unsigned long int n)
{
	return n != 1 ? msgid_plural : msgid;
}

const char* es_pgettext(const char* /*context*/, const char* msgid)
{
	return msgid;
}
