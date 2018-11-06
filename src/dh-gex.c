/*
 * dh-gex.c - diffie-hellman group exchange
 *
 * This file is part of the SSH Library
 *
 * Copyright (c) 2016 by Aris Adamantiadis <aris@0xbadc0de.be>
 *
 * The SSH Library is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or (at your
 * option) any later version.
 *
 * The SSH Library is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public
 * License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with the SSH Library; see the file COPYING.  If not, write to
 * the Free Software Foundation, Inc., 59 Temple Place - Suite 330, Boston,
 * MA 02111-1307, USA.
 */

#include "config.h"

#include <errno.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>

#include "libssh/priv.h"
#include "libssh/dh-gex.h"
#include "libssh/libssh.h"
#include "libssh/ssh2.h"
#include "libssh/callbacks.h"
#include "libssh/dh.h"
#include "libssh/buffer.h"
#include "libssh/session.h"

static SSH_PACKET_CALLBACK(ssh_packet_client_dhgex_group);
static SSH_PACKET_CALLBACK(ssh_packet_client_dhgex_reply);

static ssh_packet_callback dhgex_client_callbacks[] = {
    ssh_packet_client_dhgex_group, /* SSH_MSG_KEX_DH_GEX_GROUP */
    NULL,                          /* SSH_MSG_KEX_DH_GEX_INIT */
    ssh_packet_client_dhgex_reply  /* SSH_MSG_KEX_DH_GEX_REPLY */
};

static struct ssh_packet_callbacks_struct ssh_dhgex_client_callbacks = {
    .start = SSH2_MSG_KEX_DH_GEX_GROUP,
    .n_callbacks = 3,
    .callbacks = dhgex_client_callbacks,
    .user = NULL
};

/** @internal
 * @brief initiates a diffie-hellman-group-exchange kex
 */
int ssh_client_dhgex_init(ssh_session session)
{
    int rc;

    rc = ssh_dh_init_common(session);
    if (rc != SSH_OK){
        goto error;
    }

    /* Minimum group size, preferred group size, maximum group size */
    rc = ssh_buffer_pack(session->out_buffer,
                         "bddd",
                         SSH2_MSG_KEX_DH_GEX_REQUEST,
                         DH_PMIN,
                         DH_PREQ,
                         DH_PMAX);
    if (rc != SSH_OK) {
        goto error;
    }

    /* register the packet callbacks */
    ssh_packet_set_callbacks(session, &ssh_dhgex_client_callbacks);
    session->dh_handshake_state = DH_STATE_REQUEST_SENT;
    rc = ssh_packet_send(session);
    if (rc == SSH_ERROR) {
        goto error;
    }
    return rc;
error:
    ssh_dh_cleanup(session->next_crypto);
    return SSH_ERROR;
}

/** @internal
 *  @brief handle a DH_GEX_GROUP packet, client side. This packet contains
 *         the group parameters.
 */
SSH_PACKET_CALLBACK(ssh_packet_client_dhgex_group)
{
    int rc;
    int blen;
    bignum pmin1 = NULL, one = NULL;
    bignum_CTX ctx = bignum_ctx_new();

    SSH_LOG(SSH_LOG_PROTOCOL, "SSH_MSG_KEX_DH_GEX_GROUP received");

    if (bignum_ctx_invalid(ctx)) {
        goto error;
    }

    if (session->dh_handshake_state != DH_STATE_REQUEST_SENT) {
        ssh_set_error(session,
                      SSH_FATAL,
                      "Received DH_GEX_GROUP in invalid state");
        goto error;
    }
    one = bignum_new();
    pmin1 = bignum_new();
    if (one == NULL || pmin1 == NULL) {
        ssh_set_error_oom(session);
        goto error;
    }
    session->next_crypto->dh_group_is_mutable = 1;
    rc = ssh_buffer_unpack(packet,
                           "BB",
                           &session->next_crypto->p,
                           &session->next_crypto->g);
    if (rc != SSH_OK) {
        ssh_set_error(session, SSH_FATAL, "Invalid DH_GEX_GROUP packet");
        goto error;
    }
    /* basic checks */
    rc = bignum_set_word(one, 1);
    if (rc != 1) {
        goto error;
    }
    blen = bignum_num_bits(session->next_crypto->p);
    if (blen < DH_PMIN || blen > DH_PMAX) {
        ssh_set_error(session,
                SSH_FATAL,
                "Invalid dh group parameter p: %d not in [%d:%d]",
                blen,
                DH_PMIN,
                DH_PMAX);
        goto error;
    }
    if (bignum_cmp(session->next_crypto->p, one) <= 0) {
        /* p must be positive and preferably bigger than one */
        ssh_set_error(session, SSH_FATAL, "Invalid dh group parameter p");
    }
    if (!bignum_is_bit_set(session->next_crypto->p, 0)) {
        /* p must be a prime and therefore not divisible by 2 */
        ssh_set_error(session, SSH_FATAL, "Invalid dh group parameter p");
        goto error;
    }
    bignum_sub(pmin1, session->next_crypto->p, one);
    if (bignum_cmp(session->next_crypto->g, one) <= 0 ||
        bignum_cmp(session->next_crypto->g, pmin1) > 0) {
        /* generator must be at least 2 and smaller than p-1*/
        ssh_set_error(session, SSH_FATAL, "Invalid dh group parameter g");
        goto error;
    }
    /* compute and send DH public parameter */
    rc = ssh_dh_generate_secret(session, session->next_crypto->x);
    if (rc == SSH_ERROR) {
        goto error;
    }
    session->next_crypto->e = bignum_new();
    if (session->next_crypto->e == NULL) {
        ssh_set_error_oom(session);
        goto error;
    }
    rc = bignum_mod_exp(session->next_crypto->e,
                        session->next_crypto->g,
                        session->next_crypto->x,
                        session->next_crypto->p,
                        ctx);
    if (rc != 1) {
        goto error;
    }

    bignum_ctx_free(ctx);
    ctx = NULL;

    rc = ssh_buffer_pack(session->out_buffer,
                         "bB",
                         SSH2_MSG_KEX_DH_GEX_INIT,
                         session->next_crypto->e);
    if (rc != SSH_OK) {
        goto error;
    }

    session->dh_handshake_state = DH_STATE_INIT_SENT;

    rc = ssh_packet_send(session);

    bignum_safe_free(one);
    bignum_safe_free(pmin1);
    return SSH_PACKET_USED;
error:
    bignum_safe_free(one);
    bignum_safe_free(pmin1);
    if(!bignum_ctx_invalid(ctx)) {
        bignum_ctx_free(ctx);
    }
    ssh_dh_cleanup(session->next_crypto);
    session->session_state = SSH_SESSION_STATE_ERROR;

    return SSH_PACKET_USED;
}

