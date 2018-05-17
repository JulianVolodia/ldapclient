/*	$OpenBSD$	*/

/*
 * Copyright (c) 2018 Reyk Floeter <reyk@openbsd.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/param.h>
#include <sys/queue.h>
#include <sys/socket.h>
#include <sys/tree.h>

#include <netinet/in.h>
#include <arpa/inet.h>

#include <netdb.h>
#include <errno.h>
#include <err.h>
#include <signal.h>
#include <event.h>
#include <fcntl.h>
#include <unistd.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <readpassphrase.h>
#include <vis.h>
#include <resolv.h>

#include "aldap.h"
#include "log.h"

#define F_STARTTLS	0x01
#define F_TLS		0x02
#define F_NEEDAUTH	0x04
#define F_LDIF		0x08

#define CAPATH		"/etc/ssl/cert.pem"
#define LDAPHOST	"localhost"
#define LDAPPORT	"389"
#define LDAPFILTER	"(objectClass=*)"
#define LDIF_LINELENGTH	79

struct ldapc {
	struct aldap	*ldap_al;
	char		*ldap_host;
	char		*ldap_port;
	char		*ldap_capath;
	char		*ldap_binddn;
	char		*ldap_secret;
	unsigned int	 ldap_flags;
	enum protocol_op ldap_req;
};

struct ldapc_search {
	char		*ls_basedn;
	char		*ls_filter;
	enum scope	 ls_scope;
	char		**ls_attr;
};

__dead void	 usage(void);
int		 ldapc_connect(struct ldapc *);
int		 ldapc_search(struct ldapc *, struct ldapc_search *);
int		 ldapc_printattr(struct ldapc *, const char *, const char *);
void		 ldapc_disconnect(struct ldapc *);
const char	*ldapc_resultcode(enum result_code);

__dead void
usage(void)
{
	extern char	*__progname;

	fprintf(stderr,
"usage: ldap search [-LvxZ] [-b basedn] [-c capath] [-D binddn] [-h host]\n"
"                   [-p port] [-s scope] [-w secret|-W]\n"
"                   [filter] [attributes ...]\n"
	);

	exit(1);
}

int
main(int argc, char *argv[])
{
	char			 passbuf[256];
	struct ldapc		 ldap;
	struct ldapc_search	 ls;
	int			 ch;
	int			 verbose = 1;

	if (pledge("stdio inet tty rpath dns", NULL) == -1)
		err(1, "pledge");

	log_init(verbose, 0);

	/*
	 * Program defaults
	 */
	memset(&ldap, 0, sizeof(ldap));
	memset(&ls, 0, sizeof(ls));

	ldap.ldap_host = LDAPHOST;
	ldap.ldap_port = LDAPPORT;
	ldap.ldap_capath = CAPATH;

	ls.ls_basedn = "";
	ls.ls_scope = LDAP_SCOPE_SUBTREE;
	ls.ls_filter = LDAPFILTER;

	/*
	 * Check the command.  Currently only "search" is supported but
	 * it could be extended with others such as add, modify, or delete.
	 */
	if (argc < 2)
		usage();
	else if (strcmp("search", argv[1]) == 0)
		ldap.ldap_req = LDAP_REQ_SEARCH;
	else
		usage();
	argc--;
	argv++;

	while ((ch = getopt(argc, argv, "b:c:D:h:Lp:s:vWw:xZ")) != -1) {
		switch (ch) {
		case 'b':
			ls.ls_basedn = optarg;
			break;
		case 'c':
			ldap.ldap_capath = optarg;
			break;
		case 'D':
			ldap.ldap_binddn = optarg;
			ldap.ldap_flags |= F_NEEDAUTH;
			break;
		case 'h':
			ldap.ldap_host = optarg;
			break;
		case 'L':
			ldap.ldap_flags |= F_LDIF;
			break;
		case 'p':
			ldap.ldap_port = optarg;
			break;
		case 's':
			if (strcasecmp("base", optarg) == 0)
				ls.ls_scope = LDAP_SCOPE_BASE;
			else if (strcasecmp("one", optarg) == 0)
				ls.ls_scope = LDAP_SCOPE_ONELEVEL;
			else if (strcasecmp("sub", optarg) == 0)
				ls.ls_scope = LDAP_SCOPE_SUBTREE;
			else
				errx(1, "invalid scope: %s", optarg);
			break;
		case 'v':
			verbose++;
			break;
		case 'w':
			ldap.ldap_secret = optarg;
			ldap.ldap_flags |= F_NEEDAUTH;
			break;
		case 'W':
			ldap.ldap_flags |= F_NEEDAUTH;
			break;
		case 'x':
			/* provided for compatibility */
			break;
		case 'Z':
			ldap.ldap_flags |= F_STARTTLS;
			break;
		default:
			usage();
		}
	}
	argc -= optind;
	argv += optind;

	log_setverbose(verbose);

	if (ldap.ldap_flags & F_NEEDAUTH) {
		if (ldap.ldap_secret == NULL) {
			if (readpassphrase("Password: ",
			    passbuf, sizeof(passbuf), RPP_REQUIRE_TTY) == NULL)
				errx(1, "failed to read LDAP password");
			ldap.ldap_secret = passbuf;
		}
		if (ldap.ldap_binddn == NULL) {
			log_warnx("missing -D binddn");
			usage();
		}
	}

	if (pledge("stdio inet rpath dns", NULL) == -1)
		err(1, "pledge");

	/* optional search filter */
	if (argc && strchr(argv[0], '=') != NULL) {
		ls.ls_filter = argv[0];
		argc--;
		argv++;
	}
	/* search attributes */
	if (argc)
		ls.ls_attr = argv;

	if (ldapc_connect(&ldap) == -1)
		errx(1, "LDAP connection failed");

	if (pledge("stdio", NULL) == -1)
		err(1, "pledge");

	if (ldapc_search(&ldap, &ls) == -1)
		errx(1, "LDAP search failed");

	ldapc_disconnect(&ldap);

	return (0);
}

