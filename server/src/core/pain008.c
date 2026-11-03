#include "core/pain008.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Replacements for U+00C0..U+00FF. */
static const char *const LATIN1[64] = {
  "A","A","A","A","Ae","A","AE","C","E","E","E","E","I","I","I","I",
  "D","N","O","O","O","O","Oe",".","O","U","U","U","Ue","Y","Th","ss",
  "a","a","a","a","ae","a","ae","c","e","e","e","e","i","i","i","i",
  "d","n","o","o","o","o","oe",".","o","u","u","u","ue","y","th","y"
};

static int utf8_next(const unsigned char **p) {
  const unsigned char *s = *p;
  int cp, extra;
  if (s[0] < 0x80) { *p = s + 1; return s[0]; }
  if ((s[0] & 0xE0) == 0xC0) { cp = s[0] & 0x1F; extra = 1; }
  else if ((s[0] & 0xF0) == 0xE0) { cp = s[0] & 0x0F; extra = 2; }
  else if ((s[0] & 0xF8) == 0xF0) { cp = s[0] & 0x07; extra = 3; }
  else { *p = s + 1; return -1; }
  for (int i = 1; i <= extra; i++) {
    if ((s[i] & 0xC0) != 0x80) { *p = s + i; return -1; }
    cp = (cp << 6) | (s[i] & 0x3F);
  }
  *p = s + extra + 1;
  return cp;
}

