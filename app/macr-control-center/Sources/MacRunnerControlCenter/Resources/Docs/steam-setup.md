# Steam setup

MacRunner can read local Steam VDF manifests without a Steam Web API key. Real Steam CDN cover art and `steam://` launch URLs work without a key.

Steam Web API enrichment is opt-in. To enable owned-game counts and the Recently Played dashboard widget, get a free key from `steamcommunity.com/dev/apikey`, paste it into Settings > Stores, and set your SteamID64. Existing primary Steam accounts usually work immediately. New or limited Steam accounts may need Valve's account-unlock requirement, commonly the $5 spend wall, before a Web API key can be issued.

When a key is present, MacRunner calls Valve's Steam Web API over HTTPS to fetch owned games and recently played games. The key is stored in the macOS Keychain service `app.macrunner.steam` and is never written into reports.

Cover art uses unauthenticated Steam CDN images from cdn.cloudflare.steamstatic.com and shared.akamai.steamstatic.com. Downloaded JPEG bytes are cached under `~/Library/Caches/MacRunner/covers/steam/` and refreshed with `If-Modified-Since` when possible.
