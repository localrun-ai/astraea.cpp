// nz_tenancy — Phase 6A skeleton.
//
// Three handlers, no business logic yet — Phase 6B will swap the echo /
// synthetic-stream paths for real RAGPipeline + retrieve_anchor +
// Generator::generate_stream wiring.
//
//   GET  /health      → "ok\n"
//   POST /ask         → sanitize_question() + RAG + JSON answer + sources
//   POST /ask/stream  → sanitize_question() + RAG + SSE token stream
//                       (one "event: sources" frame with {sources, guidance_source}
//                        upfront, then "data: {\"token\":\"...\"}" per token,
//                        then "data: [DONE]\n\n")
//
// Config comes from astraea::Config::from_env() — see include/astraea/config.hpp
// for the env-var names. The only one this handler reads in Phase 6A is PORT.
//
// Test from another terminal:
//
//   curl http://localhost:8080/health
//   curl -X POST http://localhost:8080/ask -H 'Content-Type: application/json'
//        -d '{"question":"what is the bond limit?"}'
//   curl -N -X POST http://localhost:8080/ask/stream -H 'Content-Type: application/json'
//        -d '{"question":"what is the bond limit?"}'

#include <drogon/drogon.h>
#include <drogon/HttpResponse.h>
#include <mimalloc.h>
#include <glaze/glaze.hpp>
#include <spdlog/spdlog.h>

#include "astraea/anchor.hpp"
#include "astraea/config.hpp"
#include "astraea/feedback.hpp"
#include "astraea/timing.hpp"
#include "astraea/coordinator.hpp"
#include "astraea/detail/when_all.hpp"
#include "astraea/generator.hpp"
#include "astraea/detail/llm_tcp_pool.hpp"
#include "astraea/health.hpp"
#include "astraea/in_process_coordinator.hpp"
#include "astraea/pipeline.hpp"
#include "astraea/redis_coordinator.hpp"
#include "astraea/request_id.hpp"
#include "astraea/route_table.hpp"
#include "astraea/route_validator.hpp"
#include "astraea/session.hpp"
#include "astraea/retriever.hpp"
#include "astraea/sanitize.hpp"
#include "nz_tenancy/jurisdiction.hpp"
#include "mcp_handler.hpp"
#include "routes_hash.hpp"

#include <openssl/crypto.h>

#include <atomic>
#include <cassert>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// ---------------------------------------------------------------------------
// JSON shapes — named namespace (NOT anonymous) for glaze reflection.
// Anonymous-namespace types have internal linkage and break glaze's
// glz::detail::external<T>. Pattern established in PR #9.
// ---------------------------------------------------------------------------

namespace astraea::detail::nz_tenancy_app {

/// @brief Deserialized body of a POST /ask or POST /ask/stream request.
///
/// Eval-only overrides; accepted only when debug_key matches DEBUG_KEY env var.
struct EvalOptions {
    float temperature = -1.0f; ///< Override generation temperature. Allowed set: 0.0, 0.1, 0.2. -1 = use server default.
};

/// Python parity: core/api.py AskRequest.
struct AskRequest {
    std::string question; ///< Raw user question; sanitized by parse_and_sanitize before use.
    std::string mode; ///< Cheat-code mode: "search", "case", "checklist", "landlord", "pitfalls", or empty.
    std::string strategy; ///< Retrieval strategy: "vector" (default) or "mmr".
    std::string session_id; ///< Optional session ID for multi-turn context; also accepted via X-Session-Id header.
    std::string user_context; ///< Optional personal context from frontend localStorage; prepended to the anchor block (capped at 500 chars).
    /// Debug mode key. When non-empty and equal to DEBUG_KEY env var,
    /// enables debug/context_debug/debug_done SSE events and eval_options.
    std::string debug_key;
    /// When true, emit context_debug without requiring debug_key.
    bool feedback_context = false;
    /// Eval-only overrides; ignored unless debug_key is valid.
    EvalOptions eval_options;
};

/// Allowed eval temperature values. Requests outside this set are rejected.
static constexpr std::array<float,3> EVAL_TEMP_ALLOWED = {0.0f, 0.1f, 0.2f};

/// @brief Validated and sanitized output of parse_and_sanitize().
///
/// Groups the parsed fields so the function signature does not need to grow
/// every time AskRequest adds a new field.
struct ParsedAskRequest {
    std::string question; ///< Sanitized question text; safe to pass to retrieval and the LLM.
    std::string mode; ///< Forwarded from AskRequest.
    std::string strategy; ///< Forwarded from AskRequest.
    std::string session_id; ///< Forwarded from AskRequest.
    std::string user_context; ///< Forwarded from AskRequest.
    std::string debug_key; ///< Forwarded from AskRequest.
    bool        feedback_context = false; ///< Forwarded from AskRequest.
    float       eval_temperature = -1.0f; ///< Validated eval temp override; -1 = use server default.
};

/// @brief Non-streaming /ask response token field (minimal wrapper).
struct TokenResp {
    std::string token; ///< Token text; not used in the SSE path.
};

/// @brief SSE "data: {type:token, text:...}" frame emitted per LLM token.
struct TokenChunk {
    std::string type = "token"; ///< Always "token".
    std::string text; ///< Decoded token text from choices[0].delta.content.
};

/// @brief A single source citation rendered in API responses.
///
/// Shape matches Python core/pipeline.py public_sources exactly:
/// case_id, court_name, date, url. No score or label. The Qdrant internal
/// UUID is never sent to clients; case_id is the human-readable case reference
/// from the payload (e.g. "NZTT/2024/4757355").
struct SourceJson {
    std::string case_id;    ///< Case reference from the Qdrant payload (e.g. "NZTT/2024/4757355"), not the internal UUID.
    std::string court_name; ///< Court or tribunal name, used by renderSources to build the link label.
    std::string date;       ///< Decision date string, used by renderSources to build the link label.
    std::string url;        ///< Public NZLII URL; renderSources falls back to "#" if not https://.
};

} // namespace astraea::detail::nz_tenancy_app

template <>
struct glz::meta<astraea::detail::nz_tenancy_app::SourceJson> {
    using T = astraea::detail::nz_tenancy_app::SourceJson;
    static constexpr auto value = object(
        "case_id",    &T::case_id,
        "court_name", &T::court_name,
        "date",       &T::date,
        "url",        &T::url
    );
};

namespace astraea::detail::nz_tenancy_app {

/// @brief Non-streaming /ask JSON response body.
struct AskResponse {
    std::string             answer; ///< Full LLM-generated answer text.
    std::vector<SourceJson> sources; ///< Retrieved source citations parallel to the context chunks.
    std::optional<SourceJson> guidance_source; ///< MANUAL guidance chunk injected; nullopt when no guidance was used.
};

/// @brief Legislation source rendered in the SSE sources event.
struct LegSourceJson {
    std::string case_id; ///< Legislation chunk ID, e.g. "NZLEG/RTA/s18".
    std::string title; ///< Human-readable section title from the payload.
    std::string url; ///< Public URL to the legislation section.
};

/// @brief SSE "data: {type:sources,...}" event emitted before the first LLM token.
///
/// Allows the client to render citation chips while generation is in progress.
struct SourcesEvent {
    std::string type = "sources"; ///< Always "sources".
    std::vector<SourceJson>    sources; ///< Case-law citations for this response.
    std::vector<LegSourceJson> legislation; ///< Legislation anchor sections for this response.
    std::optional<SourceJson>  guidance_source; ///< MANUAL guidance chunk; absent when nothing was injected.
};

/// @brief One legislation section in the SSE verification event.
struct VerificationSection {
    std::string url; ///< Public URL to the Act section.
    std::string reference; ///< Human-readable section reference, e.g. "s 18 RTA".
    std::string excerpt; ///< Short excerpt from the retrieved legislation text.
};

/// @brief SSE "data: {type:verification,...}" event surfacing the retrieved legislation text.
///
/// Replaces the Playwright-based web_verify step; the legislation text already
/// retrieved from Qdrant is surfaced so the user can verify the answer.
struct VerificationEvent {
    std::string type = "verification"; ///< Always "verification".
    std::vector<VerificationSection> sections; ///< One entry per retrieved legislation chunk.
};

/// @brief SSE "data: {type:confidence,...}" event emitted after the sources event.
///
/// Lets the frontend render a confidence indicator alongside the answer.
/// Verbatim port of Python core/api.py _confidence() output shape.
struct ConfidenceEvent {
    std::string type = "confidence"; ///< Always "confidence".
    std::string level; ///< "high", "medium", or "low".
    int         chunks = 0; ///< Total case-law chunks retrieved.
    std::string message; ///< Human-readable message for display, from ConfidenceConfig::messages.
};

/// @brief SSE "data: {type:queue,...}" event emitted when the LLM permit is already held.
///
/// Allows the frontend to show a "queued" indicator instead of appearing hung.
/// Python parity: core/api.py line 557. position is always 1 (queue depth not tracked).
struct QueueEvent {
    std::string type             = "queue"; ///< Always "queue".
    int         position         = 1; ///< Always 1; queue depth tracking is not implemented.
    std::string reason           = "llm_busy"; ///< Always "llm_busy".
    double      estimated_wait_s = 25.0; ///< Mirrors Python's _AVG_QUERY_SECONDS = 25.
    std::string message          = "Another query is generating - queued."; ///< User-facing message.
};

/// @brief SSE "data: {type:debug,...}" event emitted in debug mode after the confidence event.
///
/// Reports retrieval strategy, timing, and chunk scores. Python parity: core/api.py line 475.
struct DebugEvent {
    std::string        type        = "debug"; ///< Always "debug".
    std::string        strategy; ///< Retrieval strategy used: "vector" or "mmr".
    double             retrieve_ms = 0.0; ///< Wall time of the parallel retrieve+anchor tasks.
    std::vector<float> scores; ///< Similarity scores of all retrieved chunks, parallel to sources.
    int                chunks      = 0; ///< Total case-law chunks retrieved.
    bool               refine_used = false; ///< True when the low-confidence second retrieval pass ran.
};

/// @brief SSE "data: {type:debug_done,...}" event emitted just before "done" in debug mode.
///
/// Python parity: core/api.py line 587.
struct DebugDoneEvent {
    std::string type         = "debug_done"; ///< Always "debug_done".
    double      generate_ms  = 0.0; ///< Wall time of the LLM generation phase.
    double      total_ms     = 0.0; ///< Total wall time from sanitize to [DONE].
};

/// @brief Routing decision entry in the context_debug event for an ignored route.
struct RoutingIgnored {
    std::string route; ///< Intent name of the suppressed route.
    std::string reason; ///< Why it was ignored, e.g. "exclude_any hit: [term]".
};

/// @brief Routing decision entry for a route that nearly fired but lacked context terms.
struct RoutingNearMiss {
    std::string              route; ///< Intent name of the near-miss route.
    std::vector<std::string> broad_matched; ///< Broad terms that matched but lacked require_context_any.
};

/// @brief Routing decision summary emitted in the context_debug SSE event.
///
/// Python parity: core/api.py lines 479-543.
struct RoutingEvent {
    bool                                    triggered         = false; ///< True when at least one route fired.
    std::vector<std::string>                matched_routes; ///< Intents of all fired routes.
    std::vector<std::string>                trigger_terms; ///< Terms that caused each route to fire.
    std::map<std::string, std::string>      trigger_paths; ///< intent -> "precise", "broad+context", or "legacy".
    std::vector<std::string>                forced_sections; ///< Union of forced_sections from all fired routes.
    std::string                             dominant_route; ///< Highest-priority intent; empty when no route fired.
    std::string                             dominance_reason; ///< Why this route was selected as dominant.
    std::vector<RoutingIgnored>             ignored_routes; ///< Routes suppressed by exclude_any.
    std::vector<RoutingNearMiss>            near_miss_routes; ///< Routes that nearly fired.
};

/// @brief One legislation anchor section in the context_debug event.
struct AnchorSection {
    std::string document_id; ///< Qdrant point ID of this legislation chunk.
    std::string title; ///< Section title from the Qdrant payload.
    int         tokens  = 0; ///< Approximate token count (chars / 4).
    std::string preview; ///< First 200 chars of the section text.
};

/// @brief Anchor retrieval summary in the context_debug event.
struct AnchorDebug {
    std::string                  method        = "vector"; ///< Always "vector" in the current implementation.
    bool                         route_matched = false; ///< True when a StatuteRoute fired; false means degraded flat top-k.
    int                          max_hits      = 0; ///< Cap applied to legislation results (compute_max_hits output).
    std::vector<AnchorSection>   sections; ///< One entry per injected legislation chunk.
};

/// @brief Guidance injection summary in the context_debug event.
struct GuidanceDebug {
    bool                     injected   = false; ///< True when a MANUAL chunk was injected.
    std::optional<std::string> source; ///< Qdrant point ID of the injected chunk; absent when not injected.
    std::optional<std::string> court_name; ///< court_name payload field; absent when not injected.
    std::optional<float>     score; ///< Vector similarity score of the injected chunk; absent when not injected.
    float                    threshold  = 0.75f; ///< Score threshold used; mirrors GUIDANCE_THRESHOLD.
    std::string              reason; ///< Injection path: "score_above_threshold" or "not_injected".
};

/// @brief One corpus chunk card in the context_debug event.
struct ChunkCard {
    int         source_index = 0; ///< Zero-based index into the sources list.
    float       score        = 0.0f; ///< Similarity score from Qdrant.
    bool        passed_gate  = true; ///< True when this chunk passed the min_score gate.
    std::string document_id; ///< Qdrant point ID.
    std::string date; ///< Decision date from the Qdrant payload.
    int         tokens       = 0; ///< Approximate token count (chars / 4).
    std::string preview; ///< First 200 chars of the chunk text.
    std::string full_text; ///< Complete chunk text as stored in the payload.
};

/// @brief Token and chunk budget summary in the context_debug event.
struct ContextBudget {
    int total_tokens    = 0; ///< Approximate total tokens in the assembled user message (chars / 4).
    int ctx_limit       = 8192; ///< Configured context window limit (informational; not enforced here).
    int anchor_tokens   = 0; ///< Approximate tokens from legislation anchor chunks.
    int chunk_tokens    = 0; ///< Approximate tokens from case-law corpus chunks.
    int sources_sent    = 0; ///< Number of case-law sources included in the LLM prompt.
    int truncated_chunks = 0; ///< Number of chunks dropped due to budget; always 0 in the current implementation.
};

/// @brief SSE "data: {type:context_debug,...}" event with the full retrieval context.
///
/// Emitted when debug_mode OR req.feedback_context. Provides the feedback
/// capture UI and debug panel with a complete view of what the LLM saw.
/// Python parity: core/api.py lines 479-543.
struct ContextDebugEvent {
    std::string               type             = "context_debug"; ///< Always "context_debug".
    std::string               original_query; ///< The raw sanitized question before any rewriting.
    std::string               rewrite_input; ///< The question after zone-prefix stripping, before the rewrite LLM call.
    std::string               rewritten_query; ///< The LLM-rewritten query; equals original_query when rewrite was skipped.
    bool                      rewrite_used     = false; ///< True when the rewritten query differs from the rewrite input.
    RoutingEvent              statute_routing; ///< Full routing decision for this request.
    AnchorDebug               anchor; ///< Legislation anchor retrieval summary.
    GuidanceDebug             guidance; ///< Guidance injection summary.
    std::vector<ChunkCard>    chunks; ///< One card per corpus chunk, in retrieval order.
    ContextBudget             budget; ///< Token and chunk budget summary.
};

/// @brief /healthz per-dependency check in the JSON response.
///
/// Mapped from astraea::HealthCheck so the wire shape can be controlled
/// independently (e.g. omit the error field when status is "ok").
struct HealthCheckJson {
    std::string                name; ///< Dependency name: "qdrant", "llm", "embed", or "rerank".
    std::string                status; ///< "ok" or "down".
    int                        latency_ms; ///< Round-trip time of the probe in milliseconds.
    std::string                url; ///< The probed URL.
    std::optional<std::string> error; ///< Error message; absent when status is "ok".
};
/// @brief /healthz coordinator backend info in the JSON response.
struct CoordinatorInfoJson {
    std::string backend; ///< Backend identifier, e.g. "in_process" or "redis".
    int         max_concurrency; ///< Configured permit count.
};
/// @brief /healthz LLM TCP pool info in the JSON response.
struct LlmPoolInfoJson {
    std::size_t idle_clients = 0; ///< Total idle pooled trantor::TcpClient instances across all (loop, endpoint) sub-pools.
};
/// @brief /healthz JSON response body.
struct HealthzResponse {
    std::string                          status; ///< "ok" when all checks pass; "down" when any fails.
    std::string                          routes_hash; ///< 8-char SHA1 prefix of routes.cpp, matches eval_judge.py cache key.
    std::vector<HealthCheckJson>         checks; ///< Per-dependency probe results.
    std::optional<CoordinatorInfoJson>   coordinator; ///< Coordinator info; absent when not configured.
    std::optional<LlmPoolInfoJson>       llm_pool; ///< LLM pool info; absent when not configured.
};

/// @brief /feedback POST request body.
struct FeedbackRequest {
    std::string question; ///< The question the user is rating.
    int         rating   = 0; ///< User rating (e.g. 1-5 or thumbs); interpretation left to the frontend.
    std::string comment; ///< Optional free-text comment.
};

/// @brief One question_log.jsonl entry written per real question (X-No-Log absent).
struct QuestionLogEntry {
    std::string ts; ///< ISO-8601 UTC timestamp.
    std::string request_id; ///< X-Request-Id for correlation.
    std::string q; ///< The sanitized question text.
};

/// @brief Compact source shape for route_debug.jsonl log entries.
struct LogSource {
    std::string id; ///< Qdrant point ID.
    float       score = 0.0f; ///< Similarity score.
};
/// @brief Compact legislation shape for route_debug.jsonl log entries.
struct LogLegSource {
    std::string id; ///< Qdrant point ID.
    std::string title; ///< Section title from the payload.
};

/// @brief One route_debug.jsonl entry written per real question.
///
/// Records routing, retrieval, and answer metadata so operators can diagnose
/// retrieval quality and prompt size issues without the full SSE debug event.
struct RouteDebugEntry {
    std::string              ts; ///< ISO-8601 UTC timestamp.
    std::string              request_id; ///< X-Request-Id for correlation.
    std::string              q; ///< Sanitized question.
    std::string              rewritten; ///< LLM-rewritten query; empty when same as q.
    bool                     triggered = false; ///< True when at least one route fired.
    std::vector<std::string> matched_intents; ///< Fired route intents.
    std::vector<LogSource>   sources; ///< Retrieved case-law sources.
    std::vector<LogLegSource> legislation; ///< Retrieved legislation sources.
    std::string              answer; ///< LLM answer truncated to 8000 chars.
    int                      context_chars  = 0; ///< Char count of the assembled user message.
    int                      context_chunks = 0; ///< Total chunks (cases + legislation + guidance) in the prompt.
    std::string              mode; ///< Request mode flag; empty for the default path.
    std::string              strategy; ///< Retrieval strategy: "" (default/vector), "vector", or "mmr".
};

/// @brief One feedback.jsonl entry written per /feedback POST.
struct FeedbackEntry {
    std::string ts; ///< ISO-8601 UTC timestamp.
    std::string request_id; ///< X-Request-Id for correlation.
    std::string question; ///< The question that was rated.
    int         rating  = 0; ///< User rating.
    std::string comment; ///< Optional free-text comment.
};

#ifdef ASTRAEA_ENABLE_TIMING
/// @brief SSE "data: {type:timing,...}" event emitted at the end of /ask/stream.
///
/// Named slots mirror Python core/timing.py's top-level aggregates.
/// `detail` carries all raw TimingStep records for client-side drill-down.
/// Only compiled when ASTRAEA_ENABLE_TIMING is defined.
struct TimingEvent {
    std::string              type           = "timing"; ///< Always "timing".
    std::string              request_id; ///< X-Request-Id for correlation.
    double                   sanitize_ms    = 0.0; ///< Time spent in sanitize_question.
    double                   rewrite_ms     = 0.0; ///< Time spent in the LLM query rewrite call.
    double                   embed_ms       = 0.0; ///< Time spent embedding the retrieval question.
    double                   retrieve_ms    = 0.0; ///< Wall time of the parallel retrieve+anchor fanout.
    double                   anchor_ms      = 0.0; ///< Wall time of retrieve_anchor specifically.
    double                   guidance_ms    = 0.0; ///< Time spent in retrieve_manual_guidance.
    double                   context_ms     = 0.0; ///< Time spent assembling the LLM messages.
    double                   llm_wait_ms    = 0.0; ///< Time spent waiting for the global LLM permit.
    double                   ttft_ms        = 0.0; ///< Time to first token from the LLM.
    double                   generation_ms  = 0.0; ///< Total LLM generation time (ttft + remaining tokens).
    double                   total_ms       = 0.0; ///< Total wall time from sanitize to [DONE].
    int                      context_chars  = 0; ///< Total chars in the assembled user message.
    int                      context_chunks = 0; ///< Total chunks (cases + legislation + guidance) in the prompt.
    float                    generation_temperature = -1.0f; ///< Effective generation temperature (-1 = server default).
    std::vector<astraea::TimingStep> detail; ///< All raw step records for fine-grained drill-down.
};
#endif // ASTRAEA_ENABLE_TIMING

/// @brief GET /health JSON response body.
///
/// Named namespace required for glaze external<T> linkage (anonymous-namespace
/// types have internal linkage; see PR #9).
struct HealthResponse {
    std::string status       = "ok"; ///< Always "ok" for the liveness probe.
    std::string jurisdiction; ///< The jurisdiction name, e.g. "nz_tenancy".
};

} // namespace astraea::detail::nz_tenancy_app

