/*
 *  OpenVPN -- An application to securely tunnel IP networks
 *             over a single TCP/UDP port, with support for SSL/TLS-based
 *             session authentication and key exchange,
 *             packet encryption, packet authentication, and
 *             packet compression.
 *
 *  Copyright (C) 2002-2010 OpenVPN Technologies, Inc. <sales@openvpn.net>
 *  Copyright (C) 2010 Fox Crypto B.V. <openvpn@fox-it.com>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2
 *  as published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program (see the file COPYING included with this
 *  distribution); if not, write to the Free Software Foundation, Inc.,
 *  59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

/**
 * @file Data Channel Cryptography OpenSSL-specific backend interface
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#elif defined(_MSC_VER)
#include "config-msvc.h"
#endif

#include "syshead.h"

#if defined(ENABLE_CRYPTO) && defined(ENABLE_CRYPTO_OPENSSL)

#include "basic.h"
#include "buffer.h"
#include "integer.h"
#include "crypto.h"
#include "crypto_backend.h"
#include <openssl/objects.h>
#include <openssl/evp.h>
#include <openssl/des.h>
#include <openssl/ssl.h>
#include <openssl/err.h>

/*
 * Check for key size creepage.
 */

#if MAX_CIPHER_KEY_LENGTH < EVP_MAX_KEY_LENGTH
#warning Some OpenSSL EVP ciphers now support key lengths greater than MAX_CIPHER_KEY_LENGTH -- consider increasing MAX_CIPHER_KEY_LENGTH
#endif

#if MAX_HMAC_KEY_LENGTH < EVP_MAX_MD_SIZE
#warning Some OpenSSL HMAC message digests now support key lengths greater than MAX_HMAC_KEY_LENGTH -- consider increasing MAX_HMAC_KEY_LENGTH
#endif

#if HAVE_OPENSSL_ENGINE
#include <openssl/engine.h>

static bool engine_initialized = false; /* GLOBAL */

static ENGINE *engine_persist = NULL;   /* GLOBAL */

/* Try to load an engine in a shareable library */
static ENGINE *
try_load_engine (const char *engine)
{
  ENGINE *e = ENGINE_by_id ("dynamic");
  if (e)
    {
      if (!ENGINE_ctrl_cmd_string (e, "SO_PATH", engine, 0)
	  || !ENGINE_ctrl_cmd_string (e, "LOAD", NULL, 0))
	{
	  ENGINE_free (e);
	  e = NULL;
	}
    }
  return e;
}

static ENGINE *
setup_engine (const char *engine)
{
  ENGINE *e = NULL;

  ENGINE_load_builtin_engines ();

  if (engine)
    {
      if (strcmp (engine, "auto") == 0)
	{
	  msg (M_INFO, "Initializing OpenSSL auto engine support");
	  ENGINE_register_all_complete ();
	  return NULL;
	}
      if ((e = ENGINE_by_id (engine)) == NULL
	 && (e = try_load_engine (engine)) == NULL)
	{
	  crypto_msg (M_FATAL, "OpenSSL error: cannot load engine '%s'",
	      engine);
	}

      if (!ENGINE_set_default (e, ENGINE_METHOD_ALL))
	{
	  crypto_msg (M_FATAL, "OpenSSL error: ENGINE_set_default failed on "
	      "engine '%s'", engine);
	}

      msg (M_INFO, "Initializing OpenSSL support for engine '%s'",
	   ENGINE_get_id (e));
    }
  return e;
}

#endif /* HAVE_OPENSSL_ENGINE */

void
crypto_init_lib_engine (const char *engine_name)
{
#if HAVE_OPENSSL_ENGINE
  if (!engine_initialized)
    {
      ASSERT (engine_name);
      ASSERT (!engine_persist);
      engine_persist = setup_engine (engine_name);
      engine_initialized = true;
    }
#else
  msg (M_WARN, "Note: OpenSSL hardware crypto engine functionality is not available");
#endif
}

/*
 *
 * Functions related to the core crypto library
 *
 */