static SSH_PACKET_CALLBACK(ssh_packet_client_dhgex_reply)
{
    struct ssh_crypto_struct *crypto=session->next_crypto;
    int rc;
    ssh_string pubkey_blob = NULL;
    (void)type;
    (void)user;
    SSH_LOG(SSH_LOG_PROTOCOL, "SSH_MSG_KEX_DH_GEX_REPLY received");

    ssh_packet_remove_callbacks(session, &ssh_dhgex_client_callbacks);
    rc = ssh_buffer_unpack(packet,
                           "SBS",
                           &pubkey_blob, &crypto->f,
                           &crypto->dh_server_signature);

    if (rc == SSH_ERROR) {
        ssh_set_error(session, SSH_FATAL, "Invalid DH_GEX_REPLY packet");
        goto error;
    }
    rc = ssh_dh_import_next_pubkey_blob(session, pubkey_blob);
    ssh_string_free(pubkey_blob);
    if (rc != 0) {
        goto error;
    }

    rc = ssh_dh_build_k(session);
    if (rc == SSH_ERROR) {
        ssh_set_error(session, SSH_FATAL, "Could not generate shared secret");
        goto error;
    }

    /* Send the MSG_NEWKEYS */
    if (ssh_buffer_add_u8(session->out_buffer, SSH2_MSG_NEWKEYS) < 0) {
        goto error;
    }

    rc = ssh_packet_send(session);
    if (rc == SSH_ERROR) {
        goto error;
    }
    SSH_LOG(SSH_LOG_PROTOCOL, "SSH_MSG_NEWKEYS sent");
    session->dh_handshake_state = DH_STATE_NEWKEYS_SENT;

    return SSH_PACKET_USED;
error:
    ssh_dh_cleanup(session->next_crypto);
    session->session_state = SSH_SESSION_STATE_ERROR;

    return SSH_PACKET_USED;
}

#ifdef WITH_SERVER

#define MODULI_FILE "/etc/ssh/moduli"
/* 2     "Safe" prime; (p-1)/2 is also prime. */
#define SAFE_PRIME 2
/* 0x04  Probabilistic Miller-Rabin primality tests. */
#define PRIM_TEST_REQUIRED 0x04

/**
 * @internal
 *
 * @brief Determines if the proposed modulus size is more appropriate than the
 * current one.
 *
 * @returns 1 if it's more appropriate. Returns 0 if same or less appropriate
 */
static bool dhgroup_better_size(uint32_t pmin,
                                uint32_t pn,
                                uint32_t pmax,
                                size_t current_size,
                                size_t proposed_size)
{
    if (current_size == proposed_size) {
        return false;
    }

    if (current_size == pn) {
        /* can't do better */
        return false;
    }

    if (current_size == 0 && proposed_size >= pmin && proposed_size <= pmax) {
        return true;
    }

    if (proposed_size < pmin || proposed_size > pmax) {
        /* out of bounds */
        return false;
    }

    if (current_size == 0) {
        /* not in the allowed window */
        return false;
    }

    if (proposed_size >= pn && proposed_size < current_size) {
        return true;
    }

    if (proposed_size <= pn && proposed_size > current_size) {
        return true;
    }

    if (proposed_size >= pn && current_size < pn) {
        return true;
    }

    /* We're in the allowed window but a better match already exists. */
    return false;
}

