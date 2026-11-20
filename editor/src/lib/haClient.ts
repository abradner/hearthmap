// Live entity fetch from a Home Assistant instance, straight from the browser.
//
// The long-lived access token is a parameter of one call and nothing else: it is not stored in
// a module variable, not written to localStorage, not put in the config, and never included in
// an error message or a log line. test/haClient.test.ts plants a token and checks every one of
// those places.

import { fromStates, type ParseResult } from "./entities";

/** `<base>/api/states` for a user-typed base URL; throws a user-facing Error when unusable. */
export function statesUrl(base: string): string {
  let url: URL;
  try {
    url = new URL(base.trim());
  } catch {
    throw new Error("enter the Home Assistant address, e.g. http://homeassistant.local:8123");
  }
  if (url.protocol !== "http:" && url.protocol !== "https:") throw new Error("the address must start with http:// or https://");
  if (url.username || url.password) throw new Error("do not put credentials in the address; use the token field");
  url.search = "";
  url.hash = "";
  url.pathname = `${url.pathname.replace(/\/+$/, "")}/api/states`;
  return url.toString();
}

export type FetchLike = (input: string, init: RequestInit) => Promise<Pick<Response, "ok" | "status" | "json">>;

/**
 * GET /api/states with a bearer token. Errors name the HTTP status or the kind of failure,
 * never the token. A browser-level failure is usually CORS (HA must list this page's origin in
 * `http: cors_allowed_origins`) or mixed content (an https page cannot call an http address).
 */
export async function fetchStates(base: string, token: string, fetchImpl: FetchLike = fetch): Promise<ParseResult> {
  const url = statesUrl(base);
  if (token.trim().length === 0) throw new Error("enter a long-lived access token");
  let res: Awaited<ReturnType<FetchLike>>;
  try {
    res = await fetchImpl(url, {
      method: "GET",
      headers: { Authorization: `Bearer ${token.trim()}` },
      credentials: "omit",
      cache: "no-store",
      referrerPolicy: "no-referrer",
    });
  } catch {
    throw new Error(
      "the browser could not reach Home Assistant: check the address, that HA allows this page's origin (http: cors_allowed_origins), and that an https page is not calling an http address",
    );
  }
  if (res.status === 401 || res.status === 403) throw new Error(`Home Assistant refused the token (HTTP ${res.status})`);
  if (!res.ok) throw new Error(`Home Assistant answered HTTP ${res.status}`);
  let body: unknown;
  try {
    body = await res.json();
  } catch {
    throw new Error("Home Assistant's answer was not JSON");
  }
  if (!Array.isArray(body)) throw new Error("Home Assistant's answer was not a list of states");
  return fromStates(body);
}
