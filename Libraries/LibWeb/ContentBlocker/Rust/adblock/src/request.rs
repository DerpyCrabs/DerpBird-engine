//! Contains structures needed to describe network requests.

use thiserror::Error;

use crate::url_parser;
use crate::utils;

#[derive(Clone, Copy, PartialEq, Debug)]
pub enum RequestMethod {
    Connect,
    Delete,
    Get,
    Head,
    Options,
    Patch,
    Post,
    Put,
    Trace,
    Other,
}

impl std::str::FromStr for RequestMethod {
    type Err = ();

    fn from_str(raw_method: &str) -> Result<Self, Self::Err> {
        match raw_method.to_ascii_lowercase().as_str() {
            "" => Err(()),
            "connect" => Ok(RequestMethod::Connect),
            "delete" => Ok(RequestMethod::Delete),
            "get" => Ok(RequestMethod::Get),
            "head" => Ok(RequestMethod::Head),
            "options" => Ok(RequestMethod::Options),
            "patch" => Ok(RequestMethod::Patch),
            "post" => Ok(RequestMethod::Post),
            "put" => Ok(RequestMethod::Put),
            "trace" => Ok(RequestMethod::Trace),
            _ => Ok(RequestMethod::Other),
        }
    }
}

/// The type of resource requested from the URL endpoint.
#[derive(Clone, PartialEq, Debug)]
pub enum RequestType {
    Beacon,
    Csp,
    Document,
    Popup,
    Popunder,
    InlineScript,
    InlineFont,
    Dtd,
    Fetch,
    Font,
    Image,
    Media,
    Object,
    Other,
    Ping,
    Script,
    Stylesheet,
    Subdocument,
    Websocket,
    Xlst,
    Xmlhttprequest,
}

/// Possible failure reasons when creating a [`Request`].
#[derive(Debug, Error, PartialEq)]
pub enum RequestError {
    #[error("hostname parsing failed")]
    HostnameParseError,
    #[error("source hostname parsing failed")]
    SourceHostnameParseError,
    #[error("invalid Unicode provided")]
    UnicodeDecodingError,
}

impl From<idna::Errors> for RequestError {
    fn from(_err: idna::Errors) -> RequestError {
        RequestError::UnicodeDecodingError
    }
}

impl From<url::ParseError> for RequestError {
    fn from(_err: url::ParseError) -> RequestError {
        RequestError::HostnameParseError
    }
}

fn cpt_match_type(cpt: &str) -> RequestType {
    match cpt {
        "beacon" => RequestType::Ping,
        "csp_report" => RequestType::Csp,
        "document" | "main_frame" => RequestType::Document,
        "popup" => RequestType::Popup,
        "popunder" => RequestType::Popunder,
        "inline-script" => RequestType::InlineScript,
        "inline-font" => RequestType::InlineFont,
        "font" => RequestType::Font,
        "image" | "imageset" => RequestType::Image,
        "media" => RequestType::Media,
        "object" | "object_subrequest" => RequestType::Object,
        "ping" => RequestType::Ping,
        "script" => RequestType::Script,
        "stylesheet" => RequestType::Stylesheet,
        "sub_frame" | "subdocument" => RequestType::Subdocument,
        "websocket" => RequestType::Websocket,
        "xhr" | "xmlhttprequest" => RequestType::Xmlhttprequest,
        _ => RequestType::Other,
    }
}

/// A network [`Request`], used as an interface for network blocking in the [`crate::Engine`].
#[derive(Clone, Debug)]
pub struct Request {
    pub request_type: RequestType,
    pub method: Option<RequestMethod>,

    pub is_http: bool,
    pub is_https: bool,
    pub is_supported: bool,
    /// Restrict popup fallback to URL matches touching the hostname.
    pub hostname_only_filter: bool,
    pub is_third_party: bool,
    pub is_strict_third_party: bool,
    pub url: String,
    pub hostname: String,
    pub source_hostname_hashes: Option<Vec<utils::Hash>>,
    pub source_hostname: String,
    /// Transport-supplied canonical addresses; empty before DNS and for cache hits.
    pub resolved_ip_addresses: Vec<String>,
    pub top_hostname: String,
    pub top_hostname_hashes: Option<Vec<utils::Hash>>,
    pub destination_hostname_hashes: Option<Vec<utils::Hash>>,

    pub(crate) url_lower_cased: String,
    pub(crate) request_tokens: Vec<utils::Hash>,
    pub(crate) original_url: String,
}

fn hostname_hashes(hostname: &str) -> Option<Vec<utils::Hash>> {
    if hostname.is_empty() { return None; }
    let mut hashes = vec![utils::fast_hash(hostname)];
    for (index, ch) in hostname.char_indices() {
        if ch == '.' && index + 1 < hostname.len() {
            hashes.push(utils::fast_hash(&hostname[index + 1..]));
        }
    }
    // Entity constraints strip the public suffix, not just the final hostname label.
    if hostname.parse::<std::net::IpAddr>().is_err() {
        let (domain_start, domain_end) = url_parser::get_host_domain(hostname);
        if let Some(dot) = hostname[domain_start..domain_end].find('.') {
            let entity = &hostname[..domain_start + dot];
            hashes.push(utils::fast_hash(&format!("{entity}.*")));
            for (index, ch) in entity.char_indices() {
                if ch == '.' { hashes.push(utils::fast_hash(&format!("{}.*", &entity[index+1..]))); }
            }
        }
    }
    Some(hashes)
}

