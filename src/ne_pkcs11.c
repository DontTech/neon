/*
   neon PKCS#11 support
   Copyright (C) 2008, Joe Orton <joe@manyfish.co.uk>

   This library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Library General Public
   License as published by the Free Software Foundation; either
   version 2 of the License, or (at your option) any later version.

   This library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Library General Public License for more details.

   You should have received a copy of the GNU Library General Public
   License along with this library; if not, write to the Free
   Software Foundation, Inc., 59 Temple Place - Suite 330, Boston,
   MA 02111-1307, USA
*/

#include "config.h"

#include "ne_pkcs11.h"

#ifdef HAVE_PAKCHOIS
#include <string.h>

#include <pakchois.h>

#include "ne_internal.h"
#include "ne_alloc.h"
#include "ne_private.h"
#include "ne_privssl.h"

struct pk11_context {
    pakchois_module_t *module;
    pakchois_session_t *session;
    ck_object_handle_t privkey;
};

static void pk11_destroy(void *userdata)
{
    struct pk11_context *pk11 = userdata;

    if (pk11->session) {
        pakchois_close_session(pk11->session);
    }
    pakchois_module_destroy(pk11->module);
    ne_free(pk11);
}

static int pk11_find_x509(pakchois_session_t *pks, ne_session *sess,
                          unsigned char *certid, unsigned long *cid_len)
{
    struct ck_attribute a[3];
    ck_object_class_t class;
    ck_certificate_type_t type;
    ck_rv_t rv;
    ck_object_handle_t obj;
    unsigned long count;
    int found = 0;

    /* Find objects with cert class and X.509 cert type. */
    class = CKO_CERTIFICATE;
    type = CKC_X_509;

    a[0].type = CKA_CLASS;
    a[0].value = &class;
    a[0].value_len = sizeof class;
    a[1].type = CKA_CERTIFICATE_TYPE;
    a[1].value = &type;
    a[1].value_len = sizeof type;

    rv = pakchois_find_objects_init(pks, a, 2);
    if (rv != CKR_OK) {
        NE_DEBUG(NE_DBG_SSL, "pk11: FindObjectsInit failed.\n");
        return 0;
    }

    while (pakchois_find_objects(pks, &obj, 1, &count) == CKR_OK
           && count == 1) {
        unsigned char value[8192], subject[8192];

        a[0].type = CKA_VALUE;
        a[0].value = value;
        a[0].value_len = sizeof value;
        a[1].type = CKA_ID;
        a[1].value = certid;
        a[1].value_len = *cid_len;
        a[2].type = CKA_SUBJECT;
        a[2].value = subject;
        a[2].value_len = sizeof subject;

        if (pakchois_get_attribute_value(pks, obj, a, 3) == CKR_OK) {
            ne_ssl_client_cert *cc;
            
            cc = ne__ssl_clicert_exkey_import(value, a[0].value_len);
            if (cc) {
                NE_DEBUG(NE_DBG_SSL, "pk11: Imported X.509 cert.\n");
                ne_ssl_set_clicert(sess, cc);
                found = 1;
                *cid_len = a[1].value_len;
                break;
            }
        }
        else {
            NE_DEBUG(NE_DBG_SSL, "pk11: Skipped cert, missing attrs.\n");
        }
    }

    pakchois_find_objects_final(pks);
    return found;    
}