namespace {

using namespace astraea::detail::nz_tenancy_app;

// ---------------------------------------------------------------------------
// Startup verification: confirm mimalloc is actually overriding the global
// allocator. The override comes from mimalloc_override.cpp in this same
// executable — if that TU is not linked, every `new` falls through to glibc
// and mi_is_in_heap_region() returns false.
//
// Skipped under ASan because ASan installs its own allocator interceptors
// that take precedence over mimalloc's; the probe would always fail and
// the dev preset would refuse to start. Production binaries (Release)
// are the only ones that actually need (and get) the override.
// ---------------------------------------------------------------------------

#if defined(__has_feature)
#  if __has_feature(address_sanitizer)
#    define ASTRAEA_ASAN_ACTIVE 1
#  endif
#endif

void verify_mimalloc_override() {
#ifdef ASTRAEA_ASAN_ACTIVE
    LOG_WARN << "ASan build: skipping mimalloc override verification "
                "(sanitiser interceptors take precedence over mimalloc)";
    return;
#else
    auto* probe = new int(42);
    const bool overridden = mi_is_in_heap_region(probe);
    delete probe;
    if (!overridden) {
        std::fprintf(stderr,
            "FATAL: mimalloc allocator override is not active. "
            "Check that apps/nz_tenancy/mimalloc_override.cpp is linked "
            "into this executable.\n");
        std::abort();
    }
    LOG_INFO << "mimalloc v" << mi_version() << " allocator override active";
#endif
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// ISO-8601 UTC timestamp matching Python's datetime.now(timezone.utc).isoformat()
// format: "2026-06-13T10:00:00.123456+00:00".
std::string utc_iso8601() {
    using namespace std::chrono;
    const auto now = system_clock::now();
    const auto us  = duration_cast<microseconds>(now.time_since_epoch()) % 1'000'000;
    const std::time_t t = system_clock::to_time_t(now);
    std::tm tm_buf{};
    gmtime_r(&t, &tm_buf);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm_buf);
    char result[48];
    std::snprintf(result, sizeof(result), "%s.%06lld+00:00", buf,
                  static_cast<long long>(us.count()));
    return result;
}

bool is_no_log(const drogon::HttpRequestPtr& req) {
    return req->getHeader("x-no-log") == "1";
}

// ---------------------------------------------------------------------------
// Response builders
// ---------------------------------------------------------------------------

drogon::HttpResponsePtr text_response(drogon::HttpStatusCode code, std::string body) {
    auto resp = drogon::HttpResponse::newHttpResponse();
    resp->setStatusCode(code);
    resp->setBody(std::move(body));
    resp->setContentTypeCode(drogon::CT_TEXT_PLAIN);
    return resp;
}

drogon::HttpResponsePtr json_response(drogon::HttpStatusCode code, std::string body) {
    auto resp = drogon::HttpResponse::newHttpResponse();
    resp->setStatusCode(code);
    resp->setBody(std::move(body));
    resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
    return resp;
}

// Per-IP concurrent request limiter. Prevents a single client from
// monopolising all LLM capacity under IP_MAX_CONCURRENCY > 0.
//
// Note: getPeerAddr().toIp() returns the direct peer address. Behind a
// Cloudflare Tunnel the peer is the tunnel edge IP, not the end-user's IP.
// For true per-client limiting in a tunnelled deployment, read the
// CF-Connecting-IP or X-Forwarded-For header instead, and set
// IP_MAX_CONCURRENCY=0 to disable this guard (use the proxy-level equivalent).
struct IpLimiter {
    explicit IpLimiter(int max) noexcept : _max(max) {}

    struct Permit {
        IpLimiter*  _lim = nullptr;
        std::string _ip;
        Permit() = default;
        Permit(IpLimiter* lim, std::string ip) : _lim(lim), _ip(std::move(ip)) {}
        Permit(Permit&& o) noexcept : _lim(o._lim), _ip(std::move(o._ip)) { o._lim = nullptr; }
        Permit& operator=(Permit&& o) noexcept {
            if (this != &o) { reset(); _lim = o._lim; _ip = std::move(o._ip); o._lim = nullptr; }
            return *this;
        }
        ~Permit() { reset(); }
        void reset() noexcept { if (_lim) { _lim->release(_ip); _lim = nullptr; } }
        Permit(const Permit&) = delete;
        Permit& operator=(const Permit&) = delete;
    };

    // Returns a Permit on success; nullopt if this IP is already at the limit.
    std::optional<Permit> try_acquire(const std::string& ip) {
        std::lock_guard<std::mutex> lock(_mu);
        auto& count = _counts[ip];
        if (count >= _max) return std::nullopt;
        ++count;
        return Permit{this, ip};
    }

