/*
 * Copyright (c) 2017-2019 Fastly, Kazuho Oku
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */
#include <sys/time.h>
#include "quicly/defaults.h"

/* profile that employs IETF specified values */
const quicly_context_t quicly_spec_context = {
    NULL,                   /* tls */
    QUICLY_MAX_PACKET_SIZE, /* max_packet_size */
    QUICLY_LOSS_SPEC_CONF,  /* loss */
    {
        {1 * 1024 * 1024, 1 * 1024 * 1024, 1 * 1024 * 1024}, /* max_stream_data */
        16 * 1024 * 1024,                                    /* max_data */
        30 * 1000,                                           /* idle_timeout (30 seconds) */
        100,                                                 /* max_concurrent_streams_bidi */
        0                                                    /* max_concurrent_streams_uni */
    },
    0, /* enforce_version_negotiation */
    0, /* is_clustered */
    &quicly_default_packet_allocator,
    NULL,
    NULL, /* on_stream_open */
    &quicly_default_stream_scheduler,
    NULL, /* on_conn_close */
    &quicly_default_now
};

/* profile with a focus on reducing latency for the HTTP use case */
const quicly_context_t quicly_performant_context = {
    NULL,                         /* tls */
    QUICLY_MAX_PACKET_SIZE,       /* max_packet_size */
    QUICLY_LOSS_PERFORMANT_CONF,  /* loss */
    {
        {1 * 1024 * 1024, 1 * 1024 * 1024, 1 * 1024 * 1024}, /* max_stream_data */
        16 * 1024 * 1024,                                    /* max_data */
        30 * 1000,                                           /* idle_timeout (30 seconds) */
        100,                                                 /* max_concurrent_streams_bidi */
        0                                                    /* max_concurrent_streams_uni */
    },
    0, /* enforce_version_negotiation */
    0, /* is_clustered */
    &quicly_default_packet_allocator,
    NULL,
    NULL, /* on_stream_open */
    &quicly_default_stream_scheduler,
    NULL, /* on_conn_close */
    &quicly_default_now
};

static quicly_datagram_t *default_alloc_packet(quicly_packet_allocator_t *self, size_t payloadsize)
{
    quicly_datagram_t *packet;

    if ((packet = malloc(sizeof(*packet) + payloadsize)) == NULL)
        return NULL;
    packet->data.base = (uint8_t *)packet + sizeof(*packet);

    return packet;
}

static void default_free_packet(quicly_packet_allocator_t *self, quicly_datagram_t *packet)
{
    free(packet);
}

quicly_packet_allocator_t quicly_default_packet_allocator = {default_alloc_packet, default_free_packet};

/**
 * The context of the default CID encryptor.  All the contexts being used here are ECB ciphers and therefore stateless - they can be
 * used concurrently from multiple threads.
 */
struct st_quicly_default_encrypt_cid_t {
    quicly_cid_encryptor_t super;
    ptls_cipher_context_t *cid_encrypt_ctx, *cid_decrypt_ctx, *reset_token_ctx;
};

static void generate_reset_token(struct st_quicly_default_encrypt_cid_t *self, void *token, const void *cid)
{
    uint8_t expandbuf[QUICLY_STATELESS_RESET_TOKEN_LEN];

    assert(self->reset_token_ctx->algo->block_size == QUICLY_STATELESS_RESET_TOKEN_LEN);

    /* expand the input to full size, if CID is shorter than the size of the reset token */
    if (self->cid_encrypt_ctx->algo->block_size != QUICLY_STATELESS_RESET_TOKEN_LEN) {
        assert(self->cid_encrypt_ctx->algo->block_size < QUICLY_STATELESS_RESET_TOKEN_LEN);
        memset(expandbuf, 0, sizeof(expandbuf));
        memcpy(expandbuf, cid, self->cid_encrypt_ctx->algo->block_size);
        cid = expandbuf;
    }

    /* transform */
    ptls_cipher_encrypt(self->reset_token_ctx, token, cid, QUICLY_STATELESS_RESET_TOKEN_LEN);
}

