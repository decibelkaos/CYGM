# Dexcom API Credentials

**Created**: 2026-01-19

---

## Your Dexcom Developer API Credentials

From your developer.dexcom.com account:

**Client ID**: `uVOjucTGg8cXmQqaVIrMRQcrl1XyUVYD`
**Client Secret**: `PKPSWmz0KghUEfXS`

**Redirect URI**: `http://localhost:8080/callback`

**Status**:
- Sandbox Access: ✅ Active
- Production Access: ❌ Not yet (apply for upgrade)

---

## Where to Put These Credentials

### File: `main/dexcom_api.h`

Replace the placeholder values (lines 12-13):

```c
// BEFORE (placeholders):
#define DEXCOM_CLIENT_ID     "REPLACE_WITH_YOUR_CLIENT_ID"
#define DEXCOM_CLIENT_SECRET "REPLACE_WITH_YOUR_CLIENT_SECRET"

// AFTER (your credentials):
#define DEXCOM_CLIENT_ID     "uVOjucTGg8cXmQqaVIrMRQcrl1XyUVYD"
#define DEXCOM_CLIENT_SECRET "PKPSWmz0KghUEfXS"
```

---

## Current API Mode: Sandbox

You're currently using **Sandbox** endpoints for testing:
- Auth: `https://sandbox-api.dexcom.com/v2/oauth2/login`
- Token: `https://sandbox-api.dexcom.com/v2/oauth2/token`
- Data: `https://sandbox-api.dexcom.com/v2/users/self/egvs`

**Sandbox Data**: Returns simulated glucose readings for testing

---

## Switching to Production

When you're ready for real glucose data:

1. Apply for production access on developer.dexcom.com
2. Wait for Dexcom approval
3. In `main/dexcom_api.h`:
   - Comment out the sandbox URLs (lines 15-17)
   - Uncomment the production URLs (lines 20-22)

```c
// Comment out sandbox:
// #define DEXCOM_AUTH_URL      "https://sandbox-api.dexcom.com/v2/oauth2/login"
// #define DEXCOM_TOKEN_URL     "https://sandbox-api.dexcom.com/v2/oauth2/token"
// #define DEXCOM_EGV_URL       "https://sandbox-api.dexcom.com/v2/users/self/egvs"

// Uncomment production:
#define DEXCOM_AUTH_URL      "https://api.dexcom.com/v2/oauth2/login"
#define DEXCOM_TOKEN_URL     "https://api.dexcom.com/v2/oauth2/token"
#define DEXCOM_EGV_URL       "https://api.dexcom.com/v2/users/self/egvs"
```

---

## Security Notes

⚠️ **DO NOT commit your credentials to a public repository!**

If you're using Git:
1. Keep `dexcom_api.h` with placeholders in the repository
2. Replace values locally after cloning
3. Or add `main/dexcom_api.h` to `.gitignore` and maintain a local copy

---

## OAuth2 Flow

When a user wants to connect their Dexcom account:

1. Device generates authorization URL with your Client ID
2. User opens URL on phone/computer
3. User logs in with their Dexcom credentials
4. Dexcom redirects to your redirect URI with auth code
5. Device exchanges auth code for access token (using Client Secret)
6. Access token used to fetch glucose data
7. Refresh token used when access token expires

**Your Client ID and Secret** identify your app to Dexcom's servers.
**User's Dexcom credentials** are never stored on the device - only OAuth tokens.

---

**Last Updated**: 2026-01-19