static int pk11_find_pkey(struct pk11_context *ctx, pakchois_session_t *pks,
                          unsigned char *certid, unsigned long cid_len)
{
    struct ck_attribute a[3];
    ck_object_class_t class;
    ck_key_type_t type;
    ck_rv_t rv;
    ck_object_handle_t obj;
    unsigned long count;
    int found = 0;

    class = CKO_PRIVATE_KEY;
    type = CKK_RSA; /* FIXME: check from the cert whether DSA or RSA */

    /* Find an object with private key class and a certificate ID
     * which matches the certificate. */
    /* FIXME: also match the cert subject. */
    a[0].type = CKA_CLASS;
    a[0].value = &class;
    a[0].value_len = sizeof class;
    a[1].type = CKA_KEY_TYPE;
    a[1].value = &type;
    a[1].value_len = sizeof type;
    a[2].type = CKA_ID;
    a[2].value = certid;
    a[2].value_len = cid_len;

    rv = pakchois_find_objects_init(pks, a, 3);
    if (rv != CKR_OK) {
        NE_DEBUG(NE_DBG_SSL, "pk11: FindObjectsInit failed.\n");
        /* TODO: error propagation */
        return 0;
    }

    rv = pakchois_find_objects(pks, &obj, 1, &count);
    if (rv == CKR_OK && count == 1) {
        NE_DEBUG(NE_DBG_SSL, "pk11: Found private key.\n");
        found = 1;
        ctx->privkey = obj;
    }

    pakchois_find_objects_final(pks);

    return found;
}

static int find_client_cert(ne_session *sess, struct pk11_context *ctx,
                            pakchois_session_t *pks)
{
    unsigned char certid[8192];
    unsigned long cid_len = sizeof certid;

    /* TODO: match cert subject too. */
    return pk11_find_x509(pks, sess, certid, &cid_len) 
        && pk11_find_pkey(ctx, pks, certid, cid_len);
}

/* Callback invoked by GnuTLS to provide the signature.  The signature
 * operation is handled here by the PKCS#11 provider.  */
static int pk11_sign_callback(gnutls_session_t session,
                              void *userdata,
                              gnutls_certificate_type_t cert_type,
                              const gnutls_datum_t *cert,
                              const gnutls_datum_t *hash,
                              gnutls_datum_t *signature)
{
    struct pk11_context *ctx = userdata;
    ck_rv_t rv;
    struct ck_mechanism mech;
    unsigned long siglen;

    if (!ctx->session || ctx->privkey == CK_INVALID_HANDLE) {
        NE_DEBUG(NE_DBG_SSL, "p11: Boo, can't sign :(\n");
        return GNUTLS_E_NO_CERTIFICATE_FOUND;
    }

    /* FIXME: from the object determine whether this should be
     * CKM_DSA, or CKM_RSA_PKCS, or something unknown (&fail). */
    mech.mechanism = CKM_RSA_PKCS;
    mech.parameter = NULL;
    mech.parameter_len = 0;

    /* Initialize signing operation; using the private key discovered
     * earlier. */
    rv = pakchois_sign_init(ctx->session, &mech, ctx->privkey);
    if (rv != CKR_OK) {
        NE_DEBUG(NE_DBG_SSL, "p11: SignInit failed: %lx.\n", rv);
        return GNUTLS_E_PK_SIGN_FAILED;
    }

    /* Work out how long the signature must be: */
    rv = pakchois_sign(ctx->session, hash->data, hash->size, NULL, &siglen);
    if (rv != CKR_OK) {
        NE_DEBUG(NE_DBG_SSL, "p11: Sign failed.\n");
        return GNUTLS_E_PK_SIGN_FAILED;
    }

    signature->data = gnutls_malloc(siglen);
    signature->size = siglen;

    rv = pakchois_sign(ctx->session, hash->data, hash->size, 
                       signature->data, &siglen);
    if (rv != CKR_OK) {
        NE_DEBUG(NE_DBG_SSL, "p11: Sign failed.\n");
        return GNUTLS_E_PK_SIGN_FAILED;
    }

    NE_DEBUG(NE_DBG_SSL, "p11: signed.\n");

    return 0;
}

static void terminate_string(unsigned char *str, size_t len)
{
    unsigned char *ptr = str + len - 1;

    while ((*ptr == ' ' || *ptr == '\t' || *ptr == '\0') && ptr >= str)
        ptr--;
    
    if (ptr == str - 1)
        str[0] = '\0';
    else if (ptr == str + len - 1)
        str[len-1] = '\0';
    else
        ptr[1] = '\0';
}

