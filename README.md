# Pinto21 Microservice Module

`p21@npinto21/microservice@v1` is the production-oriented HTTP and microservice module for Pinto21.

## Host integration context

Inside the Pinto21 workspace, the host-side module infrastructure now lives in:

- `src/modules/manifest.*`
- `src/modules/package_resolver.*`
- `src/modules/native/module_native_*`
- `src/modules/native/core_native_packages.*`
- `src/cli/mod_main.c`

This module is not part of the core runtime. It is loaded through that module infrastructure and exposes its API via package code plus `native/` bindings.

The goal of this module is simple application code with a strong operational base:

- routing and middleware similar to Express and Gin
- structured JSON logging
- Prometheus-style metrics
  - total requests
  - requests in progress
  - cancelled requests
  - method counters
  - response status-class counters
  - route counters
  - simple duration buckets
- OpenTelemetry-ready tracing
  - OTLP/HTTP JSON trace export
  - OTLP/HTTP JSON structured log export
- health/readiness endpoints
- safe defaults for body size, timeouts, and graceful shutdown
- request-scoped resilience and cancellation propagation
- default listen port `8021` to align with Pinto21 branding

## Status

This repository is currently the first functional layer of the module:

- module manifest and native integration points are in place
- the public Pinto21 wrapper API is scaffolded
- the C core is split into `server`, `router`, `context`, `middleware`, `observability`, and `bindings`
- the current C implementation already serves a production-minded MVP subset:
  - `GET`, `POST`, `HEAD`, and `OPTIONS`
  - `/health`, `/ready`, and `/metrics`
  - path params, query parsing, and body parsing
  - structured request logging and trace correlation
  - non-blocking listener with request/read timeout checks

That split is deliberate. The architecture is now stable enough to implement the real server core incrementally without redesigning the module API every time.

## HTTP Standards Strategy

This module should be standards-aligned, but it should not try to implement every edge of HTTP on day one.

The practical strategy is:

- use modern HTTP semantics as the reference baseline
- implement a production-strong subset first
- expand coverage incrementally as features are needed and validated

For Pinto21, that means:

- use HTTP semantics from RFC 9110
- use HTTP/1.1 message syntax and connection behavior from RFC 9112
- treat RFC 7231 as historical context, since it has been obsoleted

The first robust subset should cover:

- `GET`, `POST`, `PUT`, `DELETE`, `HEAD`, `OPTIONS`
- core success and error status codes
- `Content-Type`, `Content-Length`, `Accept`, `Authorization`, `User-Agent`
- JSON request/response handling
- request timeouts, body limits, keep-alive policy, and connection close behavior
- path params, query params, headers, and request body parsing

That subset is enough to support serious microservices. Features such as full content negotiation, advanced conditional requests, caching semantics, and less common protocol behaviors can be added later without blocking the first production-ready microservice slice.

## Target Pinto21 API

The intended long-term developer experience is:

```pinto21
package main

use microservice

server := microservice.new({
    port: 8021,
    host: "0.0.0.0"
})

server.get("/health", func(ctx) {
    ctx.status(200)
    ctx.json({ok: true})
})

server.start()
```

## Resilience Model

The module is being designed with resilience as a first-class concern.

### Service-level resilience

Global service configuration should cover:

- `timeout`
- `request_timeout`
- `read_timeout`
- `write_timeout`
- `idle_timeout`
- `shutdown_timeout`
- `max_body_size`
- `downstream_timeout`
- `retries`
- `backoff_ms`
- circuit-breaker policy

Example:

```pinto21
server := microservice.New({
    port: 8021,
    host: "0.0.0.0",
    timeout: 30,
    request_timeout: 10,
    read_timeout: 10,
    write_timeout: 10,
    idle_timeout: 60,
    shutdown_timeout: 15,
    downstream_timeout: 3,
    retries: 2,
    backoff_ms: 200
})
```

Timeout precedence:

- `request_timeout`, `read_timeout`, and `write_timeout` override everything else when explicitly set
- `timeout` acts as the shared default for those three values
- if `timeout` is omitted, the runtime falls back to `30` seconds

So:

```pinto21
{
    timeout: 30,
    request_timeout: 5
}
```

means:

- `request_timeout = 5`
- `read_timeout = 30`
- `write_timeout = 30`

