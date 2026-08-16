export function outboundUrl(url: string | URL, proxyBase?: string | null) {
  const target = typeof url === "string" ? url : url.toString();
  const proxy = proxyBase?.trim().replace(/\/+$/u, "");
  if (!proxy) return target;
  const proxied = new URL(proxy);
  proxied.searchParams.set("url", target);
  return proxied.toString();
}
