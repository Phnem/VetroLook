#pragma once
// Vetro Look, GPL-3.0-or-later.
// The streaming vocabulary (9, Appendix G): what an address is, why a stream
// failed, when to try again, and what a page resolver said. Pure on purpose --
// no engine, no network, no window -- so every rule here is tested against
// literal inputs rather than against a server that happens to be up.
#include <string>

// What an address is, decided from its shape. "One URL input box does not mean
// one internal code path" (9.1).
enum class StreamSource{
 DirectMedia,    // a file on a web server: open it
 Hls,            // .m3u8
 Dash,           // .mpd
 WebPage,        // a page that has a film on it somewhere: needs a resolver
 Protected,      // a service that only ships DRM: no retry, a clear message
 Unsupported,    // a scheme or shape we will not hand to anything
};
StreamSource ClassifyAddress(const std::wstring& url);
const wchar_t* StreamSourceName(StreamSource source);
// The host of an address, lower case, without credentials or port.
std::wstring AddressHost(const std::wstring& url);

// Why a stream did not play. The categories are the ones Appendix G answers
// differently; the raw text stays in Diagnostics and is never the message.
enum class StreamFailure{
 None,
 Network,          // DNS, refused, reset, timed out, stream ended early
 NotFound,         // 404, 410
 Forbidden,        // 401, 403
 Certificate,      // TLS verification -- never silently ignored
 Protected,        // DRM
 SignInRequired,   // the page wants an account
 NoPublicStream,   // the resolver found nothing playable
 ResolverMissing,
 ResolverTimeout,
 ResolverCrashed,
 Unknown,
};
const wchar_t* StreamFailureName(StreamFailure failure);
// From the engine's warnings and its final error string, together.
StreamFailure ClassifyEngineFailure(const std::wstring& log);
// From the resolver's standard error and exit code.
StreamFailure ClassifyResolverFailure(const std::wstring& errors,unsigned long exitCode);
// Whether trying again could possibly help. A 404 does not come back.
bool FailureRetryable(StreamFailure failure);
// True when the engine's newest line is a final network give-up -- not one of
// its own retries. An engine reading HTTP through curl may report nothing else
// when the line goes: no error, no end of file, the playhead parked at the end.
bool EngineGaveUpOnNetwork(const std::wstring& log);

// Reconnect with backoff: 1, 2, 4, 8, 16 seconds, then every 30.
double ReconnectDelay(int attempt);
constexpr int ReconnectAttempts=8;          // once a stream has played
constexpr int FirstConnectAttempts=2;       // before it ever has: a typo is not an outage

// A stream that has played and then ended well short of its length did not end;
// the connection did.
bool EndedPrematurely(bool live,double position,double duration);

// What a resolver produced (Appendix T).
struct ResolvedStream{
 std::wstring title;
 std::wstring video;          // the address to play
 std::wstring audio;          // a separate audio address, when the site splits them
 std::wstring userAgent,referrer;
 bool live=false;
 double duration=0;
};
// Parses the resolver's tagged output. False when there is nothing to play.
bool ParseResolverOutput(const std::string& utf8,ResolvedStream& out);
// Only http and https reach the resolver, and only as a single argument that
// cannot be mistaken for an option or split into two (Appendix T, 67).
bool ResolverAddressSafe(const std::wstring& url);