static void default_encrypt_cid(quicly_cid_encryptor_t *_self, quicly_cid_t *encrypted, void *reset_token,
                                const quicly_cid_plaintext_t *plaintext)
{
    struct st_quicly_default_encrypt_cid_t *self = (void *)_self;
    uint8_t buf[16], *p;

    /* encode */
    p = buf;
    switch (self->cid_encrypt_ctx->algo->block_size) {
    case 8:
        break;
    case 16:
        p = quicly_encode64(p, plaintext->node_id);
        break;
    default:
        assert(!"unexpected block size");
        break;
    }
    p = quicly_encode32(p, plaintext->master_id);
    p = quicly_encode32(p, (plaintext->thread_id << 8) | plaintext->path_id);
    assert(p - buf == self->cid_encrypt_ctx->algo->block_size);

    /* generate CID */
    ptls_cipher_encrypt(self->cid_encrypt_ctx, encrypted->cid, buf, self->cid_encrypt_ctx->algo->block_size);
    encrypted->len = self->cid_encrypt_ctx->algo->block_size;

    /* generate stateless reset token if requested */
    if (reset_token != NULL)
        generate_reset_token(self, reset_token, encrypted->cid);
}

static size_t default_decrypt_cid(quicly_cid_encryptor_t *_self, quicly_cid_plaintext_t *plaintext, const void *encrypted,
                                  size_t len)
{
    struct st_quicly_default_encrypt_cid_t *self = (void *)_self;
    uint8_t ptbuf[16], tmpbuf[16];
    const uint8_t *p;
    size_t cid_len;

    cid_len = self->cid_decrypt_ctx->algo->block_size;

    /* normalize the input, so that we would get consistent routing */
    if (len != 0 && len != cid_len) {
        if (len > cid_len)
            len = cid_len;
        memcpy(tmpbuf, encrypted, cid_len);
        if (len < cid_len)
            memset(tmpbuf + len, 0, cid_len - len);
        encrypted = tmpbuf;
    }

    /* decrypt */
    ptls_cipher_encrypt(self->cid_decrypt_ctx, ptbuf, encrypted, cid_len);

    /* decode */
    p = ptbuf;
    if (cid_len == 16) {
        plaintext->node_id = quicly_decode64(&p);
    } else {
        plaintext->node_id = 0;
    }
    plaintext->master_id = quicly_decode32(&p);
    plaintext->thread_id = quicly_decode24(&p);
    plaintext->path_id = *p++;
    assert(p - ptbuf == cid_len);

    return cid_len;
}

static int default_generate_reset_token(quicly_cid_encryptor_t *_self, void *token, const void *cid)
{
    struct st_quicly_default_encrypt_cid_t *self = (void *)_self;
    generate_reset_token(self, token, cid);
    return 1;
}

quicly_cid_encryptor_t *quicly_new_default_cid_encryptor(ptls_cipher_algorithm_t *cid_cipher,
                                                         ptls_cipher_algorithm_t *reset_token_cipher, ptls_hash_algorithm_t *hash,
                                                         ptls_iovec_t key)
{
    struct st_quicly_default_encrypt_cid_t *self;
    uint8_t digestbuf[PTLS_MAX_DIGEST_SIZE], keybuf[PTLS_MAX_SECRET_SIZE];

    assert(cid_cipher->block_size == 8 || cid_cipher->block_size == 16);
    assert(reset_token_cipher->block_size == 16);

    if (key.len > hash->block_size) {
        ptls_calc_hash(hash, digestbuf, key.base, key.len);
        key = ptls_iovec_init(digestbuf, hash->digest_size);
    }

    if ((self = malloc(sizeof(*self))) == NULL)
        goto Fail;
    *self = (struct st_quicly_default_encrypt_cid_t){{default_encrypt_cid, default_decrypt_cid, default_generate_reset_token}};

    if (ptls_hkdf_expand_label(hash, keybuf, cid_cipher->key_size, key, "cid", ptls_iovec_init(NULL, 0), "") != 0)
        goto Fail;
    if ((self->cid_encrypt_ctx = ptls_cipher_new(cid_cipher, 1, keybuf)) == NULL)
        goto Fail;
    if ((self->cid_decrypt_ctx = ptls_cipher_new(cid_cipher, 0, keybuf)) == NULL)
        goto Fail;
    if (ptls_hkdf_expand_label(hash, keybuf, reset_token_cipher->key_size, key, "reset", ptls_iovec_init(NULL, 0), "") != 0)
        goto Fail;
    if ((self->reset_token_ctx = ptls_cipher_new(reset_token_cipher, 1, keybuf)) == NULL)
        goto Fail;

    ptls_clear_memory(digestbuf, sizeof(digestbuf));
    ptls_clear_memory(keybuf, sizeof(keybuf));
    return &self->super;

Fail:
    if (self != NULL) {
        if (self->cid_encrypt_ctx != NULL)
            ptls_cipher_free(self->cid_encrypt_ctx);
        if (self->cid_decrypt_ctx != NULL)
            ptls_cipher_free(self->cid_decrypt_ctx);
        if (self->reset_token_ctx != NULL)
            ptls_cipher_free(self->reset_token_ctx);
        free(self);
    }
    ptls_clear_memory(digestbuf, sizeof(digestbuf));
    ptls_clear_memory(keybuf, sizeof(keybuf));
    return NULL;
}