### Request-scoped cancellation

Each request context is intended to carry:

- deadline
- cancellation state
- cancellation reason
- request and trace correlation

Current wrapper API shape:

```pinto21
if microservice.CtxIsCancelled(ctx) {
    reason := microservice.CtxCancelReason(ctx)
}
```

The intended runtime rule is:

- if a request exceeds its deadline, processing should time out
- cancellation should propagate to downstream work
- handlers and middleware should be able to stop early instead of continuing wasted work

Current practical note:

- timeout and shutdown cancellation already propagate through request reading, middleware execution, context access, and response writing
- a Pinto21 handler that blocks inside its own execution is not preempted mid-instruction by the runtime today
- once that handler yields again, the context is marked cancelled and the response is converted to timeout or shutdown error as appropriate
- `TestSleepMs(...)` exists as a module test helper to prove this behavior with a real blocking handler scenario

### Server lifecycle visibility

The module now exposes `ServerState(server)` for operational inspection during a POC:

- `running`
- `ready`
- `stopping`
- `draining`
- `active_connections`
- `shutdown_deadline_ms`
- `accepting`
- `stopped`
- `metrics_enabled`
- `tracing_enabled`
- `otlp_endpoint`
- `traces_endpoint`
- `logs_endpoint`
- `otlp_timeout_ms`
- `otlp_retry_count`
- `otlp_backoff_ms`
- `otlp_service_name`
- `otlp_logs_enabled`
- `otlp_protocol`
- `otlp_traces_protocol`
- `otlp_logs_protocol`

This is useful while hardening graceful shutdown and timeout propagation.

## Current Wrapper API

Because Pinto21 currently exposes public package members with exported names, the scaffold uses `UpperCamelCase` wrappers:

```pinto21
package p21

use p21.npinto21.microservice

server := microservice.New({
    port: 8021,
    host: "0.0.0.0"
})

microservice.Get(server, "/health", func(ctx) {
    microservice.Ok(ctx, {ok: true})
})

microservice.EnableHealth(server)
microservice.EnableMetrics(server)
microservice.EnableTracing(server)
microservice.Start(server)
```

This maps directly to the final lowercase/object-method API we want to reach.

Current ergonomic aliases already available:

- `Logger()`, `Cors(...)`, `Compress()`, `RateLimit(...)`
- `UseLogger(...)`, `UseCors(...)`, `UseCompress(...)`, `UseRateLimit(...)`
- `Header(...)`, `Query(...)`, `Param(...)`, `Body(...)`
- `Status(...)`, `Json(...)`, `SetHeader(...)`
- `Ok(...)`, `Created(...)`, `NoContent(...)`, `ErrorJson(...)`
- `HasError(...)`, `ErrorKind(...)`, `ErrorMessage(...)`, `ErrorSource(...)`

## Runtime Error Object

Runtime and native multi-result calls now return a stable error object instead of an untyped value.

Current shape:

```pinto21
{
    kind: "json_error",
    message: "chave JSON tem de ser string",
    source: "json.parse",
    detail: null
}
```

That means you can safely reuse `err` as an `obj` across calls:

```pinto21
err: obj = {}

value, err := json.parse("{\"ok\": true}")
other, err := json.parse("{")

if microservice.HasError(err) {
    kind := microservice.ErrorKind(err)
    message := microservice.ErrorMessage(err)
}
```

This is also now the recommended contract for runtime, native, and module-facing error values.

## Architecture

## Instance Safety And Package Isolation

This module must remain safe under Pinto21's package singleton model.

That means:

- the `microservice` package acts as a namespace, not as a global server instance
- each `New(...)` call must allocate and return an isolated `MS_Server`
- groups, contexts, middleware state, and observability state must remain attached to a specific server instance
- mutable runtime state must not be stored in package-level globals

In practice, two different packages importing the module must be able to do this safely:

```pinto21
server_a := microservice.New({ port: 8021 })
server_b := microservice.New({ port: 9090 })
```

without sharing route tables, middleware chains, context state, or request lifecycle data by accident.

This is one of the core safety rules of the module and should be preserved in every future implementation slice.

### Native layout

- `native/microservice.h`
  Core public C API shared by the module files.
- `native/server.c/h`
  Server lifecycle, listeners, configuration, route registration, graceful shutdown.
