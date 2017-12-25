/*      Copyright (c) 1995 NeXT Computer, Inc.  All rights reserved.
 *
 * strol.c - The functions strtol() & strtoul() are exported as public API
 *           via the header file ~driverkit/generalFuncs.h
 *
 * HISTORY
 * 25-Oct-1995    Dean Reece at NeXT
 *      Created based on BSD4.4's strtol.c & strtoul.c.
 *      Removed dependency on _ctype_ by static versions of isupper()...
 *      Added support for "0b101..." binary constants.
 *      Commented out references to errno.
 */

static inline int
isupper(char c)
{
    return (c >= 'A' && c <= 'Z');
}

static inline int
isalpha(char c)
{
    return ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'));
}


static inline int
isspace(char c)
{
    return (c == ' ' || c == '\t' || c == '\n' || c == '\12');
}

static inline int
isdigit(char c)
{
    return (c >= '0' && c <= '9');
}

#include <bootkit/runtime.h>

unsigned long
strtoul(const char *nptr, char **endptr, register int base)
{
	register const char *s = nptr;
	register unsigned long acc;
	register int c;
	register unsigned long cutoff;
	register int neg = 0, any, cutlim;
	
	/*
	 * See strtol for comments as to the logic used.
	 */
	do {
		c = *s++;
	} while (isspace(c));
	if (c == '-') {
		neg = 1;
		c = *s++;
	} else if (c == '+')
		c = *s++;
	if ((base == 0 || base == 16) &&
	    c == '0' && (*s == 'x' || *s == 'X')) {
		c = s[1];
		s += 2;
		base = 16;
	} else if ((base == 0 || base == 2) &&
			   c == '0' && (*s == 'b' || *s == 'B')) {
		c = s[1];
		s += 2;
		base = 2;
	}
	if (base == 0)
		base = c == '0' ? 8 : 10;
	cutoff = (unsigned long)ULONG_MAX / (unsigned long)base;
	cutlim = (unsigned long)ULONG_MAX % (unsigned long)base;
	for (acc = 0, any = 0;; c = *s++) {
		if (isdigit(c))
			c -= '0';
		else if (isalpha(c))
			c -= isupper(c) ? 'A' - 10 : 'a' - 10;
		else
			break;
		if (c >= base)
			break;
		if (any < 0 || acc > cutoff || (acc == cutoff && c > cutlim))
			any = -1;
		else {
			any = 1;
			acc *= base;
			acc += c;
		}
	}
	if (any < 0) {
		acc = ULONG_MAX;
		//		errno = ERANGE;
	} else if (neg)
		acc = -acc;
	if (endptr != 0)
		*endptr = (char *)(any ? s - 1 : nptr);
	return (acc);
}