int
ldapc_search(struct ldapc *ldap, struct ldapc_search *ls)
{
	struct aldap_page_control	*pg = NULL;
	struct aldap_message		*m;
	const char			*errstr;
	const char			*searchdn, *dn = NULL;
	char				*outkey;
	char				**outvalues;
	int				 ret, i, code, fail = 0;

	do {
		if (aldap_search(ldap->ldap_al, ls->ls_basedn, ls->ls_scope,
		    ls->ls_filter, ls->ls_attr, 0, 0, 0, pg) == -1) {
			aldap_get_errno(ldap->ldap_al, &errstr);
			log_warnx("LDAP search failed: %s", errstr);
			return (-1);
		}

		if (pg != NULL) {
			aldap_freepage(pg);
			pg = NULL;
		}

		while ((m = aldap_parse(ldap->ldap_al)) != NULL) {
			if (ldap->ldap_al->msgid != m->msgid) {
				goto fail;
			}

			if ((code = aldap_get_resultcode(m)) != LDAP_SUCCESS) {
				log_warnx("LDAP search failed: %s(%d)",
				    ldapc_resultcode(code), code);
				break;
			}

			if (m->message_type == LDAP_RES_SEARCH_RESULT) {
				if (m->page != NULL && m->page->cookie_len != 0)
					pg = m->page;
				else
					pg = NULL;

				aldap_freemsg(m);
				break;
			}

			if (m->message_type != LDAP_RES_SEARCH_ENTRY) {
				goto fail;
			}

			if (aldap_count_attrs(m) < 1) {
				aldap_freemsg(m);
				continue;
			}

			if ((searchdn = aldap_get_dn(m)) == NULL)
				goto fail;

			if (dn != NULL)
				printf("\n");
			else
				dn = ls->ls_basedn;
			if (strcmp(dn, searchdn) != 0)
				printf("dn: %s\n", searchdn);

			for (ret = aldap_first_attr(m, &outkey, &outvalues);
			    ret != -1;
			    ret = aldap_next_attr(m, &outkey, &outvalues)) {
				for (i = 0; outvalues != NULL &&
				    outvalues[i] != NULL; i++) {
					if (ldapc_printattr(ldap, outkey,
					    outvalues[i]) == -1) {
						fail = 1;
						break;
					}
				}
			}
			free(outkey);
			aldap_free_attr(outvalues);

			aldap_freemsg(m);
		}
	} while (pg != NULL && fail == 0);

	if (fail)
		return (-1);
	return (0);
 fail:
	ldapc_disconnect(ldap);
	return (-1);
}