void quicly_free_default_cid_encryptor(quicly_cid_encryptor_t *_self)
{
    struct st_quicly_default_encrypt_cid_t *self = (void *)_self;

    ptls_cipher_free(self->cid_encrypt_ctx);
    ptls_cipher_free(self->cid_decrypt_ctx);
    ptls_cipher_free(self->reset_token_ctx);
    free(self);
}

struct st_quicly_default_crypto_codec_t {
    quicly_crypto_codec_t super;
};

static size_t quicly_default_crypto_codec_packet_encrypt(quicly_send_context_t *s, uint16_t packet_number)
{
     s->dst = s->dst_payload_from + ptls_aead_encrypt(s->target.cipher->aead, s->dst_payload_from, s->dst_payload_from,
                                                      s->dst - s->dst_payload_from, packet_number,
                                                      s->target.first_byte_at, s->dst_payload_from - s->target.first_byte_at);

    { /* apply header protection */
        uint8_t hpmask[1 + QUICLY_SEND_PN_SIZE] = {0};
        ptls_cipher_init(s->target.cipher->header_protection, s->dst_payload_from - QUICLY_SEND_PN_SIZE + QUICLY_MAX_PN_SIZE);
        ptls_cipher_encrypt(s->target.cipher->header_protection, hpmask, hpmask, sizeof(hpmask));
        *s->target.first_byte_at ^= hpmask[0] & (QUICLY_PACKET_IS_LONG_HEADER(*s->target.first_byte_at) ? 0xf : 0x1f);
        size_t i;
        for (i = 0; i != QUICLY_SEND_PN_SIZE; ++i)
            s->dst_payload_from[i - QUICLY_SEND_PN_SIZE] ^= hpmask[i + 1];
    }
}

static void quicly_default_crypto_codec_packet_encrypt_done(ptls_cipher_context_t *header_protection, uint8_t *first_byte_at, uint8_t *dst_payload_from)
{
}