void
crypto_init_lib (void)
{
#ifndef ENABLE_SSL
  /* If SSL is enabled init is taken care of in ssl_openssl.c */
#ifndef ENABLE_SMALL
  ERR_load_crypto_strings ();
#endif
  OpenSSL_add_all_algorithms ();
#endif

  /*
   * If you build the OpenSSL library and OpenVPN with
   * CRYPTO_MDEBUG, you will get a listing of OpenSSL
   * memory leaks on program termination.
   */

#ifdef CRYPTO_MDEBUG
  CRYPTO_mem_ctrl(CRYPTO_MEM_CHECK_ON);
#endif
}

void
crypto_uninit_lib (void)
{
#ifndef ENABLE_SSL
  /* If SSL is enabled cleanup is taken care of in ssl_openssl.c */
  EVP_cleanup ();
#ifndef ENABLE_SMALL
  ERR_free_strings ();
#endif
#endif

#ifdef CRYPTO_MDEBUG
  FILE* fp = fopen ("sdlog", "w");
  ASSERT (fp);
  CRYPTO_mem_leaks_fp (fp);
  fclose (fp);
#endif

#if HAVE_OPENSSL_ENGINE
  if (engine_initialized)
    {
      ENGINE_cleanup ();
      engine_persist = NULL;
      engine_initialized = false;
    }
#endif
}

void
crypto_clear_error (void)
{
  ERR_clear_error ();
}

void
crypto_print_openssl_errors(const unsigned int flags) {
  size_t err = 0;

  while ((err = ERR_get_error ()))
    {
      /* Be more clear about frequently occurring "no shared cipher" error */
      if (err == ERR_PACK(ERR_LIB_SSL,SSL_F_SSL3_GET_CLIENT_HELLO,
	  SSL_R_NO_SHARED_CIPHER))
	{
	  msg (D_CRYPT_ERRORS, "TLS error: The server has no TLS ciphersuites "
	      "in common with the client. Your --tls-cipher setting might be "
	      "too restrictive.");
	}

      msg (flags, "OpenSSL: %s", ERR_error_string (err, NULL));
    }
}


/*
 *
 * OpenSSL memory debugging.  If dmalloc debugging is enabled, tell
 * OpenSSL to use our private malloc/realloc/free functions so that
 * we can dispatch them to dmalloc.
 *
 */

#ifdef DMALLOC
static void *
crypto_malloc (size_t size, const char *file, int line)
{
  return dmalloc_malloc(file, line, size, DMALLOC_FUNC_MALLOC, 0, 0);
}

static void *
crypto_realloc (void *ptr, size_t size, const char *file, int line)
{
  return dmalloc_realloc(file, line, ptr, size, DMALLOC_FUNC_REALLOC, 0);
}

static void
crypto_free (void *ptr)
{
  dmalloc_free (__FILE__, __LINE__, ptr, DMALLOC_FUNC_FREE);
}

void
crypto_init_dmalloc (void)
{
  CRYPTO_set_mem_ex_functions (crypto_malloc,
				crypto_realloc,
				crypto_free);
}
#endif /* DMALLOC */

const cipher_name_pair cipher_name_translation_table[] = {
    { "AES-128-CCM", "id-aes128-CCM" },
    { "AES-192-CCM", "id-aes192-CCM" },
    { "AES-256-CCM", "id-aes256-CCM" },
    { "AES-128-GCM", "id-aes128-GCM" },
    { "AES-192-GCM", "id-aes192-GCM" },
    { "AES-256-GCM", "id-aes256-GCM" },
};
const size_t cipher_name_translation_table_count =
    sizeof (cipher_name_translation_table) / sizeof (*cipher_name_translation_table);


