#include "core/access.h"

#include <string.h>

static int64_t hour_floor(int64_t t) { return t - t % 3600; }

void access_window(int64_t now, int64_t booked_end, int64_t *from, int64_t *until) {
  int64_t f = hour_floor(now);
  int64_t e = booked_end > now + 3600 ? booked_end : now + 3600;
  int64_t u = hour_floor(e + 3599);
  if (u > f + (int64_t)ACCESS_MAX_HOURS * 3600) u = f + (int64_t)ACCESS_MAX_HOURS * 3600;
  *from = f;
  *until = u;
}

bool access_usable(const access_pin_t *p, int64_t now) {
  return p->pin[0] && now < p->valid_until - ACCESS_MARGIN_S;
}

bool lock_device_id_valid(const char *s) {
  size_t n = strlen(s);
  if (n < 1 || n > 63) return false;
  for (size_t i = 0; i < n; i++) {
    char c = s[i];
    bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '.' || c == '_' || c == '-';
    if (!ok) return false;
  }
  return true;
}

const char *pin_err_log(pin_err_t e) {
  switch (e) {
    case PIN_ERR_CONFIG: return "igloo not configured";
    case PIN_ERR_AUTH: return "igloo: credentials rejected";
    case PIN_ERR_PAYMENT: return "igloo: payment required";
    case PIN_ERR_SCOPE: return "igloo: token scope lacks algopin-hourly";
    case PIN_ERR_DEVICE: return "igloo: unknown device";
    case PIN_ERR_TIMEZONE: return "igloo: device property has no timezone";
    case PIN_ERR_UNSUPPORTED: return "igloo: device does not support AlgoPIN";
    case PIN_ERR_NETWORK: return "igloo: unreachable";
    case PIN_ERR_OTHER: return "igloo: unexpected answer";
    case PIN_OK: break;
  }
  return "igloo: ok";
}

const char *pin_err_admin_de(pin_err_t e) {
  switch (e) {
    case PIN_ERR_CONFIG: return "Die igloohome-Zugangsdaten fehlen in ocs.conf.";
    case PIN_ERR_AUTH: return "igloohome lehnt die Zugangsdaten ab.";
    case PIN_ERR_PAYMENT: return "igloohome-API nicht bezahlt (Testzeitraum abgelaufen).";
    case PIN_ERR_SCOPE: return "Dem igloohome-Zugang fehlt eine Berechtigung (algopin-hourly, get-devices).";
    case PIN_ERR_DEVICE: return "Box bei igloohome nicht gefunden.";
    case PIN_ERR_TIMEZONE: return "In der igloohome-App der Box eine Zeitzone (Europe/Berlin) zuweisen.";
    case PIN_ERR_UNSUPPORTED: return "Diese Box unterstützt keine PIN-Codes.";
    case PIN_ERR_NETWORK: return "igloohome nicht erreichbar.";
    case PIN_ERR_OTHER: return "Unerwartete Antwort von igloohome.";
    case PIN_OK: break;
  }
  return "";
}
