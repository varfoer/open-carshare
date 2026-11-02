#include "core/sepa.h"
#include <stdio.h>
#include <string.h>

static bool is_upper(char c) { return c >= 'A' && c <= 'Z'; }
static bool is_digit(char c) { return c >= '0' && c <= '9'; }
static bool is_alnum(char c) { return is_upper(c) || is_digit(c); }

int sepa_normalize(const char *in, char *out, size_t cap) {
    size_t n = 0;
    if (cap == 0) return -1;
    for (; *in; in++) {
        char c = *in;
        if (c == ' ') continue;
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        if (!is_alnum(c)) return -1;
        if (n + 1 >= cap) return -1;
        out[n++] = c;
    }
    out[n] = '\0';
    return 0;
}

static const struct { char cc[3]; int len; } iban_len[] = {
    {"AD",24},{"AT",20},{"BE",16},{"BG",22},{"CH",21},{"CY",28},{"CZ",24},{"DE",22},
    {"DK",18},{"EE",20},{"ES",24},{"FI",18},{"FR",27},{"GB",22},{"GI",23},{"GR",27},
    {"HR",21},{"HU",28},{"IE",22},{"IS",26},{"IT",27},{"LI",21},{"LT",20},{"LU",20},
    {"LV",21},{"MC",27},{"MT",31},{"NL",18},{"NO",15},{"PL",28},{"PT",25},{"RO",24},
    {"SE",24},{"SI",19},{"SK",24},{"SM",27},{"VA",22},
};

/* Feed one char into a running mod-97 (letters A=10..Z=35), digit by digit. */
static int mod97_feed(int r, char c) {
    if (is_digit(c)) return (r * 10 + (c - '0')) % 97;
    int v = c - 'A' + 10;
    r = (r * 10 + v / 10) % 97;
    return (r * 10 + v % 10) % 97;
}

bool iban_valid(const char *s) {
    size_t n = strlen(s);
    if (n < 4) return false;
    for (size_t i = 0; i < n; i++) if (!is_alnum(s[i])) return false;
    if (!is_upper(s[0]) || !is_upper(s[1]) || !is_digit(s[2]) || !is_digit(s[3])) return false;
    bool ok = false;
    for (size_t i = 0; i < sizeof iban_len / sizeof iban_len[0]; i++)
        if (iban_len[i].cc[0] == s[0] && iban_len[i].cc[1] == s[1] && (size_t)iban_len[i].len == n) { ok = true; break; }
    if (!ok) return false;
    int r = 0;
    for (size_t i = 4; i < n; i++) r = mod97_feed(r, s[i]);
    for (size_t i = 0; i < 4; i++) r = mod97_feed(r, s[i]);
    return r == 1;
}

bool bic_valid(const char *s) {
    size_t n = strlen(s);
    if (n != 8 && n != 11) return false;
    for (size_t i = 0; i < 6; i++) if (!is_upper(s[i])) return false;
    for (size_t i = 6; i < n; i++) if (!is_alnum(s[i])) return false;
    return true;
}

bool creditor_id_valid(const char *s) {
    size_t n = strlen(s);
    if (n < 7 + 1) return false;
    for (size_t i = 0; i < n; i++) if (!is_alnum(s[i])) return false;
    if (!is_upper(s[0]) || !is_upper(s[1]) || !is_digit(s[2]) || !is_digit(s[3])) return false;
    if (s[0] == 'D' && s[1] == 'E') { if (n != 18) return false; }
    else if (n < 8 || n > 35) return false;
    if (n - 7 > 28) return false;
    int r = 0;
    for (size_t i = 7; i < n; i++) r = mod97_feed(r, s[i]);
    for (size_t i = 0; i < 4; i++) r = mod97_feed(r, s[i]);
    return r == 1;
}

void iban_mask(const char *iban, char *out, size_t cap) {
    size_t n = strlen(iban);
    if (cap == 0) return;
    if (n < 8) { out[0] = '\0'; return; }
    snprintf(out, cap, "%.4s **** %s", iban, iban + n - 4);
}

void mandate_ref(int64_t id, char *out, size_t cap) {
    snprintf(out, cap, "OCS-%06lld", (long long)id);
}

int mandate_text(char *out, size_t cap, const char *creditor_name, const char *creditor_id) {
    int n = snprintf(out, cap,
        "SEPA-Lastschriftmandat\n\n"
        "Zahlungsempfänger: %1$s\n"
        "Gläubiger-Identifikationsnummer: %2$s\n"
        "Mandatsreferenz: wird nach Erteilung des Mandats mitgeteilt\n\n"
        "Ich ermächtige %1$s, Zahlungen von meinem Konto mittels Lastschrift einzuziehen. "
        "Zugleich weise ich mein Kreditinstitut an, die von %1$s auf mein Konto gezogenen Lastschriften einzulösen.\n\n"
        "Hinweis: Ich kann innerhalb von acht Wochen, beginnend mit dem Belastungsdatum, "
        "die Erstattung des belasteten Betrages verlangen. "
        "Es gelten dabei die mit meinem Kreditinstitut vereinbarten Bedingungen.\n\n"
        "Das Mandat gilt für wiederkehrende Lastschriften.",
        creditor_name, creditor_id);
    if (n < 0 || (size_t)n >= cap) return -1;
    return n;
}
