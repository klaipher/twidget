// litehtml checks that need no platform: a deterministic container (tests/litehtml_test_container.h, the copy iOS
// keeps as third-party/twidget/tests/test_container.h), the document's computed values and its paint calls.
#include "litehtml_test_container.h"

#include <litehtml/html_tag.h>

#include <cmath>
#include <string>

static int failures = 0;

static litehtml::element::ptr by_id(const litehtml::document::ptr& doc, const char* id) {
    return doc->root()->select_one(std::string("#") + id);
}

// The kinds of the draw calls, space separated.
static std::string kinds(const litehtml_test::TestContainer& c) {
    std::string out;
    for (const auto& call : c.calls) {
        out += (out.empty() ? "" : " ") + call.kind;
    }
    return out;
}

struct CollapsingContainer : litehtml_test::TestContainer {
    bool paint_collapsed_borders_over_cells() const override { return true; }
};

static const char* kCollapsedTable =
    "<html><head><style>body{margin:0}table{border-collapse:collapse}td{background:#eee;border:1px solid #999;"
    "padding:0}</style></head><body><table><tr><td></td><td></td></tr></table></body></html>";

// A declaration whose var() never resolves is invalid at computed-value time (CSS Variables §3.1): the property is
// unset, a shorthand's longhands with it, over an earlier declaration (Telegram's VENDOR.md edit 57; iOS
// WidgetRuntime's ComputedStyleTests testAnUnresolvedVarIsInvalidAtComputedTime).
static void testAnUnresolvedVarUnsetsTheDeclaration() {
    litehtml_test::TestContainer c;
    auto doc = litehtml_test::layout(c, "<html><head><style>#f{display:flex}#s{flex-grow:3;flex:var(--nope)}"
                                        "#m{margin-left:10px;margin:var(--nope)}#k{flex-grow:2;flex-grow:var(--nope,4)}"
                                        "#c{color:red}#i{color:var(--nope)}</style></head><body>"
                                        "<div id=f><div id=s>s</div><div id=k>k</div></div><div id=m>m</div>"
                                        "<div id=c><div id=i>i</div></div></body></html>");
    CHECK(by_id(doc, "s")->css().get_flex_grow() == 0);
    CHECK(by_id(doc, "m")->css().get_margins().left.val() == 0);
    // A fallback still resolves it.
    CHECK(by_id(doc, "k")->css().get_flex_grow() == 4);
    // An inherited property unset inherits.
    CHECK(by_id(doc, "i")->css().get_color() == litehtml::web_color(255, 0, 0));
}

// dir=auto is a presentational hint: a direction declared for the element wins (a host resolving dir=auto from its own
// DOM declares it), and descendants with a dir of their own are skipped (HTML's auto directionality).
static void testDirAutoFollowsHtmlAndYieldsToADeclaration() {
    litehtml_test::TestContainer c;
    auto doc = litehtml_test::layout(c, "<html><head><style>#d{direction:ltr}</style></head><body>"
                                        "<p dir=auto id=p><span dir=ltr>Hi</span> \xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d</p>"
                                        "<p dir=auto id=d>\xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d</p>"
                                        "<p dir=auto id=l>abc \xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d</p></body></html>");
    CHECK(by_id(doc, "p")->css().get_direction() == litehtml::direction_rtl);
    CHECK(by_id(doc, "d")->css().get_direction() == litehtml::direction_ltr);
    CHECK(by_id(doc, "l")->css().get_direction() == litehtml::direction_ltr);
}

// By default each cell of a collapsed table paints its own borders in its own paint (upstream's order, which a
// paint_visitor walk sees whole: iOS WidgetRuntime's layer mirror); a host that opts in gets them after every cell.
static void testCollapsedBordersFollowTheHostsChoice() {
    {
        litehtml_test::TestContainer c;
        auto doc = litehtml_test::layout(c, kCollapsedTable);
        c.calls.clear();
        litehtml::position clip(0, 0, 300, 600);
        doc->draw(0, 0, 0, &clip);
        CHECK(kinds(c) == "fill borders fill borders");
    }
    {
        CollapsingContainer c;
        auto doc = litehtml_test::layout(c, kCollapsedTable);
        c.calls.clear();
        litehtml::position clip(0, 0, 300, 600);
        doc->draw(0, 0, 0, &clip);
        CHECK(kinds(c) == "fill fill borders borders");
    }
}

struct FirstStrongBidiContainer : litehtml_test::TestContainer {
    bool reorder_bidi_by_first_strong_character() const override { return true; }
};

// Without bidi_levels a line keeps its logical order (upstream's default hook, which iOS WidgetRuntime replaces with its
// own levels); a host that opts in (macOS LHContainer, TwidgetCore) gets the fork's simplified reordering, a word right
// to left by its first strong character, as before the 13.2 merge.
static void testBidiFallbackFollowsTheHostsChoice() {
    static const char* kRtlBoxes = "<body style='margin:0'><div style='direction:rtl;width:200px'>"
                                   "<i id=a style='display:inline-block;width:10px;height:8px'></i>"
                                   "<i id=b style='display:inline-block;width:10px;height:8px'></i></div>"
                                   "<div style='width:200px'><span id=h>\xd7\xa9\xd7\x9c</span> <span id=w>\xd7\x9d\xd7\x95</span></div></body>";
    const auto x = [](const litehtml::document::ptr& doc, const char* id) {
        const auto el = by_id(doc, id);
        return float((el->children().empty() ? el : el->children().front())->get_placement().x);
    };
    {
        litehtml_test::TestContainer c;
        auto doc = litehtml_test::layout(c, kRtlBoxes);
        CHECK(std::abs(x(doc, "a") - 180.0f) < 0.5f && std::abs(x(doc, "b") - 190.0f) < 0.5f);
        CHECK(x(doc, "h") < x(doc, "w"));
    }
    {
        FirstStrongBidiContainer c;
        auto doc = litehtml_test::layout(c, kRtlBoxes);
        CHECK(std::abs(x(doc, "a") - 190.0f) < 0.5f && std::abs(x(doc, "b") - 180.0f) < 0.5f);
        CHECK(x(doc, "h") > x(doc, "w"));
    }
}