void
show_available_ciphers ()
{
  int nid;

#ifndef ENABLE_SMALL
  printf ("The following ciphers and cipher modes are available for use\n"
	  "with " PACKAGE_NAME ".  Each cipher shown below may be use as a\n"
	  "parameter to the --cipher option.  The default key size is\n"
	  "shown as well as whether or not it can be changed with the\n"
          "--keysize directive.  Using a CBC or GCM mode is recommended.\n"
	  "In static key mode only CBC mode is allowed.\n\n");
#endif

  for (nid = 0; nid < 10000; ++nid)	/* is there a better way to get the size of the nid list? */
    {
      const EVP_CIPHER *cipher = EVP_get_cipherbynid (nid);
      if (cipher)
	{
	  if (cipher_kt_mode_cbc(cipher)
#ifdef ENABLE_OFB_CFB_MODE
	      || cipher_kt_mode_ofb_cfb(cipher)
#endif
#ifdef HAVE_AEAD_CIPHER_MODES
	      || cipher_kt_mode_aead(cipher)
#endif
	      )
	    {
	      const char *var_key_size =
		  (EVP_CIPHER_flags (cipher) & EVP_CIPH_VARIABLE_LENGTH) ?
		       "variable" : "fixed";
	      const char *ssl_only = cipher_kt_mode_cbc(cipher) ?
		  "" : " (TLS client/server mode)";

	      printf ("%s %d bit default key (%s)%s\n",
		  translate_cipher_name_to_openvpn(OBJ_nid2sn (nid)),
		  EVP_CIPHER_key_length (cipher) * 8, var_key_size, ssl_only);
	    }
	}
    }
  printf ("\n");
}

void
show_available_digests ()
{
  int nid;

#ifndef ENABLE_SMALL
  printf ("The following message digests are available for use with\n"
	  PACKAGE_NAME ".  A message digest is used in conjunction with\n"
	  "the HMAC function, to authenticate received packets.\n"
	  "You can specify a message digest as parameter to\n"
	  "the --auth option.\n\n");
#endif

  for (nid = 0; nid < 10000; ++nid)
    {
      const EVP_MD *digest = EVP_get_digestbynid (nid);
      if (digest)
	{
	  printf ("%s %d bit digest size\n",
		  OBJ_nid2sn (nid), EVP_MD_size (digest) * 8);
	}
    }
  printf ("\n");
}

void
show_available_engines ()
{
#if HAVE_OPENSSL_ENGINE /* Only defined for OpenSSL */
  ENGINE *e;

  printf ("OpenSSL Crypto Engines\n\n");

  ENGINE_load_builtin_engines ();

  e = ENGINE_get_first ();
  while (e)
    {
      printf ("%s [%s]\n",
	      ENGINE_get_name (e),
	      ENGINE_get_id (e));
      e = ENGINE_get_next (e);
    }
  ENGINE_cleanup ();
#else
  printf ("Sorry, OpenSSL hardware crypto engine functionality is not available.\n");
#endif
}

/*
 *
 * Random number functions, used in cases where we want
 * reasonably strong cryptographic random number generation
 * without depleting our entropy pool.  Used for random
 * IV values and a number of other miscellaneous tasks.
 *
 */

int rand_bytes(uint8_t *output, int len)
{
  return RAND_bytes (output, len);
}

/*
 *
 * Key functions, allow manipulation of keys.
 *
 */


int
key_des_num_cblocks (const EVP_CIPHER *kt)
{
  int ret = 0;
  const char *name = OBJ_nid2sn (EVP_CIPHER_nid (kt));
  if (name)
    {
      if (!strncmp (name, "DES-", 4))
	{
	  ret = EVP_CIPHER_key_length (kt) / sizeof (DES_cblock);
	}
      else if (!strncmp (name, "DESX-", 5))
	{
	  ret = 1;
	}
    }
  dmsg (D_CRYPTO_DEBUG, "CRYPTO INFO: n_DES_cblocks=%d", ret);
  return ret;
}

bool
key_des_check (uint8_t *key, int key_len, int ndc)
{
  int i;
  struct buffer b;

  buf_set_read (&b, key, key_len);

  for (i = 0; i < ndc; ++i)
    {
      DES_cblock *dc = (DES_cblock*) buf_read_alloc (&b, sizeof (DES_cblock));
      if (!dc)
	{
	  msg (D_CRYPT_ERRORS, "CRYPTO INFO: check_key_DES: insufficient key "
	      "material");
	  goto err;
	}
      if (DES_is_weak_key(dc))
	{
	  crypto_msg (D_CRYPT_ERRORS, "CRYPTO INFO: check_key_DES: weak key "
	      "detected");
	  goto err;
	}
      if (!DES_check_key_parity (dc))
	{
	  crypto_msg (D_CRYPT_ERRORS, "CRYPTO INFO: check_key_DES: bad parity "
	      "detected");
	  goto err;
	}
    }
  return true;

 err:
  ERR_clear_error ();
  return false;
}