static int quicly_default_crypto_codec_packet_decrypt(quicly_crypto_codec_t *_self, quicly_conn_t *conn, struct st_quicly_pn_space_t **space,
                                               ptls_cipher_context_t *header_protection, ptls_aead_context_t **aead, size_t epoch,
                                               uint64_t *pn, uint64_t *next_expected_pn, quicly_decoded_packet_t *packet, struct sockaddr *dest_addr, struct sockaddr *src_addr)
{
    size_t encrypted_len = packet->octets.len - packet->encrypted_off;
    uint8_t hpmask[5] = {0};
    uint32_t pnbits = 0;
    size_t pnlen, aead_index, i;

    struct st_quicly_default_crypto_codec_t *self = (void *)_self;

    /* decipher the header protection, as well as obtaining pnbits, pnlen */
    if (encrypted_len < header_protection->algo->iv_size + QUICLY_MAX_PN_SIZE)
        goto Error;
    ptls_cipher_init(header_protection, packet->octets.base + packet->encrypted_off + QUICLY_MAX_PN_SIZE);
    ptls_cipher_encrypt(header_protection, hpmask, hpmask, sizeof(hpmask));
    packet->octets.base[0] ^= hpmask[0] & (QUICLY_PACKET_IS_LONG_HEADER(packet->octets.base[0]) ? 0xf : 0x1f);
    pnlen = (packet->octets.base[0] & 0x3) + 1;
    for (i = 0; i != pnlen; ++i) {
        packet->octets.base[packet->encrypted_off + i] ^= hpmask[i + 1];
        pnbits = (pnbits << 8) | packet->octets.base[packet->encrypted_off + i];
    }

    /* determine aead index (FIXME move AEAD key selection and decryption logic to the caller?) */
    if (QUICLY_PACKET_IS_LONG_HEADER(packet->octets.base[0])) {
        aead_index = 0;
    } else {
        /* note: aead index 0 is used by 0-RTT */
        aead_index = (packet->octets.base[0] & QUICLY_KEY_PHASE_BIT) == 0;
        if (aead[aead_index] == NULL)
            goto Error;
    }

    /* AEAD */
    *pn = quicly_determine_packet_number(pnbits, pnlen * 8, *next_expected_pn);
    size_t aead_off = packet->encrypted_off + pnlen, ptlen;
    if ((ptlen = ptls_aead_decrypt(aead[aead_index], packet->octets.base + aead_off, packet->octets.base + aead_off,
                                   packet->octets.len - aead_off, *pn, packet->octets.base, aead_off)) == SIZE_MAX) {
        if (QUICLY_DEBUG)
            fprintf(stderr, "%s: aead decryption failure (pn: %" PRIu64 ")\n", __FUNCTION__, *pn);
        goto Error;
    }

    self->super.decrypt_packet_done(conn, packet, space, epoch, pn, next_expected_pn, aead_off, ptlen, dest_addr, src_addr);

    return 0;

Error:
    return 1;
}

quicly_crypto_codec_t *quicly_new_default_crypto_codec()
{
    struct st_quicly_default_crypto_codec_t *self = NULL;
    if ((self = malloc(sizeof(*self))) == NULL)
        goto Exit;

    *self = (struct st_quicly_default_crypto_codec_t){{quicly_default_crypto_codec_packet_encrypt, quicly_default_crypto_codec_packet_encrypt_done, quicly_default_crypto_codec_packet_decrypt, NULL}};

Exit:
    return &self->super;
}

/**
 * See doc-comment of `st_quicly_default_scheduler_state_t` to understand the logic.
 */
static int default_stream_scheduler_can_send(quicly_stream_scheduler_t *self, quicly_conn_t *conn, int conn_is_saturated)
{
    struct st_quicly_default_scheduler_state_t *sched = &((struct _st_quicly_conn_public_t *)conn)->_default_scheduler;

    if (!conn_is_saturated) {
        /* not saturated */
        quicly_linklist_insert_list(&sched->active, &sched->blocked);
    } else {
        /* The code below is disabled, because H2O's scheduler doesn't allow you to "walk" the priority tree without actually
         * running the round robin, and we want quicly's default to behave like H2O so that we can catch errors.  The downside is
         * that there'd be at most one spurious call of `quicly_send` when the connection is saturated, but that should be fine.
         */
        if (0) {
            /* Saturated. Lazily move such streams to the "blocked" list, at the same time checking if anything can be sent. */
            while (quicly_linklist_is_linked(&sched->active)) {
                quicly_stream_t *stream =
                    (void *)((char *)(sched->active.next - offsetof(quicly_stream_t, _send_aux.pending_link.default_scheduler)));
                if (quicly_sendstate_can_send(&stream->sendstate, NULL))
                    return 1;
                quicly_linklist_unlink(&stream->_send_aux.pending_link.default_scheduler);
                quicly_linklist_insert(sched->blocked.prev, &stream->_send_aux.pending_link.default_scheduler);
            }
        }
    }

    return quicly_linklist_is_linked(&sched->active);
}

