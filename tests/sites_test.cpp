// Tests of the sites module through its interface, with storage in memory.
// Run with: build.ps1 -Test
#include "sites.h"
#include "test.h"

using sites::List;

static void Addresses() {
    CHECK(sites::HostOf(L"HTTPS://User:pw@News.Example.CO.UK:8443/a?b=1#c") == L"news.example.co.uk");
    CHECK(sites::HostOf(L"http://[::1]:80/") == L"::1");
    CHECK(sites::HostOf(L"about:blank").empty());
    CHECK(sites::OriginOf(L"https://Example.com:443/x") == L"https://example.com");
    CHECK(sites::OriginOf(L"http://example.com:8080/x") == L"http://example.com:8080");
    CHECK(sites::OriginOf(L"http://[fe80::1]:8080/") == L"http://[fe80::1]:8080");
    CHECK(sites::OriginOf(L"data:text/html,x").empty());
    std::wstring sc, h; int port;
    sites::SplitUrl(L"http://[::1/x", sc, h, port);
    CHECK(sc == L"http" && h.empty() && port == 0);

    CHECK(sites::HostFromAuthority("User@WWW.Example.com:8080") == "www.example.com");
    CHECK(sites::HostFromAuthority("[FE80::1]:80") == "fe80::1");
    CHECK(sites::HostFromAuthority("::1") == "::1");
    CHECK(sites::HostFromAuthority("[::1").empty());
    CHECK(sites::HostFromAuthority("").empty());

    CHECK(sites::SiteOf("example.com") == "example.com");
    CHECK(sites::SiteOf("a.b.example.com") == "example.com");
    CHECK(sites::SiteOf("news.example.co.uk") == "example.co.uk");
    CHECK(sites::SiteOf("example.co.uk") == "example.co.uk");
    CHECK(sites::SiteOf("localhost") == "localhost");
    CHECK(sites::SiteOf("10.0.1.10") == "10.0.1.10" && sites::SiteOf("192.168.1.10") == "192.168.1.10");
    CHECK(sites::SiteOf("::1") == "::1" && sites::SiteOf("::ffff:1.2.3.4") == "::ffff:1.2.3.4");
    CHECK(sites::SiteOf("").empty());
}

static void EachListCoversItsHosts() {
    Memory m;
    sites::Load(m, false, nullptr);
    // blocking: the whole site
    for (List l : { List::Ads, List::Trackers }) {
        CHECK(sites::Toggle(l, "www.example.com"));
        CHECK(sites::Excepted(l, "example.com") && sites::Excepted(l, "cdn.Example.COM") && !sites::Excepted(l, "example.org"));
        CHECK(sites::Entries(l) == std::vector<std::wstring>{ L"example.com" });
        CHECK(!sites::Toggle(l, "shop.example.com"));   // the same site: off again
        CHECK(!sites::Excepted(l, "example.com"));
    }
    // Force Dark and keep awake: one host, www ignored
    for (List l : { List::ForceDark, List::KeepAwake }) {
        CHECK(sites::Toggle(l, "www.example.com"));
        CHECK(sites::Excepted(l, "example.com") && sites::Excepted(l, "www.example.com") && !sites::Excepted(l, "news.example.com"));
        CHECK(sites::Entries(l) == std::vector<std::wstring>{ L"example.com" });
    }
    // cookies: exactly the host
    CHECK(sites::Add(List::ThirdPartyCookies, L"HTTPS://User@Www.Example.com:8443/path?q#f"));
    CHECK(sites::Excepted(List::ThirdPartyCookies, "www.example.com") && !sites::Excepted(List::ThirdPartyCookies, "example.com"));
    CHECK(sites::Add(List::ThirdPartyCookies, L"[fe80::1]:80") && sites::Add(List::ThirdPartyCookies, L"::1"));
    CHECK((sites::Entries(List::ThirdPartyCookies) == std::vector<std::wstring>{ L"::1", L"fe80::1", L"www.example.com" }));
}

