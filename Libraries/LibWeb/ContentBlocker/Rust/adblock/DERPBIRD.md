# DerpBird adblock-rust changes

Vendored from the crates.io adblock 0.13.3 source package (Brave adblock-rust).
Upstream: https://github.com/brave/adblock-rust
License: MPL-2.0, preserved in LICENSE and source headers.
Local changes: named/all removeparam exceptions, all-parameter selectors,
and removal of empty-valued/key-only parameters. URL matching also includes the local modifier, domain and ECMAScript compatibility changes described below.
Inverted literal selectors and URL-form-decoded parameter names are supported. Regex selectors (including inversion, exceptions and i flag) use a separate 64-entry LRU cache. Expressions are limited to 16KiB source and 128KiB compiled/DFA budgets each. The browser supplies bounded Ladybird ECMAScript matching, including lookaround and backreferences. Standalone crate use without that host retains Rust regex limitations.

Original .crate SHA-256: f44b96a666a23c12acad7c688bfe8638a7094e7eabe765b09a6864ab991c676d

The embedded manifest omits standalone benchmark targets and development dependencies. Its library unit-test target is disabled because the published crate excludes the referenced tests/unit sources; explicit DerpBird integration targets remain enabled. Browser integration tests exercise the runtime fork. Optional runtime features remain available.

Query removal returns before allocating/splitting query parameters when no removal rule applies or an all-query exception applies. Native C++/Rust benchmarks and HTTP integration scenarios exist for this path; their presence does not establish a pass for the current source.

Option parsing respects regex delimiters, escaped slashes, character classes, commas and dollar anchors; underscore-only no-op options are accepted. Global literal parameter rules are not indexed by raw parameter names, which would miss URL-encoded names.

Added a CSS-only cosmetic query path that omits scriptlet collection/assembly, while preserving the original full-resource API for document-start injection.

Procedural parsing now supports media/attribute-watch/complement operators and
recursive has/if/if-not/not/is/where conditions and selector-list unions. Same-subject
CSS suffixes use a separate operator from relative traversal. Nested depth is 32.
The DerpBird adapter/executor is maintained separately from the pinned uBO source.

Redirect exceptions are separated from ordinary network allow rules; generic
redirect-rule exceptions, token matching independent of priority, and skipping
unavailable resources before priority selection are implemented. A dedicated
internal MIME represents the host-controlled click-to-load resource.

Response-stage `$header=` matching lives in response_headers.rs. It groups URL
rules by compiled header predicate rather than feeding them into request-stage
blocking. Presence, exact values, bounded regexes, inversion, response exceptions,
ordinary network exceptions and important precedence are implemented. Header
names are case-insensitive. Invalid predicates and mixed action modifiers are
rejected. Browser matching uses the shared bounded ECMAScript compatibility path.

Header-condition engines retain original source lines separately from transformed
network rules for diagnostic attribution. Per-group URL indexing remains enabled;
fusion is disabled for these groups to preserve exact source-line identity.

Diagnostic rule copies are bounded to 4096 UTF-8 bytes before allocation, including
optimizer-fused source text; the full compiled rule still drives matching.

The address-policy integration regression command is `cargo test --test derpbird-address-policy`. The upstream 0.13.3 crate omits its referenced unit-test source directory, so `cargo test --lib` is not a usable standalone suite in this vendor package. The explicit integration target exercises the compiled public engine and does not disable or substitute passing upstream tests. Build outputs under `target/` are excluded from the engine overlay.

Raw network regexes and modifier/header regexes use a shared dialect selector.
The linear Rust fast path is restricted to compatible ASCII syntax and ASCII
inputs without line terminators; Rust class-set operators, identity escapes,
Unicode input and ambiguous syntax use Ladybird's bounded ECMAScript evaluator.
Generated wildcard URL expressions retain their existing indexed linear path.