static void link_stream(struct st_quicly_default_scheduler_state_t *sched, quicly_stream_t *stream, int conn_is_flow_capped)
{
    if (!quicly_linklist_is_linked(&stream->_send_aux.pending_link.default_scheduler)) {
        quicly_linklist_t *slot = &sched->active;
        if (conn_is_flow_capped && !quicly_sendstate_can_send(&stream->sendstate, NULL))
            slot = &sched->blocked;
        quicly_linklist_insert(slot->prev, &stream->_send_aux.pending_link.default_scheduler);
    }
}

/**
 * See doc-comment of `st_quicly_default_scheduler_state_t` to understand the logic.
 */
static int default_stream_scheduler_do_send(quicly_stream_scheduler_t *self, quicly_conn_t *conn, quicly_send_context_t *s)
{
    struct st_quicly_default_scheduler_state_t *sched = &((struct _st_quicly_conn_public_t *)conn)->_default_scheduler;
    int conn_is_flow_capped = quicly_is_flow_capped(conn), ret = 0;

    while (quicly_can_send_stream_data((quicly_conn_t *)conn, s) && quicly_linklist_is_linked(&sched->active)) {
        /* detach the first active stream */
        quicly_stream_t *stream =
            (void *)((char *)sched->active.next - offsetof(quicly_stream_t, _send_aux.pending_link.default_scheduler));
        quicly_linklist_unlink(&stream->_send_aux.pending_link.default_scheduler);
        /* relink the stream to the blocked list if necessary */
        if (conn_is_flow_capped && !quicly_sendstate_can_send(&stream->sendstate, NULL)) {
            quicly_linklist_insert(sched->blocked.prev, &stream->_send_aux.pending_link.default_scheduler);
            continue;
        }
        /* send! */
        if ((ret = quicly_send_stream(stream, s)) != 0) {
            /* FIXME Stop quicly_send_stream emitting SENDBUF_FULL (happpens when CWND is congested). Otherwise, we need to make
             * adjustments to the scheduler after popping a stream */
            if (ret == QUICLY_ERROR_SENDBUF_FULL) {
                assert(quicly_sendstate_can_send(&stream->sendstate, &stream->_send_aux.max_stream_data));
                link_stream(sched, stream, conn_is_flow_capped);
            }
            break;
        }
        /* reschedule */
        conn_is_flow_capped = quicly_is_flow_capped(conn);
        if (quicly_sendstate_can_send(&stream->sendstate, &stream->_send_aux.max_stream_data))
            link_stream(sched, stream, conn_is_flow_capped);
    }

    return ret;
}

/**
 * See doc-comment of `st_quicly_default_scheduler_state_t` to understand the logic.
 */
static int default_stream_scheduler_update_state(quicly_stream_scheduler_t *self, quicly_stream_t *stream)
{
    struct st_quicly_default_scheduler_state_t *sched = &((struct _st_quicly_conn_public_t *)stream->conn)->_default_scheduler;

    if (quicly_sendstate_can_send(&stream->sendstate, &stream->_send_aux.max_stream_data)) {
        /* activate if not */
        link_stream(sched, stream, quicly_is_flow_capped(stream->conn));
    } else {
        /* disactivate if active */
        if (quicly_linklist_is_linked(&stream->_send_aux.pending_link.default_scheduler))
            quicly_linklist_unlink(&stream->_send_aux.pending_link.default_scheduler);
    }

    return 0;
}

quicly_stream_scheduler_t quicly_default_stream_scheduler = {default_stream_scheduler_can_send, default_stream_scheduler_do_send,
                                                             default_stream_scheduler_update_state};

quicly_stream_t *quicly_default_alloc_stream(quicly_context_t *ctx)
{
    return malloc(sizeof(quicly_stream_t));
}

void quicly_default_free_stream(quicly_stream_t *stream)
{
    free(stream);
}

static int64_t default_now(quicly_now_t *self)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

quicly_now_t quicly_default_now = {default_now};