    void release(const std::string& ip) noexcept {
        std::lock_guard<std::mutex> lock(_mu);
        auto it = _counts.find(ip);
        if (it != _counts.end() && --it->second == 0)
            _counts.erase(it);
    }

private:
    int                                  _max;
    std::mutex                           _mu;
    std::unordered_map<std::string, int> _counts;
};

// Parse + sanitise. On success populates `out` and returns nullptr; on any
// JSON-parse or sanitize failure returns a built error response (and `out`
// is left empty). Caller forwards the response directly to the Drogon
// callback when non-null.

drogon::HttpResponsePtr parse_and_sanitize(const drogon::HttpRequestPtr& req,
                                            ParsedAskRequest& out,
                                            int max_question_chars,
                                            const std::string& cfg_debug_key) {
    AskRequest parsed{};
    if (auto err = glz::read<glz::opts{.error_on_unknown_keys = false}>(parsed, req->getBody()); err) {
        return text_response(drogon::k400BadRequest, "Invalid JSON\n");
    }
    try {
        out.question = astraea::sanitize_question(parsed.question, max_question_chars);
    } catch (const astraea::SanitizeError& e) {
        return text_response(
            static_cast<drogon::HttpStatusCode>(e.http_status),
            std::string{e.what()} + "\n");
    }
    out.mode             = std::move(parsed.mode);
    out.strategy         = std::move(parsed.strategy);
    out.session_id       = std::move(parsed.session_id);
    out.user_context     = std::move(parsed.user_context);
    out.debug_key        = std::move(parsed.debug_key);
    out.feedback_context = parsed.feedback_context;

    // eval_options.temperature: only honored when debug_key is valid.
    if (!cfg_debug_key.empty() && out.debug_key == cfg_debug_key) {
        const float req_temp = parsed.eval_options.temperature;
        if (req_temp >= 0.0f) {
            bool allowed = false;
            for (float v : EVAL_TEMP_ALLOWED) {
                if (std::abs(req_temp - v) < 1e-4f) { allowed = true; break; }
            }
            if (!allowed) {
                return text_response(drogon::k400BadRequest,
                    "eval_options.temperature must be one of 0.0, 0.1, 0.2\n");
            }
            out.eval_temperature = req_temp;
        }
    }

    return nullptr;  // success
}

// ---------------------------------------------------------------------------
// Handlers
// ---------------------------------------------------------------------------

// GET /health - liveness probe. Python parity: returns JSON with status +
// jurisdiction name. Does NOT ping any upstream (that's /healthz).
// Python core/api.py also includes queue_status() fields; we omit them
// since the in_process coordinator doesn't expose a waiting count.
// HealthResponse lives in the named namespace (astraea::detail::nz_tenancy_app)
// so glaze external<T> reflection can link it - see PR #9.
drogon::HttpResponsePtr health_handler(std::string_view jurisdiction_name) {
    std::string body;
    HealthResponse hr;
    hr.jurisdiction = jurisdiction_name;
    if (auto e = glz::write_json(hr, body); e)
        return text_response(drogon::k200OK, "ok\n");
    return json_response(drogon::k200OK, std::move(body));
}

// Deep readiness probe. Pings every upstream the binary needs (Qdrant + LLM
// chat + embed + optional rerank) and surfaces the coordinator backend info.
// Returns 200 when overall=="ok", 503 otherwise - shape matches k8s
// readinessProbe expectations.
drogon::Task<drogon::HttpResponsePtr> healthz_handler(
    astraea::HealthProber&            prober,
    const astraea::CoordinatorClient* coordinator_or_null,
    const astraea::detail::LlmTcpPool* llm_pool_or_null)
{
    astraea::HealthReport rep = co_await prober.probe();

    HealthzResponse out;
    out.status      = rep.overall;
    out.routes_hash = astraea::kRoutesHash;
    out.checks.reserve(rep.checks.size());
    for (const auto& c : rep.checks) {
        out.checks.push_back({
            c.name, c.status, c.latency_ms, c.url, c.error,
        });
    }
    if (coordinator_or_null) {
        out.coordinator = CoordinatorInfoJson{
            coordinator_or_null->backend_name(),
            coordinator_or_null->max_concurrency(),
        };
    }
    if (llm_pool_or_null) {
        out.llm_pool = LlmPoolInfoJson{llm_pool_or_null->size()};
    }

    std::string body;
    if (auto e = glz::write_json(out, body); e) {
        co_return text_response(drogon::k500InternalServerError,
                                "healthz: serialization failed\n");
    }
    const auto code = (out.status == "ok")
        ? drogon::k200OK
        : drogon::k503ServiceUnavailable;
    co_return json_response(code, std::move(body));
}

// ---------------------------------------------------------------------------
// RAG assembly: shared by /ask and (in 6C.3) /ask/stream.
// retrieve -> retrieve_anchor -> retrieve_manual_guidance -> build messages.
// ---------------------------------------------------------------------------

struct AssembledRequest {
    std::vector<astraea::ChatMessage>   messages;
    std::vector<astraea::QdrantPoint>   sources;
    std::optional<astraea::QdrantPoint> guidance_source;
    // Logging fields - populated by assemble_request, written by the handler.
    std::string                         rewritten_q;     // empty when same as original
    std::vector<astraea::QdrantPoint>   leg_sources;     // from anchor
    std::vector<std::string>            matched_intents;
    bool                                route_triggered  = false;
    bool                                anchor_route_matched = false; // AnchorResult::route_matched
    int                                 anchor_max_hits  = 0;        // AnchorResult::max_hits
    // Prompt-size accounting. Populated at the end of assemble_request so
    // the timing event can correlate slow TTFT / generation against context
    // bloat. context_chars is the size of the assembled user message (incl.
    // the trailing Question:); context_chunks is the total chunk count fed
    // to the LLM (cases + legislation + guidance). See BENCHMARK_PERF.md.
    int                                 context_chars   = 0;
    int                                 context_chunks  = 0;
    // Request flags echoed from the parsed request so handlers can log them
    // in RouteDebugEntry without needing a separate copy before the move.
    std::string                         mode;
    std::string                         strategy;       // "" | "vector" | "mmr"
    // Full routing decision. Carried here so the context_debug SSE event
    // can emit trigger_terms, trigger_paths, ignored/near-miss routes without
    // re-running build_route_decision in the handler.
    astraea::RouteDecision              route_dec;
    // Stripped question (before LLM rewrite). Needed for context_debug's
    // rewrite_input field - the handler only sees the assembled messages.
    std::string                         rewrite_input;
    // True when the confidence-gated second retrieval pass (refine_retrieve)
    // ran. Reported in the debug SSE event so operators can identify
    // queries that needed a retry.
    bool                                refine_used     = false;
    // Per-chunk text payloads, parallel to sources[]. Stored separately so
    // the context_debug event can emit preview + full_text per chunk without
    // re-extracting from the assembled user message.
    std::vector<std::string>            chunk_texts;
    // Guidance injection tracking. guidance_injected=true means the guidance
    // source was NOT already in the corpus hits (new information for the LLM).
    bool                                guidance_injected = false;
    // Retrieval confidence summary, populated at end of assemble_request.
    // Mirrors Python's _confidence() shape so the SSE 'confidence' event
    // and any future JSON-response embedding can read directly.
    std::string                         confidence_level;     // "high" | "medium" | "low"
    int                                 confidence_chunks = 0;
    std::string                         confidence_message;
    // Timing instrumentation. No-op when ASTRAEA_ENABLE_TIMING is not defined.
    astraea::TimingCollector            timer;
};

std::string build_context_block(
    const astraea::RetrieveResult&            retrieved,
    const astraea::AnchorResult&              anchor,
    const astraea::GuidanceResult&            guidance)
{
    // RetrieveResult contract: texts[i] is the payload field of sources[i].
    // Belt-and-braces - silent OOB if a future retriever change diverges them.
    assert(retrieved.sources.size() == retrieved.texts.size());

    std::string ctx;
    if (!retrieved.sources.empty()) {
        ctx += "Relevant case decisions:\n";
        for (std::size_t i = 0; i < retrieved.sources.size(); ++i) {
            ctx += "\n[S" + std::to_string(i + 1) + "] " + retrieved.texts[i] + "\n";
        }
    }
    if (!anchor.anchor_text.empty()) {
        ctx += "\n\n" + anchor.anchor_text;
    }
    if (!guidance.text.empty()) {
        ctx += "\n\nGuidance:\n" + guidance.text;
    }
    return ctx;
}

// Strip leading "[Key: value]\n" prefix lines added by jurisdiction.preprocess_question()
// before the rewrite/retrieval step. Verbatim port of core/api.py:_strip_context_prefixes
// (which uses regex r"^(\[[^\]]+\]\s*\n+)+"). Hand-rolled here to avoid pulling RE2
// into this TU - input is short and the pattern is simple.
//
// Zone prefixes bias vector retrieval toward planning/RMA sections instead of building
// law. The full prefixed question is still sent to the LLM for generation - this strip
// only affects the rewrite + retrieval path.
std::string strip_context_prefixes(std::string s) {
    std::size_t pos = 0;
    while (pos < s.size() && s[pos] == '[') {
        const auto end = s.find(']', pos + 1);
        if (end == std::string::npos) break;
        auto after = end + 1;
        // Skip horizontal whitespace, then require at least one newline.
        while (after < s.size() && (s[after] == ' ' || s[after] == '\t')) ++after;
        if (after >= s.size() || s[after] != '\n') break;
        while (after < s.size() && s[after] == '\n') ++after;
        pos = after;
    }
    return pos > 0 ? s.substr(pos) : std::move(s);
}

// Cheat-code mode prefixes. Each prefix is prepended to the user's question
// in the LLM generation prompt only - retrieval / anchor / guidance still
// run against the clean question, so the corpus result quality is unaffected
// by the mode choice.
//
// Verbatim port of core/api.py:_MODES. Unknown / empty mode returns an
// empty view (no prefix applied) - silent ignore matches Python behaviour
// so older frontends that send legacy mode names don't break.
//
// Normalisation: incoming mode is lowercased and any leading '/' is stripped
// so both "/search" and "search" map to the same prefix.
std::string_view mode_prefix(std::string_view mode) noexcept {
    if (mode.empty()) return {};

    // Normalise: skip a leading '/' and case-insensitively compare. Cheap
    // hand-rolled lowercase compare avoids allocating a temp std::string
    // per request on the hot path.
    if (mode.front() == '/') mode.remove_prefix(1);
    auto ieq = [](std::string_view a, std::string_view b) noexcept {
        if (a.size() != b.size()) return false;
        for (std::size_t i = 0; i < a.size(); ++i) {
            const auto ca = static_cast<unsigned char>(a[i]);
            const auto cb = static_cast<unsigned char>(b[i]);
            if (std::tolower(ca) != std::tolower(cb)) return false;
        }
        return true;
    };

    if (ieq(mode, "search"))    return "Do not generate a full legal answer. Instead, list the most relevant case references from the retrieved sources with a 1-2 sentence summary of what each decided. Format as a numbered list.\n\n";
    if (ieq(mode, "case"))      return "Focus on Tribunal decisions and case outcomes. Cite specific case references and summarise what each Tribunal decided on this point.\n\n";
    if (ieq(mode, "checklist")) return "Answer as a numbered step-by-step action checklist. Each step is a concrete action the user can take.\n\n";
    if (ieq(mode, "landlord"))  return "Answer from the landlord's perspective. What rights, remedies, and obligations does the landlord have here?\n\n";
    if (ieq(mode, "pitfalls"))  return "Focus your answer on common mistakes, traps, and risks to avoid. Lead with the pitfalls.\n\n";
    return {};
}

// Retrieval strategy parsing. Maps req.strategy ("vector"|"mmr") to the
// use_mmr bool consumed by RAGPipeline::retrieve(). Anything other than
// the exact case-insensitive "mmr" yields false (vector) - matches
// Python's _VALID_STRATEGIES = {"vector", "mmr"} silent default. Pure +
// noexcept; called once per request.
bool use_mmr_for_strategy(std::string_view strategy) noexcept {
    if (strategy.size() != 3) return false;
    return (strategy[0] == 'm' || strategy[0] == 'M') &&
           (strategy[1] == 'm' || strategy[1] == 'M') &&
           (strategy[2] == 'r' || strategy[2] == 'R');
}

// Confidence summary from retrieved scores + jurisdiction thresholds.
// Verbatim port of Python core/api.py:_confidence(). Returns the three
// fields the SSE 'confidence' event emits: level, chunk count, message.
// {n} placeholder in message templates is substituted with the count.
struct ConfidenceSummary {
    std::string level;
    int         chunks = 0;
    std::string message;
};

ConfidenceSummary summarise_confidence(
    const std::vector<astraea::QdrantPoint>& sources,
    const astraea::ConfidenceConfig&         cfg)
{
    auto lookup_msg = [&](const std::string& key) {
        auto it = cfg.messages.find(key);
        return it != cfg.messages.end() ? it->second : std::string{};
    };
    auto substitute_n = [](std::string tmpl, int n) {
        const std::string ph = "{n}";
        for (std::size_t pos = 0; (pos = tmpl.find(ph, pos)) != std::string::npos; ) {
            tmpl.replace(pos, ph.size(), std::to_string(n));
            pos += std::to_string(n).size();
        }
        return tmpl;
    };

    ConfidenceSummary out;
    out.chunks = static_cast<int>(sources.size());
    if (out.chunks == 0) {
        out.level   = "low";
        out.message = lookup_msg("none");
        if (out.message.empty()) out.message = "No relevant sources found.";
        return out;
    }
    float top = 0.0f;
    for (const auto& s : sources) if (s.score > top) top = s.score;

    if (top >= cfg.high_score && out.chunks >= cfg.high_n) {
        out.level = "high";
    } else if (top >= cfg.medium_score && out.chunks >= cfg.medium_n) {
        out.level = "medium";
    } else {
        out.level = "low";
    }
    out.message = substitute_n(lookup_msg(out.level), out.chunks);
    return out;
}

// Single-shot LLM call that rewrites a question into a form optimised for
// vector retrieval. Verbatim port of core/api.py:_rewrite_query.
//
// Takes a dedicated Generator (constructed in main() with rewrite_max_tokens
// / rewrite_temperature) so the rewrite path doesn't reserve a 2500-token KV
// cache per request and rewrites stay deterministic at T=0.0.
//
// Returns the rewritten question on success; falls back to the input on any
// LLM error, empty response, or jurisdiction opt-out (rewrite_prompt() == "").
// Never propagates exceptions - the rewrite is best-effort and a degraded
// retrieval path is better than a 5xx for the user.
drogon::Task<std::string> rewrite_query(
    std::string                       question,
    const astraea::JurisdictionBase&  jurisdiction,
    astraea::Generator&               rewrite_gen)
{
    const auto custom = jurisdiction.rewrite_prompt();
    if (custom.has_value() && custom->empty()) {
        // Explicit jurisdiction-level opt-out.
        co_return question;
    }
    // Avoid value_or here: std::optional<T>::value_or returns T by-value and
    // would copy DEFAULT_REWRITE_PROMPT into a temporary on every request.
    const std::string& system_prompt =
        custom.has_value() ? *custom : astraea::DEFAULT_REWRITE_PROMPT;

    std::vector<astraea::ChatMessage> msgs{
        {"system", system_prompt},
        {"user",   question},
    };

    try {
        auto rewritten = co_await rewrite_gen.generate(std::move(msgs));
        const auto first = rewritten.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) co_return question;
        const auto last = rewritten.find_last_not_of(" \t\r\n");
        co_return rewritten.substr(first, last - first + 1);
    } catch (const std::exception& e) {
        SPDLOG_WARN("rewrite_query failed, using original question: {}", e.what());
        co_return question;
    }
}

drogon::Task<AssembledRequest> assemble_request(
    std::string                          question,
    std::string                          mode,
    std::string                          strategy,
    std::string                          user_context,
    astraea::RAGPipeline&                pipeline,
    astraea::Generator&                  rewrite_gen,
    astraea::VectorStore*                leg_store,
    const astraea::JurisdictionBase&     jurisdiction,
    const astraea::RouteTable&           table)
{
    // 1. Strip [Key: value] zone prefixes from the rewrite/retrieval input.
    //    The ORIGINAL `question` (with prefixes intact) is preserved for the
    //    final LLM message - the model needs that context for generation.
    const std::string stripped = strip_context_prefixes(question);

    AssembledRequest req;
    req.mode         = mode;
    req.strategy     = strategy;
    req.rewrite_input = stripped; // stored for context_debug event
    const bool use_mmr = use_mmr_for_strategy(strategy);

    // 2. Optional LLM query rewrite. Falls back to `stripped` on any failure.
    auto t0 = req.timer.now();
    std::string retrieval_q = co_await rewrite_query(stripped, jurisdiction, rewrite_gen);
    req.timer.record("rewrite_ms", t0);

    // 3. Build the route decision ONCE for the whole pipeline. retrieve_anchor,
    //    augment_case_retrieval, and retrieve_manual_guidance all need the
    //    same (question, retrieval_q) decision; computing it here and threading
    //    a pointer down skips 3 redundant AC scans per request (Python has the
    //    same redundancy in core/anchor.py - intentional dedupe here, not a
    //    behaviour divergence). Inputs: original `question` + rewritten
    //    `retrieval_q` - matches Python core/api.py:483 exactly.
    const auto route_dec = build_route_decision(question, retrieval_q, table);
    req.route_dec = route_dec; // stored for context_debug event

    // 4. Embed the retrieval question once, then fan out corpus + anchor
    //    searches in parallel. Both branches search against different Qdrant
    //    collections but need the same embedding, so embedding once halves
    //    embed-server load and eliminates the queuing delay when the server
    //    handles one request at a time (typical for llama-server default config).
    t0 = req.timer.now();
    const auto t_embed_start = std::chrono::steady_clock::now();
    auto query_vec = co_await pipeline.embed(retrieval_q);
    const double embed_ms_val = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - t_embed_start).count() / 1000.0;

    auto [retrieved, anchor] = co_await astraea::detail::when_all_pair(
        // retrieve_with_vec skips the internal embed() call; query_vec is reused.
        // Explicit args mirror the pipeline.hpp defaults so the use_mmr knob
        // (decoded from req.strategy above) is visible at this call site.
        // Defaults: top_k=5, min_score=0.75, min_chunks=2 (Python parity).
        pipeline.retrieve_with_vec(query_vec, /*top_k=*/5, /*min_score=*/0.75f,
                                   /*min_chunks=*/2, /*use_mmr=*/use_mmr),
        astraea::retrieve_anchor(
            retrieval_q, /*original_question=*/question,
            pipeline, leg_store, jurisdiction, table, &route_dec, &query_vec));
    req.timer.record("retrieve_ms", t0);
    req.timer.record_ms("embed_ms",  embed_ms_val);
    req.timer.record_ms("anchor_ms", anchor.elapsed_ms);

    // Supplementary case retrieval extends retrieved in-place. Runs after
    // when_all so corpus result is available; anchor is done by then too.
    co_await astraea::augment_case_retrieval(
        question, retrieval_q, pipeline, table,
        retrieved.texts, retrieved.sources, &route_dec);

    // Confidence-gated second retrieval pass: if the augmented corpus is
    // still "low" confidence per jurisdiction.confidence_config(), re-issue
    // retrieval against both the original and rewritten questions with
    // relaxed parameters (top_k=8, min_score=0.65, min_chunks=1) so context
    // the rewriter dropped has a chance to surface. Verbatim port of Python
    // core/api.py:421-428 / core/anchor.py:_refine_retrieve.
    //
    // Computed inline rather than promoted to a helper because the level
    // logic is 4 lines and the only call site is here.
    if (!retrieved.sources.empty()) {
        const auto& cc = jurisdiction.confidence_config();
        float top = 0.0f;
        for (const auto& s : retrieved.sources) {
            if (s.score > top) top = s.score;
        }
        const auto n = static_cast<int>(retrieved.sources.size());
        const bool is_high   = top >= cc.high_score   && n >= cc.high_n;
        const bool is_medium = top >= cc.medium_score && n >= cc.medium_n;
        if (!is_high && !is_medium) {
            // Level == "low". Re-sort cap at 6 is inside refine_retrieve.
            SPDLOG_DEBUG("/ask: low-confidence retrieval (top={:.3f}, n={}); refining", top, n);
            co_await astraea::refine_retrieve(
                question, retrieval_q, pipeline,
                retrieved.texts, retrieved.sources);
            req.refine_used = true;
        }
    } else {
        // Zero sources: refine against both queries to give the user something
        // to work with rather than failing the whole request. Python's
        // _confidence treats n==0 as "low" too.
        SPDLOG_DEBUG("/ask: empty retrieval; refining");
        co_await astraea::refine_retrieve(
            question, retrieval_q, pipeline,
            retrieved.texts, retrieved.sources);
        req.refine_used = true;
    }

    std::unordered_set<std::string> existing_ids;
    for (const auto& s : retrieved.sources) existing_ids.insert(s.id);
    for (const auto& s : anchor.leg_sources) existing_ids.insert(s.id);

    t0 = req.timer.now();
    auto guidance = co_await astraea::retrieve_manual_guidance(
        retrieval_q, /*original_question=*/question, pipeline, existing_ids,
        jurisdiction, table, &route_dec);
    req.timer.record("guidance_ms", t0);

    // Snapshot chunk texts before they are consumed into the user message.
    // Needed by context_debug to emit per-chunk preview + full_text.
    req.chunk_texts = retrieved.texts;
    // guidance_injected = guidance source was NOT in existing_ids (new info).
    req.guidance_injected = guidance.source.has_value()
        && existing_ids.find(guidance.source->id) == existing_ids.end();

    // Confidence summary (Python core/api.py _confidence() parity). Computed
    // here against the final retrieval set so the SSE 'confidence' event and
    // any future JSON response can report level/n/message consistently.
    {
        const auto cs = summarise_confidence(retrieved.sources,
                                             jurisdiction.confidence_config());
        req.confidence_level   = cs.level;
        req.confidence_chunks  = cs.chunks;
        req.confidence_message = cs.message;
    }

    // Inject the client-supplied personal context (frontend localStorage,
    // capped at 500 chars per Python core/api.py:464). Prepended to the
    // anchor block so the LLM treats it as durable user-supplied context
    // rather than transient input. No-op when user_context is empty or
    // contains only whitespace.
    {
        auto trim = [](std::string_view s) {
            std::size_t lo = 0, hi = s.size();
            while (lo < hi && std::isspace(static_cast<unsigned char>(s[lo]))) ++lo;
            while (hi > lo && std::isspace(static_cast<unsigned char>(s[hi - 1]))) --hi;
            return s.substr(lo, hi - lo);
        };
        const auto trimmed = trim(user_context);
        if (!trimmed.empty()) {
            std::string capped(trimmed.substr(0, std::min<std::size_t>(500, trimmed.size())));
            std::string prefix = "User's personal context (apply throughout your answer):\n"
                                 + std::move(capped);
            if (!anchor.anchor_text.empty())
                prefix.append("\n\n---\n\n").append(anchor.anchor_text);
            anchor.anchor_text = std::move(prefix);
        }
    }

    t0 = req.timer.now();
    req.messages.push_back({"system", jurisdiction.system_prompt()});
    // Apply cheat-code mode prefix (search/case/checklist/landlord/pitfalls)
    // here only, so retrieval / anchor / guidance above used the clean
    // question and corpus quality is unaffected by the toggle.
    const auto mp = mode_prefix(mode);
    std::string user_msg = build_context_block(retrieved, anchor, guidance);
    user_msg.append("\n\nQuestion: ").append(mp).append(question);
    req.context_chars  = static_cast<int>(user_msg.size());
    req.context_chunks = static_cast<int>(retrieved.sources.size()
                                        + anchor.leg_sources.size()
                                        + (guidance.source ? 1u : 0u));
    req.messages.push_back({"user", std::move(user_msg)});
    req.timer.record("context_ms", t0);

    req.sources          = std::move(retrieved.sources);
    req.guidance_source  = guidance.source;
    req.rewritten_q      = (retrieval_q != stripped) ? retrieval_q : std::string{};
    req.leg_sources           = std::move(anchor.leg_sources);
    req.matched_intents       = route_dec.matched_intents;
    req.route_triggered       = route_dec.triggered;
    req.anchor_route_matched  = anchor.route_matched;
    req.anchor_max_hits       = anchor.max_hits;
    co_return req;
}