int
ldapc_printattr(struct ldapc *ldap, const char *key, const char *value)
{
	char		*p = NULL, *out;
	const char	*cp;
	int		 encode;
	size_t		 inlen, outlen, left;

	if (ldap->ldap_flags & F_LDIF) {
		/* OpenLDAP encodes the userPassword by default */
		if (strcasecmp("userPassword", key) == 0)
			encode = 1;
		else
			encode = 0;

		/*
		 * The LDIF format a set of characters that can be included
		 * in SAFE-STRINGs. String value that do not match the
		 * criteria must be encoded as Base64.
		 */
		for (cp = value; encode == 0 &&*cp != '\0'; cp++) {
			/* !SAFE-CHAR %x01-09 / %x0B-0C / %x0E-7F */
			if (*cp > 127 |
			    *cp == '\0' ||
			    *cp == '\n' ||
			    *cp == '\r')
				encode = 1;
		}

		if (!encode) {
			if (asprintf(&p, "%s: %s", key, value) == -1) {
				log_warnx("asprintf");
				return (-1);
			}
		} else {
			inlen = strlen(value);
			outlen = inlen * 2 + 1;

			if ((out = calloc(1, outlen)) == NULL ||
			    b64_ntop(value, inlen, out, outlen) == -1) {
				log_warnx("Base64 encoding failed");
				free(p);
				return (-1);
			}

			/* Base64 is indicated with a double-colon */
			if (asprintf(&p, "%s: %s", key, out) == -1) {
				log_warnx("asprintf");
				free(out);
				return (-1);
			}
			free(out);
		}

		/* Wrap lines */
		for (outlen = 0, inlen = strlen(p);
		    outlen < inlen;
		    outlen += LDIF_LINELENGTH) {
			if (outlen)
				putchar(' ');
			/* max. line length - newline - optional indent */
			left = MIN(inlen - outlen, outlen ?
			    LDIF_LINELENGTH - 2 :
			    LDIF_LINELENGTH - 1);
			fwrite(p + outlen, left, 1, stdout);
			putchar('\n');
		}
	} else {
		/*
		 * Use vis(1) instead of base64 encoding of non-printable
		 * values.  This is much nicer as it always prdocues a
		 * human-readable visual output.  This can safely be done
		 * on all values no matter if they include non-printable
		 * characters.
		 */
		if (stravis(&p, value, VIS_SAFE|VIS_NL) == -1) {
			log_warn("visual encoding failed");
			return (-1);
		}

		printf("%s: %s\n", key, p);
	}

	free(p);
	return (0);
}

int
ldapc_connect(struct ldapc *ldap)
{
	struct addrinfo		 ai, *res, *res0;
	int			 ret, saved_errno, fd = -1, code = -1;
	struct aldap_message	*m;
	const char		*errstr;
	struct tls_config	*tls_config;

	memset(&ai, 0, sizeof(ai));
	ai.ai_family = AF_UNSPEC;
	ai.ai_socktype = SOCK_STREAM;
	ai.ai_protocol = IPPROTO_TCP;
	if ((ret = getaddrinfo(ldap->ldap_host, ldap->ldap_port,
	    &ai, &res0)) != 0) {
		log_warnx("%s", gai_strerror(ret));
		return (-1);
	}
	for (res = res0; res; res = res->ai_next, fd = -1) {
		if ((fd = socket(res->ai_family, res->ai_socktype,
		    res->ai_protocol)) == -1)
			continue;

		if (connect(fd, res->ai_addr, res->ai_addrlen) >= 0)
			break;

		saved_errno = errno;
		close(fd);
		errno = saved_errno;
	}
	if (fd == -1)
		return (-1);
	freeaddrinfo(res0);

	if ((ldap->ldap_al = aldap_init(fd)) == NULL) {
		warn("LDAP init failed");
		close(fd);
		return (-1);
	}

	if (ldap->ldap_flags & F_STARTTLS) {
		log_debug("%s: requesting STARTTLS", __func__);
		if (aldap_req_starttls(ldap->ldap_al) == -1) {
			log_warnx("failed to request STARTTLS");
			goto fail;
		}

		if ((m = aldap_parse(ldap->ldap_al)) == NULL) {
			log_warnx("failed to parse STARTTLS response");
			goto fail;
		}

		if (ldap->ldap_al->msgid != m->msgid ||
		    (code = aldap_get_resultcode(m)) != LDAP_SUCCESS) {
			log_warnx("STARTTLS failed: %s(%d)",
			    ldapc_resultcode(code), code);
			aldap_freemsg(m);
			goto fail;
		}
		aldap_freemsg(m);
	}

	if (ldap->ldap_flags & (F_STARTTLS | F_TLS)) {
		log_debug("%s: starting TLS", __func__);

		if ((tls_config = tls_config_new()) == NULL) {
			log_warnx("TLS config failed");
			goto fail;
		}

		if (tls_config_set_ca_file(tls_config,
		    ldap->ldap_capath) == -1) {
			log_warnx("unable to set CA %s", ldap->ldap_capath);
			goto fail;
		}

		if (aldap_tls(ldap->ldap_al, tls_config, ldap->ldap_host) < 0) {
			aldap_get_errno(ldap->ldap_al, &errstr);
			log_warnx("TLS failed: %s", errstr);
			goto fail;
		}
	}

	if (ldap->ldap_flags & F_NEEDAUTH) {
		log_debug("%s: bind request", __func__);
		if (aldap_bind(ldap->ldap_al, ldap->ldap_binddn,
		    ldap->ldap_secret) == -1) {
			log_warnx("bind request failed");
			goto fail;
		}

		if ((m = aldap_parse(ldap->ldap_al)) == NULL) {
			log_warnx("failed to parse bind response");
			goto fail;
		}

		if (ldap->ldap_al->msgid != m->msgid ||
		    (code = aldap_get_resultcode(m)) != LDAP_SUCCESS) {
			log_warnx("bind failed: %s(%d)",
			    ldapc_resultcode(code), code);
			aldap_freemsg(m);
			goto fail;
		}
		aldap_freemsg(m);
	}

	log_debug("%s: connected", __func__);

	return (0);
 fail:
	ldapc_disconnect(ldap);
	return (-1);
}

