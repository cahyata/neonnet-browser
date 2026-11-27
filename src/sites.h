// Websites: reading hosts, sites and origins out of addresses, and the lists of site exceptions
// (see GLOSSARY.md). Saved passwords and site permissions use the address functions only; their
// lists are kept elsewhere.
//
// Everything is safe on any thread, except Load() and the functions that change a list, which
// belong to the UI thread.
#pragma once
#include "settings.h"   // settings::Storage: where the lists are kept
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace sites {

// ---- addresses

// Scheme, lower-case host (without brackets or user name) and port of an address; port 0 = none given.
void SplitUrl(const std::wstring& url, std::wstring& scheme, std::wstring& host, int& port);
std::wstring HostOf(const std::wstring& url);
// scheme://host[:port], the unit a saved password or a site permission belongs to. Empty when the
// address has none.
std::wstring OriginOf(const std::wstring& url);
// The host in "user@host:port", "[v6]:port" or a bare IPv6 address: lower case, without brackets.
std::string HostFromAuthority(std::string authority);
// The part of a host name a site is known by: example.com, or example.co.uk for the common
// two-part endings. An IP address is a site of its own.
std::string_view SiteOf(std::string_view host);

// ---- site exceptions

// What an exception switches off, and which hosts one entry covers.
enum class List {
    Ads,                // ad blocking: the whole site
    Trackers,           // tracker blocking: the whole site
    ThirdPartyCookies,  // third-party cookie blocking: exactly the host that was entered
    ForceDark,          // Force Dark: one host, "www." ignored
    KeepAwake,          // putting tabs to sleep: one host, "www." ignored
    Count
};

// Called after entries were added to or removed from a list, whoever asked for the change.
using Listener = std::function<void(List, const std::vector<std::string>& entries, bool added)>;

// Reads every list. Call before anything else; `storage` must live as long as lists change.
// An incognito window keeps its changes in memory: the entries name the sites visited.
void Load(settings::Storage& storage, bool incognito, Listener onChange);

// The entry of `list` that would cover `host`; empty for an empty host.
std::string EntryFor(List list, std::string_view host);
bool Excepted(List list, std::string_view host);
// Adds the host's entry, or removes it when it is there. True when it is an exception now.
bool Toggle(List list, std::string_view host);
// An address or a host as the user typed it. False when no host is in it.
bool Add(List list, const std::wstring& typed);
void Remove(List list, const std::wstring& entry);
void Clear(List list);
std::vector<std::wstring> Entries(List list);   // sorted

}  // namespace sites