SourceJson to_source_json(const astraea::QdrantPoint& pt) {
    auto get = [&](const char* k) -> std::string {
        auto it = pt.payload.find(k);
        return it != pt.payload.end() ? it->second : "";
    };
    return SourceJson{
        get("case_id"), get("court_name"), get("date"), get("url"),
    };
}

// Real /ask handler — non-streaming. Coroutine-style so the multi-stage
// retrieve / anchor / guidance / generate chain reads sequentially.
//
// Captures pipeline + leg_store + jurisdiction + table by reference; all
// live for the lifetime of drogon::app().run() (constructed once in main()).
drogon::Task<drogon::HttpResponsePtr> ask_handler(
    drogon::HttpRequestPtr               req,
    astraea::RAGPipeline&                pipeline,
    astraea::Generator&                  rewrite_gen,
    astraea::CoordinatorClient*          llm_sem,
    int                                  llm_acquire_timeout_s,
    IpLimiter*                           ip_lim,
    astraea::VectorStore*                leg_store,
    const astraea::JurisdictionBase&     jurisdiction,
    const astraea::RouteTable&           table,
    astraea::SessionStore*               session_store,
    astraea::JsonlWriter*                question_log,
    astraea::JsonlWriter*                route_debug_log,
    const astraea::HealthProber&         health_prober,
    const std::string&                   cfg_debug_key)
{
    const std::string req_id = astraea::resolve_request_id(req->getHeader("x-request-id"));

#ifdef ASTRAEA_ENABLE_TIMING
    const auto t_sanitize = std::chrono::steady_clock::now();
#endif
    ParsedAskRequest preq;
    if (auto err = parse_and_sanitize(req, preq, jurisdiction.max_question_chars(), cfg_debug_key)) {
        err->addHeader("X-Request-Id", req_id);
        co_return err;
    }
    std::string& question     = preq.question;
    std::string& mode         = preq.mode;
    std::string& strategy     = preq.strategy;
    std::string& user_context = preq.user_context;
#ifdef ASTRAEA_ENABLE_TIMING
    const double sanitize_ms = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - t_sanitize).count() / 1000.0;
#endif

    // session_id may arrive via the X-Session-Id header OR the body field
    // 'session_id'. The live nz_tenancy frontend uses the body; the header
    // is the original C++ contract that direct API callers may still rely
    // on. Header wins when both are present.
    std::string session_id = req->getHeader("x-session-id");
    if (session_id.empty()) session_id = preq.session_id;
    const bool use_session = session_store &&
        astraea::SessionStore::valid_session_id(session_id);

    IpLimiter::Permit ip_permit;
    if (ip_lim) {
        const std::string client_ip = req->getPeerAddr().toIp();
        auto maybe_ip = ip_lim->try_acquire(client_ip);
        if (!maybe_ip) {
            SPDLOG_WARN("/ask[{}]: per-IP limit hit for {}", req_id, client_ip);
            auto r = text_response(
                drogon::k429TooManyRequests,
                "Too many concurrent requests from your IP. Please retry.\n");
            r->addHeader("X-Request-Id", req_id);
            co_return r;
        }
        ip_permit = std::move(*maybe_ip);
    }

    // Per-request LLM readiness check (Python core/api.py:_check_llm parity).
    // Returns a friendlier 503 with the "model is loading" hint during
    // llama-server cold-load instead of letting the user hit the more
    // generic upstream-unavailable error several seconds later when
    // generate() actually fails. ~5-20ms on the localhost happy path.
    if (!co_await health_prober.probe_llm()) {
        SPDLOG_INFO("/ask[{}]: LLM /v1/models probe failed, returning 503", req_id);
        auto r = text_response(drogon::k503ServiceUnavailable,
            "The AI model is currently loading. Please try again in 30 seconds.\n");
        r->addHeader("X-Request-Id", req_id);
        co_return r;
    }

    AssembledRequest assembled;
    try {
        assembled = co_await assemble_request(
            question, std::move(mode), std::move(strategy),
            std::move(user_context),
            pipeline, rewrite_gen,
            leg_store, jurisdiction, table);
#ifdef ASTRAEA_ENABLE_TIMING
        assembled.timer.record_ms("sanitize_ms", sanitize_ms);
#endif
    } catch (const std::exception& e) {
        // Detail in the log only - 503 body stays generic so internal URLs /
        // model names / collection names do not leak to clients.
        SPDLOG_WARN("/ask[{}]: retrieval failed: {}", req_id, e.what());
        auto r = text_response(drogon::k503ServiceUnavailable,
            "Upstream retrieval temporarily unavailable\n");
        r->addHeader("X-Request-Id", req_id);
        co_return r;
    }

    std::vector<astraea::ChatMessage> history;
    if (use_session) {
        history = co_await session_store->load(session_id);
        if (!history.empty()) {
            // Inject only the tail (default last 3 turn pairs = 6 messages).
            // load() returns the full stored window so we can round-trip it
            // back to Redis on save without losing older turns. The slice
            // here is purely about prompt budget - older pairs would push
            // TTFT up linearly with session depth otherwise.
            const auto inject_n = static_cast<std::size_t>(
                session_store->inject_message_count());
            auto first = history.size() > inject_n
                ? history.end() - static_cast<std::ptrdiff_t>(inject_n)
                : history.begin();
            assembled.messages.insert(assembled.messages.begin() + 1,
                                      first, history.end());
        }
    }

    const bool log = !is_no_log(req);
    if (log && question_log) {
        QuestionLogEntry qle{utc_iso8601(), req_id, question};
        std::string qle_json;
        if (!glz::write_json(qle, qle_json))
            question_log->append(qle_json);
    }

    std::string answer;
    astraea::CoordinatorClient::Permit gen_permit;
    if (llm_sem) {
        // Acquire the global LLM permit with a configurable timeout (Python
        // core/api.py global_llm_acquire(timeout=90.0) parity). Returning
        // 503 on timeout caps queue pile-up under sustained overload - one
        // slow LLM call away from a death spiral otherwise.
        auto t_wait = assembled.timer.now();
        if (llm_acquire_timeout_s > 0) {
            auto maybe = co_await llm_sem->acquire(
                std::chrono::seconds(llm_acquire_timeout_s));
            assembled.timer.record("llm_wait_ms", t_wait);
            if (!maybe) {
                SPDLOG_WARN("/ask[{}]: LLM permit timeout after {}s (backend={}, max={})",
                            req_id, llm_acquire_timeout_s,
                            llm_sem->backend_name(), llm_sem->max_concurrency());
                auto r = text_response(drogon::k503ServiceUnavailable,
                    "Server is busy. Please retry in a moment.\n");
                r->addHeader("X-Request-Id", req_id);
                co_return r;
            }
            gen_permit = std::move(*maybe);
        } else {
            gen_permit = co_await llm_sem->acquire();
            assembled.timer.record("llm_wait_ms", t_wait);
        }
    }
    auto t_gen = assembled.timer.now();
    try {
        answer = co_await pipeline.generator().generate(std::move(assembled.messages));
    } catch (const std::exception& e) {
        // Detail in the log only - 503 body stays generic.
        SPDLOG_WARN("/ask[{}]: generation failed: {}", req_id, e.what());
        auto r = text_response(drogon::k503ServiceUnavailable,
            "Upstream LLM temporarily unavailable\n");
        r->addHeader("X-Request-Id", req_id);
        co_return r;
    }
    assembled.timer.record("generation_ms", t_gen);

    if (use_session) {
        history.push_back({"user",      question});
        history.push_back({"assistant", answer});
        co_await session_store->save(session_id, std::move(history));
    }

    if (log && route_debug_log) {
        RouteDebugEntry rde;
        rde.ts       = utc_iso8601();
        rde.request_id = req_id;
        rde.q        = question;
        rde.rewritten = assembled.rewritten_q;
        rde.triggered = assembled.route_triggered;
        rde.matched_intents = assembled.matched_intents;
        for (const auto& s : assembled.sources)
            rde.sources.push_back({s.id, s.score});
        for (const auto& s : assembled.leg_sources)
            rde.legislation.push_back({s.id, s.payload.count("title")
                ? s.payload.at("title") : std::string{}});
        rde.answer = answer.substr(0, 8000);
        rde.context_chars  = assembled.context_chars;
        rde.context_chunks = assembled.context_chunks;
        rde.mode           = assembled.mode;
        rde.strategy       = assembled.strategy;
        std::string rde_json;
        if (!glz::write_json(rde, rde_json))
            route_debug_log->append(rde_json);
    }

    AskResponse out;
    out.answer = std::move(answer);
    out.sources.reserve(assembled.sources.size());
    for (const auto& s : assembled.sources)
        out.sources.push_back(to_source_json(s));
    if (assembled.guidance_source)
        out.guidance_source = to_source_json(*assembled.guidance_source);

    std::string body;
    if (auto e = glz::write_json(out, body); e) {
        auto r = text_response(drogon::k500InternalServerError, "Serialization error\n");
        r->addHeader("X-Request-Id", req_id);
        co_return r;
    }
    auto resp = json_response(drogon::k200OK, std::move(body));
    resp->addHeader("X-Request-Id", req_id);
    co_return resp;
}

// Real /ask/stream handler — SSE-shaped streaming via Generator::generate_stream.
//
// Frame format:
//   event: sources                    (one event, fired before any tokens)
//   data: [{"id":..,"label":..,"score":..}, ...]
//
//   data: {"token":"..."}             (one frame per token)
//
//   data: [DONE]                      (terminator)
//
// On retrieval failure: data: {"error":"..."} + [DONE], then close.
//
// IMPORTANT — Phase 3 streaming caveat (PR #9 Generator::generate_stream):
// sendRequestCoro buffers the full LLM response before invoking the
// TokenCallback. Tokens fire in batch AFTER the LLM finishes, not in
// real time. User-perceived UX: a long wait followed by a fast blast.
// Phase 6D will swap to true streaming once Drogon's chunked-body
// coroutine API is in. The shape above is what the client should see
// once true streaming lands, so the frontend can be coded against it
// today without rework.
void ask_stream_handler(
    const drogon::HttpRequestPtr&           req,
    std::function<void(const drogon::HttpResponsePtr&)>&& cb,
    astraea::RAGPipeline&                   pipeline,
    astraea::Generator&                     rewrite_gen,
    astraea::CoordinatorClient*             llm_sem,
    int                                     llm_acquire_timeout_s,
    IpLimiter*                              ip_lim,
    astraea::VectorStore*                   leg_store,
    const astraea::JurisdictionBase&        jurisdiction,
    const astraea::RouteTable&              table,
    astraea::SessionStore*                  session_store,
    astraea::JsonlWriter*                   question_log,
    astraea::JsonlWriter*                   route_debug_log,
    const astraea::HealthProber&            health_prober,
    const std::string&                      cfg_debug_key)
{
    const std::string req_id = astraea::resolve_request_id(req->getHeader("x-request-id"));

#ifdef ASTRAEA_ENABLE_TIMING
    const auto t_sanitize = std::chrono::steady_clock::now();
#endif
    ParsedAskRequest preq;
    if (auto err = parse_and_sanitize(req, preq, jurisdiction.max_question_chars(), cfg_debug_key)) {
        err->addHeader("X-Request-Id", req_id);
        cb(err);
        return;
    }
    std::string& question          = preq.question;
    std::string& mode              = preq.mode;
    std::string& strategy          = preq.strategy;
    std::string& user_context      = preq.user_context;
    const bool   feedback_context  = preq.feedback_context;
    // debug_mode mirrors Python: bool(_DEBUG_KEY and req.debug_key == _DEBUG_KEY).
    // Constant-time compare so key length is the only timing side-channel.
    const bool   debug_mode        = !cfg_debug_key.empty()
        && preq.debug_key.size() == cfg_debug_key.size()
        && CRYPTO_memcmp(preq.debug_key.data(),
                         cfg_debug_key.data(),
                         cfg_debug_key.size()) == 0;
#ifdef ASTRAEA_ENABLE_TIMING
    const double sanitize_ms = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - t_sanitize).count() / 1000.0;