static const char *translit_cp(int cp, char *buf) {
  if (cp >= 0xC0 && cp <= 0xFF) return LATIN1[cp - 0xC0];
  if (cp == '&') return "+";
  if (cp >= 0 && cp < 0x80 &&
      ((cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z') || (cp >= '0' && cp <= '9') ||
       strchr("/-?:().,'+ ", cp) != NULL)) {
    buf[0] = (char)cp; buf[1] = 0;
    return buf;
  }
  return ".";
}

void sepa_translit(const char *utf8, char *out, size_t cap, size_t max_chars) {
  if (cap == 0) return;
  size_t limit = max_chars < cap - 1 ? max_chars : cap - 1;
  size_t n = 0;
  char buf[2];
  const unsigned char *p = (const unsigned char *)(utf8 ? utf8 : "");
  while (*p && n < limit) {
    const char *r = translit_cp(utf8_next(&p), buf);
    if (n == 0 && r[0] == ' ') continue;   /* trim leading */
    for (; *r && n < limit; r++) out[n++] = *r;
  }
  while (n > 0 && out[n - 1] == ' ') n--;  /* trim trailing */
  out[n] = 0;
}

typedef struct { char *p; size_t len, cap; int err; } sb_t;

static void sb_raw(sb_t *b, const char *s, size_t n) {
  if (b->err) return;
  if (b->len + n + 1 > b->cap) {
    size_t nc = b->cap ? b->cap : 1024;
    while (nc < b->len + n + 1) nc *= 2;
    char *np = realloc(b->p, nc);
    if (!np) { b->err = 1; return; }
    b->p = np; b->cap = nc;
  }
  memcpy(b->p + b->len, s, n);
  b->len += n;
  b->p[b->len] = 0;
}

static void sb_str(sb_t *b, const char *s) { sb_raw(b, s, strlen(s)); }

static void sb_esc(sb_t *b, const char *s) {
  for (; *s; s++) {
    switch (*s) {
      case '&': sb_str(b, "&amp;"); break;
      case '<': sb_str(b, "&lt;"); break;
      case '>': sb_str(b, "&gt;"); break;
      case '"': sb_str(b, "&quot;"); break;
      case '\'': sb_str(b, "&apos;"); break;
      default: sb_raw(b, s, 1);
    }
  }
}

static void sb_elem(sb_t *b, const char *tag, const char *val) {
  sb_str(b, "<"); sb_str(b, tag); sb_str(b, ">");
  sb_esc(b, val);
  sb_str(b, "</"); sb_str(b, tag); sb_str(b, ">");
}

static void amount_str(int64_t cents, char *out, size_t cap) {
  snprintf(out, cap, "%lld.%02d", (long long)(cents / 100), (int)(cents % 100));
}

static void sb_agent(sb_t *b, const char *tag, const char *bic) {
  sb_str(b, "<"); sb_str(b, tag); sb_str(b, "><FinInstnId>");
  if (bic[0]) sb_elem(b, "BIC", bic);
  else sb_str(b, "<Othr><Id>NOTPROVIDED</Id></Othr>");
  sb_str(b, "</FinInstnId></"); sb_str(b, tag); sb_str(b, ">");
}

int pain008_build(const pain008_hdr_t *h, const pain008_tx_t *tx, size_t n, char **out, size_t *len) {
  if (!h || !tx || !out || n == 0) return -1;
  int64_t sum = 0;
  for (size_t i = 0; i < n; i++) {
    if (tx[i].amount_cents <= 0) return -1;
    sum += tx[i].amount_cents;
  }
  char sums[32], cnt[24], t[142];
  amount_str(sum, sums, sizeof sums);
  snprintf(cnt, sizeof cnt, "%zu", n);

  sb_t b = {0};
  sb_str(&b, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
             "<Document xmlns=\"urn:iso:std:iso:20022:tech:xsd:pain.008.001.02\" "
             "xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\">"
             "<CstmrDrctDbtInitn><GrpHdr>");
  sb_elem(&b, "MsgId", h->msg_id);
  sb_elem(&b, "CreDtTm", h->created);
  sb_elem(&b, "NbOfTxs", cnt);
  sb_elem(&b, "CtrlSum", sums);
  sepa_translit(h->creditor_name, t, sizeof t, 70);
  sb_str(&b, "<InitgPty>"); sb_elem(&b, "Nm", t); sb_str(&b, "</InitgPty></GrpHdr><PmtInf>");
  sb_elem(&b, "PmtInfId", h->msg_id);
  sb_elem(&b, "PmtMtd", "DD");
  sb_elem(&b, "BtchBookg", "true");
  sb_elem(&b, "NbOfTxs", cnt);
  sb_elem(&b, "CtrlSum", sums);
  sb_str(&b, "<PmtTpInf><SvcLvl><Cd>SEPA</Cd></SvcLvl><LclInstrm><Cd>CORE</Cd></LclInstrm>"
             "<SeqTp>RCUR</SeqTp></PmtTpInf>");
  sb_elem(&b, "ReqdColltnDt", h->collection_date);
  sb_str(&b, "<Cdtr>"); sb_elem(&b, "Nm", t); sb_str(&b, "</Cdtr>");
  sb_str(&b, "<CdtrAcct><Id>"); sb_elem(&b, "IBAN", h->creditor_iban); sb_str(&b, "</Id></CdtrAcct>");
  sb_agent(&b, "CdtrAgt", h->creditor_bic);
  sb_elem(&b, "ChrgBr", "SLEV");
  sb_str(&b, "<CdtrSchmeId><Id><PrvtId><Othr>");
  sb_elem(&b, "Id", h->creditor_id);
  sb_str(&b, "<SchmeNm><Prtry>SEPA</Prtry></SchmeNm></Othr></PrvtId></Id></CdtrSchmeId>");

  for (size_t i = 0; i < n; i++) {
    const pain008_tx_t *x = &tx[i];
    sb_str(&b, "<DrctDbtTxInf><PmtId>");
    sb_elem(&b, "EndToEndId", x->end_to_end);
    sb_str(&b, "</PmtId>");
    amount_str(x->amount_cents, sums, sizeof sums);
    sb_str(&b, "<InstdAmt Ccy=\"EUR\">"); sb_esc(&b, sums); sb_str(&b, "</InstdAmt>");
    sb_str(&b, "<DrctDbtTx><MndtRltdInf>");
    sb_elem(&b, "MndtId", x->mandate_ref);
    sb_elem(&b, "DtOfSgntr", x->mandate_date);
    sb_str(&b, "</MndtRltdInf></DrctDbtTx>");
    sb_agent(&b, "DbtrAgt", x->bic);
    sepa_translit(x->name, t, sizeof t, 70);
    sb_str(&b, "<Dbtr>"); sb_elem(&b, "Nm", t); sb_str(&b, "</Dbtr>");
    sb_str(&b, "<DbtrAcct><Id>"); sb_elem(&b, "IBAN", x->iban); sb_str(&b, "</Id></DbtrAcct>");
    sepa_translit(x->remittance, t, sizeof t, 140);
    if (t[0]) { sb_str(&b, "<RmtInf>"); sb_elem(&b, "Ustrd", t); sb_str(&b, "</RmtInf>"); }
    sb_str(&b, "</DrctDbtTxInf>");
  }
  sb_str(&b, "</PmtInf></CstmrDrctDbtInitn></Document>\n");
  if (b.err) { free(b.p); return -1; }
  *out = b.p;
  if (len) *len = b.len;
  return 0;
}