/** @internal
 * @brief returns 1 with 1/n probability
 * @returns 1 on with P(1/n), 0 with P(n-1/n).
 */
static bool invn_chance(int n)
{
    uint32_t nounce;
    ssh_get_random(&nounce, sizeof(nounce), 0);
    return (nounce % n) == 0;
}

/** @internal
 * @brief retrieves a DH group from an open moduli file.
 */
static int ssh_retrieve_dhgroup_file(FILE *moduli,
                                     uint32_t pmin,
                                     uint32_t pn,
                                     uint32_t pmax,
                                     size_t *best_size,
                                     char **best_generator,
                                     char **best_modulus)
{
    char timestamp[32] = {0};
    char generator[32] = {0};
    char modulus[4096] = {0};
    size_t type, tests, tries, size, proposed_size;
    int firstbyte;
    int rc;
    size_t line = 0;
    size_t best_nlines = 0;

    for(;;) {
        line++;
        firstbyte = getc(moduli);
        if (firstbyte == '#'){
            do {
                firstbyte = getc(moduli);
            } while(firstbyte != '\n' && firstbyte != EOF);
            continue;
        }
        if (firstbyte == EOF) {
            break;
        }
        ungetc(firstbyte, moduli);
        rc = fscanf(moduli,
                    "%31s %zu %zu %zu %zu %31s %4095s\n",
                    timestamp,
                    &type,
                    &tests,
                    &tries,
                    &size,
                    generator,
                    modulus);
        if (rc != 7){
            if (rc == EOF) {
                break;
            }
            SSH_LOG(SSH_LOG_INFO, "Invalid moduli entry line %zu", line);
            do {
                firstbyte = getc(moduli);
            } while(firstbyte != '\n' && firstbyte != EOF);
            continue;
        }

        /* we only want safe primes that were tested */
        if (type != SAFE_PRIME || !(tests & PRIM_TEST_REQUIRED)) {
            continue;
        }

        proposed_size = size + 1;
        if (proposed_size != *best_size &&
            dhgroup_better_size(pmin, pn, pmax, *best_size, proposed_size)) {
            best_nlines = 0;
            *best_size = proposed_size;
        }
        if (proposed_size == *best_size) {
            best_nlines++;
        }

        /* Use reservoir sampling algorithm */
        if (proposed_size == *best_size && invn_chance(best_nlines)) {
            SAFE_FREE(*best_generator);
            SAFE_FREE(*best_modulus);
            *best_generator = strdup(generator);
            if (*best_generator == NULL) {
                return SSH_ERROR;
            }
            *best_modulus = strdup(modulus);
            if (*best_modulus == NULL) {
                SAFE_FREE(*best_generator);
                return SSH_ERROR;
            }
        }
    }
    if (*best_size != 0) {
        SSH_LOG(SSH_LOG_INFO,
                "Selected %zu bits modulus out of %zu candidates in %zu lines",
                *best_size,
                best_nlines - 1,
                line);
    } else {
        SSH_LOG(SSH_LOG_WARNING,
                "No moduli found for [%u:%u:%u]",
                pmin,
                pn,
                pmax);
    }

    return SSH_OK;
}

/** @internal
 * @brief retrieves a DH group from the moduli file based on bits len parameters
 * @param[in] pmin minimum group size in bits
 * @param[in] pn preferred group size
 * @param[in] pmax maximum group size
 * @param[out] size size of the chosen modulus
 * @param[out] p modulus
 * @param[out] g generator
 * @return SSH_OK on success, SSH_ERROR otherwise.
 */
/* TODO Make this function static when only used in this file */
int ssh_retrieve_dhgroup(uint32_t pmin,
                         uint32_t pn,
                         uint32_t pmax,
                         size_t *size,
                         bignum *p,
                         bignum *g);
int ssh_retrieve_dhgroup(uint32_t pmin,
                         uint32_t pn,
                         uint32_t pmax,
                         size_t *size,
                         bignum *p,
                         bignum *g)
{
    FILE *moduli = NULL;
    char *generator = NULL;
    char *modulus = NULL;
    int rc;

    moduli = fopen(MODULI_FILE, "r");
    if (moduli == NULL) {
        SSH_LOG(SSH_LOG_WARNING,
                "Unable to open moduli file: %s",
                strerror(errno));
        return SSH_ERROR;
    }

    *size = 0;
    *p = NULL;
    *g = NULL;

    rc = ssh_retrieve_dhgroup_file(moduli,
                                   pmin,
                                   pn,
                                   pmax,
                                   size,
                                   &generator,
                                   &modulus);
    if (rc == SSH_ERROR || *size == 0) {
        goto error;
    }
    rc = bignum_hex2bn(generator, g);
    if (rc == 0) {
        goto error;
    }
    rc = bignum_hex2bn(modulus, p);
    if (rc == 0) {
        goto error;
    }
    SAFE_FREE(generator);
    SAFE_FREE(modulus);

    return SSH_OK;

error:
    bignum_safe_free(*g);
    bignum_safe_free(*p);
    SAFE_FREE(generator);
    SAFE_FREE(modulus);

    return SSH_ERROR;
}

#endif /* WITH_SERVER */
