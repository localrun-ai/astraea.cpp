// hello_sse — Drogon SSE streaming spike.
//
// Purpose: validate the async/coroutine + streaming spine before any business
// logic lands on top. Two endpoints:
//
//   GET /health      → "ok"  (sanity-check handler path)
//   GET /sse/hello   → streams 20 "data: chunk N\n\n" events at 200ms apart,
//                       then "data: [DONE]\n\n", and closes the stream.
//
// Test from another terminal:
//
//   curl -N http://localhost:8080/sse/hello
//
// `-N` disables curl's output buffering so you see each chunk arrive in
// real time — the whole point of the spike.
//
// Configurable via env:
//   ASTRAEA_HELLO_SSE_PORT    default 8080
//   ASTRAEA_HELLO_SSE_THREADS default 2

#include <drogon/drogon.h>
#include <drogon/HttpResponse.h>
#include <trantor/net/EventLoop.h>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

namespace {

int env_int(const char* key, int def) {
    const char* v = std::getenv(key);
    if (!v) return def;
    try { return std::stoi(v); } catch (...) { return def; }
}

drogon::HttpResponsePtr health_handler() {
    auto resp = drogon::HttpResponse::newHttpResponse();
    resp->setStatusCode(drogon::k200OK);
    resp->setBody("ok\n");
    resp->setContentTypeCode(drogon::CT_TEXT_PLAIN);
    return resp;
}

// Build the SSE response. The outer coroutine returns the response object;
// Drogon then invokes the inner stream callback once it has wired the
// response to the socket. The inner callback hands off to a coroutine that
// emits chunks with non-blocking sleeps so the event loop is never stalled.
//
// IMPORTANT: capture the CURRENT-THREAD event loop before any suspension.
// drogon::app().getLoop() returns the main app loop, but with setThreadNum>1
// the connection lives on an I/O-thread loop. Sleeping on the main loop
// would resume the coroutine on the main thread, and a subsequent
// stream->send() would then fire from the main thread on a socket owned by
// an I/O thread - thread-unsafe under load.
drogon::HttpResponsePtr sse_hello_response() {
    auto resp = drogon::HttpResponse::newAsyncStreamResponse(
        [](drogon::ResponseStreamPtr stream) {
            drogon::async_run(
                [stream = std::move(stream)]() -> drogon::Task<> {
                    auto* loop = trantor::EventLoop::getEventLoopOfCurrentThread();
                    for (int i = 0; i < 20; ++i) {
                        std::string chunk = "data: chunk " + std::to_string(i) + "\n\n";
                        stream->send(chunk);
                        co_await drogon::sleepCoro(loop, std::chrono::milliseconds(200));
                    }
                    stream->send("data: [DONE]\n\n");
                    stream->close();
                });
        });
    resp->setContentTypeCodeAndCustomString(drogon::CT_CUSTOM, "text/event-stream");
    resp->addHeader("Cache-Control", "no-cache");
    resp->addHeader("X-Accel-Buffering", "no"); // disable nginx response buffering
    return resp;
}

} // namespace

int main() {
    const int port    = env_int("ASTRAEA_HELLO_SSE_PORT",    8080);
    const int threads = env_int("ASTRAEA_HELLO_SSE_THREADS", 2);

    drogon::app()
        .setLogLevel(trantor::Logger::kInfo)
        .addListener("0.0.0.0", port)
        .setThreadNum(threads);

    drogon::app().registerHandler(
        "/health",
        [](const drogon::HttpRequestPtr& /*req*/,
           std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
            callback(health_handler());
        },
        {drogon::Get});

    drogon::app().registerHandler(
        "/sse/hello",
        [](const drogon::HttpRequestPtr& /*req*/,
           std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
            callback(sse_hello_response());
        },
        {drogon::Get});

    LOG_INFO << "hello_sse listening on 0.0.0.0:" << port
             << " (" << threads << " threads)";
    LOG_INFO << "try: curl -N http://localhost:" << port << "/sse/hello";

    drogon::app().run();
    return 0;
}
