/**
 * Tenancy Gateway Worker
 *
 * Transparent reverse proxy for tenancy.localrun.ai. On any 5xx response
 * (or a network-level failure reaching the Cloudflare Tunnel), serves a
 * branded maintenance page instead of Cloudflare's generic bad-gateway screen.
 *
 * API paths (/ask*, /feedback*, /health*, /debug*) get a JSON error body so
 * XHR/fetch callers can handle it programmatically. All other paths get HTML.
 *
 * CORS: on error responses, the worker sets the same Access-Control-Allow-Origin
 * the origin server would have sent (configured via ALLOWED_ORIGIN env var, or
 * "*" by default). Without this the browser blocks the 503 from JS fetch().
 */

// Paths the server exposes - used to decide JSON vs HTML error body.
const API_PATH_RE = /^\/(ask|feedback|health|healthz|debug)\b/;

// ---------------------------------------------------------------------------
// Maintenance page HTML
// ---------------------------------------------------------------------------
const MAINTENANCE_HTML = `<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>NZ Tenancy Advisor - Back shortly</title>
  <style>
    *, *::before, *::after { box-sizing: border-box; margin: 0; padding: 0; }
    body {
      font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif;
      min-height: 100vh;
      display: flex;
      align-items: center;
      justify-content: center;
      background: #f1f5f9;
      color: #1e293b;
      padding: 2rem;
    }
    .card {
      max-width: 440px;
      width: 100%;
      background: #ffffff;
      border-radius: 14px;
      box-shadow: 0 1px 4px rgba(0,0,0,.08), 0 6px 24px rgba(0,0,0,.06);
      padding: 2.5rem 2.5rem 2rem;
      text-align: center;
    }
    .logo {
      width: 52px;
      height: 52px;
      margin: 0 auto 1.25rem;
      background: #1d4ed8;
      border-radius: 12px;
      display: flex;
      align-items: center;
      justify-content: center;
    }
    .logo svg { width: 28px; height: 28px; fill: #fff; }
    h1 {
      font-size: 1.35rem;
      font-weight: 700;
      color: #0f172a;
      margin-bottom: 0.6rem;
    }
    p {
      color: #475569;
      line-height: 1.65;
      margin-bottom: 0.4rem;
      font-size: 0.975rem;
    }
    .divider {
      border: none;
      border-top: 1px solid #e2e8f0;
      margin: 1.5rem 0 1rem;
    }
    .hint {
      font-size: 0.8rem;
      color: #94a3b8;
    }
  </style>
</head>
<body>
  <div class="card">
    <div class="logo">
      <!-- scales-of-justice SVG (single path, no Unicode) -->
      <svg viewBox="0 0 24 24" xmlns="http://www.w3.org/2000/svg" aria-hidden="true">
        <path d="M12 2a1 1 0 0 1 .894.553L15.382 7H19a1 1 0 1 1 0 2h-.382l1.276 3.553A3 3 0 0 1 17 16a3 3 0 0 1-2.894-3.447L15.382 9h-2.764l.276.553A1 1 0 0 1 12 11H3a1 1 0 0 1-.894-1.447L3.382 7H7a1 1 0 1 1 0-2h3.618L13.106 2.553A1 1 0 0 1 12 2ZM5.618 9 4.5 11.553A1 1 0 0 0 5.5 13h3a1 1 0 0 0 .9-1.447L8.382 9H5.618ZM12 22a1 1 0 0 1-1-1v-8a1 1 0 1 1 2 0v8a1 1 0 0 1-1 1Z"/>
      </svg>
    </div>
    <h1>NZ Tenancy Advisor</h1>
    <p>The service is briefly unavailable while we restart or deploy an update.</p>
    <p>Please try again in a minute or two.</p>
    <hr class="divider">
    <p class="hint">If the problem persists after a few minutes, please check back later.</p>
  </div>
</body>
</html>`;

// ---------------------------------------------------------------------------
// Worker entry point
// ---------------------------------------------------------------------------

export default {
  async fetch(request, env) {
    const allowedOrigin = env.ALLOWED_ORIGIN || "*";

    // Pass OPTIONS straight through - the origin handles CORS preflight.
    // If the origin is down, a 503 preflight response will cause the
    // subsequent POST to fail with a CORS error rather than our custom
    // message, but that's an acceptable edge case for maintenance mode.
    let response;
    try {
      response = await fetch(request);
    } catch {
      // Network-level failure: tunnel disconnected or DNS resolution error.
      return maintenanceResponse(request, allowedOrigin);
    }

    if (response.status >= 500) {
      return maintenanceResponse(request, allowedOrigin);
    }

    return response;
  },
};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

function isApiRequest(request) {
  return API_PATH_RE.test(new URL(request.url).pathname);
}

function corsHeaders(allowedOrigin, request) {
  // For the error response we need at minimum Allow-Origin so the browser
  // does not swallow the 503 with a CORS error before JS can read it.
  const headers = {
    "Access-Control-Allow-Origin": allowedOrigin,
    "Access-Control-Allow-Methods": "GET, POST, OPTIONS",
    "Access-Control-Allow-Headers": "Content-Type, X-API-Key, X-No-Log",
    "Retry-After": "30",
    "Cache-Control": "no-store",
  };
  // Echo Vary so downstream caches know the response differs by origin.
  if (allowedOrigin !== "*") {
    headers["Vary"] = "Origin";
  }
  return headers;
}

function maintenanceResponse(request, allowedOrigin) {
  const headers = corsHeaders(allowedOrigin, request);

  if (isApiRequest(request)) {
    return Response.json(
      {
        error: "service_unavailable",
        message: "The service is temporarily down. Please try again shortly.",
      },
      { status: 503, headers }
    );
  }

  return new Response(MAINTENANCE_HTML, {
    status: 503,
    headers: { ...headers, "Content-Type": "text/html; charset=utf-8" },
  });
}