impl Request {
    pub fn with_ip_addresses(mut self, addresses: &str) -> Self {
        self.resolved_ip_addresses = addresses.lines().take(256)
            .filter_map(|value| value.parse::<std::net::IpAddr>().ok())
            .map(|value| value.to_string()).collect();
        self
    }

    /// Set the actual top document for subframe requests. The source remains the initiator.
    pub fn with_top_url(mut self, top_url: &str) -> Self {
        self.top_hostname = url_parser::parse_url(top_url).map(|url| url.hostname().to_owned()).unwrap_or_default();
        self.top_hostname_hashes = hostname_hashes(&self.top_hostname);
        self
    }

    pub(crate) fn get_url(&self, case_sensitive: bool) -> &str {
        if case_sensitive {
            &self.url
        } else {
            &self.url_lower_cased
        }
    }

    pub(crate) fn get_source_hostname_hashes_for_match(
        &self,
    ) -> impl Iterator<Item = &utils::Hash> {
        self.source_hostname_hashes.as_ref().into_iter().flatten()
    }

    pub fn get_tokens_for_match(&self) -> impl Iterator<Item = &utils::Hash> {
        self.get_tokens().iter()
    }

    pub fn get_tokens(&self) -> &Vec<utils::Hash> {
        &self.request_tokens
    }

    #[allow(clippy::too_many_arguments)]
    fn from_detailed_parameters(
        raw_type: &str,
        url: &str,
        schema: &str,
        hostname: &str,
        source_hostname: &str,
        third_party: bool,
        original_url: String,
        method: Option<RequestMethod>,
    ) -> Request {
        let is_http: bool;
        let is_https: bool;
        let is_supported: bool;
        let request_type: RequestType;

        if schema.is_empty() {
            // no ':' was found
            is_https = true;
            is_http = false;
            is_supported = true;
            request_type = cpt_match_type(raw_type);
        } else {
            is_http = schema == "http";
            is_https = !is_http && schema == "https";

            let is_websocket = !is_http && !is_https && (schema == "ws" || schema == "wss");
            is_supported = is_http || is_https || is_websocket;
            if is_websocket {
                request_type = RequestType::Websocket;
            } else {
                request_type = cpt_match_type(raw_type);
            }
        }

        let source_hostname_hashes = hostname_hashes(source_hostname);

        let url_lower_cased = url.to_ascii_lowercase();

        Request {
            request_type,
            method,
            url: url.to_owned(),
            url_lower_cased: url_lower_cased.to_owned(),
            hostname: hostname.to_owned(),
            request_tokens: calculate_tokens(&url_lower_cased),
            source_hostname: source_hostname.to_owned(),
            resolved_ip_addresses: Vec::new(),
            top_hostname: source_hostname.to_owned(),
            top_hostname_hashes: source_hostname_hashes.clone(),
            destination_hostname_hashes: hostname_hashes(hostname),
            source_hostname_hashes,
            is_third_party: third_party,
            is_strict_third_party: !hostname.eq_ignore_ascii_case(source_hostname),
            is_http,
            is_https,
            is_supported,
            hostname_only_filter: false,
            original_url,
        }
    }

    /// Construct a new [`Request`].
    pub fn new(
        url: &str,
        source_url: &str,
        request_type: &str,
        method: &str,
    ) -> Result<Request, RequestError> {
        let parsed_url = url_parser::parse_url(url).ok_or(RequestError::HostnameParseError)?;
        let parsed_method = method.parse::<RequestMethod>().ok();

        let parsed_source = url_parser::parse_url(source_url);
        let (source_domain, third_party) = match &parsed_source {
            Some(parsed_source) => (
                parsed_source.hostname(),
                parsed_source.domain() != parsed_url.domain(),
            ),
            None => ("", true),
        };

        Ok(Request::from_detailed_parameters(
            request_type,
            &parsed_url.url,
            parsed_url.schema(),
            parsed_url.hostname(),
            source_domain,
            third_party,
            url.to_string(),
            parsed_method,
        ))
    }

    /// If you're building a [`Request`] in a context that already has access to parsed
    /// representations of the input URLs, you can use this constructor to avoid extra lookups from
    /// the public suffix list. Take care to pass data correctly.
    pub fn preparsed(
        url: &str,
        hostname: &str,
        source_hostname: &str,
        request_type: &str,
        third_party: bool,
        method: &str,
    ) -> Request {
        let splitter = memchr::memchr(b':', url.as_bytes()).unwrap_or(0);
        let schema: &str = &url[..splitter];

        Request::from_detailed_parameters(
            request_type,
            url,
            schema,
            hostname,
            source_hostname,
            third_party,
            url.to_string(),
            method.parse::<RequestMethod>().ok(),
        )
    }
}

fn calculate_tokens(url_lower_cased: &str) -> Vec<utils::Hash> {
    let mut tokens = utils::TokensBuffer::default();
    utils::tokenize_pooled(url_lower_cased, &mut tokens);
    // Add zero token as a fallback to wildcard rule bucket
    tokens.push(0);
    tokens.into_iter().collect()
}

#[cfg(test)]
#[path = "../tests/unit/request.rs"]
mod unit_tests;
