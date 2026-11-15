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

int main() {
    testAnUnresolvedVarUnsetsTheDeclaration();
    testDirAutoFollowsHtmlAndYieldsToADeclaration();
    testCollapsedBordersFollowTheHostsChoice();
    testBidiFallbackFollowsTheHostsChoice();
    std::printf("%s (%d failed)\n", failures ? "FAILED" : "OK", failures);
    return failures ? 1 : 0;
}