// A patch keeps the style bytes a fresh build of its result charges (edit 21; iOS WidgetRuntime's
// testRandomEditsMatchARebuild): an element the session matched again (here the reduced-motion rule's list items) and then
// removed or moved is not restyled at the commit, which charged its var() splices again with nothing left to give them back.
// The shorthands keep their var() tokens, so every compute splices them; out of the tree a var() finds no custom property
// above it and splices its fallback (--bg's).
static void testAPatchKeepsTheStyleBytesOfAFreshBuild() {
    const std::string sheet = "<style>:root{--pad:3px 4px} .b{padding:var(--pad);background:var(--bg, #ccc)} .x{color:#c00}"
                              " ul{list-style-type:square} @media (prefers-reduced-motion:reduce){li{padding-left:9px}}</style>";
    litehtml_test::TestContainer c;
    auto doc = litehtml_test::layout(c, sheet + "<body style='margin:0'><ul id=l><li id=m>one <span class=b>s</span></li>"
                                                "<li id=n class=x>two <span class=b>t</span></li></ul><div id=d style='color:#090'>d</div>"
                                                "<div id=k>k</div><p class=b>p</p></body>");
    const size_t built = doc->style_bytes_charged();
    CHECK(built > 0);
    // One session: the environment first (WidgetRuntime's order), then a removal, a move (the old element out, a new one
    // built and inserted), a class and a style change.
    c.reduced = 1;
    CHECK(doc->environment_changed());
    CHECK(doc->remove_subtree(by_id(doc, "m")));
    CHECK(doc->remove_subtree(by_id(doc, "n")));
    const auto moved = doc->create_element("li", {{"id", "n"}, {"class", "x"}});
    const auto span = doc->create_element("span", {{"class", "b"}});
    std::vector<litehtml::element::ptr> pieces;
    doc->append_text(moved, "two ", pieces);
    doc->insert_child(moved, moved->children().size(), span);
    doc->append_text(span, "t", pieces);
    const auto k = by_id(doc, "k");
    CHECK(doc->insert_subtree(k, k->children().size(), moved));
    CHECK(doc->set_element_attributes(by_id(doc, "d"), {{"id", "d"}, {"class", "x b"}, {"style", "padding:2px"}}));
    doc->commit_patch();
    doc->render(litehtml::pixel_t(300.0f));

    litehtml_test::TestContainer fc;
    fc.reduced = 1;
    auto fresh = litehtml_test::layout(fc, sheet + "<body style='margin:0'><ul id=l></ul><div id=d class='x b' style='padding:2px'>d</div>"
                                                   "<div id=k>k<li id=n class=x>two <span class=b>t</span></li></div><p class=b>p</p></body>");
    if (doc->style_bytes_charged() != fresh->style_bytes_charged())
        std::printf("  style bytes %zu after the patch, %zu in a fresh build\n", doc->style_bytes_charged(), fresh->style_bytes_charged());
    CHECK(doc->style_bytes_charged() == fresh->style_bytes_charged());
}

// A commit near the text-piece cap replays the style pass (edit 84) from the caps a build's style pass starts at: a document
// whose own text the parse cut stays text_limited, as a fresh build is, though its generated content makes no piece (iOS
// TwidgetExternalTests' testSetAttributeFixRound: `attr (title)` is no attr() function here, so the ::before is empty).
static void testAReplayKeepsTheParsesTextCut() {
    std::string words;
    for (int n = 0; n < 5004; n++)
        words += n ? " w" : "w";
    const std::string head = "<style>.x::before{content:attr (title)}</style><body><p>" + words + "</p>";
    litehtml_test::TestContainer c;
    auto doc = litehtml_test::layout(c, head + "<div id=t title='t t'>d</div></body>");
    CHECK(doc->text_limited());
    CHECK(doc->set_element_attribute(by_id(doc, "t"), "class", "x"));
    const uint64_t replays = doc->generated_replays();
    doc->commit_patch();
    doc->render(litehtml::pixel_t(300.0f));
    CHECK(doc->generated_replays() == replays + 1);
    litehtml_test::TestContainer fc;
    auto fresh = litehtml_test::layout(fc, head + "<div id=t class=x title='t t'>d</div></body>");
    CHECK(fresh->text_limited());
    CHECK(doc->text_limited() == fresh->text_limited());
    CHECK(doc->text_elements() == fresh->text_elements());
}

int main() {
    testAnUnresolvedVarUnsetsTheDeclaration();
    testDirAutoFollowsHtmlAndYieldsToADeclaration();
    testCollapsedBordersFollowTheHostsChoice();
    testBidiFallbackFollowsTheHostsChoice();
    testAPatchKeepsTheStyleBytesOfAFreshBuild();
    testAReplayKeepsTheParsesTextCut();
    std::printf("%s (%d failed)\n", failures ? "FAILED" : "OK", failures);
    return failures ? 1 : 0;
}