#endif

    IpLimiter::Permit ip_permit;
    if (ip_lim) {
        const std::string client_ip = req->getPeerAddr().toIp();
        auto maybe_ip = ip_lim->try_acquire(client_ip);
        if (!maybe_ip) {
            SPDLOG_WARN("/ask/stream[{}]: per-IP limit hit for {}", req_id, client_ip);
            auto r = text_response(
                drogon::k429TooManyRequests,
                "Too many concurrent requests from your IP. Please retry.\n");
            r->addHeader("X-Request-Id", req_id);
            cb(r);
            return;
        }
        ip_permit = std::move(*maybe_ip);
    }

    // session_id may arrive via the X-Session-Id header OR the body field.
    // The live nz_tenancy frontend uses the body; header was the original
    // C++ contract direct API callers may still rely on. Header wins.
    std::string sess_id;
    if (session_store) {
        std::string raw = req->getHeader("x-session-id");
        if (raw.empty()) raw = preq.session_id;
        if (astraea::SessionStore::valid_session_id(raw)) sess_id = std::move(raw);
    }

    const bool log = !is_no_log(req);
    if (log && question_log) {
        QuestionLogEntry qle{utc_iso8601(), req_id, question};
        std::string qle_json;
        if (!glz::write_json(qle, qle_json))
            question_log->append(qle_json);
    }

    auto ip_permit_ptr = std::make_shared<IpLimiter::Permit>(std::move(ip_permit));
    const float eval_temperature = preq.eval_temperature;
    auto resp = drogon::HttpResponse::newAsyncStreamResponse(
        [q = std::move(question), mode = std::move(mode), strategy = std::move(strategy),
         user_context = std::move(user_context),
         debug_mode, feedback_context, eval_temperature,
         &pipeline, &rewrite_gen,
         llm_sem, llm_acquire_timeout_s,
         ip_permit_ptr, leg_store, &jurisdiction, &table,
         sess_id = std::move(sess_id), session_store, log, route_debug_log,
         &health_prober,
#ifdef ASTRAEA_ENABLE_TIMING
         req_id, sanitize_ms](
#else
         req_id](
#endif
            drogon::ResponseStreamPtr stream) mutable {
            drogon::async_run(
                [stream = std::move(stream), q = std::move(q), mode = std::move(mode),
                 strategy = std::move(strategy),
                 user_context = std::move(user_context),
                 debug_mode, feedback_context, eval_temperature,
                 &pipeline, &rewrite_gen,
                 llm_sem, llm_acquire_timeout_s, ip_permit_ptr,
                 leg_store, &jurisdiction, &table,
                 sess_id = std::move(sess_id), session_store,
                 &health_prober,
#ifdef ASTRAEA_ENABLE_TIMING
                 log, route_debug_log, req_id, sanitize_ms]() mutable -> drogon::Task<> {
#else
                 log, route_debug_log, req_id]() mutable -> drogon::Task<> {
#endif
                    // ResponseStreamPtr is std::unique_ptr<ResponseStream>; the
                    // TokenCallback below cannot copy it. Convert ownership to a
                    // shared_ptr so both this coroutine and the callback hold
                    // owning refs - survives Phase 6D's switch to async chunked
                    // streaming where the callback may fire from a different
                    // coroutine frame than the one currently suspended.
                    auto shared_stream = std::shared_ptr<drogon::ResponseStream>(
                        std::move(stream));

                    // Capture the event loop that owns this client connection
                    // before any co_await that may shift the coroutine to a
                    // different trantor I/O thread. Used by pin_to_loop below.
                    auto* client_loop =
                        trantor::EventLoop::getEventLoopOfCurrentThread();

                    // Defensive send wrapper: ResponseStream::send() returns
                    // false when the underlying TCP connection has been closed
                    // (browser tab closed, peer RST mid-stream, etc). Calling
                    // send() after that point doesn't crash by itself, but it
                    // widens the race window for the trantor Channel::remove
                    // assertion (events_ != kNoneEvent) when the LLM token
                    // callback - which runs on a different event-loop thread
                    // than the response stream's owning loop - races with
                    // Drogon's connection teardown. Two mitigations bundled:
                    //   1. Short-circuit further sends after the first false
                    //      return - stops feeding a dead pipe.
                    //   2. Log a single INFO line per disconnect so production
                    //      operators can correlate trantor crash bursts with
                    //      client-side disconnect rates.
                    // When peer_dead is set, generate_stream passes it to
                    // LlmStreamSession as a cancellation flag. The session
                    // aborts the upstream LLM connection on the next token,
                    // releasing the global semaphore without waiting for [DONE].
                    //
                    // NOTE: the LLM token callback (further down) does NOT
                    // call safe_send - it checks peer_dead and calls
                    // shared_stream->send() inline to avoid std::function
                    // indirection at per-token call rates. It sets peer_dead
                    // on the same false-return path so the two sites stay in
                    // sync. Grep for `peer disconnected mid-stream` in logs
                    // - that one phrase covers both call sites.
                    auto peer_dead = std::make_shared<std::atomic<bool>>(false);
                    auto safe_send = [shared_stream, peer_dead, req_id](
                        const std::string& data) -> bool {
                        if (peer_dead->load(std::memory_order_relaxed)) return false;
                        if (!shared_stream->send(data)) {
                            if (!peer_dead->exchange(true, std::memory_order_relaxed))
                                SPDLOG_INFO("/ask/stream[{}]: peer disconnected mid-stream", req_id);
                            return false;
                        }
                        return true;
                    };

                    // Per-request LLM readiness check (Python core/api.py:_check_llm parity).
                    // Returns a friendlier SSE error during llama-server cold-load
                    // instead of letting the user hit a generic upstream error
                    // 200-2000ms later when generate_stream actually fails.
                    // Probe is ~5-20ms on the localhost happy path.
                    if (!co_await health_prober.probe_llm()) {
                        SPDLOG_INFO("/ask/stream[{}]: LLM /v1/models probe failed, returning SSE error", req_id);
                        // probe_llm internally co_awaits an HTTP call that may
                        // resume on a non-client loop; re-pin before send.
                        co_await astraea::detail::pin_to_loop(client_loop);
                        safe_send("data: {\"error\":\"The AI model is currently loading. "
                                  "Please try again in 30 seconds.\"}\n\n");
                        safe_send("data: {\"type\":\"done\"}\n\n");
                        shared_stream->close();
                        co_return;
                    }

                    // assemble_request reuses 6C.2's coroutine - retrieve,
                    // anchor, guidance, build [system, user] messages.
                    // Uses when_all_pair/async_run internally so it may resume
                    // on a different event-loop thread. We must not co_await
                    // inside the catch block (C++ restriction), so use a flag
                    // and handle the error after the pin_to_loop below.
                    AssembledRequest assembled;
                    bool retrieval_ok = true;
                    try {
                        assembled = co_await assemble_request(
                            q, std::move(mode), std::move(strategy),
                            std::move(user_context),
                            pipeline, rewrite_gen,
                            leg_store, jurisdiction, table);
#ifdef ASTRAEA_ENABLE_TIMING
                        assembled.timer.record_ms("sanitize_ms", sanitize_ms);
#endif
                    } catch (const std::exception& e) {
                        // Detail in the log only - client sees a generic error.
                        SPDLOG_WARN("/ask/stream[{}]: retrieval failed: {}", req_id, e.what());
                        retrieval_ok = false;
                    }
                    // Re-pin #1: assemble_request (when_all_pair) may have
                    // shifted us to another loop. Pin before safe_send so the
                    // sources event and error sends are on client_loop.
                    co_await astraea::detail::pin_to_loop(client_loop);
                    if (!retrieval_ok) {
                        safe_send("data: {\"error\":\"upstream retrieval temporarily unavailable\"}\n\n");
                        safe_send("data: {\"type\":\"done\"}\n\n");
                        shared_stream->close();
                        co_return;
                    }

                    std::vector<astraea::ChatMessage> history;
                    if (session_store && !sess_id.empty()) {
                        history = co_await session_store->load(sess_id);
                        if (!history.empty()) {
                            const auto inject_n = static_cast<std::size_t>(
                                session_store->inject_message_count());
                            auto first = history.size() > inject_n
                                ? history.end() - static_cast<std::ptrdiff_t>(inject_n)
                                : history.begin();
                            assembled.messages.insert(
                                assembled.messages.begin() + 1,
                                first, history.end());
                        }
                    }

                    // Emit a sources event upfront so the client renders
                    // citation chips even before the first token arrives.
                    // Includes guidance_source as a sibling field so the SSE
                    // contract has full parity with /ask's AskResponse.
                    SourcesEvent srcs_ev;
                    srcs_ev.sources.reserve(assembled.sources.size());
                    for (const auto& s : assembled.sources)
                        srcs_ev.sources.push_back(to_source_json(s));
                    for (const auto& s : assembled.leg_sources)
                        srcs_ev.legislation.push_back({
                            s.payload.count("case_id") ? s.payload.at("case_id") : s.id,
                            s.payload.count("title")   ? s.payload.at("title")   : "",
                            s.payload.count("url")     ? s.payload.at("url")     : "",
                        });
                    if (assembled.guidance_source)
                        srcs_ev.guidance_source =
                            to_source_json(*assembled.guidance_source);

                    std::string srcs_body;
                    if (auto e = glz::write_json(srcs_ev, srcs_body); !e) {
                        safe_send("data: " + srcs_body + "\n\n");
                    } else {
                        SPDLOG_WARN("/ask/stream[{}]: sources serialisation failed", req_id);
                    }

                    // Confidence event: render the retrieval confidence indicator
                    // alongside the answer. Same field shape as Python's
                    // _confidence() output (level/chunks/message). Empty level
                    // shouldn't happen post-summarise_confidence; skip the send
                    // defensively to avoid a malformed event reaching the client.
                    if (!assembled.confidence_level.empty()) {
                        ConfidenceEvent cev;
                        cev.level   = assembled.confidence_level;
                        cev.chunks  = assembled.confidence_chunks;
                        cev.message = assembled.confidence_message;
                        std::string cev_json;
                        if (!glz::write_json(cev, cev_json))
                            safe_send("data: " + cev_json + "\n\n");
                    }

                    // debug event: strategy, retrieve timing, scores, chunk count.
                    // Mirrors Python core/api.py line 475. Only emitted in debug_mode.
                    if (debug_mode) {
                        DebugEvent dev;
                        dev.strategy    = assembled.strategy.empty()
                                          ? "vector" : assembled.strategy;
                        dev.retrieve_ms = assembled.timer.agg({"retrieve_ms"});
                        for (const auto& s : assembled.sources)
                            dev.scores.push_back(s.score);
                        dev.chunks      = static_cast<int>(assembled.sources.size());
                        dev.refine_used = assembled.refine_used;
                        std::string dev_json;
                        if (!glz::write_json(dev, dev_json))
                            safe_send("data: " + dev_json + "\n\n");
                    }

                    // context_debug event: full retrieval context for the feedback
                    // capture UI and debug panel. Emitted when debug_mode OR
                    // feedback_context. Python core/api.py lines 479-543.
                    if (debug_mode || feedback_context) {
                        ContextDebugEvent cde;
                        cde.original_query  = q;
                        cde.rewrite_input   = assembled.rewrite_input;
                        cde.rewritten_query = assembled.rewritten_q.empty()
                                              ? q : assembled.rewritten_q;
                        cde.rewrite_used    = !assembled.rewritten_q.empty();

                        // Routing decision -> RoutingEvent.
                        {
                            const auto& rd = assembled.route_dec;
                            cde.statute_routing.triggered        = rd.triggered;
                            cde.statute_routing.matched_routes   = rd.matched_intents;
                            cde.statute_routing.trigger_terms    = rd.trigger_terms;
                            for (const auto& tp : rd.trigger_paths)
                                cde.statute_routing.trigger_paths[tp.intent] = tp.path;
                            cde.statute_routing.forced_sections  = rd.forced_sections;
                            cde.statute_routing.dominant_route   = rd.dominant_route;
                            cde.statute_routing.dominance_reason = rd.dominance_reason;
                            for (const auto& ir : rd.ignored_routes)
                                cde.statute_routing.ignored_routes.push_back(
                                    {ir.intent, ir.reason});
                            for (const auto& nm : rd.near_miss_routes)
                                cde.statute_routing.near_miss_routes.push_back(
                                    {nm.intent, nm.broad_matched});
                        }

                        // Anchor retrieval metadata.
                        cde.anchor.route_matched = assembled.anchor_route_matched;
                        cde.anchor.max_hits      = assembled.anchor_max_hits;

                        // Anchor sections from leg_sources.
                        for (const auto& ls : assembled.leg_sources) {
                            AnchorSection as;
                            as.document_id = ls.id;
                            as.title = ls.payload.count("title")
                                       ? ls.payload.at("title") : "";
                            const auto& txt = ls.payload.count("text")
                                              ? ls.payload.at("text") : std::string{};
                            as.tokens  = static_cast<int>(
                                std::max<std::size_t>(1, (txt.size() + 3) / 4));
                            as.preview = txt.size() > 200
                                         ? txt.substr(0, 200) + "..." : txt;
                            cde.anchor.sections.push_back(std::move(as));
                        }

                        // Guidance debug.
                        {
                            cde.guidance.injected = assembled.guidance_injected;
                            if (assembled.guidance_source) {
                                cde.guidance.source = assembled.guidance_source->id;
                                const auto& gp = assembled.guidance_source->payload;
                                if (gp.count("court_name"))
                                    cde.guidance.court_name = gp.at("court_name");
                                cde.guidance.score = assembled.guidance_source->score;
                            }
                            cde.guidance.threshold =
                                astraea::GUIDANCE_THRESHOLD;
                            cde.guidance.reason = assembled.guidance_injected
                                ? "score_above_threshold" : "not_injected";
                        }

                        // Chunk cards: one per corpus source, with full text.
                        for (std::size_t i = 0; i < assembled.sources.size(); ++i) {
                            const auto& src = assembled.sources[i];
                            const auto& txt = i < assembled.chunk_texts.size()
                                              ? assembled.chunk_texts[i]
                                              : std::string{};
                            ChunkCard cc;
                            cc.source_index = static_cast<int>(i);
                            cc.score        = src.score;
                            cc.passed_gate  = true;
                            cc.document_id  = src.id;
                            cc.date = src.payload.count("date")
                                      ? src.payload.at("date") : "";
                            cc.tokens  = static_cast<int>(
                                std::max<std::size_t>(1, (txt.size() + 3) / 4));
                            cc.preview = txt.size() > 200
                                         ? txt.substr(0, 200) + "..." : txt;
                            cc.full_text = txt;
                            cde.chunks.push_back(std::move(cc));
                        }

                        // Context budget (token approximation: 1 token ~= 4 chars).
                        {
                            int anchor_tok = 0;
                            for (const auto& ls : assembled.leg_sources) {
                                const auto& txt = ls.payload.count("text")
                                    ? ls.payload.at("text") : std::string{};
                                anchor_tok += static_cast<int>(
                                    std::max<std::size_t>(1, (txt.size() + 3) / 4));
                            }
                            int chunk_tok = 0;
                            for (const auto& txt : assembled.chunk_texts) {
                                chunk_tok += static_cast<int>(
                                    std::max<std::size_t>(1, (txt.size() + 3) / 4));
                            }
                            cde.budget.total_tokens    =
                                static_cast<int>((assembled.context_chars + 3) / 4);
                            cde.budget.ctx_limit       = 8192;
                            cde.budget.anchor_tokens   = anchor_tok;
                            cde.budget.chunk_tokens    = chunk_tok;
                            cde.budget.sources_sent    =
                                static_cast<int>(assembled.sources.size());
                            cde.budget.truncated_chunks = 0;
                        }

                        std::string cde_json;
                        if (!glz::write_json(cde, cde_json))
                            safe_send("data: " + cde_json + "\n\n");
                    }

                    // Stream tokens via Generator::generate_stream + TokenCallback.
                    // True per-token streaming as of Phase 6D - on_token fires
                    // for each SSE chunk the LLM emits, not in a single batch
                    // after generation completes.
                    astraea::CoordinatorClient::Permit gen_permit;
                    if (llm_sem) {
                        // Queue event: probe the LLM semaphore non-blockingly.
                        // If it's already saturated, tell the user we're queued
                        // before we sit on the real (timed) acquire. Matches
                        // Python core/api.py:557 - lets the frontend show a
                        // "queued" indicator instead of looking hung. Cheap
                        // dry-run on the in_process backend; one extra Redis
                        // round-trip on the redis backend (still <1ms typical).
                        auto probe = co_await llm_sem->acquire(
                            std::chrono::milliseconds(0));
                        if (!probe) {
                            QueueEvent qev{};
                            std::string qev_json;
                            if (!glz::write_json(qev, qev_json))
                                safe_send("data: " + qev_json + "\n\n");
                        } else {
                            // Already got the permit on the dry-run; stash it
                            // so we skip the real acquire below.
                            gen_permit = std::move(*probe);
                        }
                        // Acquire the global LLM permit with timeout (Python
                        // core/api.py global_llm_acquire(timeout=90) parity).
                        // On timeout, emit a JSON error event in the SSE stream
                        // rather than 503ing - the headers are already on the
                        // wire by the time this coroutine runs.
                        auto t_wait = assembled.timer.now();
                        if (!gen_permit.held() && llm_acquire_timeout_s > 0) {
                            auto maybe = co_await llm_sem->acquire(
                                std::chrono::seconds(llm_acquire_timeout_s));
                            assembled.timer.record("llm_wait_ms", t_wait);
                            if (!maybe) {
                                SPDLOG_WARN("/ask/stream[{}]: LLM permit timeout after {}s "
                                            "(backend={}, max={})",
                                            req_id, llm_acquire_timeout_s,
                                            llm_sem->backend_name(),
                                            llm_sem->max_concurrency());
                                // Re-pin before emitting + closing - the acquire()
                                // coroutine may have left us on a non-client loop,
                                // and we don't fall through to Re-pin #2 on this
                                // early-return path. Same race the assemble_request
                                // catch path closes via Re-pin #1.
                                co_await astraea::detail::pin_to_loop(client_loop);
                                safe_send(
                                    "data: {\"error\":\"server is busy, please retry\"}\n\n");
                                safe_send("data: {\"type\":\"done\"}\n\n");
                                shared_stream->close();
                                co_return;
                            }
                            gen_permit = std::move(*maybe);
                        } else if (!gen_permit.held()) {
                            // Untimed path (llm_acquire_timeout_s == 0 and the
                            // dry-run probe above didn't already secure the
                            // permit). Blocks until a permit is available.
                            gen_permit = co_await llm_sem->acquire();
                            assembled.timer.record("llm_wait_ms", t_wait);
                        } else {
                            // Dry-run probe already secured the permit; nothing
                            // to wait on. Still record llm_wait_ms = 0 for
                            // bookkeeping symmetry with the other branches.
                            assembled.timer.record("llm_wait_ms", t_wait);
                        }
                    }
                    // Re-pin #2: session_store->load() and llm_sem->acquire()
                    // may have drifted the coroutine off client_loop. Pin back
                    // so generate_stream is created on L_client; token callbacks
                    // then fire on the same thread as TCP teardown, eliminating
                    // the Channel::remove() race (events_ != kNoneEvent assert).
                    co_await astraea::detail::pin_to_loop(client_loop);

                    std::string full_answer;
                    bool first_token = true;
                    auto t_gen = assembled.timer.now();
                    try {
                        co_await pipeline.generator().generate_stream(
                            std::move(assembled.messages),
                            // req_id captured BY VALUE here. The reference
                            // would be valid today (outer-lambda owns the
                            // std::string, coroutine frame stays alive for
                            // the duration of co_await), but the invariant
                            // is non-obvious: any future refactor that
                            // hoists generate_stream out of co_await, or
                            // changes the outer lambda's capture mode,
                            // turns &req_id into a dangling reference.
                            // String is small, copy is cheap, value avoids
                            // the footgun.
                            [shared_stream, peer_dead, &full_answer, &first_token,
                             &assembled, &t_gen, req_id](std::string_view token) {
                                // NOTE: peer_dead is also the cancellation
                                // flag passed to generate_stream below. When
                                // send() returns false here and peer_dead is
                                // set to true, LlmStreamSession sees it on
                                // the next token and calls finish() to abort
                                // the upstream LLM connection.
                                if (peer_dead->load(std::memory_order_relaxed)) return;
                                if (first_token) {
                                    assembled.timer.record("ttft_ms", t_gen);
                                    first_token = false;
                                }
                                full_answer.append(token);
                                std::string body;
                                if (auto e = glz::write_json(
                                        TokenChunk{"token", std::string(token)}, body); e) {
                                    SPDLOG_WARN("/ask/stream[{}]: token serialisation failed",
                                                req_id);
                                    return;
                                }
                                if (!shared_stream->send("data: " + body + "\n\n")) {
                                    if (!peer_dead->exchange(true, std::memory_order_relaxed))
                                        SPDLOG_INFO("/ask/stream[{}]: peer disconnected mid-stream",
                                                    req_id);
                                }
                            },
                            peer_dead,
                            eval_temperature);
                    } catch (const std::exception& e) {
                        SPDLOG_WARN("/ask/stream[{}]: generation failed: {}", req_id, e.what());
                        safe_send(
                            "data: {\"error\":\"upstream LLM temporarily unavailable\"}\n\n");
                    }
                    assembled.timer.record("generation_ms", t_gen);

                    // Skip session save if the client disconnected mid-stream:
                    // the conversation is incomplete and the peer is gone.
                    if (session_store && !sess_id.empty() && !full_answer.empty()
                            && !peer_dead->load(std::memory_order_relaxed)) {
                        history.push_back({"user",      q});
                        history.push_back({"assistant", full_answer});
                        co_await session_store->save(sess_id, std::move(history));
                    }

#ifdef ASTRAEA_ENABLE_TIMING
                    // Emit timing SSE event (compile-time opt-in via ASTRAEA_ENABLE_TIMING).
                    {
                        TimingEvent te;
                        te.request_id    = req_id;
                        te.sanitize_ms   = assembled.timer.agg({"sanitize_ms"});
                        te.rewrite_ms    = assembled.timer.agg({"rewrite_ms"});
                        te.embed_ms      = assembled.timer.agg({"embed_ms"});
                        te.retrieve_ms   = assembled.timer.agg({"retrieve_ms"});
                        te.anchor_ms     = assembled.timer.agg({"anchor_ms"});
                        te.guidance_ms   = assembled.timer.agg({"guidance_ms"});
                        te.context_ms    = assembled.timer.agg({"context_ms"});
                        te.llm_wait_ms   = assembled.timer.agg({"llm_wait_ms"});
                        te.ttft_ms       = assembled.timer.agg({"ttft_ms"});
                        te.generation_ms = assembled.timer.agg({"generation_ms"});
                        te.total_ms      = assembled.timer.elapsed_ms();
                        te.context_chars  = assembled.context_chars;
                        te.context_chunks = assembled.context_chunks;
                        // -1.0 means server default was used; otherwise the eval override value.
                        te.generation_temperature = eval_temperature;
                        te.detail        = assembled.timer.steps();
                        std::string te_json;
                        if (!glz::write_json(te, te_json))
                            safe_send("data: " + te_json + "\n\n");
                    }
#endif // ASTRAEA_ENABLE_TIMING

                    if (log && route_debug_log && !full_answer.empty()) {
                        RouteDebugEntry rde;
                        rde.ts            = utc_iso8601();
                        rde.request_id    = req_id;
                        rde.q             = q;
                        rde.rewritten     = assembled.rewritten_q;
                        rde.triggered     = assembled.route_triggered;
                        rde.matched_intents = assembled.matched_intents;
                        for (const auto& s : assembled.sources)
                            rde.sources.push_back({s.id, s.score});
                        for (const auto& s : assembled.leg_sources)
                            rde.legislation.push_back({s.id,
                                s.payload.count("title")
                                    ? s.payload.at("title") : std::string{}});
                        rde.answer = full_answer.substr(0, 8000);
                        rde.context_chars  = assembled.context_chars;
                        rde.context_chunks = assembled.context_chunks;
                        rde.mode           = assembled.mode;
                        rde.strategy       = assembled.strategy;
                        std::string rde_json;
                        if (!glz::write_json(rde, rde_json))
                            route_debug_log->append(rde_json);
                    }

                    // Emit legislation-grounded verification event using the
                    // legislation sections already retrieved by retrieve_anchor.
                    // This replaces the Playwright web_verify from Python v1 -
                    // the Qdrant corpus text is the same source the answer is
                    // grounded in, so we show it directly without HTTP scraping.
                    if (!assembled.leg_sources.empty()) {
                        VerificationEvent vev;
                        vev.sections.reserve(assembled.leg_sources.size());
                        for (const auto& s : assembled.leg_sources) {
                            VerificationSection vs;
                            vs.url = s.payload.count("url")   ? s.payload.at("url")   : "";
                            vs.reference = s.payload.count("title") ? s.payload.at("title") : "";
                            const std::string& txt = s.payload.count("text")
                                ? s.payload.at("text") : std::string{};
                            vs.excerpt = txt.size() > 500 ? txt.substr(0, 500) + "..." : txt;
                            if (!vs.url.empty() || !vs.reference.empty())
                                vev.sections.push_back(std::move(vs));
                        }
                        if (!vev.sections.empty()) {
                            std::string vev_json;
                            if (!glz::write_json(vev, vev_json))
                                safe_send("data: " + vev_json + "\n\n");
                        }
                    }

                    // debug_done event: wall-time summary for debug panel.
                    // Mirrors Python core/api.py line 587. Only in debug_mode.
                    if (debug_mode) {
                        DebugDoneEvent dde;
                        dde.generate_ms = assembled.timer.agg({"generation_ms"});
                        dde.total_ms    = assembled.timer.elapsed_ms();
                        std::string dde_json;
                        if (!glz::write_json(dde, dde_json))
                            safe_send("data: " + dde_json + "\n\n");
                    }

                    safe_send("data: {\"type\":\"done\"}\n\n");
                    shared_stream->close();
                });
        });
    resp->setContentTypeCodeAndCustomString(drogon::CT_CUSTOM, "text/event-stream");
    resp->addHeader("Cache-Control",    "no-cache");
    resp->addHeader("X-Accel-Buffering", "no");
    resp->addHeader("X-Request-Id",     req_id);
    cb(resp);
}