void
ldapc_disconnect(struct ldapc *ldap)
{
	if (ldap->ldap_al == NULL)
		return;
	aldap_close(ldap->ldap_al);
	ldap->ldap_al = NULL;
}

const char *
ldapc_resultcode(enum result_code code)
{
#define CODE(_X)	case _X:return (#_X)
	switch (code) {
	CODE(LDAP_SUCCESS);
	CODE(LDAP_OPERATIONS_ERROR);
	CODE(LDAP_PROTOCOL_ERROR);
	CODE(LDAP_TIMELIMIT_EXCEEDED);
	CODE(LDAP_SIZELIMIT_EXCEEDED);
	CODE(LDAP_COMPARE_FALSE);
	CODE(LDAP_COMPARE_TRUE);
	CODE(LDAP_STRONG_AUTH_NOT_SUPPORTED);
	CODE(LDAP_STRONG_AUTH_REQUIRED);
	CODE(LDAP_REFERRAL);
	CODE(LDAP_ADMINLIMIT_EXCEEDED);
	CODE(LDAP_UNAVAILABLE_CRITICAL_EXTENSION);
	CODE(LDAP_CONFIDENTIALITY_REQUIRED);
	CODE(LDAP_SASL_BIND_IN_PROGRESS);
	CODE(LDAP_NO_SUCH_ATTRIBUTE);
	CODE(LDAP_UNDEFINED_TYPE);
	CODE(LDAP_INAPPROPRIATE_MATCHING);
	CODE(LDAP_CONSTRAINT_VIOLATION);
	CODE(LDAP_TYPE_OR_VALUE_EXISTS);
	CODE(LDAP_INVALID_SYNTAX);
	CODE(LDAP_NO_SUCH_OBJECT);
	CODE(LDAP_ALIAS_PROBLEM);
	CODE(LDAP_INVALID_DN_SYNTAX);
	CODE(LDAP_ALIAS_DEREF_PROBLEM);
	CODE(LDAP_INAPPROPRIATE_AUTH);
	CODE(LDAP_INVALID_CREDENTIALS);
	CODE(LDAP_INSUFFICIENT_ACCESS);
	CODE(LDAP_BUSY);
	CODE(LDAP_UNAVAILABLE);
	CODE(LDAP_UNWILLING_TO_PERFORM);
	CODE(LDAP_LOOP_DETECT);
	CODE(LDAP_NAMING_VIOLATION);
	CODE(LDAP_OBJECT_CLASS_VIOLATION);
	CODE(LDAP_NOT_ALLOWED_ON_NONLEAF);
	CODE(LDAP_NOT_ALLOWED_ON_RDN);
	CODE(LDAP_ALREADY_EXISTS);
	CODE(LDAP_NO_OBJECT_CLASS_MODS);
	CODE(LDAP_AFFECTS_MULTIPLE_DSAS);
	CODE(LDAP_OTHER);
	default:
		return ("UNKNOWN_ERROR");
	}
};