void
key_des_fixup (uint8_t *key, int key_len, int ndc)
{
  int i;
  struct buffer b;

  buf_set_read (&b, key, key_len);
  for (i = 0; i < ndc; ++i)
    {
      DES_cblock *dc = (DES_cblock*) buf_read_alloc(&b, sizeof(DES_cblock));
      if (!dc)
	{
	  msg (D_CRYPT_ERRORS, "CRYPTO INFO: fixup_key_DES: insufficient key material");
	  ERR_clear_error ();
	  return;
	}
      DES_set_odd_parity (dc);
    }
}


/*
 *
 * Generic cipher key type functions
 *
 */


const EVP_CIPHER *
cipher_kt_get (const char *ciphername)
{
  const EVP_CIPHER *cipher = NULL;

  ASSERT (ciphername);

  cipher = EVP_get_cipherbyname (ciphername);

  if (NULL == cipher)
    crypto_msg (M_FATAL, "Cipher algorithm '%s' not found", ciphername);

  if (EVP_CIPHER_key_length (cipher) > MAX_CIPHER_KEY_LENGTH)
    msg (M_FATAL, "Cipher algorithm '%s' uses a default key size (%d bytes) which is larger than " PACKAGE_NAME "'s current maximum key size (%d bytes)",
	 ciphername,
	 EVP_CIPHER_key_length (cipher),
	 MAX_CIPHER_KEY_LENGTH);

  return cipher;
}

const char *
cipher_kt_name (const EVP_CIPHER *cipher_kt)
{
  if (NULL == cipher_kt)
    return "[null-cipher]";
  return EVP_CIPHER_name (cipher_kt);
}

int
cipher_kt_key_size (const EVP_CIPHER *cipher_kt)
{
  return EVP_CIPHER_key_length (cipher_kt);
}

int
cipher_kt_iv_size (const EVP_CIPHER *cipher_kt)
{
  return EVP_CIPHER_iv_length (cipher_kt);
}

int
cipher_kt_block_size (const EVP_CIPHER *cipher_kt)
{
  return EVP_CIPHER_block_size (cipher_kt);
}

int
cipher_kt_tag_size (const EVP_CIPHER *cipher_kt)
{
  if (cipher_kt_mode_aead(cipher_kt))
    return OPENVPN_AEAD_TAG_LENGTH;
  else
    return 0;
}

int
cipher_kt_mode (const EVP_CIPHER *cipher_kt)
{
  ASSERT(NULL != cipher_kt);
  return EVP_CIPHER_mode (cipher_kt);
}

bool
cipher_kt_mode_cbc(const cipher_kt_t *cipher)
{
  return cipher_kt_mode(cipher) == OPENVPN_MODE_CBC
#ifdef EVP_CIPH_FLAG_AEAD_CIPHER
      /* Exclude AEAD cipher modes, they require a different API */
      && !(EVP_CIPHER_flags(cipher) & EVP_CIPH_FLAG_AEAD_CIPHER)
#endif
    ;
}

bool
cipher_kt_mode_ofb_cfb(const cipher_kt_t *cipher)
{
  return (cipher_kt_mode(cipher) == OPENVPN_MODE_OFB ||
	  cipher_kt_mode(cipher) == OPENVPN_MODE_CFB)
#ifdef EVP_CIPH_FLAG_AEAD_CIPHER
      /* Exclude AEAD cipher modes, they require a different API */
      && !(EVP_CIPHER_flags(cipher) & EVP_CIPH_FLAG_AEAD_CIPHER)
#endif
    ;
}

bool
cipher_kt_mode_aead(const cipher_kt_t *cipher)
{
#ifdef HAVE_AEAD_CIPHER_MODES
  return (cipher_kt_mode(cipher) == OPENVPN_MODE_GCM ||
	  cipher_kt_mode(cipher) == OPENVPN_MODE_CCM);
#else
  return false;
#endif
}