// POST /feedback — collect thumbs-up/thumbs-down ratings from users.
//
// Request body: {"question":"...","rating":1-5,"comment":"..."}
// rating is required (1=bad, 5=great). question and comment are free text.
// Per-IP rate gate (IpCooldown, 30 s TTL) prevents trivial spam. Responds
// 200 "ok\n" on success; 400 on bad input; 429 on rate limit.
drogon::HttpResponsePtr feedback_handler(
    const drogon::HttpRequestPtr& req,
    astraea::JsonlWriter*         feedback_log,
    astraea::IpCooldown*          feedback_cooldown)
{
    const std::string req_id = astraea::resolve_request_id(req->getHeader("x-request-id"));

    auto reply = [&req_id](drogon::HttpStatusCode code, std::string body) {
        auto r = text_response(code, std::move(body));
        r->addHeader("X-Request-Id", req_id);
        return r;
    };

    FeedbackRequest parsed{};
    if (auto err = glz::read<glz::opts{.error_on_unknown_keys = false}>(parsed, req->getBody()); err) {
        return reply(drogon::k400BadRequest, "Invalid JSON\n");
    }
    if (parsed.rating < 1 || parsed.rating > 5) {
        return reply(drogon::k400BadRequest, "rating must be 1-5\n");
    }
    if (parsed.question.empty()) {
        return reply(drogon::k400BadRequest, "question is required\n");
    }
    if (feedback_cooldown) {
        const std::string ip = req->getPeerAddr().toIp();
        if (!feedback_cooldown->try_consume(ip)) {
            return reply(drogon::k429TooManyRequests,
                "Feedback rate limit: please wait 30 seconds between submissions.\n");
        }
    }
    if (feedback_log) {
        FeedbackEntry fe{utc_iso8601(), req_id, parsed.question, parsed.rating, parsed.comment};
        std::string fe_json;
        if (!glz::write_json(fe, fe_json))
            feedback_log->append(fe_json);
    }
    return reply(drogon::k200OK, "ok\n");
}

// GET /debug/ping — frontend's debug-mode bootstrap. Returns 200 {"ok":true}
// when X-Debug-Key matches the configured DEBUG_KEY env var; 403 otherwise.
// X-API-Key is ALSO required (the existing auth advice handles that). When
// DEBUG_KEY is unset (empty), every probe gets 403 - debug mode is disabled.
//
// Python parity: core/api.py `@app.get("/debug/ping")`. The frontend (app.js
// _activateDebug) gates the entire debug UI on an OK response from this
// endpoint.
drogon::HttpResponsePtr debug_ping_handler(
    const drogon::HttpRequestPtr& req,
    const std::string&            debug_key)
{
    const std::string req_id = astraea::resolve_request_id(req->getHeader("x-request-id"));
    auto reply = [&req_id](drogon::HttpStatusCode code, std::string body) {
        auto r = text_response(code, std::move(body));
        r->addHeader("X-Request-Id", req_id);
        return r;
    };

    if (debug_key.empty()) {
        // No DEBUG_KEY configured -> debug mode disabled. Same response as
        // a wrong key, so an attacker can't tell whether debug is unset
        // vs the key is wrong.
        return reply(drogon::k403Forbidden, "Invalid debug key.\n");
    }
    const auto& provided = req->getHeader("x-debug-key");
    // Constant-time compare for the same reason as PUBLIC_TOKEN (#38).
    if (provided.size() != debug_key.size() ||
        CRYPTO_memcmp(provided.data(), debug_key.data(), debug_key.size()) != 0) {
        return reply(drogon::k403Forbidden, "Invalid debug key.\n");
    }

    auto resp = drogon::HttpResponse::newHttpResponse();
    resp->setStatusCode(drogon::k200OK);
    resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
    resp->setBody("{\"ok\":true}");
    resp->addHeader("X-Request-Id", req_id);
    return resp;
}