- `native/router.c/h`
  Route registration and path matching. The intended production structure is a radix tree or trie with parameter extraction.
- `native/context.c/h`
  Request/response context, headers, params, query access, JSON response helpers.
- `native/middleware.c/h`
  Middleware chain execution with global -> group -> route ordering.
- `native/observability.c/h`
  JSON logging, metrics, tracing hooks, health checks.
- `native/bindings.c`
  C bridge entrypoint for the Pinto21 module runtime.
- `native/module_native_microservice_registry.c`
  Declares the `microservice_native` package surface inside the module registry provider.
- `native/module_native_microservice_runtime.c`
  Converts Pinto21 values into C structs and returns Pinto21-friendly objects and results.

### Runtime model

The intended runtime shape is:

- `MS_Server` owns routes, middleware, config, metrics, tracer, and listener state
- `MS_Context` represents one request pipeline execution
- `MS_Context` also carries cancellation and deadline information
- middleware order is:
  - global middleware
  - group middleware
  - route middleware
  - handler
  - optional post-processing

### Observability model

The observability layer is intended to expose:

- structured JSON logs
- trace and request correlation IDs
- Prometheus-compatible metrics
- OTLP/HTTP export for traces and logs
- health and readiness checks

### OTLP production profile

The current module uses a pragmatic production profile:

- metrics stay on `/metrics` in Prometheus format
- traces are exported with OTLP/HTTP JSON to `/v1/traces`
- structured logs can also be exported with OTLP/HTTP JSON to `/v1/logs`
- request logs still go to stdout as JSON, so local debugging and container logs remain useful even if the collector is down
- exporter retries use a small bounded retry loop with backoff
- collector timeouts are short by default, so observability export does not dominate request latency

This keeps the module simple and robust:

- Prometheus handles metrics scraping well
- OTLP handles traces and logs well
- the server does not need a second runtime or agent inside the process

### OTLP environment variables

The current exporter recognizes:

- `PINTO21_OTEL_ENDPOINT`
  fallback base OTLP endpoint when `OTEL_EXPORTER_OTLP_ENDPOINT` is not set
- `OTEL_EXPORTER_OTLP_ENDPOINT`
  base OTLP endpoint, for example `http://otel-collector:4318`
- `OTEL_EXPORTER_OTLP_TRACES_ENDPOINT`
  explicit traces endpoint, for example `http://otel-collector:4318/v1/traces`
- `OTEL_EXPORTER_OTLP_LOGS_ENDPOINT`
  explicit logs endpoint, for example `http://otel-collector:4318/v1/logs`
- `OTEL_EXPORTER_OTLP_HEADERS`
  common OTLP headers in `key=value,key2=value2` form
- `OTEL_EXPORTER_OTLP_TRACES_HEADERS`
  trace-specific OTLP headers
- `OTEL_EXPORTER_OTLP_LOGS_HEADERS`
  log-specific OTLP headers
- `OTEL_EXPORTER_OTLP_TIMEOUT`
  common exporter timeout in milliseconds
- `OTEL_EXPORTER_OTLP_PROTOCOL`
  common exporter protocol. The current module supports `http/json`
- `OTEL_EXPORTER_OTLP_TRACES_TIMEOUT`
  trace exporter timeout in milliseconds
- `OTEL_EXPORTER_OTLP_TRACES_PROTOCOL`
  trace exporter protocol. The current module supports `http/json`
- `OTEL_EXPORTER_OTLP_LOGS_TIMEOUT`
  log exporter timeout in milliseconds
- `OTEL_EXPORTER_OTLP_LOGS_PROTOCOL`
  log exporter protocol. The current module supports `http/json`
- `OTEL_SERVICE_NAME`
  service name attached to OTLP resources
- `PINTO21_OTEL_RETRIES`
  bounded retry count for OTLP export
- `PINTO21_OTEL_BACKOFF_MS`
  linear backoff between retries in milliseconds
- `PINTO21_OTEL_EXPORT_LOGS`
  `1/true` to export structured logs to OTLP, `0/false` to keep them only on stdout

Recommended baseline for production:

```text
OTEL_SERVICE_NAME=users-service
OTEL_EXPORTER_OTLP_ENDPOINT=http://otel-collector:4318
OTEL_EXPORTER_OTLP_PROTOCOL=http/json
OTEL_EXPORTER_OTLP_HEADERS=authorization=Bearer my-token
OTEL_EXPORTER_OTLP_TIMEOUT=10000
PINTO21_OTEL_RETRIES=2
PINTO21_OTEL_BACKOFF_MS=200
PINTO21_OTEL_EXPORT_LOGS=true
```

### OTLP operational notes

What is already strong in the current POC:

- short exporter timeout
- bounded retries with backoff
- retry support for transient failures and `Retry-After`
- custom headers support for authenticated collectors
- independent endpoints for traces and logs
- explicit OTLP protocol visibility in `ServerState(server)`
- server-side visibility through `ServerState(server)`

What is intentionally still simple:

- no in-process batching queue yet
- no OTLP metrics export, because metrics are exposed through Prometheus scraping
- no async background exporter thread
- no `grpc` exporter path in this module
- no `http/protobuf` exporter path in this module yet

That tradeoff is deliberate for the first production-oriented Pinto21 microservice slice: fewer moving parts, clear operational behavior, and easier failure handling.

## Integration testing

The module now includes a repeatable local OTLP integration test:

```sh
make microservice-otlp-integration
```

That test:

- prepares the module
- starts a local OTLP capture server on `127.0.0.1:43218`
- starts the Pinto21 `health_server`
- performs a real `GET /health`
- verifies that the module exported:
  - `POST /v1/logs`
  - `POST /v1/traces`
- validates the captured payload shape for both signals

Environment overrides:

- `P21_OTLP_TEST_PORT`
  local collector port for the integration test
- `P21_MS_TEST_PORT`
  local microservice port, default `8021`
- `PYTHON_BIN`
  Python interpreter used for the capture helper
- `CURL_BIN`
  curl binary used for the HTTP probe

## POC release readiness

For the current POC, the minimum release confidence checklist is:

- semantic examples pass
- the module prepares with `pinto21-mod prepare-module`
- `/health`, `/ready`, and `/metrics` work locally
- the OTLP integration test passes with:
  - `make microservice-otlp-integration`

Recommended pre-release flow:

```sh
make step-interpreter step-semantic step-modtool
./build/bin/pinto21-mod prepare-module modules/npinto21/microservice/module.p21
./build/bin/pinto21-semantic modules/npinto21/microservice/examples/health_server.p21
make microservice-otlp-integration
```

Files that should normally be committed for the module repository:

- `module.p21`
- `README.md`
- `packages/...`
- `native/...`
- `examples/...`
- `tests/...`
- `.gitignore`

Files that should not be part of the published module repository:

- `.p21/`
- local build artifacts
- local temporary files

## Recommended implementation phases

### Phase 1

- config parsing
- server handle lifecycle
- route registration
- middleware registration
- structured logger hooks
- health/ready endpoints

### Phase 2

- real HTTP parser and non-blocking socket loop
- request context population
- response writing
- route parameters and query parsing

Current status:

- implemented

### Phase 3

- metrics
- tracing
- gzip/deflate
- graceful shutdown
- body limits and timeouts

Current status:

- metrics endpoint baseline implemented
- metrics now include method and status-class counters plus cancelled request tracking
- metrics now include route-level counters and simple duration buckets for the POC
- timeout and body limit baseline implemented
- tracing correlation baseline implemented
- OTLP/HTTP JSON export for traces and logs implemented with timeout, custom headers, and bounded retries
- OTLP exporter now respects protocol config, default 10s timeout, transient retry handling, and `Retry-After`
- graceful shutdown still needs a fuller in-flight connection drain

### Phase 4

- production adapters for PostgreSQL, Redis, outbound HTTP, and service instrumentation
- Docker/Kubernetes examples
- Prometheus/Loki/Grafana/Tempo stack

## Notes on dependencies

This module prefers:

- pure C where practical
- Rust only when it provides a major reliability or performance advantage and can be bridged cleanly into C
- no secondary VM or runtime dependency such as Node.js

That means the first implementation pass should aim for:

- C11 core
- non-blocking sockets with `epoll`/`kqueue` abstraction or a very small portability layer
- a lightweight HTTP parser and JSON strategy chosen for predictable integration

## Publishing

This module is designed to be published as:

- repository: `npinto21/microservice`
- module reference: `p21@npinto21/microservice@v1`
