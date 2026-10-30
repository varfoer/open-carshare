#ifndef OCS_AUTH_SESSION_H
#define OCS_AUTH_SESSION_H

#include <stdint.h>

#include "app.h"
#include "core/model.h"

/* Tokens are 64 lowercase hex chars (32 random bytes). Only the BLAKE2b-256 hex
 * of a token is stored. A time is valid while now < expires_at. */
#define SESSION_TTL_SECS 2592000      /* 30 days */
#define TOKEN_RESET_TTL_SECS 3600     /* 1 hour */
#define TOKEN_INVITE_TTL_SECS 604800  /* 7 days */

/* 0 ok, -1 on random or storage failure. Expires now + 30 d. */
int session_create(app_t *a, int64_t user_id, int64_t now, char token_out[65]);
/* NULL if token malformed/unknown/expired or user missing/inactive. The pointer
 * points into the users table and is invalid after any db_put_user. */
user_t *session_user(app_t *a, const char *token, int64_t now);
/* 0 ok (also if unknown), -1 on storage failure. */
int session_destroy(app_t *a, const char *token);
/* Reset tokens live 1 h, invite tokens 7 d. 0 ok, -1 on random or storage failure. */
int token_issue(app_t *a, int64_t user_id, token_kind_t kind, int64_t now, char token_out[65]);
/* user_id, or -1 if malformed/unknown/used/expired or marking it used failed. */
int64_t token_consume(app_t *a, const char *token, int64_t now);
/* BLAKE2b-256 hex of token (any string). */
void session_hash(const char *token, char out[65]);

#endif