/*
 *
 * Generic cipher context functions
 *
 */


void
cipher_ctx_init (EVP_CIPHER_CTX *ctx, uint8_t *key, int key_len,
    const EVP_CIPHER *kt, int enc)
{
  ASSERT(NULL != kt && NULL != ctx);

  CLEAR (*ctx);

  EVP_CIPHER_CTX_init (ctx);
  if (!EVP_CipherInit (ctx, kt, NULL, NULL, enc))
    crypto_msg (M_FATAL, "EVP cipher init #1");
#ifdef HAVE_EVP_CIPHER_CTX_SET_KEY_LENGTH
  if (!EVP_CIPHER_CTX_set_key_length (ctx, key_len))
    crypto_msg (M_FATAL, "EVP set key size");
#endif
  if (!EVP_CipherInit (ctx, NULL, key, NULL, enc))
    crypto_msg (M_FATAL, "EVP cipher init #2");

  /* make sure we used a big enough key */
  ASSERT (EVP_CIPHER_CTX_key_length (ctx) <= key_len);
}

void
cipher_ctx_cleanup (EVP_CIPHER_CTX *ctx)
{
  EVP_CIPHER_CTX_cleanup (ctx);
}

int
cipher_ctx_iv_length (const EVP_CIPHER_CTX *ctx)
{
  return EVP_CIPHER_CTX_iv_length (ctx);
}

int cipher_ctx_get_tag (EVP_CIPHER_CTX *ctx, uint8_t *tag_buf, int tag_size)
{
#ifdef HAVE_AEAD_CIPHER_MODES
  return EVP_CIPHER_CTX_ctrl (ctx, EVP_CTRL_GCM_GET_TAG, tag_size, tag_buf);
#else
  ASSERT (0);
#endif
}

int
cipher_ctx_block_size(const EVP_CIPHER_CTX *ctx)
{
  return EVP_CIPHER_CTX_block_size (ctx);
}

int
cipher_ctx_mode (const EVP_CIPHER_CTX *ctx)
{
  return EVP_CIPHER_CTX_mode (ctx);
}

const cipher_kt_t *
cipher_ctx_get_cipher_kt (const cipher_ctx_t *ctx)
{
  return EVP_CIPHER_CTX_cipher(ctx);
}


int
cipher_ctx_reset (EVP_CIPHER_CTX *ctx, uint8_t *iv_buf)
{
  return EVP_CipherInit (ctx, NULL, NULL, iv_buf, -1);
}

int
cipher_ctx_update_ad (EVP_CIPHER_CTX *ctx, uint8_t* src, int src_len)
{
#ifdef HAVE_AEAD_CIPHER_MODES
  int len;
  return EVP_CipherUpdate (ctx, NULL, &len, src, src_len);
#else
  ASSERT (0);
#endif
}

int
cipher_ctx_update (EVP_CIPHER_CTX *ctx, uint8_t *dst, int *dst_len,
    uint8_t *src, int src_len)
{
  return EVP_CipherUpdate (ctx, dst, dst_len, src, src_len);
}

int
cipher_ctx_final (EVP_CIPHER_CTX *ctx, uint8_t *dst, int *dst_len)
{
  return EVP_CipherFinal (ctx, dst, dst_len);
}

int
cipher_ctx_final_check_tag (EVP_CIPHER_CTX *ctx, uint8_t *dst, int *dst_len,
    const uint8_t *tag, size_t tag_len)
{
#ifdef HAVE_AEAD_CIPHER_MODES
  /* Setting a tag does not change the tag, so casting away const... */
  if (!EVP_CIPHER_CTX_ctrl (ctx, EVP_CTRL_GCM_SET_TAG, tag_len,
      (uint8_t *) tag))
    return 0;

  return cipher_ctx_final (ctx, dst, dst_len);
#else
  ASSERT (0);
#endif
}