static int pk11_login(pakchois_module_t *module, ck_slot_id_t slot_id,
                      ne_session *sess, pakchois_session_t *pks, 
                      struct ck_slot_info *sinfo)
{
    struct ck_token_info tinfo;
    int attempt = 0;
    ck_rv_t rv;

    if (pakchois_get_token_info(module, slot_id, &tinfo) != CKR_OK) {
        NE_DEBUG(NE_DBG_SSL, "pk11: GetTokenInfo failed\n");
        /* TODO: propagate error. */
        return -1;
    }

    if ((tinfo.flags & CKF_LOGIN_REQUIRED) == 0) {
        NE_DEBUG(NE_DBG_SSL, "pk11: No login required.\n");
        return 0;
    }

    /* For a token with a "protected" (out-of-band) authentication
     * path, calling login with a NULL username is all that is
     * required. */
    if (tinfo.flags & CKF_PROTECTED_AUTHENTICATION_PATH) {
        if (pakchois_login(pks, CKU_USER, NULL, 0) == CKR_OK) {
            return 0;
        }
        else {
            NE_DEBUG(NE_DBG_SSL, "pk11: Protected login failed.\n");
            /* TODO: error propagation. */
            return -1;
        }
    }

    /* Otherwise, PIN entry is necessary for login, so fail if there's
     * no callback. */
    if (!sess->ssl_pk11pin_fn) {
        NE_DEBUG(NE_DBG_SSL, "pk11: No pin callback but login required.\n");
        /* TODO: propagate error. */
        return -1;
    }

    terminate_string(sinfo->slot_description, sizeof sinfo->slot_description);

    do {
        char pin[NE_SSL_P11PINLEN];
        unsigned int flags = 0;

        /* If login has been attempted once already, check the token
         * status again, the flags might change. */
        if (attempt++) {
            if (pakchois_get_token_info(module, slot_id, &tinfo) != CKR_OK) {
                NE_DEBUG(NE_DBG_SSL, "pk11: GetTokenInfo failed\n");
                /* TODO: propagate error. */
                return -1;
            }
        }

        if (tinfo.flags & CKF_USER_PIN_COUNT_LOW)
            flags |= NE_SSL_P11PIN_COUNT_LOW;
        if (tinfo.flags & CKF_USER_PIN_FINAL_TRY)
            flags |= NE_SSL_P11PIN_FINAL_TRY;
        
        terminate_string(tinfo.label, sizeof tinfo.label);

        if (sess->ssl_pk11pin_fn(sess->ssl_pk11pin_ud, attempt,
                                 (const char *)sinfo->slot_description,
                                 (const char *)tinfo.label,
                                 flags, pin)) {
            return -1;
        }

        rv = pakchois_login(pks, CKU_USER, (unsigned char *)pin, strlen(pin));
        
        /* Try to scrub the pin off the stack.  Clever compilers will
         * probably optimize this away, oh well. */
        memset(pin, 0, sizeof pin);
    } while (rv == CKR_PIN_INCORRECT);

    NE_DEBUG(NE_DBG_SSL, "pk11: Login result = %lu\n", rv);

    return rv == CKR_OK ? 0 : -1;
}