// POST /feedback/full — comprehensive feedback with full request context
// (sources, legislation, timing, debug payloads). Frontend's astraea.js
// `saveFullFeedback` calls this for debug-mode submissions.
//
// The body shape varies by frontend version (~25 typed fields in Python's
// FeedbackFullRequest, several nested dicts/arrays). Single-pass DOM parse:
// read the body as glz::json_t, extract the two fields we validate
// (rating + is_debug), augment with server-side fields, write to JSONL.
// No typed validation struct - that would either need to live outside the
// anonymous namespace (PR #9 glaze-linkage rule) or duplicate the parse.
drogon::HttpResponsePtr feedback_full_handler(
    const drogon::HttpRequestPtr& req,
    const std::string&            jurisdiction_name,
    astraea::JsonlWriter&         feedback_full_log,
    astraea::IpCooldown&          full_cooldown)
{
    const std::string req_id = astraea::resolve_request_id(req->getHeader("x-request-id"));
    auto reply = [&req_id](drogon::HttpStatusCode code, std::string body) {
        auto r = text_response(code, std::move(body));
        r->addHeader("X-Request-Id", req_id);
        return r;
    };

    // Single-pass parse of the body as a generic JSON DOM. Reject anything
    // that isn't an object - the frontend always sends one, and an array or
    // scalar would skip our augment-with-server-fields step.
    glz::json_t entry;
    if (auto err = glz::read_json(entry, req->getBody()); err || !entry.is_object()) {
        return reply(drogon::k400BadRequest, "Invalid JSON\n");
    }
    auto& obj = entry.get_object();

    int  rating   = 0;
    bool is_debug = false;
    if (auto it = obj.find("rating"); it != obj.end() && it->second.is_number())
        rating = static_cast<int>(it->second.get<double>());
    if (auto it = obj.find("is_debug"); it != obj.end() && it->second.is_boolean())
        is_debug = it->second.get<bool>();

    // Python: `if not is_debug and rating not in (1, -1)`. Debug-mode
    // submissions bypass the rating check (context-only, not user feedback).
    if (!is_debug && rating != 1 && rating != -1) {
        return reply(drogon::k400BadRequest, "Rating must be 1 or -1.\n");
    }

    // Per-IP rate gate. Python uses 1 s TTL for /feedback/full (vs 30 s
    // for /feedback) - debug mode fires multiple consecutive submissions.
    const std::string ip = req->getPeerAddr().toIp();
    if (!full_cooldown.try_consume(ip)) {
        return reply(drogon::k429TooManyRequests,
            "Feedback rate limit: please wait briefly between submissions.\n");
    }

    // Augment with server-side fields. Frontend can't be trusted to set
    // these (timestamp, request_id, jurisdiction).
    obj["ts"]           = utc_iso8601();
    obj["request_id"]   = req_id;
    obj["jurisdiction"] = jurisdiction_name;

    std::string out;
    if (!glz::write_json(entry, out))
        feedback_full_log.append(out);

    return reply(drogon::k200OK, "ok\n");
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main() {
    // spdlog setup before anything that might log. Pattern includes
    // millisecond timestamps + log level + message. Runtime level can be
    // overridden via LOG_LEVEL env (trace|debug|info|warn|err|critical|off);
    // default is info. Compile-time SPDLOG_ACTIVE_LEVEL (dev preset sets it
    // to DEBUG, prod leaves it at INFO) controls whether SPDLOG_DEBUG calls
    // are emitted at all.
    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v");
    if (const char* lvl = std::getenv("LOG_LEVEL")) {
        spdlog::set_level(spdlog::level::from_str(lvl));
    } else {
        spdlog::set_level(spdlog::level::info);
    }

    verify_mimalloc_override();

    const auto cfg = astraea::Config::from_env();
    const astraea::nz_tenancy::NZTenancyJurisdiction jurisdiction;

    // Singletons that live for the lifetime of drogon::app().run() and are
    // captured by reference into the request handlers below. Constructing
    // them here proves the long ctor chains succeed before any traffic
    // arrives — a misconfigured URL or model would fail loudly at startup
    // rather than on the first request.
    astraea::RAGPipeline pipeline{
        cfg.qdrant_url,
        jurisdiction.corpus().qdrant_collection,
        cfg.embed_base_url,
        cfg.embed_model,
        cfg.llm_base_url,
        cfg.llm_model,
        cfg.rerank_base_url,                   // reranker endpoint (default :8081 embed server)
        cfg.rerank_model,                      // reranker model (default bge-m3)
        jurisdiction.corpus().courts.empty()   // VectorStore court_name filter
            ? std::string{}
            : jurisdiction.corpus().courts.front(),
        /*embed_dims=*/               cfg.embed_dims,
        /*llm_max_tokens=*/           cfg.llm_max_tokens,
        /*llm_temperature=*/          cfg.llm_temperature,
        /*enable_reranker=*/          cfg.enable_reranker,
        /*enable_thinking=*/          cfg.enable_thinking,
        /*upstream_timeout_s=*/       static_cast<double>(cfg.upstream_timeout_s),
        /*stream_idle_timeout_s=*/    static_cast<double>(cfg.llm_stream_idle_timeout_s),
    };

    // Dedicated Generator for query rewrite: capped at rewrite_max_tokens (100
    // default) so each /ask request doesn't reserve a 2500-token KV cache for
    // a ~10-20 token rewrite, and pinned to T=0.0 so the same input produces
    // the same retrieval query (defeats the "optimised for retrieval" intent
    // otherwise). Same base_url + model as the generation Generator - hits
    // the same LLM server, just with a tighter request body.
    astraea::Generator rewrite_gen{
        cfg.llm_base_url,
        cfg.llm_model,
        cfg.rewrite_max_tokens,
        cfg.rewrite_temperature,
        /*enable_thinking=*/false,
        /*stream_idle_timeout_s=*/static_cast<double>(cfg.llm_stream_idle_timeout_s),
    };

    // Distributed-permit coordinator that caps concurrent generation calls
    // when LLM_GLOBAL_CONCURRENCY > 0. Wraps the final generate() /
    // generate_stream() only (Python core/api.py:553 parity) - retrieval
    // and rewrite stay parallel. nullptr below means unlimited concurrency.
    //
    // Backend selection via COORDINATOR_BACKEND env var:
    //   "in_process" (default) - single-binary deployments
    //   "redis"                - multi-host / cross-process (PR #34)
    std::unique_ptr<astraea::CoordinatorClient> coordinator;
    if (cfg.llm_global_concurrency > 0) {
        if (cfg.coordinator_backend == "redis") {
            coordinator = std::make_unique<astraea::RedisCoordinator>(
                cfg.redis_url, cfg.llm_global_concurrency);
        } else if (cfg.coordinator_backend == "in_process") {
            coordinator = std::make_unique<astraea::InProcessCoordinator>(
                cfg.llm_global_concurrency);
        } else {
            std::fprintf(stderr,
                "FATAL: COORDINATOR_BACKEND='%s' is unknown; expected "
                "'in_process' or 'redis'.\n",
                cfg.coordinator_backend.c_str());
            std::abort();
        }
    }
    astraea::CoordinatorClient* const llm_sem = coordinator.get();

    // Per-IP concurrency limiter. nullptr when IP_MAX_CONCURRENCY == 0 (unlimited).
    std::optional<IpLimiter> ip_lim_storage;
    if (cfg.ip_max_concurrency > 0)
        ip_lim_storage.emplace(cfg.ip_max_concurrency);
    IpLimiter* const ip_lim = ip_lim_storage ? &*ip_lim_storage : nullptr;

    // Optional separate VectorStore for the legislation collection. The
    // anchor pipeline accepts a nullable pointer; nullptr falls back to
    // single-collection mode (vector store from RAGPipeline).
    std::unique_ptr<astraea::VectorStore> leg_store;
    if (!jurisdiction.corpus().leg_collection.empty()) {
        leg_store = std::make_unique<astraea::VectorStore>(
            cfg.qdrant_url,
            jurisdiction.corpus().leg_collection,
            /*court_name=*/std::string{},
            /*timeout_s=*/static_cast<double>(cfg.upstream_timeout_s));
    }

    // Startup contract: every section a route declares as forced_sections
    // (or LOW_PRIORITY_SECTIONS gate key) MUST exist in the legislation
    // corpus. A missing section silently no-ops at runtime — the engine
    // injects nothing, the LLM never sees it, and eval section_recall is
    // capped no matter how perfect the routing is. Fail-fast at boot
    // unless ASTRAEA_ALLOW_MISSING_FORCED_SECTIONS=1 is set (degraded
    // mode for emergency uptime; log loudly).
    if (leg_store) {
        const auto t_val = std::chrono::steady_clock::now();
        auto report = astraea::validate_forced_sections_against_corpus(
            jurisdiction, *leg_store);
        const auto val_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t_val).count();
        LOG_INFO << "route validator: probed " << report.sections_checked
                 << " distinct section ids across " << report.routes_checked
                 << " routes in " << val_ms << "ms";
        if (!report.missing.empty()) {
            for (const auto& m : report.missing) {
                LOG_ERROR << "missing forced section: "
                          << "section=" << m.section_id
                          << " declared_by=" << m.declared_by
                          << " collection=" << jurisdiction.corpus().leg_collection;
            }
            const char* allow_env = std::getenv("ASTRAEA_ALLOW_MISSING_FORCED_SECTIONS");
            const bool allow = allow_env && std::string_view(allow_env) == "1";
            if (!allow) {
                LOG_FATAL << "route validator: "
                          << report.missing.size()
                          << " missing forced section(s) — refusing to start. "
                          << "Set ASTRAEA_ALLOW_MISSING_FORCED_SECTIONS=1 to "
                          << "boot in degraded mode (NOT recommended outside "
                          << "of emergencies).";
                return 1;
            }
            LOG_WARN << "route validator: starting in DEGRADED mode "
                     << "(ASTRAEA_ALLOW_MISSING_FORCED_SECTIONS=1); "
                     << report.missing.size()
                     << " forced section(s) will silently no-op at runtime.";
        }
    }

    // Redis session store. Optional: only constructed when REDIS_URL is set.
    // Keeps turn history per X-Session-Id header across requests so the LLM
    // can refer back to earlier context in the same conversation. Disabled if
    // redis_url is empty (the default). Fail-open: Redis errors never surface
    // to callers - see session.hpp for details.
    std::optional<astraea::SessionStore> session_store_storage;
    if (!cfg.redis_url.empty()) {
        session_store_storage.emplace(
            cfg.redis_url,
            std::string(jurisdiction.name()),
            cfg.session_ttl_s,
            cfg.session_max_turns,
            cfg.session_inject_turns,
            cfg.session_answer_cap);
    }
    astraea::SessionStore* const session_store =
        session_store_storage ? &*session_store_storage : nullptr;

    // JSONL feedback writers. Fail-open: errors are logged and swallowed,
    // never surfaced to callers. All three land in feedback_dir; route_debug
    // gets a larger per-file cap because each entry includes the full answer.
    astraea::JsonlWriter question_log{
        std::filesystem::path(cfg.feedback_dir) / "question_log.jsonl",
        static_cast<std::uintmax_t>(cfg.feedback_max_mb) * 1024ULL * 1024};
    astraea::JsonlWriter route_debug_log{
        std::filesystem::path(cfg.feedback_dir) / "route_debug.jsonl",
        static_cast<std::uintmax_t>(cfg.route_debug_max_mb) * 1024ULL * 1024};
    astraea::JsonlWriter feedback_log{
        std::filesystem::path(cfg.feedback_dir) / "feedback.jsonl",
        static_cast<std::uintmax_t>(cfg.feedback_max_mb) * 1024ULL * 1024};
    // 30 s per-IP cooldown for /feedback to prevent trivial spam.
    astraea::IpCooldown feedback_cooldown{std::chrono::seconds(30)};

    // /feedback/full lands in a separate JSONL because the per-entry size is
    // much larger (full request context, sources, legislation, timing) and
    // the use case is debug-mode capture rather than user-facing feedback.
    // Python ports use feedback_full_max_bytes = 50 MB; we reuse the
    // route_debug_max_mb cap which is also 50 MB by default.
    astraea::JsonlWriter feedback_full_log{
        std::filesystem::path(cfg.feedback_dir) / "feedback_debug.jsonl",
        static_cast<std::uintmax_t>(cfg.route_debug_max_mb) * 1024ULL * 1024};
    // 1 s TTL per Python's feedback_full pattern - lets debug-mode capture
    // multiple consecutive submissions but bounds it.
    astraea::IpCooldown feedback_full_cooldown{std::chrono::seconds(1)};

    // Pre-built RouteTable: AC automaton built once at startup, reused by
    // every request. Live for the process lifetime; safe to capture by
    // const reference into handler lambdas.
    const astraea::RouteTable route_table{jurisdiction.routes()};

    // Deep readiness probe for /healthz. Rerank URL is empty when the
    // reranker is disabled - HealthProber skips that probe in that case.
    astraea::HealthProber health_prober{
        cfg.qdrant_url,
        cfg.llm_base_url,
        cfg.embed_base_url,
        cfg.enable_reranker ? cfg.rerank_base_url : std::string{},
    };

    drogon::app()
        .setLogLevel(trantor::Logger::kInfo)
        .addListener("0.0.0.0", cfg.port)
        .setThreadNum(cfg.thread_count)
        .setClientMaxBodySize(cfg.max_body_bytes);

    // ---------------------------------------------------------------------------
    // Auth: reject requests missing a valid X-API-Key when PUBLIC_TOKEN is set.
    // OPTIONS preflight is exempt — browsers send it before auth headers are
    // attached, and the CORS spec requires a 200 response for the preflight to
    // proceed. If PUBLIC_TOKEN is empty (default) all requests are allowed.
    // ---------------------------------------------------------------------------
    if (!cfg.public_token.empty()) {
        drogon::app().registerSyncAdvice(
            [token = cfg.public_token](const drogon::HttpRequestPtr& req)
            -> drogon::HttpResponsePtr {
                if (req->method() == drogon::Options) return nullptr;
                const auto& path = req->getPath();
                if (path == "/health" || path == "/healthz") return nullptr;
                if (path == "/token") return nullptr;
                if (path == "/" || path == "/index.html"
                    || path == "/favicon.svg" || path == "/robots.txt") return nullptr;
                if (path.starts_with("/static/")) return nullptr;
                const auto& provided = req->getHeader("x-api-key");
                // Constant-time compare prevents timing side-channel: an
                // attacker measuring response latency cannot recover the
                // token byte-by-byte via std::string::operator!= early-exit.
                // Length mismatch leaks only that the length is wrong, which
                // is acceptable (token length is not a secret).
                const bool ok = provided.size() == token.size() &&
                    CRYPTO_memcmp(provided.data(), token.data(), token.size()) == 0;
                if (!ok) {
                    auto resp = drogon::HttpResponse::newHttpResponse();
                    resp->setStatusCode(drogon::k401Unauthorized);
                    resp->setContentTypeCode(drogon::CT_TEXT_PLAIN);
                    resp->setBody("Unauthorized\n");
                    return resp;
                }
                return nullptr;
            });
    }

    // ---------------------------------------------------------------------------
    // CORS: attach Access-Control-* headers to every response so browsers can
    // reach the API from a different origin. ALLOWED_ORIGIN defaults to "*";
    // set it to the exact frontend origin in production.
    // ---------------------------------------------------------------------------
    drogon::app().registerPreSendingAdvice(
        [origin = cfg.allowed_origin](const drogon::HttpRequestPtr&,
                                      const drogon::HttpResponsePtr& resp) {
            // Remove before add: addHeader appends and browsers reject duplicate
            // Access-Control-Allow-Origin values (the others are CSV-mergeable).
            resp->removeHeader("Access-Control-Allow-Origin");
            resp->addHeader("Access-Control-Allow-Origin",  origin);
            resp->addHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
            resp->addHeader("Access-Control-Allow-Headers",
                            "Content-Type, X-API-Key, Cache-Control, X-Session-Id, X-No-Log, X-Request-Id, X-Debug-Key");
            // Expose-Headers is the read side: by default XHR/fetch only sees
            // a small whitelist of response headers. X-Request-Id must be
            // exposed explicitly so browser clients can pick it up for
            // correlation / display in error UI.
            resp->addHeader("Access-Control-Expose-Headers", "X-Request-Id");
        });

    // ---------------------------------------------------------------------------
    // Security headers: standard defensive defaults applied to every response.
    // Defending against common browser-side attack classes:
    //   - X-Content-Type-Options: nosniff
    //       Prevents MIME-sniffing. Critical for SSE (text/event-stream) and
    //       JSON responses - stops a browser from "helpfully" treating a JSON
    //       response as HTML when an attacker controls part of the payload.
    //   - X-Frame-Options: DENY
    //       Refuses iframe embedding entirely - clickjacking mitigation.
    //   - Referrer-Policy: strict-origin-when-cross-origin
    //       Cross-origin requests reveal only the origin, not the full URL
    //       (which can contain session ids, /ask?q=... query content, etc.).
    //   - X-XSS-Protection: 0
    //       Explicit opt-out of the legacy IE/old-Chrome XSS auditor. The
    //       auditor is itself an attack surface; modern guidance is to
    //       disable it via this exact value rather than omit the header.
    //   - Strict-Transport-Security: opt-in via HSTS_MAX_AGE_S > 0
    //       Only meaningful when TLS termination is in front of this binary
    //       (Cloudflare Tunnel, nginx, etc). 0 (default) skips the header
    //       entirely - sending HSTS over plain HTTP is silently ignored by
    //       browsers but pollutes logs/audits with a misleading directive.
    //
    // CSP / Permissions-Policy intentionally omitted at the server level -
    // they're HTML-response concerns and the frontend can set them via meta
    // tags. Adding broad CSP at the server breaks the SSE / JSON API paths.
    // ---------------------------------------------------------------------------
    drogon::app().registerPreSendingAdvice(
        [hsts_max_age = cfg.hsts_max_age_s](
            const drogon::HttpRequestPtr&,
            const drogon::HttpResponsePtr& resp) {
            resp->addHeader("X-Content-Type-Options", "nosniff");
            resp->addHeader("X-Frame-Options",        "DENY");
            resp->addHeader("Referrer-Policy",        "strict-origin-when-cross-origin");
            resp->addHeader("X-XSS-Protection",       "0");
            if (hsts_max_age > 0) {
                resp->addHeader("Strict-Transport-Security",
                    "max-age=" + std::to_string(hsts_max_age) + "; includeSubDomains");
            }
        });

    drogon::app().registerHandler("/health",
        [jur_name = std::string{jurisdiction.name()}](
            const drogon::HttpRequestPtr&,
            std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            cb(health_handler(jur_name));
        }, {drogon::Get});

    // Deep readiness probe. /health stays as the fast liveness check
    // (no upstream calls); /healthz pings every dependency and returns
    // a per-dep JSON status with HTTP 200 (ok) or 503 (any down).
    // Coroutine-via-async_run pattern for the same Drogon FunctionTraits
    // reason as /ask: captured lambdas cannot use the coroutine-style
    // registerHandler overload.
    drogon::app().registerHandler("/healthz",
        [&health_prober, llm_sem, &pipeline](
            const drogon::HttpRequestPtr&,
            std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            drogon::async_run(
                [cb = std::move(cb), &health_prober, llm_sem, &pipeline]()
                -> drogon::Task<> {
                    auto resp = co_await healthz_handler(
                        health_prober, llm_sem, pipeline.generator().stream_pool());
                    cb(resp);
                });
        }, {drogon::Get});

    // Real /ask handler. The OUTER lambda is callback-style (which Drogon's
    // FunctionTraits accepts with captures); the INNER coroutine via
    // drogon::async_run runs the multi-stage retrieve / anchor / guidance /
    // generate chain. Drogon's coroutine-style registerHandler overload does
    // not accept captured lambdas (FunctionTraits rejects them with
    // "no type named first_param_type"), so we cannot use that form here.
    // Pipeline, leg_store, jurisdiction, and route_table are all stack-bound
    // to drogon::app().run()'s lifetime; capturing by reference is safe.
    drogon::app().registerHandler("/ask",
        [&pipeline, &rewrite_gen, llm_sem, llm_to = cfg.llm_acquire_timeout_s, ip_lim,
         leg_ptr = leg_store.get(), &jurisdiction, &route_table, session_store,
         &question_log, &route_debug_log, &health_prober,
         debug_key = cfg.debug_key](
            const drogon::HttpRequestPtr& req,
            std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            drogon::async_run(
                [req, cb = std::move(cb), &pipeline, &rewrite_gen, llm_sem, llm_to, ip_lim,
                 leg_ptr, &jurisdiction, &route_table, session_store,
                 &question_log, &route_debug_log, &health_prober, debug_key]() -> drogon::Task<> {
                    auto resp = co_await ask_handler(
                        req, pipeline, rewrite_gen, llm_sem, llm_to, ip_lim, leg_ptr,
                        jurisdiction, route_table, session_store,
                        &question_log, &route_debug_log, health_prober, debug_key);
                    cb(resp);
                });
        }, {drogon::Post});

    drogon::app().registerHandler("/ask/stream",
        [&pipeline, &rewrite_gen, llm_sem, llm_to = cfg.llm_acquire_timeout_s, ip_lim,
         leg_ptr = leg_store.get(), &jurisdiction, &route_table, session_store,
         &question_log, &route_debug_log, &health_prober,
         debug_key = cfg.debug_key](
            const drogon::HttpRequestPtr& req,
            std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            ask_stream_handler(req, std::move(cb), pipeline, rewrite_gen, llm_sem, llm_to,
                                ip_lim, leg_ptr, jurisdiction, route_table, session_store,
                                &question_log, &route_debug_log, health_prober, debug_key);
        }, {drogon::Post});

    drogon::app().registerHandler("/feedback",
        [&feedback_log, &feedback_cooldown](
            const drogon::HttpRequestPtr& req,
            std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            cb(feedback_handler(req, &feedback_log, &feedback_cooldown));
        }, {drogon::Post});

    // /feedback/full: comprehensive frontend debug feedback. Body shape varies
    // by frontend version - we validate rating/is_debug and pass-through the
    // rest as raw JSON, augmented server-side with ts/request_id/jurisdiction.
    drogon::app().registerHandler("/feedback/full",
        [&feedback_full_log, &feedback_full_cooldown,
         jur_name = std::string{jurisdiction.name()}](
            const drogon::HttpRequestPtr& req,
            std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            cb(feedback_full_handler(req, jur_name,
                                     feedback_full_log, feedback_full_cooldown));
        }, {drogon::Post});

    // /debug/ping: frontend's debug-mode bootstrap. Returns 200 if the
    // X-Debug-Key header matches Config::debug_key, 403 otherwise. The
    // existing X-API-Key auth advice still gates the call - both headers
    // are required when PUBLIC_TOKEN is set.
    drogon::app().registerHandler("/debug/ping",
        [debug_key = cfg.debug_key](
            const drogon::HttpRequestPtr& req,
            std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            cb(debug_ping_handler(req, debug_key));
        }, {drogon::Get});

    // CORS preflight — 200 OK with no body; the pre-sending advice above adds
    // the Access-Control-* headers automatically.
    auto options_cb = [](const drogon::HttpRequestPtr&,
                          std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
        cb(drogon::HttpResponse::newHttpResponse());
    };
    drogon::app().registerHandler("/health",        options_cb, {drogon::Options});
    drogon::app().registerHandler("/healthz",       options_cb, {drogon::Options});
    drogon::app().registerHandler("/ask",           options_cb, {drogon::Options});
    drogon::app().registerHandler("/ask/stream",    options_cb, {drogon::Options});
    drogon::app().registerHandler("/feedback",      options_cb, {drogon::Options});
    drogon::app().registerHandler("/feedback/full", options_cb, {drogon::Options});
    drogon::app().registerHandler("/debug/ping",    options_cb, {drogon::Options});

    // MCP Streamable-HTTP server: POST /mcp (JSON-RPC 2.0, stateless).
    // OPTIONS /mcp is registered inside register_mcp_handler.
    astraea::detail::nz_tenancy_app::register_mcp_handler(
        pipeline, leg_store.get(), jurisdiction, cfg.embed_dims,
        llm_sem, cfg.llm_acquire_timeout_s);

    // /token: returns the public API token so the browser frontend can
    // authenticate subsequent /ask and /ask/stream requests. No auth required
    // (it is itself the bootstrap endpoint that provides the token).
    drogon::app().registerHandler("/token",
        [token = cfg.public_token](
            const drogon::HttpRequestPtr&,
            std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            std::string body;
            (void)glz::write_json(TokenResp{token}, body);
            auto resp = drogon::HttpResponse::newHttpResponse();
            resp->setStatusCode(drogon::k200OK);
            resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
            resp->setBody(std::move(body));
            cb(resp);
        }, {drogon::Get});

    // Static frontend: serve index.html + /static/* when STATIC_DIR is set.
    // The document root also handles GET / -> index.html automatically.
    // The separate astraea.js lives one level above the jurisdiction static dir
    // (core/frontend/astraea.js), so we register it as a dedicated handler.
    if (!cfg.static_dir.empty()) {
        namespace fs = std::filesystem;
        const fs::path sdir{cfg.static_dir};

        // GET / -> index.html. Registered explicitly because index.html lives
        // inside sdir, but the document root is set to sdir's parent so that
        // /static/* URL prefix maps to the sdir filesystem path.
        const fs::path index_html = sdir / "index.html";
        drogon::app().registerHandler("/",
            [p = index_html.string()](
                const drogon::HttpRequestPtr&,
                std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
                cb(drogon::HttpResponse::newFileResponse(p));
            }, {drogon::Get});

        // /static/astraea/astraea.js lives outside sdir (shared core/frontend/).
        const fs::path astraea_js = sdir.parent_path().parent_path().parent_path()
                                    / "core" / "frontend" / "astraea.js";
        if (fs::exists(astraea_js)) {
            const std::string ajs_content = [&]{
                std::ifstream f(astraea_js);
                return std::string(std::istreambuf_iterator<char>(f), {});
            }();
            drogon::app().registerHandler("/static/astraea/astraea.js",
                [body = ajs_content](
                    const drogon::HttpRequestPtr&,
                    std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
                    auto resp = drogon::HttpResponse::newHttpResponse();
                    resp->setContentTypeCode(drogon::CT_TEXT_JAVASCRIPT);
                    resp->setBody(body);
                    cb(resp);
                }, {drogon::Get});
        }

        // Document root = parent of sdir so that /static/* URLs map to
        // sdir/* files (e.g. /static/app.js -> sdir/app.js).
        drogon::app().setDocumentRoot(sdir.parent_path().string());
    }

    LOG_INFO << "jurisdiction: " << jurisdiction.name()
             << " (" << jurisdiction.description() << ")";
    LOG_INFO << "routes:       " << jurisdiction.routes().size()
             << "; leg_sources: " << jurisdiction.leg_sources().size();
    LOG_INFO << "listener:     0.0.0.0:" << cfg.port
             << " (" << cfg.thread_count << " threads, max_body="
             << cfg.max_body_bytes << " bytes)";
    LOG_INFO << "upstreams:";
    LOG_INFO << "  qdrant " << cfg.qdrant_url
             << " (collections: " << jurisdiction.corpus().qdrant_collection
             << (jurisdiction.corpus().leg_collection.empty()
                 ? std::string{}
                 : ", " + jurisdiction.corpus().leg_collection)
             << ")";
    LOG_INFO << "  embed  " << cfg.embed_base_url   << " (" << cfg.embed_model  << ")";
    LOG_INFO << "  rerank " << cfg.rerank_base_url  << " (" << cfg.rerank_model << ")";
    LOG_INFO << "  llm    " << cfg.llm_base_url     << " (" << cfg.llm_model
             << ", thinking=" << (cfg.enable_thinking ? "on" : "off") << ")";
    LOG_INFO << "auth:         "
             << (cfg.public_token.empty() ? "disabled (PUBLIC_TOKEN not set)"
                                          : "X-API-Key required");
    LOG_INFO << "cors:         Access-Control-Allow-Origin: " << cfg.allowed_origin;
    LOG_INFO << "security:     X-Content-Type-Options, X-Frame-Options=DENY, "
             << "Referrer-Policy, X-XSS-Protection=0"
             << (cfg.hsts_max_age_s > 0
                 ? ", HSTS=" + std::to_string(cfg.hsts_max_age_s) + "s"
                 : ", HSTS=off");
    LOG_INFO << "rate_limit:   "
             << (cfg.ip_max_concurrency > 0
                 ? "per-IP max=" + std::to_string(cfg.ip_max_concurrency)
                 : "disabled (IP_MAX_CONCURRENCY=0)");
    if (llm_sem) {
        LOG_INFO << "coordinator:  " << llm_sem->backend_name()
                 << " (max=" << llm_sem->max_concurrency()
                 << ", acquire_timeout=" << cfg.llm_acquire_timeout_s << "s)";
    } else {
        LOG_INFO << "coordinator:  none (LLM_GLOBAL_CONCURRENCY=0; unlimited)";
    }
    if (session_store) {
        LOG_INFO << "session:      redis " << cfg.redis_url
                 << " (ttl=" << cfg.session_ttl_s
                 << "s, max_turns=" << cfg.session_max_turns
                 << ", inject=" << cfg.session_inject_turns
                 << ", answer_cap=" << cfg.session_answer_cap << ")";
    } else {
        LOG_INFO << "session:      disabled (REDIS_URL not set)";
    }
    LOG_INFO << "feedback_dir: " << cfg.feedback_dir
             << " (question_log/route_debug max=" << cfg.feedback_max_mb << "/"
             << cfg.route_debug_max_mb << " MB, feedback max=" << cfg.feedback_max_mb << " MB)";
    LOG_INFO << "endpoints:";
    LOG_INFO << "  GET/OPTIONS  /health        - liveness probe (no upstream checks)";
    LOG_INFO << "  GET/OPTIONS  /healthz       - readiness probe (pings qdrant + llm + embed + rerank)";
    LOG_INFO << "  POST/OPTIONS /ask           - real RAG (retrieve + anchor + guidance + generate)";
    LOG_INFO << "  POST/OPTIONS /ask/stream    - real RAG + SSE token stream (true per-token, Phase 6D)";
    LOG_INFO << "  POST/OPTIONS /feedback      - user rating submission (1-5 stars, 30 s per-IP)";
    LOG_INFO << "  POST/OPTIONS /feedback/full - debug-mode feedback with full context (1 s per-IP)";
    LOG_INFO << "  GET/OPTIONS  /debug/ping    - debug-mode gate ("
             << (cfg.debug_key.empty() ? "DISABLED: DEBUG_KEY not set" : "X-Debug-Key required")
             << ")";

    drogon::app().run();
    return 0;
}