void
cipher_des_encrypt_ecb (const unsigned char key[DES_KEY_LENGTH],
    unsigned char *src,
    unsigned char *dst)
{
    DES_key_schedule sched;

    DES_set_key_unchecked((DES_cblock*)key, &sched);
    DES_ecb_encrypt((DES_cblock *)src, (DES_cblock *)dst, &sched, DES_ENCRYPT);
}

/*
 *
 * Generic message digest information functions
 *
 */


const EVP_MD *
md_kt_get (const char *digest)
{
  const EVP_MD *md = NULL;
  ASSERT (digest);
  md = EVP_get_digestbyname (digest);
  if (!md)
    crypto_msg (M_FATAL, "Message hash algorithm '%s' not found", digest);
  if (EVP_MD_size (md) > MAX_HMAC_KEY_LENGTH)
    crypto_msg (M_FATAL, "Message hash algorithm '%s' uses a default hash size "
	 "(%d bytes) which is larger than " PACKAGE_NAME "'s current maximum "
	 "hash size (%d bytes)",
	 digest,
	 EVP_MD_size (md),
	 MAX_HMAC_KEY_LENGTH);
  return md;
}

const char *
md_kt_name (const EVP_MD *kt)
{
  if (NULL == kt)
    return "[null-digest]";
  return EVP_MD_name (kt);
}

int
md_kt_size (const EVP_MD *kt)
{
  return EVP_MD_size(kt);
}


/*
 *
 * Generic message digest functions
 *
 */

int
md_full (const EVP_MD *kt, const uint8_t *src, int src_len, uint8_t *dst)
{
  unsigned int in_md_len = 0;

  return EVP_Digest(src, src_len, dst, &in_md_len, kt, NULL);
}

void
md_ctx_init (EVP_MD_CTX *ctx, const EVP_MD *kt)
{
  ASSERT(NULL != ctx && NULL != kt);

  CLEAR (*ctx);

  EVP_MD_CTX_init (ctx);
  EVP_DigestInit(ctx, kt);
}

void
md_ctx_cleanup(EVP_MD_CTX *ctx)
{
  EVP_MD_CTX_cleanup(ctx);
}

int
md_ctx_size (const EVP_MD_CTX *ctx)
{
  return EVP_MD_CTX_size(ctx);
}

void
md_ctx_update (EVP_MD_CTX *ctx, const uint8_t *src, int src_len)
{
  EVP_DigestUpdate(ctx, src, src_len);
}

void
md_ctx_final (EVP_MD_CTX *ctx, uint8_t *dst)
{
  unsigned int in_md_len = 0;

  EVP_DigestFinal(ctx, dst, &in_md_len);
}


/*
 *
 * Generic HMAC functions
 *
 */


void
hmac_ctx_init (HMAC_CTX *ctx, const uint8_t *key, int key_len,
    const EVP_MD *kt)
{
  ASSERT(NULL != kt && NULL != ctx);

  CLEAR(*ctx);

  HMAC_CTX_init (ctx);
  HMAC_Init_ex (ctx, key, key_len, kt, NULL);

  /* make sure we used a big enough key */
  ASSERT (HMAC_size (ctx) <= key_len);
}

void
hmac_ctx_cleanup(HMAC_CTX *ctx)
{
  HMAC_CTX_cleanup (ctx);
}

int
hmac_ctx_size (const HMAC_CTX *ctx)
{
  return HMAC_size (ctx);
}

void
hmac_ctx_reset (HMAC_CTX *ctx)
{
  HMAC_Init_ex (ctx, NULL, 0, NULL, NULL);
}

void
hmac_ctx_update (HMAC_CTX *ctx, const uint8_t *src, int src_len)
{
  HMAC_Update (ctx, src, src_len);
}

void
hmac_ctx_final (HMAC_CTX *ctx, uint8_t *dst)
{
  unsigned int in_hmac_len = 0;

  HMAC_Final (ctx, dst, &in_hmac_len);
}

bool
crypto_aead_mode (int mode)
{
#ifdef HAVE_AEAD_CIPHER_MODES
  return mode == OPENVPN_MODE_CCM || mode == OPENVPN_MODE_GCM;
#else
  return false;
#endif
}

#endif /* ENABLE_CRYPTO && ENABLE_CRYPTO_OPENSSL */