static void pk11_provide(void *userdata, ne_session *sess,
                         const ne_ssl_dname *const *dnames,
                         int dncount)
{
    struct pk11_context *ctx = userdata;
    ck_slot_id_t *slots;
    unsigned long scount, n;

    if (pakchois_get_slot_list(ctx->module, 1, NULL, &scount) != CKR_OK
        || scount == 0) {
        NE_DEBUG(NE_DBG_SSL, "pk11: No slots.\n");
        /* TODO: propagate error. */
        return;
    }

    slots = ne_malloc(scount * sizeof *slots);
    if (pakchois_get_slot_list(ctx->module, 1, slots, &scount) != CKR_OK)  {
        ne_free(slots);
        NE_DEBUG(NE_DBG_SSL, "pk11: Really, no slots?\n");
        /* TODO: propagate error. */
        return;
    }

    NE_DEBUG(NE_DBG_SSL, "pk11: Found %ld slots.\n", scount);

    for (n = 0; n < scount; n++) {
        pakchois_session_t *pks;
        ck_rv_t rv;
        struct ck_slot_info sinfo;

        if (pakchois_get_slot_info(ctx->module, slots[n], &sinfo) != CKR_OK) {
            NE_DEBUG(NE_DBG_SSL, "pk11: GetSlotInfo failed\n");
            continue;
        }

        if ((sinfo.flags & CKF_TOKEN_PRESENT) == 0) {
            NE_DEBUG(NE_DBG_SSL, "pk11: slot empty, ignoring\n");
            continue;
        }
        
        rv = pakchois_open_session(ctx->module, slots[n], 
                                   CKF_SERIAL_SESSION,
                                   NULL, NULL, &pks);
        if (rv != CKR_OK) {
            NE_DEBUG(NE_DBG_SSL, "pk11: could not open slot, %ld (%ld: %ld)\n", 
                     rv, n, slots[n]);
            continue;
        }

        if (pk11_login(ctx->module, slots[n], sess, pks, &sinfo) == 0) {
            if (find_client_cert(sess, ctx, pks)) {
                NE_DEBUG(NE_DBG_SSL, "pk11: Setup complete.\n");
                ctx->session = pks;
                
                return;
            }
        }

        pakchois_close_session(pks);
    }

    ne_free(slots);
}

static int pk11_setup(ne_session *sess, pakchois_module_t *module)
{
    struct pk11_context *ctx;

    ctx = ne_malloc(sizeof *ctx);
    ctx->module = module;
    ctx->session = NULL;
    ctx->privkey = CK_INVALID_HANDLE;

    ne_hook_destroy_session(sess, pk11_destroy, ctx);

    ne_ssl_provide_clicert(sess, pk11_provide, ctx);
    
    /* FIXME: this is bad, because it means only one PKCS#11 provider
     * can be used at a time ("last through the door wins"), but needs
     * to be done early currently so that ne_sock_connect_ssl installs
     * the callback. */

    sess->ssl_context->sign_func = pk11_sign_callback;
    sess->ssl_context->sign_data = ctx;

    return NE_OK;
}

int ne_ssl_provide_pkcs11_clicert(ne_session *sess, const char *provider)
{
    pakchois_module_t *pm;
    ck_rv_t rv;
 
    rv = pakchois_module_load(&pm, provider);
    if (rv != CKR_OK) {
        ne_set_error(sess, _("Could not load PKCS#11 module: %ld"), rv);
        return NE_LOOKUP;
    }

    return pk11_setup(sess, pm);
}

int ne_ssl_provide_nsspk11_clicert(ne_session *sess, 
                                   const char *provider,
                                   const char *directory,
                                   const char *cert_prefix,
                                   const char *key_prefix,
                                   const char *secmod_db)
{
    pakchois_module_t *pm;
    ck_rv_t rv;

    rv = pakchois_module_nssload(&pm, provider, 
                                 directory, cert_prefix,
                                 key_prefix, secmod_db);
    if (rv != CKR_OK) {
        ne_set_error(sess, _("Could not load PKCS#11 module: %ld"), rv);
        return NE_LOOKUP;
    }

    return pk11_setup(sess, pm);
}

#else /* !HAVE_PAKCHOIS */

int ne_ssl_provide_pkcs11_clicert(ne_session *sess, const char *provider)
{
    return NE_FAILED;
}

int ne_ssl_provide_nsspk11_clicert(ne_session *sess, 
                                   const char *provider,
                                   const char *directory,
                                   const char *cert_prefix,
                                   const char *key_prefix,
                                   const char *secmod_db)
{
    return NE_FAILED;
}

#endif /* HAVE_PAKCHOIS */