static void NothingToAdd() {
    Memory m;
    int calls = 0;
    sites::Load(m, false, [&](List, const std::vector<std::string>&, bool) { calls++; });
    CHECK(!sites::Add(List::ThirdPartyCookies, L"") && !sites::Add(List::ThirdPartyCookies, L"https://") &&
          !sites::Add(List::ThirdPartyCookies, L"a;b") && !sites::Add(List::ThirdPartyCookies, std::wstring(300, L'a')));
    CHECK(!sites::Toggle(List::Ads, "") && !sites::Excepted(List::Ads, ""));
    sites::Remove(List::Ads, L"nothing.example");
    sites::Clear(List::Ads);
    CHECK(calls == 0 && m.writes == 0);
}

static void ListsAreStored() {
    Memory m;
    sites::Load(m, false, nullptr);
    for (int i = 0; i < 60; i++) sites::Add(List::KeepAwake, L"site" + std::to_wstring(100 + i) + L".example");
    CHECK(m.texts[L"KeepAwakeHosts"].size() > 511);
    CHECK(m.texts[L"KeepAwakeHosts"].rfind(L"site100.example;site101.example;", 0) == 0);
    sites::Load(m, false, nullptr);   // as after a restart
    CHECK(sites::Entries(List::KeepAwake).size() == 60);
    sites::Clear(List::KeepAwake);
    CHECK(m.texts.count(L"KeepAwakeHosts") == 0);

    // the names NeonNet has always used
    m.texts = { { L"AdsExemptHosts", L"a.test" }, { L"TrackersExemptHosts", L"b.test" }, { L"Cookie3pExemptHosts", L"c.test" },
                { L"ForceDarkExempt", L"d.test;;e.test;" }, { L"KeepAwakeHosts", std::wstring(300, L'x') + L";f.test" } };
    sites::Load(m, false, nullptr);
    CHECK(sites::Excepted(List::Ads, "a.test") && sites::Excepted(List::Trackers, "b.test") && sites::Excepted(List::ThirdPartyCookies, "c.test"));
    CHECK((sites::Entries(List::ForceDark) == std::vector<std::wstring>{ L"d.test", L"e.test" }));
    CHECK(sites::Entries(List::KeepAwake) == std::vector<std::wstring>{ L"f.test" });   // an over-long entry is dropped
}

static void IncognitoKeepsThemInMemory() {
    Memory m;
    m.texts[L"ForceDarkExempt"] = L"kept.test";
    sites::Load(m, true, nullptr);
    CHECK(sites::Excepted(List::ForceDark, "kept.test"));
    sites::Toggle(List::ForceDark, "visited.test");
    sites::Add(List::ThirdPartyCookies, L"visited.test");
    sites::Remove(List::ForceDark, L"kept.test");
    sites::Clear(List::ForceDark);
    CHECK(m.writes == 0 && m.texts[L"ForceDarkExempt"] == L"kept.test");
}

static void ListenerHearsWhatChanged() {
    Memory m;
    std::vector<std::string> heard;
    sites::Load(m, false, [&](List l, const std::vector<std::string>& entries, bool added) {
        for (const auto& e : entries) heard.push_back(std::to_string((int)l) + (added ? "+" : "-") + e);
    });
    sites::Toggle(List::Ads, "www.a.test");
    sites::Toggle(List::Ads, "a.test");
    sites::Add(List::ThirdPartyCookies, L"b.test");
    sites::Add(List::ThirdPartyCookies, L"b.test");   // applied again
    sites::Add(List::ThirdPartyCookies, L"c.test");
    sites::Remove(List::ThirdPartyCookies, L"c.test");
    sites::Add(List::ThirdPartyCookies, L"d.test");
    sites::Clear(List::ThirdPartyCookies);
    CHECK((heard == std::vector<std::string>{ "0+a.test", "0-a.test", "2+b.test", "2+b.test", "2+c.test", "2-c.test", "2+d.test", "2-b.test", "2-d.test" }));
}

void SitesTests() {
    Addresses();
    EachListCoversItsHosts();
    NothingToAdd();
    ListsAreStored();
    IncognitoKeepsThemInMemory();
    ListenerHearsWhatChanged();
}
