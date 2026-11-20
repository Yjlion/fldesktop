// fl-web: address bar input, bookmarks and history files
#include <unistd.h>

#include "adblock_rules.h"
#include "browser_util.h"
#include "fld/json.h"
#include "fld/util.h"
#include "test.h"

int main() {
  using namespace web;
  const std::string ddg = search_template("DuckDuckGo");
  CHECK_EQ(ddg, std::string("https://duckduckgo.com/?q=%s"));
  CHECK_EQ(search_template("google"), std::string("https://www.google.com/search?q=%s"));
  CHECK_EQ(search_template("https://s.example/?q=%s"), std::string("https://s.example/?q=%s"));
  CHECK_EQ(search_template("nonsense"), ddg);
  CHECK_EQ(url_from_input("example.com", ddg), std::string("https://example.com"));
  CHECK_EQ(url_from_input("  example.com/a?b=c ", ddg), std::string("https://example.com/a?b=c"));
  CHECK_EQ(url_from_input("http://example.com", ddg), std::string("http://example.com"));
  CHECK_EQ(url_from_input("about:blank", ddg), std::string("about:blank"));
  CHECK_EQ(url_from_input("localhost:8080", ddg), std::string("http://localhost:8080"));
  CHECK_EQ(url_from_input("192.168.1.1/admin", ddg), std::string("http://192.168.1.1/admin"));
  CHECK_EQ(url_from_input("/tmp/a b.html", ddg), std::string("file:///tmp/a%20b.html"));
  CHECK_EQ(url_from_input("hello world", ddg), std::string("https://duckduckgo.com/?q=hello+world"));
  CHECK_EQ(url_from_input("3.14", ddg), std::string("https://duckduckgo.com/?q=3.14"));
  CHECK_EQ(url_from_input("c++ & rust", ddg), std::string("https://duckduckgo.com/?q=c%2B%2B+%26+rust"));
  CHECK_EQ(url_from_input("fltk", ddg), std::string("https://duckduckgo.com/?q=fltk"));
  CHECK_EQ(url_from_input("", ddg), std::string(""));
  CHECK_EQ(url_host("https://user@www.example.com:8443/x"), std::string("www.example.com"));
  CHECK_EQ(url_host("about:blank"), std::string(""));
  CHECK_EQ(fallback_title("https://www.example.com/x"), std::string("example.com"));
  CHECK_EQ(fallback_title("file:///home/me/a%20b.html"), std::string("a b.html"));

  char tmpl[] = "/tmp/fl-web-test-XXXXXX";
  std::string dir = mkdtemp(tmpl);
  std::string bm = dir + "/bookmarks.tsv", hist = dir + "/history.tsv";
  CHECK(save_bookmarks(bm, {{"https://a.example/", "A\tsite"}, {"https://b.example/", ""}}));
  auto b = load_bookmarks(bm);
  CHECK_EQ(b.size(), (size_t)2);
  if (b.size() == 2) {
    CHECK_EQ(b[0].title, std::string("A site"));
    CHECK_EQ(b[1].url, std::string("https://b.example/"));
  }
  for (int i = 0; i < 30; i++)
    add_history(hist, {(time_t)(1000 + i), "https://h.example/" + std::to_string(i % 10), "Page " + std::to_string(i % 10)}, 8);
  auto h = load_history(hist);
  CHECK(!h.empty());
  auto view = history_view(h, "", 100);
  CHECK(!view.empty() && view.size() <= 10);
  if (!view.empty()) CHECK_EQ(view[0].url, std::string("https://h.example/9"));
  CHECK_EQ(history_view(h, "page 3", 100).size(), (size_t)1);
  auto s = suggestions("h.ex", b, h, 3);
  CHECK_EQ(s.size(), (size_t)3);
  auto s2 = suggestions("a.ex", b, h, 5);
  CHECK(!s2.empty() && s2[0].url == "https://a.example/");
  CHECK(suggestions("", b, h).empty());
  unlink(bm.c_str());
  unlink(hist.c_str());
  rmdir(dir.c_str());

  // ad blocking: Adblock Plus filters to WebKit content blocker rules
  CHECK_EQ(abp_url_filter("||ads.example^"), std::string("^[^:]+://+([^:/]+\\.)?ads\\.example[/:?=&]"));
  CHECK_EQ(abp_url_filter("|https://x.example/a*b|"), std::string("^https://x\\.example/a.*b$"));
  CHECK_EQ(abp_url_filter("/banner/*/img"), std::string("/banner/.*/img"));
  CHECK_EQ(abp_url_filter("*ad.js*"), std::string("ad\\.js"));
  CHECK_EQ(abp_url_filter("a(b)+c?"), std::string("a\\(b\\)\\+c\\?"));
  CHECK_EQ(abp_url_filter(""), std::string(".*"));
  CHECK_EQ(abp_url_filter("a|b"), std::string(""));
  CHECK_EQ(abp_url_filter("/werbung\xc3\xa4"), std::string(""));
  CHECK_EQ(abp_domain("Example.COM"), std::string("*example.com"));
  CHECK_EQ(abp_domain("example.*"), std::string(""));
  CHECK_EQ(abp_domain(".example.com"), std::string(""));

  std::string list = "[Adblock Plus 2.0]\n"
                     "! comment\n"
                     "||ads.example^\n"
                     "||track.example^$script,third-party\n"
                     "/banner/*$image,domain=news.example|~sub.news.example\n"
                     "||frames.example^$subdocument\n"
                     "@@||ads.example/ok.js$script\n"
                     "@@||trusted.example^$document\n"
                     "##.ad-banner\n"
                     "##.ad-banner\n"
                     "###sidebar-ad\n"
                     "news.example,blog.example##.promo\n"
                     "~shop.example##.sponsor\n"
                     "##.only-elsewhere\n"
                     "shop.example#@#.only-elsewhere\n"
                     "##.never\n"
                     "#@#.never\n"
                     "/^https?:\\/\\/regex/\n"
                     "||x.example^$redirect=noop.js\n"
                     "||y.example^$csp=script-src 'none'\n"
                     "example.com##+js(abort-on-property-read, x)\n"
                     "example.com#?#div:-abp-has(.ad)\n"
                     "example.com##div:has-text(Ad)\n"
                     "example.com#$#body { color: red }\n"
                     "||z.example^$object\n";
  AdblockResult ar = abp_to_webkit({list}, {"mine.example"});
  fld::Json rules;
  std::string jerr;
  CHECK(fld::Json::parse(ar.json, rules, &jerr));
  CHECK(rules.is_array());
  CHECK_EQ(rules.items().size(), ar.rules);
  CHECK_EQ(ar.skipped, (size_t)8);
  auto trig = [&](size_t i) -> const fld::Json & { return rules.items()[i]["trigger"]; };
  auto act = [&](size_t i) { return rules.items()[i]["action"]["type"].as_string(); };
  // ||ads.example^: what pages load, and frames and pages of that host
  CHECK_EQ(trig(0)["url-filter"].as_string(), abp_url_filter("||ads.example^"));
  CHECK_EQ(act(0), std::string("block"));
  CHECK_EQ(trig(0)["resource-type"].items().size(), (size_t)9);
  CHECK_EQ(trig(0)["resource-type"].items()[8].as_string(), std::string("document"));
  // $script,third-party
  CHECK_EQ(trig(1)["resource-type"].items()[0].as_string(), std::string("script"));
  CHECK_EQ(trig(1)["load-type"].items()[0].as_string(), std::string("third-party"));
  // $domain=a|~b: WebKit takes one list, the sites it is for; a pattern
  // with types never blocks pages
  CHECK_EQ(trig(2)["if-domain"].items()[0].as_string(), std::string("*news.example"));
  CHECK(!trig(2).has("unless-domain"));
  CHECK_EQ(trig(2)["resource-type"].items().size(), (size_t)1);
  // $subdocument: frames only
  CHECK_EQ(trig(3)["load-context"].items()[0].as_string(), std::string("child-frame"));
  // the exception follows the blocking rules
  CHECK_EQ(act(4), std::string("ignore-previous-rules"));
  CHECK_EQ(trig(4)["url-filter"].as_string(), abp_url_filter("||ads.example/ok.js"));
  // element hiding after the exceptions: duplicates dropped, exempt selectors
  // left out or limited, sites grouped
  size_t i = 5;
  CHECK_EQ(act(i), std::string("css-display-none"));
  CHECK_EQ(trig(i)["unless-domain"].items()[0].as_string(), std::string("*shop.example"));
  CHECK_EQ(rules.items()[i]["action"]["selector"].as_string(), std::string(".only-elsewhere"));
  i++;
  CHECK_EQ(rules.items()[i]["action"]["selector"].as_string(), std::string(".ad-banner, #sidebar-ad"));
  CHECK(!trig(i).has("if-domain"));
  i++;
  CHECK_EQ(rules.items()[i]["action"]["selector"].as_string(), std::string(".promo"));
  CHECK_EQ(trig(i)["if-domain"].items().size(), (size_t)2);
  i++;
  CHECK_EQ(rules.items()[i]["action"]["selector"].as_string(), std::string(".sponsor"));
  CHECK_EQ(trig(i)["unless-domain"].items()[0].as_string(), std::string("*shop.example"));
  i++;
  // whole-site exceptions last: $document and the allowed sites
  CHECK_EQ(act(i), std::string("ignore-previous-rules"));
  CHECK_EQ(trig(i)["if-domain"].items()[0].as_string(), std::string("*trusted.example"));
  i++;
  CHECK_EQ(trig(i)["if-domain"].items()[0].as_string(), std::string("*mine.example"));
  CHECK_EQ(ar.rules, i + 1);
  CHECK(ar.json.find(".never") == std::string::npos);
  // a pattern without types does not block the page itself
  AdblockResult pat = abp_to_webkit({"/banner/ads.js\n"}, {});
  CHECK(pat.json.find("\"document\"") == std::string::npos);
  CHECK_EQ(pat.rules, (size_t)1);
  // without element hiding, and with a cap: exceptions are kept
  AdblockResult net_only = abp_to_webkit({list}, {}, false);
  CHECK(net_only.json.find("css-display-none") == std::string::npos);
  AdblockResult capped = abp_to_webkit({list}, {}, true, 3);
  CHECK_EQ(capped.rules, (size_t)3);
  CHECK(capped.json.find("ignore-previous-rules") != std::string::npos);
  // many generic selectors are batched
  std::string many;
  for (int k = 0; k < 250; k++) many += "##.ad-" + std::to_string(k) + "\n";
  CHECK_EQ(abp_to_webkit({many}, {}).rules, (size_t)3);
  return test_result("web");
}
