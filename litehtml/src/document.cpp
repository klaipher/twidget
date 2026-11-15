#include "html.h"
#include <set>
#include "document.h"
#include <string_view>
#include <unordered_set>
#include "document_container.h"
#include "css_parser.h" // Telegram (VENDOR.md edit 29.3): normalize, for a <style media> prelude
#include "el_anchor.h"
#include "el_base.h"
#include "el_body.h"
#include "el_break.h"
#include "el_cdata.h"
#include "el_comment.h"
#include "el_div.h"
#include "el_font.h"
#include "el_form.h"
#include "el_svg.h"
#include "el_image.h"
#include <unordered_set>
#include <map>
#include "el_link.h"
#include "el_para.h"
#include "el_script.h"
#include "el_space.h"
#include "el_style.h"
#include "el_table.h"
#include "el_td.h"
#include "el_text.h"
#include "el_title.h"
#include "el_tr.h"
#include "gumbo.h"
#include "html_tag.h"
#include "render_block.h"
#include "render_block_context.h"
#include "render_flex.h"
#include "render_grid.h"
#include "render_inline.h"
#include "render_item.h"
#include "render_inline_context.h"
#include "render_table.h"
#include "stylesheet.h"
#include "types.h"

#include <cstdlib>
#include <cmath>
#include <iterator>
#include <mutex>
#include <typeinfo>
#include <string>
#include <unordered_map>

namespace litehtml
{

    document::document(document_container* container)
    {
        m_container = container;
    }

    document::~document()
    {
        if(m_gumbo)
        {
            gumbo_destroy_output(&kGumboDefaultOptions, m_gumbo);
            m_gumbo = nullptr;
        }
        // Telegram (VENDOR.md edit 18): the selector-step session points into this document.
        if(m_step_session_open)
            set_selector_step_session(nullptr);
        m_over_element = m_active_element = nullptr;
        if(m_container)
        {
            for(auto& font : m_fonts)
            {
                m_container->delete_font(font.second.font);
            }
        }
    }

    document::ptr document::createFromString(const estring& str, document_container* container,
                                             const std::string& master_styles, const std::string& user_styles)
    {
        // Create litehtml::document
        document::ptr doc = std::make_shared<document>(container);

        // Parse document into GumboOutput
        GumboOutput* output = doc->parse_html(str);

        // mode must be set before doc->create_node because it is used in html_tag::set_attr
        switch(output->document->v.document.doc_type_quirks_mode)
        {
        case GUMBO_DOCTYPE_NO_QUIRKS:
            doc->m_mode = no_quirks_mode;
            break;
        case GUMBO_DOCTYPE_QUIRKS:
            doc->m_mode = quirks_mode;
            break;
        case GUMBO_DOCTYPE_LIMITED_QUIRKS:
            doc->m_mode = limited_quirks_mode;
            break;
        }

        // Create litehtml::elements.
        elements_list root_elements;
        doc->create_node(output->root, root_elements, true, true);
        element::ptr root;
        if(!root_elements.empty())
        {
            root = root_elements.back();
        }

        // Destroy GumboOutput
        gumbo_destroy_output(&kGumboDefaultOptions, output);

        doc->finalize_from_external_root(root, master_styles, user_styles);

        return doc;
    }

    document::ptr document::createUpdatableFromString(const std::string& html, document_container* container,
                                                      const std::string& master_styles)
    {
        document::ptr doc = std::make_shared<document>(container);
        doc->m_text        = html;
        GumboOutput* output = gumbo_parse_with_options(&kGumboDefaultOptions, doc->m_text.data(), doc->m_text.size());
        switch(output->document->v.document.doc_type_quirks_mode)
        {
        case GUMBO_DOCTYPE_NO_QUIRKS:
            doc->m_mode = no_quirks_mode;
            break;
        case GUMBO_DOCTYPE_QUIRKS:
            doc->m_mode = quirks_mode;
            break;
        case GUMBO_DOCTYPE_LIMITED_QUIRKS:
            doc->m_mode = limited_quirks_mode;
            break;
        }
        doc->m_track_nodes = true;
        elements_list root_elements;
        doc->create_node(output->root, root_elements, true, true);
        doc->m_track_nodes = false;
        element::ptr root;
        if(!root_elements.empty())
        {
            root = root_elements.back();
        }
        doc->m_gumbo         = output;
        doc->m_master_styles = master_styles;
        doc->finalize_from_external_root(root, master_styles, {});
        doc->build_template();
        return doc;
    }

    namespace
    {
        bool same_text(const char* a, const char* b)
        {
            return (a ? std::string_view(a) : std::string_view()) == (b ? std::string_view(b) : std::string_view());
        }

        const GumboVector* node_children(const GumboNode* node)
        {
            if(node->type == GUMBO_NODE_DOCUMENT)
                return &node->v.document.children;
            if(node->type == GUMBO_NODE_ELEMENT || node->type == GUMBO_NODE_TEMPLATE)
                return &node->v.element.children;
            return nullptr;
        }

        bool is_element_node(const GumboNode* node)
        {
            return node->type == GUMBO_NODE_ELEMENT || node->type == GUMBO_NODE_TEMPLATE;
        }

        std::string_view element_name(const GumboNode* node)
        {
            const char* known = gumbo_normalized_tagname(node->v.element.tag);
            if(known[0])
                return known;
            GumboStringPiece piece = node->v.element.original_tag;
            if(piece.data && piece.length >= 2 && piece.data[0] == '<' && piece.data[piece.length - 1] == '>')
                gumbo_tag_from_original_text(&piece);
            return piece.data ? std::string_view(piece.data, piece.length) : std::string_view();
        }

        bool same_element_kind(const GumboNode* a, const GumboNode* b)
        {
            if(a->type != b->type || !is_element_node(a))
                return false;
            const GumboElement& ea = a->v.element;
            const GumboElement& eb = b->v.element;
            if(ea.tag != eb.tag || ea.tag_namespace != eb.tag_namespace)
                return false;
            return ea.tag != GUMBO_TAG_UNKNOWN || element_name(a) == element_name(b);
        }

        std::string_view node_key(const GumboNode* node)
        {
            if(!is_element_node(node))
                return {};
            const GumboVector& attrs = node->v.element.attributes;
            if(attrs.length)
            {
                auto* first = static_cast<const GumboAttribute*>(attrs.data[0]);
                if(first->name && !strcmp(first->name, "data-lh-id"))
                    return first->value ? std::string_view(first->value) : std::string_view();
            }
            const GumboAttribute* attr = gumbo_get_attribute(&attrs, "data-lh-id");
            return attr && attr->value ? std::string_view(attr->value) : std::string_view();
        }

        bool raw_text_element(const GumboNode* node)
        {
            if(!is_element_node(node))
                return false;
            const GumboTag tag = node->v.element.tag;
            return tag == GUMBO_TAG_STYLE || tag == GUMBO_TAG_SCRIPT || tag == GUMBO_TAG_TITLE || tag == GUMBO_TAG_TEXTAREA ||
                   tag == GUMBO_TAG_NOSCRIPT || tag == GUMBO_TAG_XMP || tag == GUMBO_TAG_IFRAME || tag == GUMBO_TAG_NOEMBED ||
                   tag == GUMBO_TAG_NOFRAMES || tag == GUMBO_TAG_PLAINTEXT;
        }

        bool has_document_level_element(const GumboNode* node)
        {
            if(is_element_node(node))
            {
                switch(node->v.element.tag)
                {
                case GUMBO_TAG_HTML:
                case GUMBO_TAG_HEAD:
                case GUMBO_TAG_BODY:
                case GUMBO_TAG_STYLE:
                case GUMBO_TAG_LINK:
                case GUMBO_TAG_BASE:
                case GUMBO_TAG_META:
                case GUMBO_TAG_TITLE:
                case GUMBO_TAG_SCRIPT:
                case GUMBO_TAG_FRAMESET:
                case GUMBO_TAG_SELECT:
                case GUMBO_TAG_OPTION:
                case GUMBO_TAG_OPTGROUP:
                case GUMBO_TAG_DATALIST:
                    return true;
                default:
                    break;
                }
            }
            if(const GumboVector* children = node_children(node))
            {
                for(unsigned int i = 0; i < children->length; i++)
                {
                    if(has_document_level_element(static_cast<const GumboNode*>(children->data[i])))
                        return true;
                }
            }
            return false;
        }

        bool mentions_counters(const std::string& text)
        {
            for(size_t i = 0; i + 7 <= text.size(); i++)
            {
                if((text[i] | 0x20) != 'c' || t_strncasecmp(text.data() + i, "counter", 7) != 0)
                    continue;
                const size_t next = i + 7;
                if(next < text.size() && (text[next] == '-' || text[next] == '(' || ((text[next] | 0x20) == 's')))
                    return true;
            }
            return false;
        }

        void count_elements(const GumboNode* node, int depth, int& elements, int& deepest)
        {
            if(is_element_node(node))
            {
                elements++;
                deepest = std::max(deepest, depth);
                depth++;
            }
            if(const GumboVector* children = node_children(node))
            {
                for(unsigned int i = 0; i < children->length; i++)
                    count_elements(static_cast<const GumboNode*>(children->data[i]), depth, elements, deepest);
            }
        }

        std::string_view quoted_value(const GumboAttribute* attr, char close)
        {
            const GumboStringPiece& v = attr->original_value;
            if(!v.data || v.length < 2 || v.data[0] != close || v.data[v.length - 1] != close)
                return {};
            return std::string_view(v.data + 1, v.length - 2);
        }

        struct selector_names
        {
            std::unordered_set<string_id>& classes;
            std::unordered_set<string_id>& ids;
            std::unordered_set<string_id>& attrs;
            std::unordered_set<string_id>& pseudo; // Telegram (VENDOR.md edit 75)
        };

        void collect_ancestor_names(const css_selector& selector, bool self, selector_names& names)
        {
            for(const css_selector* sel = &selector; sel; sel = sel->m_left.get())
            {
                const bool ancestor = !self || sel != &selector;
                for(const auto& attr : sel->m_right.m_attrs)
                {
                    // Telegram (VENDOR.md edit 75): a pseudo-class whose change reaches the subject's descendants or siblings.
                    if(ancestor && attr.type == select_pseudo_class)
                        names.pseudo.insert(attr.name);
                    if(ancestor)
                    {
                        if(attr.type == select_class)
                            names.classes.insert(attr.name);
                        else if(attr.type == select_id)
                            names.ids.insert(attr.name);
                        else if(attr.type == select_attr)
                            names.attrs.insert(attr.name);
                        else if(attr.type == select_pseudo_class && attr.name == _checked_)
                        {
                            names.attrs.insert(_id("checked"));
                            names.attrs.insert(_id("selected"));
                        } else if(attr.type == select_pseudo_class && (attr.name == _disabled_ || attr.name == _enabled_))
                            names.attrs.insert(_id("disabled"));
                        else if(attr.type == select_pseudo_class && attr.name == _link_)
                            names.attrs.insert(_id("href")); // Telegram (VENDOR.md edit 17): el_anchor pushes :link for an href
                    }
                    // Telegram (VENDOR.md edit 31.4): :lang() reads the lang attribute of the subject and its ancestors.
                    if(attr.type == select_pseudo_class && attr.name == _lang_)
                        names.attrs.insert(_id("lang"));
                    for(const auto& inner : attr.selector_list)
                    {
                        if(inner)
                            collect_ancestor_names(*inner, !ancestor, names);
                    }
                }
            }
        }

        // Telegram (VENDOR.md edit 75): the pseudo-classes `selector` names anywhere (inside :not(), :is() too).
        void collect_pseudo_classes(const css_selector& selector, std::unordered_set<string_id>& names)
        {
            for(const css_selector* sel = &selector; sel; sel = sel->m_left.get())
            {
                for(const auto& attr : sel->m_right.m_attrs)
                {
                    if(attr.type == select_pseudo_class)
                        names.insert(attr.name);
                    for(const auto& inner : attr.selector_list)
                    {
                        if(inner)
                            collect_pseudo_classes(*inner, names);
                    }
                }
            }
        }

        // Telegram (VENDOR.md edit 77): the classes, ids and attribute names `selector` names anywhere (inside :not(),
        // :is() too), with the attributes the pseudo-classes read (as collect_ancestor_names maps them).
        void collect_names(const css_selector& selector, std::unordered_set<string_id>& classes,
                           std::unordered_set<string_id>& ids, std::unordered_set<string_id>& attrs)
        {
            for(const css_selector* sel = &selector; sel; sel = sel->m_left.get())
            {
                for(const auto& attr : sel->m_right.m_attrs)
                {
                    if(attr.type == select_class)
                        classes.insert(attr.name);
                    else if(attr.type == select_id)
                        ids.insert(attr.name);
                    else if(attr.type == select_attr)
                        attrs.insert(attr.name);
                    else if(attr.type == select_pseudo_class && attr.name == _checked_)
                    {
                        attrs.insert(_id("checked"));
                        attrs.insert(_id("selected"));
                    } else if(attr.type == select_pseudo_class && (attr.name == _disabled_ || attr.name == _enabled_))
                        attrs.insert(_id("disabled"));
                    else if(attr.type == select_pseudo_class && attr.name == _link_)
                        attrs.insert(_id("href"));
                    else if(attr.type == select_pseudo_class && attr.name == _lang_)
                        attrs.insert(_id("lang"));
                    for(const auto& inner : attr.selector_list)
                    {
                        if(inner)
                            collect_names(*inner, classes, ids, attrs);
                    }
                }
            }
        }

        // Telegram (VENDOR.md edit 31.5): whether `selector` uses :empty anywhere (inside :not(), :is() too).
        bool uses_empty(const css_selector& selector)
        {
            for(const css_selector* sel = &selector; sel; sel = sel->m_left.get())
            {
                for(const auto& attr : sel->m_right.m_attrs)
                {
                    if(attr.type == select_pseudo_class && attr.name == _empty_)
                        return true;
                    for(const auto& inner : attr.selector_list)
                    {
                        if(inner && uses_empty(*inner))
                            return true;
                    }
                }
            }
            return false;
        }

        bool restylable_in_place(const element& el)
        {
            const std::type_info& type = typeid(el);
            return type == typeid(html_tag) || type == typeid(el_div) || type == typeid(el_para) || type == typeid(el_td) ||
                   type == typeid(el_tr) || type == typeid(el_body);
        }

        void scan_selector(const css_selector& selector, bool& attributes, bool& positions)
        {
            for(const css_selector* sel = &selector; sel; sel = sel->m_left.get())
            {
                if(sel->m_left && (sel->m_combinator == combinator_adjacent_sibling || sel->m_combinator == combinator_general_sibling))
                {
                    attributes = true;
                    positions  = true;
                }
                for(const auto& attr : sel->m_right.m_attrs)
                {
                    if(attr.type == select_pseudo_class)
                    {
                        switch(attr.name)
                        {
                        case _only_child_:
                        case _only_of_type_:
                        case _first_child_:
                        case _first_of_type_:
                        case _last_child_:
                        case _last_of_type_:
                        case _nth_child_:
                        case _nth_of_type_:
                        case _nth_last_child_:
                        case _nth_last_of_type_:
                            positions = true;
                            if(!attr.selector_list.empty())
                                attributes = true;
                            break;
                        default:
                            break;
                        }
                    }
                    for(const auto& inner : attr.selector_list)
                    {
                        if(inner)
                            scan_selector(*inner, attributes, positions);
                    }
                }
            }
        }
    }

    namespace
    {
        // Telegram (VENDOR.md edit 42): scan_selector, telling a positional pseudo-class in the subject compound (or in a
        // selector list inside it) from one anywhere else: `subject_positions` for the first, `positions` for the second.
        void scan_positions(const css_selector& selector, bool& attributes, bool& positions, bool& subject_positions,
                            bool& backward, bool& backward_of, bool subject)
        {
            for(const css_selector* sel = &selector; sel; sel = sel->m_left.get())
            {
                const bool here = subject && sel == &selector;
                if(sel->m_left && (sel->m_combinator == combinator_adjacent_sibling || sel->m_combinator == combinator_general_sibling))
                {
                    attributes = true;
                    positions  = true;
                }
                for(const auto& attr : sel->m_right.m_attrs)
                {
                    if(attr.type == select_pseudo_class)
                    {
                        switch(attr.name)
                        {
                        case _only_child_:
                        case _only_of_type_:
                        case _first_child_:
                        case _first_of_type_:
                        case _last_child_:
                        case _last_of_type_:
                        case _nth_child_:
                        case _nth_of_type_:
                        case _nth_last_child_:
                        case _nth_last_of_type_:
                            (here ? subject_positions : positions) = true;
                            if(!attr.selector_list.empty())
                                attributes = true;
                            // Telegram (VENDOR.md edit 90): one that reads the following siblings.
                            if(!here && (attr.name == _last_child_ || attr.name == _last_of_type_ ||
                                         attr.name == _only_child_ || attr.name == _only_of_type_ ||
                                         attr.name == _nth_last_child_ || attr.name == _nth_last_of_type_))
                                backward = true;
                            if(!attr.selector_list.empty() &&
                               (attr.name == _nth_last_child_ || attr.name == _nth_last_of_type_))
                                backward_of = true;
                            break;
                        default:
                            break;
                        }
                    }
                    for(const auto& inner : attr.selector_list)
                    {
                        if(inner)
                            scan_positions(*inner, attributes, positions, subject_positions, backward, backward_of, here);
                    }
                }
            }
        }
    } // namespace

    // The document as literal text with holes at its style attribute values and text nodes. A frame that
    // differs only inside the holes is matched without parsing: a quoted attribute value cannot contain its
    // quote and text cannot contain '<', so each hole ends at the first delimiter that starts the next literal.
    static bool svg_geometry_attribute(const char* name)
    {
        static const std::unordered_set<std::string>& names = *new std::unordered_set<std::string>{
            "points", "d", "x", "y", "x1", "y1", "x2", "y2", "cx", "cy", "r", "rx", "ry", "dx", "dy", "transform",
            "fill", "stroke", "stroke-width", "stroke-dasharray", "stroke-dashoffset", "opacity", "fill-opacity",
            "stroke-opacity", "offset", "stop-color", "stop-opacity", "text-anchor", "width", "height", "display", "visibility",
        };
        return names.count(name) != 0;
    }

    static std::vector<std::pair<std::string, std::string>> split_style_declarations(const std::string& text)
    {
        std::vector<std::pair<std::string, std::string>> out;
        size_t start = 0;
        int    depth = 0;
        char   quote = 0;
        for(size_t i = 0; i <= text.size(); i++)
        {
            const char c = i < text.size() ? text[i] : ';';
            if(quote)
            {
                if(c == quote)
                    quote = 0;
                continue;
            }
            if(c == '"' || c == '\'')
                quote = c;
            else if(c == '(')
                depth++;
            else if(c == ')')
                depth--;
            else if(c == ';' && depth <= 0)
            {
                std::string decl = text.substr(start, i - start);
                start = i + 1;
                size_t colon = decl.find(':');
                if(colon == std::string::npos)
                {
                    if(decl.find_first_not_of(" \t\n") != std::string::npos)
                        out.emplace_back(decl, std::string());
                    continue;
                }
                size_t a = decl.find_first_not_of(" \t\n"), b = decl.find_last_not_of(" \t\n", colon - 1);
                std::string name = a == std::string::npos || a >= colon ? std::string() : decl.substr(a, b - a + 1);
                if(name.compare(0, 2, "--") != 0)
                    for(char& ch : name)
                        if(ch >= 'A' && ch <= 'Z')
                            ch = static_cast<char>(ch - 'A' + 'a');
                out.emplace_back(name, decl.substr(colon + 1));
            }
        }
        return out;
    }

    // A style hole may be patched in place unless the edit touches custom properties, var() or counters,
    // whose effects reach other elements.
    static bool paint_only_property(const std::string& name)
    {
        static const std::unordered_set<std::string>& paint = *new std::unordered_set<std::string>{
            "color", "background", "background-color", "background-image", "background-position", "background-repeat",
            "opacity", "transform", "transform-origin", "box-shadow", "text-shadow", "filter", "outline", "outline-color",
            "outline-width", "outline-style", "outline-offset", "visibility", "fill", "fill-opacity", "stroke", "stroke-opacity",
            "stroke-width", "stroke-dasharray", "stroke-dashoffset", "stop-color", "stop-opacity", "border-color",
            "border-top-color", "border-right-color", "border-bottom-color", "border-left-color", "text-decoration-color",
            "caret-color", "cursor", "-lh-anim"};
        return paint.count(name) != 0;
    }

    // Telegram (VENDOR.md edit 63): a property only layout reads: a box's size, margins, padding, offsets, gaps and flex
    // sizes. Creating and initialising render items reads display, position, float, list-style-type, white-space and
    // flex-direction, never these, so a change of them keeps the render tree; the restyle marks the box's layout dirty.
    static bool geometry_only_property(const std::string& name)
    {
        static const std::unordered_set<std::string>& geometry = *new std::unordered_set<std::string>{
            "width", "height", "min-width", "min-height", "max-width", "max-height", "margin", "margin-top", "margin-right",
            "margin-bottom", "margin-left", "padding", "padding-top", "padding-right", "padding-bottom", "padding-left",
            "top", "right", "bottom", "left", "inset", "gap", "row-gap", "column-gap", "flex-grow", "flex-shrink",
            "flex-basis"};
        return geometry.count(name) != 0;
    }

    // Telegram (VENDOR.md edit 27): whether a change of the property can change paint order or start or end a paint
    // group (commit_patch tells the render observer).
    static bool paint_order_property(const std::string& property)
    {
        return property == "opacity" || property == "transform" || property == "transform-origin" || property == "filter" ||
               property == "visibility" || property == "-lh-anim";
    }

    // Telegram (VENDOR.md edit 27): the properties whose declaration one text has and the other has not, or has with
    // another value.
    static std::vector<std::string> changed_declarations(const std::string& before, const std::string& after)
    {
        // Telegram (VENDOR.md edit 62): a value without the whitespace around it, so that a re-serialization (CSSOM
        // writes `width: 40px;` for `width:40px`) changes nothing.
        auto trimmed = [](const std::string& value) {
            const size_t a = value.find_first_not_of(" \t\n");
            return a == std::string::npos ? std::string() : value.substr(a, value.find_last_not_of(" \t\n") - a + 1);
        };
        std::map<std::string, std::string> left, right;
        for(const auto& d : split_style_declarations(before))
            left[d.first] = trimmed(d.second);
        for(const auto& d : split_style_declarations(after))
            right[d.first] = trimmed(d.second);
        std::vector<std::string> changed;
        for(const auto& d : left)
        {
            auto it = right.find(d.first);
            if(it == right.end() || it->second != d.second)
                changed.push_back(d.first);
        }
        for(const auto& d : right)
        {
            if(!left.count(d.first))
                changed.push_back(d.first);
        }
        return changed;
    }

    static bool style_change_is_background_color(const std::string& before, const std::string& after)
    {
        if(getenv("LH_NO_BACKGROUND_PATCH") || before.find_first_of("\\{}") != std::string::npos ||
           after.find_first_of("\\{}") != std::string::npos || before.find("/*") != std::string::npos || after.find("/*") != std::string::npos ||
           before.find("counter") != std::string::npos || after.find("counter") != std::string::npos)
            return false;
        auto a = split_style_declarations(before), b = split_style_declarations(after);
        auto remove_background = [](auto& declarations) {
            declarations.erase(std::remove_if(declarations.begin(), declarations.end(), [](const auto& entry) {
                return entry.first == "background-color" || entry.first == "-lh-anim";
            }), declarations.end());
        };
        remove_background(a);
        remove_background(b);
        return a == b;
    }

    static bool style_change_property_patch(const std::string& before, const std::string& after, document::property_patch& patch)
    {
        static const bool disabled = getenv("LH_NO_PROPERTY_PATCH") != nullptr;
        if(disabled)
            return false;
        for(const std::string* text : {&before, &after})
        {
            if(text->find_first_of("\\{}!\"'") != std::string::npos || text->find("/*") != std::string::npos ||
               text->find("var(") != std::string::npos || text->find("--") != std::string::npos ||
               text->find("counter") != std::string::npos || text->find("-lh-") != std::string::npos ||
               text->find("url(") != std::string::npos)
                return false;
        }
        auto a = split_style_declarations(before), b = split_style_declarations(after);
        std::map<std::string, std::string> left, right;
        auto collect = [](const std::vector<std::pair<std::string, std::string>>& list, std::map<std::string, std::string>& out) {
            for(const auto& d : list)
            {
                if(d.first.empty() || d.first == "all" || d.first == "background" ||
                   d.first.find_first_not_of("abcdefghijklmnopqrstuvwxyz-") != std::string::npos ||
                   !out.emplace(d.first, d.second).second)
                    return false;
            }
            return true;
        };
        if(!collect(a, left) || !collect(b, right))
            return false;
        for(const auto& d : left)
            if(!right.count(d.first))
                return false;
        for(const auto& d : b)
        {
            auto it = left.find(d.first);
            if(it != left.end() && it->second == d.second)
                continue;
            string_id id;
            if(d.first == "transform")
                id = _transform_;
            else if(d.first == "opacity")
                id = _opacity_;
            else if(d.first == "background-color")
                id = _background_color_;
            else
                return false;
            if(id != _background_color_)
                patch.box = false;
            patch.names.push_back(id);
            patch.text += d.first;
            patch.text += ':';
            patch.text += d.second;
            patch.text += ';';
        }
        return !patch.names.empty();
    }

    static bool style_change_is_width_only(const std::string& before, const std::string& after)
    {
        std::map<std::string, std::string> declarations[2];
        std::vector<std::pair<std::string, std::string>> unchanged[2];
        size_t index = 0;
        for(const std::string* text : {&before, &after})
        {
            const std::string lower = lowcase(*text);
            if(text->find_first_of("\\{}!\"'") != std::string::npos || text->find("/*") != std::string::npos ||
               lower.find("var(") != std::string::npos || lower.find("--") != std::string::npos ||
               lower.find("counter") != std::string::npos || lower.find("-lh-") != std::string::npos ||
               lower.find("url(") != std::string::npos)
                return false;
            auto& values = declarations[index++];
            for(const auto& entry : split_style_declarations(*text))
            {
                if(entry.first.empty() || entry.first == "all" ||
                   entry.first.find_first_not_of("abcdefghijklmnopqrstuvwxyz-") != std::string::npos ||
                   !values.emplace(entry.first, entry.second).second)
                    return false;
                if(entry.first != "width")
                    unchanged[index - 1].push_back(entry);
            }
            auto width = values.find("width");
            if(width == values.end())
                return false;
            const css_token_vector tokens = normalize(width->second, f_componentize);
            css_length length;
            if(tokens.size() != 1 || !length.from_token(tokens.front(), f_length_percentage | f_positive) ||
               length.is_predefined() || !std::isfinite(length.val()) ||
               (length.units() != css_units_px && length.units() != css_units_percentage))
                return false;
            values.erase(width);
        }
        return unchanged[0] == unchanged[1];
    }

    static bool shallow_width_owner(const element::ptr& owner)
    {
        const std::type_info& type = typeid(*owner);
        if(type != typeid(html_tag) && type != typeid(el_div) && type != typeid(el_para) && type != typeid(el_body))
            return false;
        switch(owner->css().get_display())
        {
        case display_block:
        case display_inline_block:
        case display_flex:
        case display_inline_flex:
        case display_grid:
        case display_inline_grid:
            return std::static_pointer_cast<html_tag>(owner)->can_restyle_width_shallow();
        default:
            return false;
        }
    }

    static bool has_generated_children(const element::ptr& owner)
    {
        for(const auto& child : owner->children())
        {
            if(child->tag() == __tag_before_ || child->tag() == __tag_after_)
                return true;
        }
        return false;
    }

    static bool style_change_is_paint_only(const std::string& before, const std::string& after)
    {
        auto a = split_style_declarations(before), b = split_style_declarations(after);
        std::map<std::string, std::string> left, right;
        for(const auto& d : a)
            left[d.first] = d.second;
        for(const auto& d : b)
            right[d.first] = d.second;
        for(const auto& d : left)
        {
            auto it = right.find(d.first);
            if((it == right.end() || it->second != d.second) && !paint_only_property(d.first))
                return false;
        }
        for(const auto& d : right)
        {
            if(!left.count(d.first) && !paint_only_property(d.first))
                return false;
        }
        return true;
    }

    // The edit only repaints inside the element's own border box (svg = false), or inside the outermost <svg>
    // around it (svg = true); nothing it changes is inherited or paints outside those boxes.
    static int style_change_region(const std::string& before, const std::string& after)
    {
        static const std::unordered_set<std::string>& box = *new std::unordered_set<std::string>{
            "background", "background-color", "background-image", "background-position", "background-repeat",
            "border-color", "border-top-color", "border-right-color", "border-bottom-color", "border-left-color",
            "caret-color", "cursor", "-lh-anim"};
        static const std::unordered_set<std::string>& svg = *new std::unordered_set<std::string>{
            "fill", "fill-opacity", "stroke", "stroke-opacity", "stroke-width", "stroke-dasharray", "stroke-dashoffset",
            "stop-color", "stop-opacity"};
        auto a = split_style_declarations(before), b = split_style_declarations(after);
        std::map<std::string, std::string> left, right;
        for(const auto& d : a)
            left[d.first] = d.second;
        for(const auto& d : b)
            right[d.first] = d.second;
        int  region = 1;
        auto note   = [&](const std::string& name) {
            if(box.count(name))
                return;
            if(name == "color" || name == "text-decoration-color")
            {
                if(region == 1)
                    region = 3;
                return;
            }
            region = svg.count(name) && region > 0 ? 2 : 0;
        };
        for(const auto& d : left)
        {
            auto it = right.find(d.first);
            if(it == right.end() || it->second != d.second)
                note(d.first);
        }
        for(const auto& d : right)
        {
            if(!left.count(d.first))
                note(d.first);
        }
        return region;
    }

    static int declared_region(const std::string& name)
    {
        static const std::unordered_set<std::string>& box = *new std::unordered_set<std::string>{
            "border-color", "border-top-color", "border-right-color", "border-bottom-color", "border-left-color", "caret-color", "cursor"};
        if(name.compare(0, 10, "background") == 0 || box.count(name))
            return 1;
        if(name == "color" || name == "text-decoration-color")
            return 3;
        return 0;
    }

    static bool push_text_owner_region(const element::ptr& owner, std::vector<element::ptr>& regions)
    {
        static const bool disabled = getenv("LH_NO_COLOR_REGIONS") != nullptr;
        if(disabled || !owner || owner->children().empty() || owner->css().get_display() == display_list_item)
            return false;
        for(const auto& child : owner->children())
        {
            if(!child->is_text())
                return false;
        }
        regions.push_back(owner);
        for(const auto& child : owner->children())
        {
            if(typeid(*child) == typeid(el_text))
                regions.push_back(child);
        }
        return true;
    }

    static bool text_regions_disabled()
    {
        static const bool disabled = getenv("LH_NO_TEXT_REGIONS") != nullptr;
        return disabled;
    }

    static element::ptr outermost_svg(const element::ptr& el)
    {
        element::ptr found;
        if(!el)
            return found;
        if(auto doc = el->get_document(); doc && !doc->has_svg())
            return found;
        for(auto p = el; p; p = p->parent())
        {
            if(std::dynamic_pointer_cast<el_svg>(p))
                found = p;
        }
        return found;
    }

    static bool style_change_is_local(const std::string& before, const std::string& after)
    {
        if(before.find("counter") != std::string::npos || after.find("counter") != std::string::npos)
            return false;
        auto a = split_style_declarations(before), b = split_style_declarations(after);
        auto sensitive = [](const std::pair<std::string, std::string>& d) {
            return d.first.compare(0, 2, "--") == 0;
        };
        size_t i = 0;
        while(i < a.size() && i < b.size() && a[i] == b[i])
            i++;
        size_t ea = a.size(), eb = b.size();
        while(ea > i && eb > i && a[ea - 1] == b[eb - 1])
        {
            ea--;
            eb--;
        }
        for(size_t k = i; k < ea; k++)
            if(sensitive(a[k]))
                return false;
        for(size_t k = i; k < eb; k++)
            if(sensitive(b[k]))
                return false;
        return true;
    }

    void document::build_template()
    {
        m_template_ok = false;
        m_literals.clear();
        m_holes.clear();
        m_style_holes.clear();
        m_owner_holes.clear();
        m_node_holes.clear();
        if(!m_gumbo)
            return;
        const char* base = m_text.data();
        const size_t size = m_text.size();
        struct span
        {
            size_t   start, end;
            html_hole hole;
        };
        std::vector<span> spans;
        bool              ok = true;
        std::unordered_set<std::string> selected;
        for(const char* p = static_cast<const char*>(memchr(base, '[', size)); p; p = static_cast<const char*>(memchr(p + 1, '[', base + size - p - 1)))
        {
            const char* q = p + 1;
            while(q < base + size && (isalnum(static_cast<unsigned char>(*q)) || *q == '-' || *q == '_' || *q == ':'))
                q++;
            if(q > p + 1)
                selected.emplace(p + 1, q - p - 1);
            if(q >= base + size - 1)
                break;
        }
        std::function<void(const GumboNode*, bool)> walk = [&](const GumboNode* node, bool raw) {
            if(!ok)
                return;
            switch(node->type)
            {
            case GUMBO_NODE_DOCUMENT:
                for(unsigned int i = 0; i < node->v.document.children.length; i++)
                    walk(static_cast<const GumboNode*>(node->v.document.children.data[i]), false);
                break;
            case GUMBO_NODE_ELEMENT:
            case GUMBO_NODE_TEMPLATE:
            {
                const GumboElement& e = node->v.element;
                if(m_node_elements.find(node) == m_node_elements.end())
                    break;
                for(unsigned int i = 0; i < e.attributes.length; i++)
                {
                    auto*      attr = static_cast<const GumboAttribute*>(e.attributes.data[i]);
                    const bool src  = (e.tag == GUMBO_TAG_IMG || e.tag == GUMBO_TAG_CANVAS) && strcmp(attr->name, "src") == 0;
                    const bool geometry = !src && e.tag_namespace == GUMBO_NAMESPACE_SVG && e.tag != GUMBO_TAG_SVG &&
                                          svg_geometry_attribute(attr->name) && !selected.count(attr->name);
                    const bool cls = !src && !geometry && e.tag_namespace == GUMBO_NAMESPACE_HTML && strcmp(attr->name, "class") == 0;
                    if(!src && !geometry && !cls && strcmp(attr->name, "style") != 0)
                        continue;
                    const GumboStringPiece& v = attr->original_value;
                    auto it = m_node_elements.find(node);
                    if(!v.data || v.length < 2 || (v.data[0] != '"' && v.data[0] != '\'') || v.data < base ||
                       v.data + v.length > base + size || it == m_node_elements.end() || it->second.size() != 1)
                    {
                        if(cls)
                            continue;
                        ok = false;
                        return;
                    }
                    span s;
                    s.start       = (size_t)(v.data - base) + 1;
                    s.end         = (size_t)(v.data - base) + v.length - 1;
                    s.hole.close  = v.data[0];
                    s.hole.src    = src;
                    s.hole.cls    = cls;
                    if(geometry)
                        s.hole.attr = attr->name;
                    s.hole.owner  = it->second.front();
                    s.hole.node   = node;
                    s.hole.name   = attr->name;
                    s.hole.value  = std::string(base + s.start, s.end - s.start);
                    spans.push_back(std::move(s));
                }
                const char* content = e.original_tag.data ? e.original_tag.data + e.original_tag.length : nullptr;
                const bool textarea_newline = e.tag == GUMBO_TAG_TEXTAREA && content && content < base + size &&
                                              (*content == '\n' || *content == '\r');
                const bool child_raw = e.tag == GUMBO_TAG_STYLE || e.tag == GUMBO_TAG_SCRIPT || e.tag == GUMBO_TAG_TITLE ||
                                       textarea_newline || e.tag == GUMBO_TAG_NOSCRIPT;
                for(unsigned int i = 0; i < e.children.length; i++)
                    walk(static_cast<const GumboNode*>(e.children.data[i]), child_raw);
                break;
            }
            case GUMBO_NODE_TEXT:
            {
                if(raw)
                    break;
                const GumboStringPiece& t  = node->v.text.original_text;
                auto                    it = m_node_elements.find(node);
                if(!t.data || t.length == 0 || t.data < base || t.data + t.length > base + size ||
                   it == m_node_elements.end() || it->second.empty())
                {
                    ok = false;
                    return;
                }
                span s;
                s.start                = (size_t)(t.data - base);
                s.end                  = s.start + t.length;
                s.hole.text            = true;
                s.hole.node            = node;
                s.hole.text_elements   = it->second;
                s.hole.value           = std::string(base + s.start, t.length);
                if(s.end >= size || base[s.end] != '<')
                {
                    ok = false;
                    return;
                }
                spans.push_back(std::move(s));
                break;
            }
            default:
                break;
            }
        };
        walk(m_gumbo->document, false);
        if(!ok)
            return;
        std::sort(spans.begin(), spans.end(), [](const span& a, const span& b) { return a.start < b.start; });
        size_t pos = 0;
        for(auto& s : spans)
        {
            if(s.start < pos)
                return;
            m_literals.emplace_back(base + pos, s.start - pos);
            m_holes.push_back(std::move(s.hole));
            pos = s.end;
        }
        m_literals.emplace_back(base + pos, size - pos);
        for(size_t i = 0; i < m_holes.size(); i++)
            m_node_holes[m_holes[i].node].push_back(i);
        m_template_ok = true;
    }

    namespace
    {
        bool table_part(style_display display)
        {
            switch(display)
            {
            case display_table_cell:
            case display_table_row:
            case display_table_column:
            case display_table_column_group:
            case display_table_row_group:
            case display_table_header_group:
            case display_table_footer_group:
            case display_table_caption:
                return true;
            default:
                return false;
            }
        }

        bool tree_neutral_change(const std::string& before, const std::string& after, bool inline_box, bool table_cached)
        {
            static const std::unordered_set<std::string>& neutral = *new std::unordered_set<std::string>{
                "width", "height", "min-width", "min-height", "max-width", "max-height", "left", "top", "right", "bottom",
                "margin", "margin-left", "margin-right", "margin-top", "margin-bottom", "padding", "padding-left",
                "padding-right", "padding-top", "padding-bottom", "border-radius", "border-top-left-radius",
                "border-top-right-radius", "border-bottom-left-radius", "border-bottom-right-radius", "border-color",
                "border-top-color", "border-right-color", "border-bottom-color", "border-left-color", "color", "background",
                "background-color", "background-image", "background-position", "background-size", "opacity", "transform",
                "transform-origin", "box-shadow", "text-shadow", "outline-color", "filter", "fill", "stroke", "stroke-width",
                "stroke-dasharray", "stroke-dashoffset", "fill-opacity", "stroke-opacity", "z-index", "text-align",
                "vertical-align", "letter-spacing", "flex-grow", "flex-shrink", "flex-basis", "gap", "row-gap", "column-gap",
                "justify-content", "align-items", "align-self"};
            auto parse = [](const std::string& text, std::map<std::string, std::string>& out) {
                if(text.find_first_of("\"'\\{}") != std::string::npos || text.find("/*") != std::string::npos ||
                   text.find("url(") != std::string::npos)
                    return false;
                size_t start = 0;
                while(start <= text.size())
                {
                    size_t end   = text.find(';', start);
                    std::string part = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
                    size_t colon = part.find(':');
                    if(colon != std::string::npos)
                    {
                        std::string name = part.substr(0, colon);
                        name.erase(0, name.find_first_not_of(" \t\n"));
                        name.erase(name.find_last_not_of(" \t\n") + 1);
                        for(char& c : name)
                            c = char(tolower(static_cast<unsigned char>(c)));
                        out[name] = part.substr(colon + 1);
                    } else if(part.find_first_not_of(" \t\n") != std::string::npos)
                        return false;
                    if(end == std::string::npos)
                        break;
                    start = end + 1;
                }
                return true;
            };
            std::map<std::string, std::string> a, b;
            if(!parse(before, a) || !parse(after, b))
                return false;
            auto allowed = [&](const std::string& name) {
                if(inline_box && (name.compare(0, 6, "margin") == 0 || name.compare(0, 7, "padding") == 0 || name == "left" ||
                                  name == "top" || name == "right" || name == "bottom"))
                    return false;
                if(table_cached && (name == "width" || name == "height" || name.compare(0, 4, "min-") == 0 ||
                                    name.compare(0, 4, "max-") == 0 || name.compare(0, 7, "padding") == 0 ||
                                    name.compare(0, 6, "margin") == 0))
                    return false;
                return neutral.count(name) != 0;
            };
            for(const auto& kv : a)
            {
                auto it = b.find(kv.first);
                if((it == b.end() || it->second != kv.second) && !allowed(kv.first))
                    return false;
            }
            for(const auto& kv : b)
            {
                if(!a.count(kv.first) && !allowed(kv.first))
                    return false;
            }
            return true;
        }
    }

    // 1: patched (or nothing changed), 0: the frame does not fit the template, -1: fits but needs a rebuild.
    int document::update_from_template(const std::string& html, bool* changed)
    {
        invalidate_paint();
        std::vector<std::pair<size_t, std::string>> changes;
        size_t                                      pos = 0;
        for(size_t i = 0; i < m_holes.size(); i++)
        {
            const std::string& literal = m_literals[i];
            if(html.compare(pos, literal.size(), literal) != 0)
                return 0;
            pos += literal.size();
            size_t end = html.find(m_holes[i].text ? '<' : m_holes[i].close, pos);
            if(end == std::string::npos)
                return 0;
            if(html.compare(pos, end - pos, m_holes[i].value) != 0)
                changes.emplace_back(i, html.substr(pos, end - pos));
            pos = end;
        }
        if(html.size() - pos != m_literals.back().size() || html.compare(pos, std::string::npos, m_literals.back()) != 0)
            return 0;
        return apply_template_values(changes, changed);
    }

    bool document::update_styles(const std::vector<std::pair<std::shared_ptr<element>, std::string>>& styles, bool* changed)
    {
        invalidate_paint();
        m_paint_only_patch = false;
        m_patch_confined   = false;
        m_patch_regions.clear();
        if(changed)
            *changed = false;
        if(!m_root || !m_finalized || !m_template_ok || getenv("LH_NO_TEMPLATE"))
            return false;
        if(m_style_holes.empty())
        {
            for(size_t i = 0; i < m_holes.size(); i++)
            {
                const html_hole& hole = m_holes[i];
                if(!hole.text && !hole.src && !hole.cls && hole.attr.empty() && hole.owner)
                    m_style_holes.emplace(hole.owner.get(), i);
            }
        }
        std::vector<std::pair<size_t, std::string>> changes;
        changes.reserve(styles.size());
        for(const auto& entry : styles)
        {
            auto it = entry.first ? m_style_holes.find(entry.first.get()) : m_style_holes.end();
            if(it == m_style_holes.end())
                return false;
            const html_hole& hole = m_holes[it->second];
            if(entry.second.find(hole.close) != std::string::npos || entry.second.find('<') != std::string::npos)
                return false;
            if(entry.second == hole.value)
                continue;
            for(const auto& change : changes)
            {
                if(change.first == it->second)
                    return false;
            }
            changes.emplace_back(it->second, entry.second);
        }
        std::sort(changes.begin(), changes.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        return apply_template_values(changes, changed) == 1;
    }

    void document::begin_layout_patch()
    {
        m_layout_origins.clear();
        m_restyle_regions.clear();
        m_restyle_confined    = true;
        m_layout_before_valid = false;
        m_layout_after_render = false;
        if(m_root_render && layout_cache_enabled())
            snapshot_layout();
    }

    bool document::update_holes(const std::vector<hole_change>& changes, bool* changed)
    {
        invalidate_paint();
        m_paint_only_patch = false;
        m_patch_confined   = false;
        m_patch_regions.clear();
        if(changed)
            *changed = false;
        if(!m_root || !m_finalized || !m_template_ok || getenv("LH_NO_TEMPLATE"))
            return false;
        if(m_owner_holes.empty())
        {
            for(size_t i = 0; i < m_holes.size(); i++)
            {
                const html_hole& hole = m_holes[i];
                const element*   owner = nullptr;
                if(!hole.text)
                    owner = hole.owner.get();
                else if(!hole.text_elements.empty() && hole.text_elements.front()->parent())
                    owner = hole.text_elements.front()->parent().get();
                if(owner)
                    m_owner_holes[owner].push_back(i);
            }
        }
        std::vector<std::pair<size_t, std::string>> list;
        std::vector<size_t>                         seen;
        list.reserve(changes.size());
        seen.reserve(changes.size());
        for(const auto& change : changes)
        {
            auto it = change.owner ? m_owner_holes.find(change.owner.get()) : m_owner_holes.end();
            if(it == m_owner_holes.end())
                return false;
            size_t found = m_holes.size();
            for(size_t i : it->second)
            {
                const html_hole& hole = m_holes[i];
                if(change.text ? !hole.text : (hole.text || hole.name != change.name))
                    continue;
                if(found != m_holes.size())
                    return false;
                found = i;
            }
            if(found == m_holes.size())
                return false;
            const html_hole& hole = m_holes[found];
            if(change.value.find('<') != std::string::npos || (!hole.text && change.value.find(hole.close) != std::string::npos))
                return false;
            if(hole.text && (hole.text_elements.empty() || hole.text_elements.front()->parent().get() != change.owner.get()))
                return false;
            if(std::find(seen.begin(), seen.end(), found) != seen.end())
                return false;
            seen.push_back(found);
            if(change.value != hole.value)
                list.emplace_back(found, change.value);
        }
        std::sort(list.begin(), list.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        return apply_template_values(list, changed) == 1;
    }

    bool document::template_html(std::string& out) const
    {
        if(!m_template_ok || m_literals.size() != m_holes.size() + 1)
            return false;
        size_t size = 0;
        for(const auto& literal : m_literals)
            size += literal.size();
        for(const auto& hole : m_holes)
            size += hole.value.size();
        out.clear();
        out.reserve(size);
        for(size_t i = 0; i < m_holes.size(); i++)
        {
            out += m_literals[i];
            out += m_holes[i].value;
        }
        out += m_literals.back();
        return true;
    }

    bool document::class_change_allowed(const html_hole& hole, const std::string& value, string_map& attrs)
    {
        static const bool disabled = getenv("LH_NO_CLASS_HOLES") != nullptr;
        if(disabled || !hole.owner || !hole.owner->parent() || (selector_scope() & 5) != 0)
            return false;
        for(auto p = hole.owner->parent(); p; p = p->parent())
        {
            if(p->tag() == _select_)
                return false;
        }
        auto tag = std::dynamic_pointer_cast<html_tag>(hole.owner);
        if(!tag)
            return false;
        attrs          = tag->attributes();
        attrs["class"] = value;
        return retag_allowed(hole.owner, hole.owner->get_tagName(), false, attrs, false);
    }

    int document::apply_template_values(std::vector<std::pair<size_t, std::string>>& changes, bool* changed)
    {
        begin_layout_patch();
        invalidate_paint();
        std::vector<size_t>                             dirty;
        std::vector<bool> background_changes(changes.size(), false);
        std::vector<bool> width_changes(changes.size(), false);
        std::vector<property_patch> property_patches(changes.size());
        std::vector<std::pair<size_t, string_map>>      classes;
        for(const auto& change : changes)
        {
            const size_t       i = change.first;
            const std::string& v = change.second;
            if(i >= m_holes.size() || v == m_holes[i].value)
                continue;
            if(v.find_first_of("&\r") != std::string::npos || v.find('\0') != std::string::npos)
                return -1;
            if(m_holes[i].cls)
            {
                string_map attrs;
                if(!class_change_allowed(m_holes[i], v, attrs))
                    return -1;
                classes.emplace_back(&change - changes.data(), std::move(attrs));
                continue;
            }
            if(m_holes[i].text)
            {
                if(v.find_first_not_of(" \t\n\f") == std::string::npos)
                    return -1;
            } else if(!m_holes[i].src && m_holes[i].attr.empty())
            {
                if(style_attributes_selectable())
                    return template_requires_fresh_document;
                property_patch& patch = property_patches[&change - changes.data()];
                if(!style_change_property_patch(m_holes[i].value, v, patch))
                {
                    patch.names.clear();
                    width_changes[&change - changes.data()] = style_change_is_width_only(m_holes[i].value, v);
                    if(width_changes[&change - changes.data()] && has_generated_children(m_holes[i].owner))
                        return template_requires_fresh_document;
                    const bool background = style_change_is_background_color(m_holes[i].value, v);
                    background_changes[&change - changes.data()] = background;
                    if(!background && !style_change_is_local(m_holes[i].value, v))
                        return -1;
                }
            }
            if(m_holes[i].text && !m_holes[i].text_elements.front()->parent())
                return -1;
            if(m_holes[i].text && v[0] == '\n')
            {
                const char* tag = m_holes[i].text_elements.front()->parent()->get_tagName();
                if(tag && (!strcmp(tag, "pre") || !strcmp(tag, "listing") || !strcmp(tag, "textarea")))
                    return -1;
            }
            dirty.push_back(&change - changes.data());
        }
        if(dirty.empty() && classes.empty())
        {
            m_paint_only_patch = true;
            m_patch_confined   = true;
            m_patch_regions.clear();
            return 1;
        }
        if(changed)
            *changed = true;

        std::vector<element::ptr> restyle;
        std::unordered_set<const element*> background_only;
        std::unordered_map<const element*, const property_patch*> patched;
        std::unordered_set<const element*> shallow_width;
        bool allow_shallow_width = classes.empty() && !m_has_svg && m_tabular_elements.empty() &&
                                   !getenv("LH_NO_SHALLOW_WIDTH_PATCH");
        std::vector<element::ptr> new_text;
        std::vector<element::ptr> retext;
        std::vector<std::string>  words;
        bool                      keep_tree  = !getenv("LH_NO_KEEP_TREE");
        bool                      paint_only = !getenv("LH_NO_PAINT_ONLY");
        m_patch_regions.clear();
        m_patch_confined = !getenv("LH_NO_PATCH_REGIONS");
        for(const auto& entry : classes)
        {
            html_hole& hole   = m_holes[changes[entry.first].first];
            int        region = 0;
            auto       tag    = std::static_pointer_cast<html_tag>(hole.owner);
            const bool fast   = tag->flip_class(hole.value, changes[entry.first].second, m_match_epoch, &region);
            if(!fast)
            {
                if(!apply_template_retag(hole.owner, entry.second, true, &region))
                {
                    keep_tree  = false;
                    paint_only = false;
                    tag->forget_flips();
                } else
                {
                    tag->note_flip(hole.value, changes[entry.first].second, region);
                }
            }
            if(region == 1)
                m_restyle_regions.push_back(hole.owner);
            else if(region != 3 || outermost_svg(hole.owner) || !push_text_owner_region(hole.owner, m_restyle_regions))
                m_restyle_confined = false;
            if(m_patch_confined && region == 1)
                m_patch_regions.push_back(hole.owner);
            else if(!m_patch_confined || region != 3 || outermost_svg(hole.owner) || !push_text_owner_region(hole.owner, m_patch_regions))
                m_patch_confined = false;
            hole.value       = changes[entry.first].second;
            if(!fast)
                restyle.push_back(hole.owner);
        }
        static const bool                               resize_only = getenv("LH_NO_SAME_SIZE_TEXT") != nullptr;
        std::vector<std::pair<element::ptr, size>>      measured;
        for(size_t k : dirty)
        {
            const std::string& value   = changes[k].second;
            html_hole&         hole    = m_holes[changes[k].first];
            const bool         inplace = hole.text && retext_in_place(hole.text_elements, value.c_str(), words);
            const bool         background = background_changes[k];
            const property_patch* patch   = property_patches[k].names.empty() ? nullptr : &property_patches[k];
            if(hole.text ? !inplace : hole.src || !hole.attr.empty() || (!width_changes[k] && !patch && !background))
                allow_shallow_width = false;
            if(allow_shallow_width && width_changes[k] && shallow_width_owner(hole.owner))
                shallow_width.insert(hole.owner.get());
            if(!hole.text && !hole.src)
            {
                const int kind = hole.attr.empty() ? (background ? 1 : patch ? (patch->box ? 1 : 0) : style_change_region(hole.value, value)) : 2;
                if(kind == 1)
                    m_restyle_regions.push_back(hole.owner);
                else if(kind == 2 && outermost_svg(hole.owner))
                    m_restyle_regions.push_back(outermost_svg(hole.owner));
                else if(kind != 3 || outermost_svg(hole.owner) || !push_text_owner_region(hole.owner, m_restyle_regions))
                    m_restyle_confined = false;
            }
            if(m_patch_confined)
            {
                element::ptr region;
                bool         words_region = false;
                bool         continue_region = false;
                if(!hole.text && !hole.src)
                {
                    const int kind = hole.attr.empty() ? (background ? 1 : patch ? (patch->box ? 1 : 0) : style_change_region(hole.value, value)) : 2;
                    if(kind == 1)
                        region = hole.owner;
                    else if(kind == 2)
                        region = outermost_svg(hole.owner);
                    else if(kind == 3 && !outermost_svg(hole.owner) && push_text_owner_region(hole.owner, m_patch_regions))
                        continue_region = true;
                } else if(inplace && !text_regions_disabled())
                {
                    region       = outermost_svg(hole.text_elements.front());
                    words_region = !region;
                }
                if(continue_region)
                {
                } else if(words_region)
                {
                    for(const auto& el : hole.text_elements)
                    {
                        if(typeid(*el) == typeid(el_text))
                            m_patch_regions.push_back(el);
                    }
                } else if(region)
                    m_patch_regions.push_back(region);
                else
                    m_patch_confined = false;
            }
            if(paint_only)
            {
                if(hole.src || (hole.text && (!inplace || resize_only)))
                {
                    paint_only = false;
                } else if(!hole.text && !hole.attr.empty())
                {
                    bool inside_svg = false;
                    for(auto p = hole.owner->parent(); p; p = p->parent())
                    {
                        if(std::dynamic_pointer_cast<el_svg>(p))
                        {
                            inside_svg = true;
                            break;
                        }
                    }
                    paint_only = inside_svg;
                } else if(!hole.text)
                {
                    paint_only = background || patch || style_change_is_paint_only(hole.value, value);
                }
            }
            if(inplace)
            {
                hole.value = value;
                for(size_t w = 0; w < words.size(); w++)
                {
                    size before;
                    hole.text_elements[w]->get_content_size(before, 0_px);
                    measured.emplace_back(hole.text_elements[w], before);
                    std::static_pointer_cast<el_text>(hole.text_elements[w])->set_text(words[w]);
                    retext.push_back(hole.text_elements[w]);
                }
                continue;
            }
            if(keep_tree && (hole.src ||
                             (!hole.text && hole.attr.empty() && !background && !patch && !tree_neutral_change(hole.value, value, hole.owner->css().get_display() == display_inline,
                                                                         table_part(hole.owner->css().get_display())))))
                keep_tree = false;
            if(background)
            {
                auto tag = dynamic_cast<html_tag*>(hole.owner.get());
                if(tag && (typeid(*tag) == typeid(html_tag) || typeid(*tag) == typeid(el_div) || typeid(*tag) == typeid(el_td)) &&
                   tag->background_color_is_local())
                    background_only.insert(hole.owner.get());
            }
            hole.value      = value;
            if(hole.src)
            {
                string_map  attrs = {{"src", hole.value}};
                std::string tag   = hole.owner->get_tagName() ? hole.owner->get_tagName() : "img";
                const bool  keep  = m_container->sanitize_element(tag, false, attrs) && attrs.count("src");
                hole.owner->set_attr("src", keep ? attrs["src"].c_str() : "");
                if(auto image = std::dynamic_pointer_cast<el_image>(hole.owner))
                    image->update_src();
                restyle.push_back(hole.owner);
                continue;
            }
            if(!hole.attr.empty())
            {
                string_map  attrs = {{hole.attr, hole.value}};
                std::string tag   = hole.owner->get_tagName();
                const bool  keep  = m_container->sanitize_element(tag, true, attrs) && attrs.count(hole.attr);
                hole.owner->set_attr(hole.attr.c_str(), keep ? attrs[hole.attr].c_str() : "");
                restyle.push_back(hole.owner);
                continue;
            }
            if(!hole.text)
            {
                hole.owner->set_attr("style", hole.value.c_str());
                restyle.push_back(hole.owner);
                if(patch && !outermost_svg(hole.owner))
                    patched[hole.owner.get()] = patch;
                continue;
            }
            elements_list created;
            m_container->split_text(
                hole.value.c_str(),
                [this, &created](const char* text) { created.push_back(std::make_shared<el_text>(text, shared_from_this())); },
                [this, &created](const char* text) { created.push_back(std::make_shared<el_space>(text, shared_from_this())); });
            hole.text_elements.front()->parent()->replace_children(hole.text_elements, created);
            if(keep_tree && !replace_text_renderers(hole.text_elements, created))
                keep_tree = false;
            hole.text_elements.assign(created.begin(), created.end());
            new_text.insert(new_text.end(), created.begin(), created.end());
        }
        std::vector<std::pair<bool, bool>> groups;
        if(paint_only)
        {
            groups.reserve(restyle.size());
            for(const auto& el : restyle)
                groups.emplace_back(el->css().is_paint_group(), el->css().get_transform().empty());
        }
        for(const auto& entry : classes)
        {
            background_only.erase(m_holes[changes[entry.first].first].owner.get());
            patched.erase(m_holes[changes[entry.first].first].owner.get());
        }
        std::unordered_set<const element*> seen;
        for(const auto& el : restyle)
            if(!seen.insert(el.get()).second)
            {
                background_only.erase(el.get());
                patched.erase(el.get());
                shallow_width.erase(el.get());
            }
        for(const auto* list : {&restyle, &new_text, &retext})
            for(const auto& el : *list)
                for(auto parent = el->parent(); parent; parent = parent->parent())
                {
                    background_only.erase(parent.get());
                    patched.erase(parent.get());
                }
        if(!allow_shallow_width || !keep_tree || !new_text.empty())
            shallow_width.clear();
        restyle_after_patch(restyle, new_text, keep_tree, retext, background_only, patched, shallow_width);
        if(!keep_tree || !m_root_render)
            replace_root_render();
        for(size_t k = 0; k < measured.size() && paint_only; k++)
        {
            size after;
            measured[k].first->get_content_size(after, 0_px);
            if(after.width != measured[k].second.width || after.height != measured[k].second.height)
                paint_only = false;
        }
        if(paint_only)
        {
            for(size_t k = 0; k < restyle.size() && paint_only; k++)
            {
                if(restyle[k]->css().is_paint_group() != groups[k].first || restyle[k]->css().get_transform().empty() != groups[k].second)
                    paint_only = false;
            }
        }
        m_paint_only_patch = paint_only && keep_tree;
        if(keep_tree && !m_paint_only_patch)
            forget_positions();
        if(!m_paint_only_patch)
        {
            m_patch_confined = false;
            m_patch_regions.clear();
        }
        return 1;
    }

    bool document::take_table_cells(int count)
    {
        if(m_table_cells + count > max_table_cells)
        {
            if(!m_table_limited)
            {
                m_table_limited = true;
                if(!m_table_quiet) // Telegram (VENDOR.md edit 84)
                    m_container->sanitizer_limit("table");
            }
            return false;
        }
        m_table_cells += count;
        return true;
    }

    // Telegram (VENDOR.md edit 10).
    bool document::text_element_allowed()
    {
#if 0
        if(m_text_elements < max_text_elements + m_text_allowance)
            return true;
#else
        // Telegram (VENDOR.md edit 17): a patch session does not count the pieces it replaced or forgot.
        if(m_text_elements - m_patch_released < max_text_elements + m_text_allowance)
            return true;
#endif
        if(!m_text_limited)
        {
            m_text_limited = true;
            m_container->sanitizer_limit("text");
            note_style_pass_limit("text"); // Telegram (VENDOR.md edit 84)
        }
        return false;
    }

    // Telegram (VENDOR.md edit 19).
    bool document::pseudo_element_allowed()
    {
        // Telegram (VENDOR.md edit 84): not the pseudo-elements the patch session released (held to its commit).
        if(m_build_elements + m_pseudo_elements - m_patch_pseudo_released < max_build_elements)
            return true;
        if(!m_pseudo_limited)
        {
            m_pseudo_limited = true;
            m_container->sanitizer_limit("elements");
            note_style_pass_limit("elements"); // Telegram (VENDOR.md edit 84)
        }
        return false;
    }

    void document::forget_positions()
    {
        if(!m_root_render)
            return;
        // Telegram (VENDOR.md edit 20): what the observer compares when the layout ends is what was there before this.
        if(m_render_observer)
        {
            if(layout_cache_enabled())
            {
                for(render_item* item : m_layout_marked)
                    item->observe_geometry();
            } else
            {
                std::vector<render_item*> observed{m_root_render.get()};
                while(!observed.empty())
                {
                    render_item* item = observed.back();
                    observed.pop_back();
                    item->observe_geometry();
                    for(const auto& child : item->children())
                        observed.push_back(child.get());
                }
            }
        }
        if(layout_cache_enabled())
        {
            for(render_item* item : m_layout_marked)
                item->pos() = position();
            m_layout_marked.clear();
            return;
        }
        std::vector<render_item*> pending{m_root_render.get()};
        while(!pending.empty())
        {
            render_item* item = pending.back();
            pending.pop_back();
            item->pos() = position();
            for(const auto& child : item->children())
                pending.push_back(child.get());
        }
    }

    bool document::retext_in_place(const std::vector<element::ptr>& current, const char* text,
                                   std::vector<std::string>& words) const
    {
        static const bool disabled = getenv("LH_NO_INPLACE_TEXT") != nullptr;
        if(disabled || current.empty() || !text)
            return false;
        const element::ptr parent = current.front()->parent();
        if(!parent)
            return false;
        std::vector<std::pair<bool, std::string>> parts;
        m_container->split_text(
            text, [&parts](const char* word) { parts.emplace_back(false, word); },
            [&parts](const char* space) { parts.emplace_back(true, space); });
        if(parts.size() != current.size())
            return false;
        words.clear();
        words.reserve(parts.size());
        for(size_t i = 0; i < parts.size(); i++)
        {
            const element& el    = *current[i];
            const bool     space = typeid(el) == typeid(el_space);
            if((!space && typeid(el) != typeid(el_text)) || space != parts[i].first || current[i]->parent() != parent)
                return false;
            if(space)
            {
                std::string before;
                el.get_text(before);
                if(before != parts[i].second)
                    return false;
            }
            words.push_back(std::move(parts[i].second));
        }
        return true;
    }

    bool document::replace_text_renderers(const std::vector<element::ptr>& current,
                                          const elements_list& replacement)
    {
#if 0
        if(current.empty() || replacement.empty())
            return false;
        auto owner = current.front()->parent();
        if(!owner || owner->children().size() != replacement.size() ||
           !std::equal(owner->children().begin(), owner->children().end(), replacement.begin()))
            return false;
        std::shared_ptr<render_item> target;
        owner->run_on_renderers([&](const std::shared_ptr<render_item>& item) {
            if(!std::dynamic_pointer_cast<render_item_inline_context>(item) || item->children().size() != current.size())
                return true;
            auto root = item;
            while(auto parent = root->parent())
                root = parent;
            if(root != m_root_render)
                return true;
            auto old = current.begin();
            for(const auto& child : item->children())
            {
                if(child->src_el() != *old++)
                    return true;
            }
            target = item;
            return false;
        });
        if(!target)
            return false;
        for(const auto& child : target->children())
            m_layout_marked.erase(std::remove(m_layout_marked.begin(), m_layout_marked.end(), child.get()), m_layout_marked.end());
        // Telegram (VENDOR.md edit 20): the inline context's old pieces go, its new ones come, and its text was replaced.
        if(m_render_observer)
        {
            for(const auto& child : target->children())
            {
                m_observed.erase(std::remove(m_observed.begin(), m_observed.end(), child.get()), m_observed.end());
                m_render_observer->item_removed(child.get());
            }
        }
        target->children().clear();
        for(const auto& child : replacement)
            target->add_child(child->create_render_item(target)->init());
        if(m_render_observer)
        {
            for(const auto& child : target->children())
                m_render_observer->item_added(child);
            m_render_observer->text_replaced(target.get());
        }
        target->mark_layout_dirty();
        m_render_tree_serial++;
        return true;
    #else
        // Telegram (VENDOR.md edit 16, amended): the old pieces' renderers are a run among the children of their owner's
        // render item, which is the owner's inline context (a <p>'s text) or an inline element's own item (a <span>'s, a
        // <b>'s, whose pieces its inline context lays out), beside other children or not (an element, a ::before). The
        // run is replaced, nothing else; the owner's children, which hold the replacement already, must match the item's
        // children one for one afterwards.
        if(current.empty() || replacement.empty())
            return false;
        auto owner = current.front()->parent();
        if(!owner)
            return false;
        std::shared_ptr<render_item>                          target;
        std::list<std::shared_ptr<render_item>>::iterator run;
        owner->run_on_renderers([&](const std::shared_ptr<render_item>& item) {
            auto root = item;
            while(auto parent = root->parent())
                root = parent;
            if(root != m_root_render || item->src_el() != owner)
                return true;
            // An inline context, or an inline element's own item, which its inline context lays out: nothing else.
            if(!std::dynamic_pointer_cast<render_item_inline_context>(item) &&
               !(owner->css().get_display() == display_inline && std::dynamic_pointer_cast<render_item_inline>(item)))
                return true;
            auto& kids = item->children();
            for(auto at = kids.begin(); at != kids.end(); ++at)
            {
                auto probe = at;
                auto old   = current.begin();
                while(probe != kids.end() && old != current.end() && (*probe)->src_el() == *old)
                {
                    ++probe;
                    ++old;
                }
                if(old == current.end())
                {
                    target = item;
                    run    = at;
                    return false;
                }
            }
            return true;
        });
        if(!target)
            return false;
        auto& kids = target->children();
        // One for one: the item's children before the run, the replacement, and those after it are the owner's.
        if(kids.size() - current.size() + replacement.size() != owner->children().size())
            return false;
        {
            auto el = owner->children().begin();
            for(auto at = kids.begin(); at != run; ++at, ++el)
            {
                if((*at)->src_el() != *el)
                    return false;
            }
            for(const auto& piece : replacement)
            {
                if(*el++ != piece)
                    return false;
            }
            auto after = run;
            std::advance(after, current.size());
            for(auto at = after; at != kids.end(); ++at, ++el)
            {
                if((*at)->src_el() != *el)
                    return false;
            }
        }
        auto after = run;
        std::advance(after, current.size());
        for(auto at = run; at != after; ++at)
            m_layout_marked.erase(std::remove(m_layout_marked.begin(), m_layout_marked.end(), at->get()), m_layout_marked.end());
        // Telegram (VENDOR.md edit 20): the run's old pieces go, its new ones come, and the item's text was replaced.
        if(m_render_observer)
        {
            for(auto at = run; at != after; ++at)
            {
                m_observed.erase(std::remove(m_observed.begin(), m_observed.end(), at->get()), m_observed.end());
                m_render_observer->item_removed(at->get());
            }
        }
        std::vector<std::shared_ptr<render_item>> added;
        added.reserve(replacement.size());
        auto at = kids.erase(run, after);
        for(const auto& child : replacement)
        {
            auto item = child->create_render_item(target)->init();
            item->parent(target);
            kids.insert(at, item);
            added.push_back(item);
        }
        if(m_render_observer)
        {
            for(const auto& item : added)
                m_render_observer->item_added(item);
            m_render_observer->text_replaced(target.get());
        }
        target->mark_layout_dirty();
        m_render_tree_serial++;
        return true;
#endif
    }

    void document::restyle_after_patch(const std::vector<element::ptr>& restyle, const std::vector<element::ptr>& new_text,
                                       bool keep_tree, const std::vector<element::ptr>& retext,
                                       const std::unordered_set<const element*>& background_only,
                                       const std::unordered_map<const element*, const property_patch*>& patched,
                                       const std::unordered_set<const element*>& shallow_width)
    {
        invalidate_paint();
        m_share_cache.clear();
        std::unordered_set<const element*> pending;
        for(const auto& el : restyle)
            pending.insert(el.get());
        for(const auto& el : new_text)
            pending.insert(el.get());
        for(const auto& el : retext)
            pending.insert(el.get());
        auto covered = [&pending, &shallow_width](const element::ptr& el) {
            for(auto p = el->parent(); p; p = p->parent())
            {
                if(pending.count(p.get()) && !shallow_width.count(p.get()))
                    return true;
            }
            return false;
        };
        std::unordered_map<const element*, const property_patch*> fast;
        for(const auto& el : restyle)
        {
            auto it = patched.find(el.get());
            if(it != patched.end() && !covered(el))
                fast[el.get()] = it->second;
            else if(auto tag = std::dynamic_pointer_cast<html_tag>(el))
                tag->restore_base_style();
        }
        if(!keep_tree || !new_text.empty())
            m_layout_before_valid = false;
        const bool positioned_fresh = keep_tree && new_text.empty() && m_positioned_epoch == m_style_epoch &&
                                      m_positioned_tree == m_render_tree_serial;
        // Telegram (VENDOR.md edit 87): what add_positioned reads of each element: its position, and whether it holds
        // the positioned boxes inside it (a transform, contain: layout or paint).
        auto holds_positioned = [](const element* el) {
            return !el->css().get_transform().empty() || el->css().contains(contain_layout | contain_paint);
        };
        std::vector<std::tuple<const element*, element_position, bool>> positions;
        auto collect_positions = [&positions, &holds_positioned, &shallow_width](const element::ptr& root) {
            if(shallow_width.count(root.get()))
            {
                positions.emplace_back(root.get(), root->css().get_position(), holds_positioned(root.get()));
                return;
            }

            std::vector<const element*> stack{root.get()};
            while(!stack.empty())
            {
                const element* el = stack.back();
                stack.pop_back();
                positions.emplace_back(el, el->css().get_position(), holds_positioned(el));
                for(const auto& child : el->children())
                    stack.push_back(child.get());
            }
        };
        std::vector<element::ptr> roots;
        for(const auto& list : {&restyle, &new_text, &retext})
        {
            for(const auto& el : *list)
            {
                if(!covered(el))
                    roots.push_back(el);
            }
        }
        if(positioned_fresh)
        {
            for(const auto& el : roots)
                collect_positions(el);
        }
        for(const auto& el : roots)
        {
            auto patch = fast.find(el.get());
            if(patch != fast.end())
            {
                auto tag = std::static_pointer_cast<html_tag>(el);
                if(!tag->patch_inline_properties(patch->second->names, patch->second->text))
                {
                    tag->restore_base_style();
                    tag->compute_styles(true);
                }
            } else if(background_only.count(el.get()))
                std::static_pointer_cast<html_tag>(el)->compute_background_style();
            else
                el->compute_styles(!shallow_width.count(el.get()));
        }
        if(positioned_fresh)
        {
            bool same = true;
            for(const auto& [el, position, holds] : positions)
            {
                if(el->css().get_position() != position || holds_positioned(el) != holds)
                {
                    same = false;
                    break;
                }
            }
            if(same)
                m_positioned_epoch = m_style_epoch;
        }

    }

    std::shared_ptr<render_item> document::live_item(const element& el) const
    {
        std::shared_ptr<render_item> found;
        bool several = false;
        el.for_each_render_item([&](const std::shared_ptr<render_item>& item) {
            auto parent = item->parent();
            if(parent && parent->src_el().get() == &el)
                return;
            auto top = item;
            while(top->parent())
                top = top->parent();
            if(top != m_root_render || item == found)
                return;
            several = several || found;
            found = item;
        });
        return several ? nullptr : found;
    }

    // Telegram (VENDOR.md edit 74): an element's outer box, the part of its computed style its parent's render structure
    // reads (render_item_block::init, split_inlines and render_item_flex::init read a child's display and float).
    document::outer_box document::outer_box_of(const element& el)
    {
        return outer_box{el.css().get_display(), el.css().get_float(), el.css().get_position()};
    }

    void document::record_boxes(const element::ptr& seed)
    {
        // An element recorded already had its ancestors recorded with it.
        for(element::ptr el = seed; el; el = el->parent())
        {
            m_replacement_steps++;
            if(!m_patch_boxes.emplace(el.get(), outer_box_of(*el)).second)
                break;
        }
    }

    void document::note_replacement(const element::ptr& seed, std::string why)
    {
        m_patch_keep_tree = false;
        if(seed)
            m_patch_seeds.push_back(seed);
        note_rebuild(std::move(why));
    }

    // Whether `el` or an ancestor is in `set`; each element's answer is kept in `memo`, so a commit steps through each
    // element once whatever the number of questions.
    bool document::inside_any(const element* el, const std::unordered_set<const element*>& set,
                              std::unordered_map<const element*, bool>& memo) const
    {
        std::vector<const element*> path;
        bool inside = false;
        for(const element* e = el; e;)
        {
            m_replacement_steps++;
            if(set.count(e))
            {
                inside = true;
                break;
            }
            auto known = memo.find(e);
            if(known != memo.end())
            {
                inside = known->second;
                break;
            }
            path.push_back(e);
            e = e->parent().get();
        }
        for(const element* e : path)
            memo[e] = inside;
        return inside;
    }

    // Where the session's seeds are replaced (the boundary rule): from each seed in the document up, the first element
    // whose outer box is what it was and whose render item stands alone (a block-level box, an inline-level box with its
    // own formatting, or a replaced element, but no table part, which its table's grid holds; with one live item, not in
    // an anonymous block made from it, which a flex container makes for an item that is not a block box and which
    // inherits from it). A seed under an element that renders nothing and still does not is dropped, unless the document
    // has had an <svg>, which a <use> or a url(#…) paint can draw from anywhere. The root when a walk reaches it or the
    // table-cell cap has cut the document; the outer ones of nested boundaries. Each element a walk passes keeps where it
    // led, so a commit steps through each element once.
    std::vector<element::ptr> document::replacement_boundaries(const std::vector<element::ptr>& seeds,
                                                               const std::unordered_map<const element*, outer_box>& boxes) const
    {
        if(m_table_limited)
            return {m_root};
        std::unordered_map<const element*, const element*> led; // the boundary, or null: dropped
        std::vector<element::ptr> found;
        for(const auto& seed : seeds)
        {
            std::vector<const element*> path;
            const element* verdict = nullptr;
            for(element::ptr b = seed; b; b = b->parent())
            {
                m_replacement_steps++;
                auto known = led.find(b.get());
                if(known != led.end())
                {
                    verdict = known->second;
                    break;
                }
                path.push_back(b.get());
                if(b == m_root)
                    return {m_root};
                auto was = boxes.find(b.get());
                if(was == boxes.end() || !(was->second == outer_box_of(*b)))
                    continue;
                const style_display display = b->css().get_display();
                if(display == display_none)
                {
                    if(m_has_svg)
                        continue;
                    break;
                }
                const bool table_part = display == display_table_row || display == display_table_cell ||
                                        display == display_table_caption || display == display_table_column ||
                                        display == display_table_column_group || display == display_table_row_group ||
                                        display == display_table_header_group || display == display_table_footer_group;
                const bool stands_alone = !table_part && (display == display_block || display == display_list_item ||
                                                          display == display_inline_block || display == display_table ||
                                                          display == display_inline_table || display == display_flex ||
                                                          display == display_inline_flex || display == display_grid ||
                                                          display == display_inline_grid || b->is_replaced());
                if(!stands_alone)
                    continue;
                std::shared_ptr<render_item> item = live_item(*b);
                if(!item)
                    continue;
                std::shared_ptr<render_item> parent = item->parent();
                if(parent && parent->src_el() && parent->src_el()->parent() == b)
                    continue;
                verdict = b.get();
                found.push_back(b);
                break;
            }
            for(const element* e : path)
                led[e] = verdict;
        }
        std::unordered_set<const element*> boundaries;
        for(const auto& b : found)
            boundaries.insert(b.get());
        std::unordered_map<const element*, bool> above;
        std::vector<element::ptr> outer;
        for(const auto& b : found)
        {
            auto parent = b->parent();
            if(!parent || !inside_any(parent.get(), boundaries, above))
                outer.push_back(b);
        }
        return outer;
    }

    // Telegram (VENDOR.md edit 74): each boundary's render subtree made again, as a build makes it, and put where the old
    // one was; the render observer hears subtree_replaced for each while the old one is alive. The root among them, or a
    // boundary with no live item under a parent, replaces the root: everything is made again. True when the root was.
    bool document::replace_render_subtrees(const std::vector<element::ptr>& boundaries)
    {
        struct replacement
        {
            element::ptr                 el;
            std::shared_ptr<render_item> old;
            std::shared_ptr<render_item> parent;
            std::shared_ptr<render_item> item;
        };
        std::vector<replacement> made;
        bool root = !m_root_render;
        for(const auto& el : boundaries)
        {
            std::shared_ptr<render_item> old = el == m_root ? nullptr : live_item(*el);
            std::shared_ptr<render_item> parent = old ? old->parent() : nullptr;
            if(!parent)
            {
                root = true;
                break;
            }
            made.push_back({el, old, parent, nullptr});
        }
        if(root)
        {
            replace_root_render();
            return true;
        }
        // The old subtrees go, and the new ones are made as a build makes the tree.
        std::vector<render_item*> tops;
        for(const auto& r : made)
            tops.push_back(r.old.get());
        release_render_subtrees(tops);
        for(auto& r : made)
            r.item = make_render_subtree(r.el, r.parent);
        // Each parent's children once: the old items swapped for the new ones where they were.
        std::unordered_map<const render_item*, const replacement*> by_old;
        std::vector<render_item*> parents;
        for(const auto& r : made)
        {
            by_old.emplace(r.old.get(), &r);
            if(std::find(parents.begin(), parents.end(), r.parent.get()) == parents.end())
                parents.push_back(r.parent.get());
        }
        size_t spliced = 0;
        for(render_item* parent : parents)
        {
            auto& kids = parent->children();
            for(auto at = kids.begin(); at != kids.end();)
            {
                m_replacement_steps++;
                auto r = by_old.find(at->get());
                if(r == by_old.end())
                {
                    ++at;
                    continue;
                }
                spliced++;
                if(r->second->item)
                {
                    *at = r->second->item;
                    r->second->item->parent(r->second->parent);
                    ++at;
                } else
                {
                    at = kids.erase(at);
                }
            }
        }
        if(spliced != made.size())
        {
            replace_root_render();
            return true;
        }
        for(const auto& r : made)
        {
            r.old->parent(nullptr);
            // Line boxes, flex lines and positioned lists can point at the old items until the next layout.
            m_replaced_hold.push_back(r.old);
            if(r.item)
                r.item->mark_layout_dirty();
            else
                r.parent->mark_layout_dirty();
        }
        m_render_tree_serial++;
        m_layout_before_valid = false;
        for(const auto& r : made)
        {
            if(r.item)
                m_pending_replacements.emplace_back(r.old, r.item);
        }
        return false;
    }

    // Telegram (VENDOR.md edits 74, 89): a render subtree's tables give their cells back, and its items leave the layout's
    // lists and their elements.
    void document::release_render_subtrees(const std::vector<render_item*>& tops)
    {
        std::unordered_set<render_item*> gone;
        for(render_item* top : tops)
        {
            std::vector<render_item*> pending{top};
            while(!pending.empty())
            {
                render_item* item = pending.back();
                pending.pop_back();
                m_replacement_steps++;
                gone.insert(item);
                if(auto table = dynamic_cast<render_item_table*>(item))
                    m_table_cells -= table->cells_taken();
                for(const auto& child : item->children())
                    pending.push_back(child.get());
            }
        }
        const auto in_gone = [&gone](render_item* item) { return gone.count(item) != 0; };
        m_layout_marked.erase(std::remove_if(m_layout_marked.begin(), m_layout_marked.end(), in_gone), m_layout_marked.end());
        m_relayout_roots.erase(std::remove_if(m_relayout_roots.begin(), m_relayout_roots.end(), in_gone), m_relayout_roots.end());
        m_observed.erase(std::remove_if(m_observed.begin(), m_observed.end(), in_gone), m_observed.end());
        for(render_item* item : gone)
            item->src_el()->remove_render(item);
    }

    // Telegram (VENDOR.md edits 74, 89): `el`'s render subtree made under `parent` as a build makes it.
    std::shared_ptr<render_item> document::make_render_subtree(const element::ptr& el, const std::shared_ptr<render_item>& parent)
    {
        m_tabular_elements.clear();
        std::shared_ptr<render_item> item = el->create_render_item(parent);
        fix_tables_layout();
        item = item ? item->init() : nullptr;
        m_tabular_elements.clear();
        return item;
    }

    // Telegram (VENDOR.md edit 89): whether `parent`'s render children follow the session's changes to its children by
    // links alone. They do when its one live item is a block formatting context's and the children render_item_block::init
    // would make from its children now are its render children but the fresh block-level ones and those of the removed
    // children; the first reason that holds otherwise is the refusal. Each child is stepped through once, and each
    // anonymous block's children.
    document::link_plan document::plan_link(const element::ptr& parent, const std::unordered_set<const element*>& fresh) const
    {
        link_plan plan;
        const auto refuse = [&plan](const char* why) {
            plan.refusal = why;
            plan.children.clear();
            plan.unlinked.clear();
            return plan;
        };
        if(m_table_limited)
            return refuse("at the table-cell cap");
        const std::shared_ptr<render_item> item = live_item(*parent);
        if(!item)
        {
            bool any = false;
            parent->for_each_render_item([&any](const std::shared_ptr<render_item>&) { any = true; });
            return refuse(any ? "several render items" : "renders nothing");
        }
        if(parent->is_replaced())
            return refuse("in a replaced element");
        const render_item&    object = *item;
        const std::type_info& type   = typeid(object);
        if(type == typeid(render_item_inline_context))
            return refuse("in an inline context");
        if(type == typeid(render_item_inline))
            return refuse("in an inline");
        if(type == typeid(render_item_flex))
            return refuse("in a flex container");
        if(type == typeid(render_item_grid))
            return refuse("in a grid");
        if(type == typeid(render_item_table) || type == typeid(render_item_table_row) || type == typeid(render_item_table_part))
            return refuse("in a table");
        if(type != typeid(render_item_block_context))
            return refuse("elsewhere");
        if(parent->tag() == _details_ && !parent->get_attr("open"))
            return refuse("in a closed details");
        // What render_item_block::init makes of the children in a block formatting context: each child that is not inline
        // alone, and each run of inline children an anonymous block when it holds more than white space (a run of white
        // space alone is dropped). A child create_render_item makes nothing for (display: none, a comment) makes nothing.
        // A table part outside a table is wrapped with its like siblings in an anonymous table (fix_table_parent), which
        // this does not follow.
        const auto table_part = [](const element& el) {
            switch(el.css().get_display())
            {
            case display_table_caption:
            case display_table_cell:
            case display_table_column:
            case display_table_column_group:
            case display_table_footer_group:
            case display_table_header_group:
            case display_table_row:
            case display_table_row_group:
                return true;
            default:
                return false;
            }
        };
        struct token
        {
            element::ptr                block;
            std::vector<const element*> run;
        };
        std::vector<token>                 expected;
        std::vector<const element*>        run;
        bool                               solid       = false;
        bool                               block_level = false;
        std::unordered_set<const element*> kids;
        const auto close = [&] {
            if(solid)
                expected.push_back({nullptr, run});
            run.clear();
            solid = false;
        };
        for(const auto& child : parent->children())
        {
            m_replacement_steps++;
            kids.insert(child.get());
            if(child->css().get_display() == display_none || std::dynamic_pointer_cast<el_comment>(child))
                continue;
            if(table_part(*child))
                return refuse("a table part outside a table");
            if(!child->is_float() && child->is_block_box())
                block_level = true;
            if(!child->is_inline())
            {
                close();
                expected.push_back({child, {}});
                continue;
            }
            run.push_back(child.get());
            solid = solid || !child->is_white_space();
        }
        close();
        if(!block_level)
            return refuse("the formatting context flips");
        // The render children: a child's own item, an anonymous block (made from the parent: its element is the parent's
        // but none of its children), or a removed child's item (its element left the document's tree). Anything else is a
        // block an inline holding it was split around (render_item::split_inlines).
        std::vector<std::shared_ptr<render_item>> current;
        for(const auto& child : item->children())
        {
            m_replacement_steps++;
            const element* el = child->src_el().get();
            if(!kids.count(el) && el->parent() == parent && dynamic_cast<render_item_table*>(child.get()))
                return refuse("a table part outside a table");
            if(kids.count(el) || (el->parent() == parent))
                current.push_back(child);
            else if(!el->parent())
            {
                if(table_part(*el))
                    return refuse("a table part outside a table");
                plan.unlinked.push_back(child);
            }
            else
                return refuse("an inline holding blocks");
        }
        const auto out_of_flow = [](const element& el) {
            return el.is_float() || el.css().get_position() == element_position_absolute ||
                   el.css().get_position() == element_position_fixed;
        };
        for(const auto& gone : plan.unlinked)
        {
            if(out_of_flow(*gone->src_el()))
                return refuse("a float or an out-of-flow box");
        }
        for(const token& t : expected)
        {
            if(t.block && fresh.count(t.block.get()) && out_of_flow(*t.block))
                return refuse("a float or an out-of-flow box");
        }
        // The kept children in order: each expected child that is not fresh is the next render child, its own item or an
        // anonymous block holding the same run.
        auto at = current.begin();
        for(const token& t : expected)
        {
            if(t.block && fresh.count(t.block.get()))
            {
                plan.children.push_back({t.block, nullptr});
                continue;
            }
            if(at == current.end())
                return refuse("inline content");
            const std::shared_ptr<render_item>& kept = *at++;
            if(t.block)
            {
                if(kept->src_el() != t.block)
                    return refuse("inline content");
            } else
            {
                if(kids.count(kept->src_el().get()) || kept->children().size() != t.run.size())
                    return refuse("inline content");
                auto member = t.run.begin();
                for(const auto& inner : kept->children())
                {
                    m_replacement_steps++;
                    if(inner->src_el().get() != *member++)
                        return refuse("inline content");
                }
            }
            plan.children.push_back({nullptr, kept});
        }
        if(at != current.end())
            return refuse("inline content");
        return plan;
    }

    // Telegram (VENDOR.md edit 89): the removed children's render subtrees leave the parent's item (alive until the next
    // layout and the report), the fresh block-level ones are made where they go, and the list items after the change are
    // numbered again, as render_item_block::init numbers them (from the parent's `start`, every list item counted).
    void document::link_render_children(const element::ptr& parent, const link_plan& plan,
                                        const std::unordered_set<const element*>& fresh,
                                        std::vector<element::ptr>& renumbered, std::vector<element::ptr>& linked)
    {
        const std::shared_ptr<render_item> item = live_item(*parent);
        std::vector<render_item*> tops;
        for(const auto& gone : plan.unlinked)
            tops.push_back(gone.get());
        release_render_subtrees(tops);
        for(const auto& gone : plan.unlinked)
        {
            gone->parent(nullptr);
            m_replaced_hold.push_back(gone);
            m_pending_unlinks.push_back(gone);
        }
        std::list<std::shared_ptr<render_item>> children;
        for(const auto& child : plan.children)
        {
            m_replacement_steps++;
            if(!child.fresh)
            {
                children.push_back(child.item);
                continue;
            }
            std::shared_ptr<render_item> made = make_render_subtree(child.fresh, item);
            if(!made)
                continue;
            made->parent(item);
            made->mark_layout_dirty();
            children.push_back(made);
            m_pending_links.push_back(made);
            linked.push_back(child.fresh);
        }
        item->children() = std::move(children);
        if(!plan.unlinked.empty())
            item->mark_layout_dirty();
        bool lists = false;
        for(const auto& child : parent->children())
            lists = lists || child->css().get_display() == display_list_item;
        if(lists)
        {
            int val = atoi(parent->get_attr("start", "1"));
            for(const auto& child : parent->children())
            {
                m_replacement_steps++;
                if(child->css().get_display() != display_list_item)
                    continue;
                const std::string index = std::to_string(val++);
                if(child->css().get_list_style_type() < list_style_type_armenian || fresh.count(child.get()))
                    continue;
                const char* now = child->get_attr("list_index");
                if(now && index == now)
                    continue;
                child->set_attr("list_index", index.c_str());
                if(auto li = live_item(*child))
                    li->mark_layout_dirty();
                renumbered.push_back(child);
            }
        }
        m_render_tree_serial++;
        m_layout_before_valid = false;
    }

    // Telegram (VENDOR.md edits 74, 89): the replacements, unlinks and links made since the last report, told once each,
    // at the end of what made them; a root replaced among them is told alone, from the root as it was before the first.
    void document::report_replacements()
    {
        std::vector<std::pair<std::shared_ptr<render_item>, std::shared_ptr<render_item>>> pending;
        pending.swap(m_pending_replacements);
        std::vector<std::shared_ptr<render_item>> links, unlinks;
        links.swap(m_pending_links);
        unlinks.swap(m_pending_unlinks);
        if(!m_render_observer)
            return;
        const auto is_root = [](const auto& r) { return !r.second || !r.second->parent(); };
        const auto first_root = std::find_if(pending.begin(), pending.end(), is_root);
        if(first_root != pending.end())
        {
            const auto last_root = std::find_if(pending.rbegin(), pending.rend(), is_root);
            m_render_observer->subtree_replaced(first_root->first.get(), last_root->second);
            return;
        }
        for(const auto& r : pending)
            m_render_observer->subtree_replaced(r.first.get(), r.second);
        for(const auto& gone : unlinks)
            m_render_observer->subtree_unlinked(gone.get());
        for(const auto& made : links)
            m_render_observer->subtree_linked(made);
    }

    void document::replace_root_render()
    {
        std::shared_ptr<render_item> old = m_root_render;
        // The old items leave their elements first: the new ones register as they are made.
        if(old)
        {
            std::vector<render_item*> pending{old.get()};
            while(!pending.empty())
            {
                render_item* item = pending.back();
                pending.pop_back();
                item->src_el()->remove_render(item);
                for(const auto& child : item->children())
                    pending.push_back(child.get());
            }
        }
        m_tabular_elements.clear();
        m_table_cells = 0;
        // Telegram (VENDOR.md edit 84): the cells count from zero in tree order, as a build's; a cap reached already is not
        // reported again.
        const bool table_limited = m_table_limited;
        m_table_limited          = false;
        m_table_quiet            = table_limited;
        m_observed.clear();
        m_root_render = m_root->create_render_item(nullptr);
        m_render_tree_serial++;
        m_layout_marked.clear();
        m_relayout_roots.clear();
        m_layout_before_valid = false;
        fix_tables_layout();
        if(m_root_render)
            m_root_render = m_root_render->init();
        m_table_quiet = false;
        m_tabular_elements.clear();
        if(!m_root_render)
        {
            // A root that goes (`display: none` on the document element) leaves no size, as a fresh document's, and no
            // layout to point at the replaced items (the report holds the old root until it is told).
            m_size.width  = 0;
            m_size.height = 0;
            m_replaced_hold.clear();
        }
        // A root that appears or goes is reported too, its missing side null.
        if(old || m_root_render)
            m_pending_replacements.emplace_back(old, m_root_render);
    }

    bool document::update_from_string(const std::string& html, bool* changed)
    {
        invalidate_paint();
        m_paint_only_patch = false;
        m_patch_confined   = false;
        m_patch_regions.clear();
        if(changed)
            *changed = false;
        if(!m_gumbo || !m_root || !m_finalized)
            return false;
        if(m_template_ok && !getenv("LH_NO_TEMPLATE"))
        {
            const int result = update_from_template(html, changed);
            if(result == 1)
                return true;
            if(result == template_requires_fresh_document)
                return false;
        }
        return update_structure(html, changed);
    }

    int document::selector_scope()
    {
        if(m_selector_scope >= 0)
            return m_selector_scope;
        bool           attributes = false;
        bool           positions  = false;
        bool           backward   = false;
        bool           backward_of = false;
        selector_names names{m_left_classes, m_left_ids, m_left_attrs, m_left_pseudo};
        for(const css* sheet : {&m_master_css, &m_styles, &m_user_css})
        {
            for(const auto& sel : sheet->selectors())
            {
                if(!sel)
                    continue;
#if 0
                scan_selector(*sel, attributes, positions);
#else
                // Telegram (VENDOR.md edit 42): a rule positional in its subject alone is kept apart.
                bool subject = false;
                scan_positions(*sel, attributes, positions, subject, backward, backward_of, true);
                if(subject)
                {
                    const bool child = sel->m_left && sel->m_combinator == combinator_child;
                    m_subject_positions.push_back({sel->m_right.m_tag, child ? sel->m_left->m_right.m_tag : star_id});
                }
#endif
                collect_ancestor_names(*sel, !(sel->m_style && sel->m_style->declares_custom_property()), names);
                // Telegram (VENDOR.md edit 75)
                std::unordered_set<string_id> pseudo;
                collect_pseudo_classes(*sel, pseudo);
                for(string_id name : pseudo)
                    m_pseudo_class_rules[name].push_back(sel.get());
                // Telegram (VENDOR.md edit 77)
                std::unordered_set<string_id> classes, ids, attrs;
                collect_names(*sel, classes, ids, attrs);
                for(string_id name : classes)
                    m_class_rules[name].push_back(sel.get());
                for(string_id name : ids)
                    m_id_rules[name].push_back(sel.get());
                for(string_id name : attrs)
                    m_attr_rules[name].push_back(sel.get());
            }
        }
        bool counters = mentions_counters(m_master_styles);
        for(const auto& sheet : m_css)
            counters = counters || mentions_counters(sheet.text);
        m_selector_scope = (attributes ? 1 : 0) | (positions ? 2 : 0) | (counters ? 4 : 0);
        m_backward_positions = backward; // Telegram (VENDOR.md edit 90)
        m_backward_of_lists  = backward_of;
        if(!m_subject_positions.empty())
            m_selector_scope |= 16; // Telegram (VENDOR.md edit 42)
        // Telegram (VENDOR.md edit 31.5): 8 when :empty is used: a change of an element's children changes how the
        // element itself matches, which the commit matches again where it flipped (edit 88).
        for(const css* sheet : {&m_master_css, &m_styles, &m_user_css})
        {
            for(const auto& sel : sheet->selectors())
            {
                if(sel && uses_empty(*sel))
                {
                    m_selector_scope |= 8;
                    break;
                }
            }
        }
        return m_selector_scope;
    }

    namespace
    {
        // Telegram (VENDOR.md edit 73): the first attribute, in name order, whose presence or value differs.
        std::string first_changed_key(const string_map& before, const string_map& after)
        {
            std::set<std::string> names;
            for(const auto& entry : before)
                names.insert(entry.first);
            for(const auto& entry : after)
                names.insert(entry.first);
            for(const auto& name : names)
            {
                auto a = before.find(name), b = after.find(name);
                if((a == before.end()) != (b == after.end()) || (a != before.end() && a->second != b->second))
                    return name;
            }
            return std::string();
        }
    }

    bool document::apply_retag(const element::ptr& el, const string_map& attrs, bool check)
    {
        auto             tag          = std::static_pointer_cast<html_tag>(el);
        const string_map attrs_before = tag->attributes();
        tag->reset_attributes(attrs);
        return rematch(el, check, attrs_before);
    }

    namespace
    {
        bool has_pseudo_children(const elements_list& children)
        {
            for(const auto& child : children)
                if(child->tag() == __tag_before_ || child->tag() == __tag_after_)
                    return true;
            return false;
        }
    }

    bool document::rematch(const element::ptr& el, bool check, const string_map& attrs_before_change)
    {
        const rematch_state state = rematch_begin(el, check, attrs_before_change);
        auto                tag   = std::static_pointer_cast<html_tag>(el);
        elements_list children;
        children.swap(tag->m_children);
        const css* sheets[3] = {&m_master_css, &m_styles, &m_user_css};
        {
            // Telegram (VENDOR.md edit 31.5): :empty reads the children set aside, not the empty list matching sees.
            struct Aside
            {
                html_tag& tag;
                ~Aside() { tag.set_children_for_matching(nullptr); }
            } aside{*tag};
            tag->set_children_for_matching(&children);
            tag->apply_matching(sheets, m_match_epoch);
        }
        elements_list pseudo;
        pseudo.swap(tag->m_children);
        for(const auto& child : pseudo)
        {
            if(child->tag() == __tag_before_)
                children.push_front(child);
            else
                children.push_back(child);
        }
        tag->m_children.swap(children);
        return rematch_finish(state);
    }

    document::rematch_state document::rematch_begin(const element::ptr& el, bool check, const string_map& attrs_before_change)
    {
        auto          tag = std::static_pointer_cast<html_tag>(el);
        rematch_state state;
        // Telegram (VENDOR.md edit 84): the generated content before; what the match makes and drops is compared with it
        // at rematch_finish, not noted as it happens.
        state.generated_before = generated_fingerprint(*el);
        m_rematching++;
        state.el      = el;
        state.check   = check;
        state.counted = tag->counted(); // edit 78: read before the match forgets it
        // Telegram (VENDOR.md edit 91): the ::before and ::after forget_matched_styles drops below, kept so that a match
        // that changes nothing can put them back (take_back_generated).
        for(const auto& child : el->m_children)
        {
            if(child->tag() != __tag_before_ && child->tag() != __tag_after_)
                continue;
            state.generated_aside.push_back(child);
            if(const auto pseudo = std::dynamic_pointer_cast<html_tag>(child); pseudo && pseudo->reads_var())
                state.generated_reads_var = true;
        }
        state.neutral = check;
        if(state.neutral)
        {
            state.rules_before.reserve(el->m_used_styles.size());
            state.held_before.reserve(el->m_used_styles.size()); // Telegram (VENDOR.md edit 79)
            for(const auto& used : el->m_used_styles)
            {
                state.rules_before.emplace_back(used->m_selector.get(), used->m_used);
                state.held_before.push_back(used->m_selector);
            }
            std::sort(state.rules_before.begin(), state.rules_before.end());
            state.attrs_before = attrs_before_change;
        }
        // Telegram (VENDOR.md edit 17): an element declaring custom properties before or after may change what the var()s
        // below it read (commit_patch matches those again).
        if(tag->declares_custom_properties())
            note_custom_top(el);
        tag->forget_matched_styles();
        return state;
    }

    void document::rematch_pass(const rematch_state& state, int sheet)
    {
        const css* sheets[3] = {&m_master_css, &m_styles, &m_user_css};
        std::static_pointer_cast<html_tag>(state.el)->apply_matching_pass(*sheets[sheet], sheet);
    }

    bool document::rematch_finish(const rematch_state& state)
    {
        // Telegram (VENDOR.md edit 84): generated content the match made again equal changes nothing the cut reads.
        if(m_rematching > 0)
            m_rematching--;
        if(m_generated_watch && generated_fingerprint(*state.el) != state.generated_before)
            m_generated_changed = true;
        return rematch_settle(state);
    }

    bool document::rematch_settle(const rematch_state& state)
    {
        const element::ptr& el  = state.el;
        auto                tag = std::static_pointer_cast<html_tag>(el);
        const bool          check   = state.check;
        // Telegram (VENDOR.md edit 90): changed until the comparison below says otherwise.
        m_rematch_changed = true;
        m_patch_quiet.erase(el.get());
        auto used_rules = [&el]() {
            std::vector<std::pair<const css_selector*, bool>> rules;
            rules.reserve(el->m_used_styles.size());
            for(const auto& used : el->m_used_styles)
                rules.emplace_back(used->m_selector.get(), used->m_used);
            std::sort(rules.begin(), rules.end());
            return rules;
        };
        const auto&  rules_before = state.rules_before;
        string_map   attrs_before = state.attrs_before;
        if(tag->declares_custom_properties())
            note_custom_top(el); // edit 17
        // Telegram (VENDOR.md edit 78): a counted element matched again, before or after, changes what the counters count.
        if((state.counted || tag->counted()) && (selector_scope() & 4))
            m_patch_counters = true;
        // Telegram (VENDOR.md edit 73): why the tree cannot be kept, for rebuild_reason.
        if(!check)
        {
            m_retag_reason = std::string("element: ") + (el->get_tagName() ? el->get_tagName() : "");
            return false;
        }
        // Telegram (VENDOR.md edit 91): an element with a ::before or ::after before or after the match keeps its
        // render tree only when the match changes nothing and the old ones go back (below).
        const bool generated = !state.generated_aside.empty() || has_pseudo_children(el->m_children);
        const auto refuse    = [&](std::string reason) {
            m_retag_reason = generated ? std::string("pseudo-element children") : std::move(reason);
            return false;
        };
        attrs_before.erase("class");
        string_map attrs_after = tag->attributes();
        attrs_after.erase("class");
        // Telegram (VENDOR.md edit 27): an animated element's declarations change on every frame. A change of them keeps
        // the tree when every property it changes is paint-only, as a class change whose rules declare only those does.
        auto take_animated = [](string_map& attrs) {
            auto        it   = attrs.find(animated_style_attribute);
            std::string text = it == attrs.end() ? std::string() : it->second;
            if(it != attrs.end())
                attrs.erase(it);
            return text;
        };
        const std::string animated_before = take_animated(attrs_before);
        const std::string animated_after  = take_animated(attrs_after);
#if 0
        if(attrs_before != attrs_after)
            return false;
        const std::vector<std::string> animated = changed_declarations(animated_before, animated_after);
#else
        // Telegram (VENDOR.md edit 62): a script's inline style changes on every frame too (an rAF loop setting
        // el.style.transform), and keeps the tree on the same terms: its changed properties join the animated ones.
        auto take_style = [](string_map& attrs) {
            auto        it   = attrs.find("style");
            std::string text = it == attrs.end() ? std::string() : it->second;
            if(it != attrs.end())
                attrs.erase(it);
            return text;
        };
        const std::string style_before = take_style(attrs_before);
        const std::string style_after  = take_style(attrs_after);
        if(attrs_before != attrs_after)
            return refuse("attribute: " + first_changed_key(attrs_before, attrs_after)); // edit 73
        std::vector<std::string> animated = changed_declarations(animated_before, animated_after);
        for(auto& property : changed_declarations(style_before, style_after))
            animated.push_back(std::move(property));
#endif
        // Telegram (VENDOR.md edit 63): an inline element's geometry is its line's, which a kept tree does not lay out
        // again: only a box that is not inline keeps the tree for it.
        const bool geometry = el->css().get_display() != display_inline;
        for(const auto& property : animated)
        {
            // Telegram (VENDOR.md edit 63): or one only layout reads (the caller lays the document out again).
            if(!paint_only_property(property) && !(geometry && geometry_only_property(property)))
                return refuse("property: " + property); // edit 73
        }
        const auto                                        rules_after = used_rules();
        std::vector<std::pair<const css_selector*, bool>> changed;
        std::set_symmetric_difference(rules_before.begin(), rules_before.end(), rules_after.begin(), rules_after.end(),
                                      std::back_inserter(changed));
        // Telegram (VENDOR.md edit 73): every changed rule is looked at, so that rebuild_reason names the smallest property
        // that keeps the tree from being kept, whatever order the rules come in (`changed` is sorted by address).
        bool        kept_all = true;
        std::string offending;
        for(const auto& rule : changed)
        {
            if(!rule.first->m_style)
            {
                kept_all = false;
                continue;
            }
            rule.first->m_style->only_declares([geometry, &kept_all, &offending](string_id name) {
                const std::string& text     = _s(name);
                const std::string  property = text.compare(0, 8, "-lh-raw-") == 0 ? text.substr(8) : text;
                if(!paint_only_property(property) && !(geometry && geometry_only_property(property))) // edit 63
                {
                    kept_all = false;
                    if(offending.empty() || property < offending)
                        offending = property;
                }
                return true;
            });
        }
        if(!kept_all)
            return refuse(offending.empty() ? std::string("rule") : "rule: " + offending);
        // Telegram (VENDOR.md edit 91): the old ::before and ::after go back only when the match changed nothing.
        if(generated && (!changed.empty() || !animated.empty() || !take_back_generated(state)))
            return refuse("pseudo-element children");
        // Telegram (VENDOR.md edit 20): whether a rule that started or stopped matching decides paint order or a paint
        // group, which commit_patch tells the render observer.
        if(m_render_observer)
        {
            for(const auto& rule : changed)
            {
                const bool other = rule.first->m_style->only_declares([](string_id name) {
                    const std::string& text     = _s(name);
                    const std::string  property = text.compare(0, 8, "-lh-raw-") == 0 ? text.substr(8) : text;
                    return !paint_order_property(property);
                });
                if(!other)
                {
                    m_patch_paint_order.insert(el.get());
                    break;
                }
            }
            // Telegram (VENDOR.md edit 27): an animated property that decides paint order, as a rule's.
            if(std::any_of(animated.begin(), animated.end(), paint_order_property))
                m_patch_paint_order.insert(el.get());
        }
        m_rematch_changed = !changed.empty() || !animated.empty(); // Telegram (VENDOR.md edit 90)
        return true;
    }

    bool document::retag_allowed(const element::ptr& el, std::string name, bool svg, string_map& attrs, bool blocked)
    {
        auto tag = std::dynamic_pointer_cast<html_tag>(el);
        if(!tag || blocked || !restylable_in_place(*el))
            return false;
        if(name.empty() || lowcase(name) != el->get_tagName() || !m_container->sanitize_element(name, svg, attrs) ||
           lowcase(name) != el->get_tagName())
            return false;
        auto unsafe_style = [](std::string_view value) {
            return value.find("counter") != std::string_view::npos || value.find("var(") != std::string_view::npos ||
                   value.find("--") != std::string_view::npos;
        };
        const string_map& before    = tag->attributes();
        auto              was       = before.find("style");
        auto              now       = attrs.find("style");
        const std::string old_style = was == before.end() ? std::string() : was->second;
        const std::string new_style = now == attrs.end() ? std::string() : now->second;
        if(old_style != new_style && (unsafe_style(old_style) || unsafe_style(new_style)))
            return false;
        auto normalized = [this](const std::string* value) {
            std::string text = value ? *value : std::string();
            if(m_mode == quirks_mode)
                lcase(text);
            return text;
        };
        auto reaches_others = [&](const std::string& attr, const std::string* old, const std::string* next) {
            if(m_left_attrs.count(_id(attr)))
                return true;
            if(attr == "id")
                return m_left_ids.count(_id(normalized(old))) != 0 || m_left_ids.count(_id(normalized(next))) != 0;
            if(attr != "class")
                return false;
            std::unordered_set<string_id> a, c;
            for(const auto& cls : split_string(normalized(old), whitespace, "", ""))
                a.insert(_id(cls));
            for(const auto& cls : split_string(normalized(next), whitespace, "", ""))
                c.insert(_id(cls));
            for(string_id cls : a)
            {
                if(!c.count(cls) && m_left_classes.count(cls))
                    return true;
            }
            for(string_id cls : c)
            {
                if(!a.count(cls) && m_left_classes.count(cls))
                    return true;
            }
            return false;
        };
        for(const auto& entry : before)
        {
            auto it = attrs.find(entry.first);
            if((it == attrs.end() || it->second != entry.second) &&
               reaches_others(entry.first, &entry.second, it == attrs.end() ? nullptr : &it->second))
                return false;
        }
        for(const auto& entry : attrs)
        {
            if(!before.count(entry.first) && reaches_others(entry.first, nullptr, &entry.second))
                return false;
        }
        return true;
    }

    bool document::apply_template_retag(const element::ptr& el, const string_map& attrs, bool check, int* region)
    {
        if(region)
            *region = 0;
        auto has_pseudo = [&el]() {
            for(const auto& child : el->m_children)
                if(child->tag() == __tag_before_ || child->tag() == __tag_after_)
                    return true;
            return false;
        };
        auto used_rules = [&el]() {
            std::vector<std::pair<const css_selector*, bool>> rules;
            rules.reserve(el->m_used_styles.size());
            for(const auto& used : el->m_used_styles)
                rules.emplace_back(used->m_selector.get(), used->m_used);
            std::sort(rules.begin(), rules.end());
            return rules;
        };
        auto                                              tag     = std::static_pointer_cast<html_tag>(el);
        bool                                              neutral = check && !has_pseudo();
        std::vector<std::pair<const css_selector*, bool>> rules_before;
        string_map                                        attrs_before;
        if(neutral)
        {
            rules_before = used_rules();
            attrs_before = tag->attributes();
        }
        tag->reset_attributes(attrs);
        tag->forget_matched_styles();
        elements_list children;
        children.swap(tag->m_children);
        const css* sheets[3] = {&m_master_css, &m_styles, &m_user_css};
        tag->apply_matching(sheets, m_match_epoch);
        elements_list pseudo;
        pseudo.swap(tag->m_children);
        for(const auto& child : pseudo)
        {
            if(child->tag() == __tag_before_)
                children.push_front(child);
            else
                children.push_back(child);
        }
        tag->m_children.swap(children);
        if(!neutral || has_pseudo())
            return false;
        attrs_before.erase("class");
        string_map attrs_after = tag->attributes();
        attrs_after.erase("class");
        if(attrs_before != attrs_after)
            return false;
        const auto                                        rules_after = used_rules();
        std::vector<std::pair<const css_selector*, bool>> changed;
        std::set_symmetric_difference(rules_before.begin(), rules_before.end(), rules_after.begin(), rules_after.end(),
                                      std::back_inserter(changed));
        int kind = 1;
        for(const auto& rule : changed)
        {
            if(!rule.first->m_style || !rule.first->m_style->only_declares([&kind](string_id name) {
                   const std::string& text     = _s(name);
                   const std::string  property = text.compare(0, 8, "-lh-raw-") == 0 ? text.substr(8) : text;
                   const int          declared = declared_region(property);
                   kind                        = declared == 0 ? 0 : (kind == 0 ? 0 : std::max(kind, declared));
                   return paint_only_property(property);
               }))
                return false;
        }
        if(region)
            *region = kind;
        return true;
    }

    bool document::update_structure(const std::string& html, bool* changed)
    {
        m_match_epoch++;
        begin_layout_patch();
        invalidate_paint();
        note_style_change();
        if(!m_gumbo || !m_root || !m_finalized || m_build_limited || getenv("LH_NO_STRUCTURE"))
            return false;
        const int scope = selector_scope();
        if((scope & 4) || mentions_counters(html))
            return false;
        const bool attribute_scope = (scope & 1) != 0;
        const bool position_scope  = (scope & 2) != 0;

        std::string  text   = html;
        GumboOutput* output = gumbo_parse_with_options(&kGumboDefaultOptions, text.data(), text.size());
        if(!output)
            return false;
        auto fail = [&]() {
            gumbo_destroy_output(&kGumboDefaultOptions, output);
            return false;
        };
        if(output->document->v.document.doc_type_quirks_mode != m_gumbo->document->v.document.doc_type_quirks_mode)
            return fail();
        int total = 0, deepest = 0;
        count_elements(output->document, 0, total, deepest);
        if(total > max_build_elements || deepest >= max_build_depth)
            return fail();

        auto holes_of = [&](const GumboNode* node) -> const std::vector<size_t>* {
            if(!m_template_ok)
                return nullptr;
            auto it = m_node_holes.find(node);
            return it == m_node_holes.end() ? nullptr : &it->second;
        };
        auto hole_in = [&](const std::vector<size_t>* holes, const char* name) -> const html_hole* {
            if(!holes)
                return nullptr;
            for(size_t index : *holes)
            {
                const html_hole& hole = m_holes[index];
                if(name ? (!hole.text && hole.name == name) : hole.text)
                    return &hole;
            }
            return nullptr;
        };
        auto mapped_element = [&](const GumboNode* node) -> const element::ptr* {
            auto it = m_node_elements.find(node);
            return it == m_node_elements.end() || it->second.size() != 1 ? nullptr : &it->second.front();
        };
        auto current_text = [&](const GumboNode* node) -> std::vector<element::ptr> {
            if(const html_hole* hole = hole_in(holes_of(node), nullptr))
                return hole->text_elements;
            auto it = m_node_elements.find(node);
            return it == m_node_elements.end() ? std::vector<element::ptr>() : it->second;
        };
        auto same_text_node = [&](const GumboNode* a, const GumboNode* b) {
            if(const html_hole* hole = hole_in(holes_of(a), nullptr))
            {
                const GumboStringPiece& t = b->v.text.original_text;
                return t.data && std::string_view(t.data, t.length) == hole->value;
            }
            return same_text(a->v.text.text, b->v.text.text);
        };
        std::function<bool(const GumboNode*, const GumboNode*)> same_subtree = [&](const GumboNode* a, const GumboNode* b) {
            if(a->type != b->type)
                return false;
            if(is_element_node(a))
            {
                if(!same_element_kind(a, b))
                    return false;
                const GumboVector& x = a->v.element.attributes;
                const GumboVector& y = b->v.element.attributes;
                if(x.length != y.length)
                    return false;
                for(unsigned int i = 0; i < x.length; i++)
                {
                    auto* p = static_cast<const GumboAttribute*>(x.data[i]);
                    auto* q = static_cast<const GumboAttribute*>(y.data[i]);
                    if(!same_text(p->name, q->name) || !same_text(p->value, q->value))
                        return false;
                }
            } else if(a->type != GUMBO_NODE_DOCUMENT)
            {
                return same_text(a->v.text.text, b->v.text.text);
            }
            const GumboVector* ca = node_children(a);
            const GumboVector* cb = node_children(b);
            if(ca->length != cb->length)
                return false;
            for(unsigned int i = 0; i < ca->length; i++)
            {
                if(!same_subtree(static_cast<const GumboNode*>(ca->data[i]), static_cast<const GumboNode*>(cb->data[i])))
                    return false;
            }
            return true;
        };

        struct rebuild
        {
            element::ptr                           parent;
            const GumboNode*                       node;
            std::vector<std::vector<element::ptr>> keep;
            std::vector<char>                      create;
        };
        std::vector<std::pair<element::ptr, std::string>>                    styles;
        std::vector<std::pair<element::ptr, string_map>>                     retags;
        std::vector<rebuild>                                                 rebuilds;
        std::vector<std::pair<const GumboNode*, element::ptr>>               kept;
        std::vector<std::pair<const GumboNode*, std::vector<element::ptr>>> kept_text;
        std::vector<std::pair<std::vector<element::ptr>, std::vector<std::string>>> retexts;
        kept.reserve(m_node_elements.size());
        enum
        {
            diff_kept,
            diff_replace,
            diff_failed
        };
        auto unsafe_style = [](std::string_view value) {
            return value.find("counter") != std::string_view::npos || value.find("var(") != std::string_view::npos ||
                   value.find("--") != std::string_view::npos;
        };
        auto retag = [&](const GumboNode* b, const element::ptr& el, bool in_select) {
            string_map attrs;
            for(unsigned int i = 0; i < b->v.element.attributes.length; i++)
            {
                auto* attr        = static_cast<const GumboAttribute*>(b->v.element.attributes.data[i]);
                attrs[attr->name] = attr->value;
            }
            if(!retag_allowed(el, std::string(element_name(b)), b->v.element.tag_namespace == GUMBO_NAMESPACE_SVG, attrs,
                              in_select || attribute_scope))
                return false;
            retags.emplace_back(el, std::move(attrs));
            return true;
        };
        std::function<bool(const GumboNode*, const GumboNode*, const element::ptr&, bool)> diff_children;
        auto diff_element = [&](const GumboNode* a, const GumboNode* b, const element::ptr& el, bool in_select) {
            const GumboVector&           x       = a->v.element.attributes;
            const GumboVector&           y       = b->v.element.attributes;
            const std::vector<size_t>*   holes   = holes_of(a);
            const GumboAttribute*        style   = nullptr;
            bool                         renamed = x.length != y.length;
            for(unsigned int i = 0; i < x.length && !renamed; i++)
            {
                auto* p = static_cast<const GumboAttribute*>(x.data[i]);
                auto* q = static_cast<const GumboAttribute*>(y.data[i]);
                if(!same_text(p->name, q->name))
                {
                    renamed = true;
                    break;
                }
                const html_hole* hole   = hole_in(holes, p->name);
                std::string_view before = hole ? std::string_view(hole->value) : std::string_view(p->value);
                bool             same   = false;
                if(hole)
                {
                    const GumboStringPiece& v = q->original_value;
                    same = v.data && v.length >= 2 && v.data[0] == hole->close && v.data[v.length - 1] == hole->close &&
                           std::string_view(v.data + 1, v.length - 2) == before;
                } else
                {
                    same = before == std::string_view(q->value);
                }
                if(same)
                    continue;
                if(strcmp(p->name, "style") != 0)
                {
                    renamed = true;
                    break;
                }
                if(unsafe_style(before) || unsafe_style(q->value))
                    return diff_replace;
                if(style_attributes_selectable() ||
                   (has_generated_children(el) && style_change_is_width_only(std::string(before), q->value)))
                    return diff_failed;
                style = q;
            }
            if(raw_text_element(a))
            {
                if(renamed || style || !same_subtree(a, b))
                    return diff_replace;
                kept.emplace_back(b, el);
                return diff_kept;
            }
            if(renamed)
            {
                style = nullptr;
                if(!retag(b, el, in_select))
                    return diff_replace;
            }
            if(!diff_children(a, b, el, in_select || a->v.element.tag == GUMBO_TAG_SELECT))
                return diff_failed;
            if(style)
                styles.emplace_back(el, style->value);
            kept.emplace_back(b, el);
            return diff_kept;
        };
        diff_children = [&](const GumboNode* a, const GumboNode* b, const element::ptr& parent, bool in_select) {
            const GumboVector* ca = node_children(a);
            const GumboVector* cb = node_children(b);
            bool               aligned = ca->length == cb->length;
            for(unsigned int i = 0; aligned && i < ca->length; i++)
            {
                auto* x = static_cast<const GumboNode*>(ca->data[i]);
                auto* y = static_cast<const GumboNode*>(cb->data[i]);
                if(x->type != y->type || (is_element_node(x) && (!same_element_kind(x, y) || node_key(x) != node_key(y))))
                    aligned = false;
            }
            const size_t styles_mark    = styles.size();
            const size_t retags_mark    = retags.size();
            const size_t rebuilds_mark  = rebuilds.size();
            const size_t kept_mark      = kept.size();
            const size_t kept_text_mark = kept_text.size();
            const size_t retexts_mark   = retexts.size();
            rebuild      plan;
            if(aligned)
            {
                bool replaced = false;
                for(unsigned int i = 0; i < cb->length; i++)
                {
                    auto* x      = static_cast<const GumboNode*>(ca->data[i]);
                    auto* y      = static_cast<const GumboNode*>(cb->data[i]);
                    bool  create = false;
                    if(is_element_node(x))
                    {
                        const element::ptr* el = mapped_element(x);
                        if(!el)
                        {
                            create = !same_subtree(x, y);
                            replaced = replaced || create;
                        } else
                        {
                            const int result = diff_element(x, y, *el, in_select);
                            if(result == diff_failed)
                                return false;
                            create   = result == diff_replace;
                            replaced = replaced || create;
                        }
                    } else if(x->type == GUMBO_NODE_TEXT)
                    {
                        create = !same_text_node(x, y);
                        std::vector<std::string> words;
                        if(create && !in_select && retext_in_place(current_text(x), y->v.text.text, words))
                        {
                            create = false;
                            retexts.emplace_back(current_text(x), std::move(words));
                        }
                        if(!create)
                            kept_text.emplace_back(y, current_text(x));
                    } else
                    {
                        create = !same_text(x->v.text.text, y->v.text.text);
                    }
                    if(create && plan.create.empty())
                        plan.create.assign(cb->length, 0);
                    if(create)
                        plan.create[i] = 1;
                }
                if(plan.create.empty())
                    return true;
                plan.keep.resize(cb->length);
                const bool everything = replaced && attribute_scope;
                if(everything)
                {
                    styles.resize(styles_mark);
                    retags.resize(retags_mark);
                    rebuilds.resize(rebuilds_mark);
                    kept.resize(kept_mark);
                    kept_text.resize(kept_text_mark);
                    retexts.resize(retexts_mark);
                }
                for(unsigned int i = 0; i < cb->length; i++)
                {
                    auto* x = static_cast<const GumboNode*>(ca->data[i]);
                    if(everything || (x->type != GUMBO_NODE_TEXT && !is_element_node(x)))
                        plan.create[i] = 1;
                    if(plan.create[i])
                        continue;
                    if(x->type == GUMBO_NODE_TEXT)
                        plan.keep[i] = current_text(x);
                    else if(const element::ptr* el = mapped_element(x))
                        plan.keep[i] = {*el};
                }
            } else
            {
                plan.keep.resize(cb->length);
                plan.create.assign(cb->length, 1);
                std::unordered_map<std::string_view, const GumboNode*> keyed;
                std::unordered_set<std::string_view>                   duplicates;
                std::unordered_set<std::string_view>                   seen;
                if(!position_scope)
                {
                    for(unsigned int i = 0; i < ca->length; i++)
                    {
                        std::string_view key = node_key(static_cast<const GumboNode*>(ca->data[i]));
                        if(!key.empty() && !keyed.emplace(key, static_cast<const GumboNode*>(ca->data[i])).second)
                            duplicates.insert(key);
                    }
                    for(unsigned int i = 0; i < cb->length; i++)
                    {
                        std::string_view key = node_key(static_cast<const GumboNode*>(cb->data[i]));
                        if(!key.empty() && !seen.insert(key).second)
                            duplicates.insert(key);
                    }
                }
                for(unsigned int i = 0; i < cb->length && !position_scope; i++)
                {
                    auto*            y   = static_cast<const GumboNode*>(cb->data[i]);
                    std::string_view key = node_key(y);
                    if(key.empty() || duplicates.count(key))
                        continue;
                    auto match = keyed.find(key);
                    if(match == keyed.end() || !same_element_kind(match->second, y))
                        continue;
                    const element::ptr* el = mapped_element(match->second);
                    if(!el)
                        continue;
                    const int result = diff_element(match->second, y, *el, in_select);
                    if(result == diff_failed)
                        return false;
                    if(result == diff_kept)
                    {
                        plan.create[i] = 0;
                        plan.keep[i]   = {*el};
                    }
                }
            }
            if(!parent || in_select || (is_element_node(a) && (a->v.element.tag == GUMBO_TAG_HTML || a->v.element.tag == GUMBO_TAG_HEAD)))
                return false;
            std::unordered_set<const element*> reused;
            for(unsigned int i = 0; i < cb->length; i++)
            {
                if(plan.create[i] && has_document_level_element(static_cast<const GumboNode*>(cb->data[i])))
                    return false;
                for(const auto& el : plan.keep[i])
                    reused.insert(el.get());
            }
            for(unsigned int i = 0; i < ca->length; i++)
            {
                auto* x = static_cast<const GumboNode*>(ca->data[i]);
                if(!is_element_node(x))
                    continue;
                const element::ptr* el = mapped_element(x);
                if((!el || !reused.count(el->get())) && has_document_level_element(x))
                    return false;
            }
            plan.parent = parent;
            plan.node   = b;
            rebuilds.push_back(std::move(plan));
            return true;
        };
        if(!diff_children(m_gumbo->document, output->document, nullptr, false))
            return fail();

        auto chain = [](element::ptr el) {
            std::vector<element::ptr> out;
            for(; el; el = el->parent())
                out.push_back(el);
            return out;
        };
        auto nearest_attached = [this](const std::vector<element::ptr>& path) -> element::ptr {
            for(const auto& candidate : path)
            {
                element::ptr el = candidate;
                while(el && el != m_root)
                    el = el->parent();
                if(el == m_root)
                    return candidate;
            }
            return nullptr;
        };
        const std::vector<element::ptr> over_chain   = chain(m_over_element);
        const std::vector<element::ptr> active_chain = chain(m_active_element);

        std::unordered_map<const void*, std::vector<element::ptr>> remapped;
        remapped.reserve(kept.size() + kept_text.size());
        for(auto& entry : kept)
            remapped[entry.first] = {std::move(entry.second)};
        for(auto& entry : kept_text)
            remapped[entry.first] = std::move(entry.second);
        m_node_elements.swap(remapped);

        std::vector<element::ptr> restyle;
        for(const auto& patch : styles)
        {
            patch.first->set_attr("style", patch.second.c_str());
            restyle.push_back(patch.first);
        }
        std::vector<element::ptr> retext;
        for(auto& entry : retexts)
        {
            for(size_t w = 0; w < entry.second.size(); w++)
            {
                std::static_pointer_cast<el_text>(entry.first[w])->set_text(entry.second[w]);
                retext.push_back(entry.first[w]);
            }
        }
        static const bool no_keep_tree = getenv("LH_NO_KEEP_TREE") != nullptr;
        bool              keep_tree    = !no_keep_tree && rebuilds.empty() && styles.empty();
        std::vector<element::ptr> fresh;
        std::vector<element::ptr> created;
        m_track_nodes    = true;
        m_build_elements = 0;
        for(auto& plan : rebuilds)
        {
            const element::ptr& parent = plan.parent;
            int                 depth  = 0;
            for(auto p = parent->parent(); p; p = p->parent())
                depth++;
            m_build_depth = depth + 1;
            element::ptr before, after;
            if(!parent->m_children.empty() && parent->m_children.front()->tag() == __tag_before_)
                before = parent->m_children.front();
            if(!parent->m_children.empty() && parent->m_children.back()->tag() == __tag_after_)
                after = parent->m_children.back();
            std::unordered_set<const element*> reused;
            elements_list                      children;
            if(before)
                children.push_back(before);
            const GumboVector* nodes = node_children(plan.node);
            for(unsigned int i = 0; i < nodes->length; i++)
            {
                if(!plan.create[i])
                {
                    for(const auto& el : plan.keep[i])
                    {
                        reused.insert(el.get());
                        children.push_back(el);
                    }
                    continue;
                }
                elements_list made;
                create_node(static_cast<GumboNode*>(nodes->data[i]), made, true, true);
                for(const auto& el : made)
                {
                    children.push_back(el);
                    if(std::dynamic_pointer_cast<html_tag>(el))
                        fresh.push_back(el);
                    created.push_back(el);
                }
            }
            if(after)
                children.push_back(after);
            for(const auto& old : parent->m_children)
            {
                if(old != before && old != after && !reused.count(old.get()))
                    old->parent(nullptr);
            }
            for(const auto& el : children)
                el->parent(parent);
            parent->m_children.swap(children);
        }
        m_track_nodes = false;
        m_build_depth = 0;

        for(auto& entry : retags)
        {
            keep_tree = apply_template_retag(entry.first, entry.second, keep_tree) && keep_tree;
            restyle.push_back(entry.first);
        }

        const bool any = !styles.empty() || !rebuilds.empty() || !retags.empty() || !retexts.empty();
        if(any)
        {
            for(const auto& el : fresh)
                el->apply_stylesheet(m_master_css);
            for(const auto& el : fresh)
                el->parse_attributes();
            for(const auto& el : fresh)
                el->apply_stylesheet(m_styles);
            for(const auto& el : fresh)
                el->apply_stylesheet(m_user_css);
            restyle_after_patch(restyle, created, keep_tree, retext);
            if(!keep_tree || !m_root_render)
                replace_root_render();
            if(keep_tree)
                forget_positions();
            m_over_element   = nearest_attached(over_chain);
            m_active_element = nearest_attached(active_chain);
        }

        gumbo_destroy_output(&kGumboDefaultOptions, m_gumbo);
        m_gumbo = output;
        m_text.swap(text);
        build_template();
        if(changed)
            *changed = any;
        return true;
    }

    bool document::style_attributes_selectable()
    {
        if(m_style_attributes_selectable >= 0)
            return m_style_attributes_selectable != 0;
        std::function<bool(const css_selector&)> reads_style = [&](const css_selector& selector) {
            for(const css_selector* sel = &selector; sel; sel = sel->m_left.get())
            {
                for(const auto& attr : sel->m_right.m_attrs)
                {
                    if(attr.type == select_attr && t_strcasecmp(_s(attr.name).c_str(), "style") == 0)
                        return true;
                    for(const auto& inner : attr.selector_list)
                    {
                        if(inner && reads_style(*inner))
                            return true;
                    }
                }
            }
            return false;
        };
        for(const css* sheet : {&m_master_css, &m_styles, &m_user_css})
        {
            for(const auto& selector : sheet->selectors())
            {
                if(selector && reads_style(*selector))
                {
                    m_style_attributes_selectable = 1;
                    return true;
                }
            }
        }
        m_style_attributes_selectable = 0;
        return false;
    }

    bool document::ids_selectable()
    {
        if(m_ids_selectable >= 0)
            return m_ids_selectable != 0;
        static const bool disabled = getenv("LH_NO_ID_SHARE") != nullptr;
        std::function<bool(const css_selector&)> sees_ids = [&](const css_selector& selector) {
            for(const css_selector* sel = &selector; sel; sel = sel->m_left.get())
            {
                for(const auto& attr : sel->m_right.m_attrs)
                {
                    if(attr.type == select_id || (attr.type == select_attr && t_strcasecmp(_s(attr.name).c_str(), "id") == 0))
                        return true;
                    for(const auto& inner : attr.selector_list)
                    {
                        if(inner && sees_ids(*inner))
                            return true;
                    }
                }
            }
            return false;
        };
        bool seen = disabled;
        for(const css* sheet : {&m_master_css, &m_styles, &m_user_css})
        {
            for(const auto& sel : sheet->selectors())
            {
                if(seen)
                    break;
                seen = sel && sees_ids(*sel);
            }
        }
        m_ids_selectable = seen ? 1 : 0;
        return seen;
    }

    bool document::style_sharing_enabled() const
    {
        static const bool enabled = getenv("LH_NO_STYLE_SHARING") == nullptr;
        return enabled;
    }

    void document::set_document_mode(document_mode mode)
    {
        m_mode = mode;
    }

    // Telegram (VENDOR.md edit 79): the master sheet parsed as finalize parses it, through its cache, under the
    // container's environment as it is now.
    css document::parsed_master(const std::string& master_styles)
    {
        css parsed;
        static std::mutex&                           master_cache_mutex = *new std::mutex;
        static std::unordered_map<std::string, css>& master_cache = *new std::unordered_map<std::string, css>;
        const bool                                  cacheable =
            master_styles.find("@media") == std::string::npos && master_styles.find("@import") == std::string::npos;
        bool hit = false;
        // Telegram (VENDOR.md edit 13): light-dark() resolves at parse, so the cache is keyed by the scheme too.
        media_features scheme_media;
        container()->get_media_features(scheme_media);
#if 0
        const std::string master_key =
            std::string((scheme_media.color_scheme || scheme_media.dark_color_scheme) ? "\x03" : "\x02") + master_styles;
#else
        // Telegram (VENDOR.md edit 29.7): and by the container's sanitizer key.
        const uint32_t    master_sanitizer = container()->sanitizer_key();
        const std::string master_key       = std::string((scheme_media.color_scheme || scheme_media.dark_color_scheme) ? "\x03" : "\x02") +
                                       std::string(reinterpret_cast<const char*>(&master_sanitizer), sizeof master_sanitizer) +
                                       master_styles + environment_key(master_styles); // edit 79
#endif
        if(cacheable)
        {
            std::lock_guard<std::mutex> lock(master_cache_mutex);
#if 0
            auto                        it = master_cache.find(master_styles);
#else
            auto                        it = master_cache.find(master_key); // Telegram (VENDOR.md edit 13)
#endif
            if(it != master_cache.end())
            {
                parsed = it->second;
                hit    = true;
            }
        }
        if(!hit)
        {
            container()->trusted_styles(true);
            parsed.set_layered_origins(true); // Telegram (VENDOR.md edit 41)
            parsed.parse_css_stylesheet(master_styles, "", shared_from_this());
            container()->trusted_styles(false);
            parsed.sort_selectors();
            if(cacheable)
            {
                std::lock_guard<std::mutex> lock(master_cache_mutex);
                if(master_cache.size() > 64)
                    master_cache.clear();
#if 0
                master_cache.emplace(master_styles, parsed);
#else
                master_cache.emplace(master_key, parsed); // Telegram (VENDOR.md edit 13): keyed by the scheme
#endif
            }
        }
        return parsed;
    }

    // Telegram (VENDOR.md edit 79): the document's sheets (m_css) parsed as finalize parses them, through their cache,
    // under the container's environment as it is now.
    css document::parsed_author()
    {
        css            parsed;
        media_features scheme_media;
        container()->get_media_features(scheme_media);
        // Parsed and sorted document stylesheets are immutable, so documents built from the same
        // <style> text share one parse. Media and imports register per-document state while parsing,
        // so sheets that use them are parsed every time.
        static std::mutex&                           styles_cache_mutex = *new std::mutex;
        static std::unordered_map<std::string, css>& styles_cache = *new std::unordered_map<std::string, css>;
        std::string                                 styles_key;
        bool                                        styles_cacheable = !m_css.empty();
        for(const auto& css : m_css)
        {
            if(css.media != "" || css.text.find("@media") != std::string::npos || css.text.find("@import") != std::string::npos)
            {
                styles_cacheable = false;
                break;
            }
        }
        bool styles_hit = false;
        if(styles_cacheable)
        {
            styles_key.push_back(static_cast<char>('0' + static_cast<int>(m_mode)));
            // Telegram (VENDOR.md edit 13): keyed by the container's scheme too.
            styles_key.push_back((scheme_media.color_scheme || scheme_media.dark_color_scheme) ? '\x03' : '\x02');
            // Telegram (VENDOR.md edit 29.7): and by the container's sanitizer key.
            {
                const uint32_t sanitizer = container()->sanitizer_key();
                styles_key.append(reinterpret_cast<const char*>(&sanitizer), sizeof sanitizer);
            }
            for(const auto& css : m_css)
            {
                styles_key.append(css.baseurl);
                styles_key.push_back('\x1e');
                styles_key.append(css.text);
                styles_key.append(environment_key(css.text)); // edit 79
                styles_key.push_back('\x1f');
            }
            std::lock_guard<std::mutex> lock(styles_cache_mutex);
            auto                        it = styles_cache.find(styles_key);
            if(it != styles_cache.end())
            {
                parsed     = it->second;
                styles_hit = true;
            }
        }
        if(!styles_hit)
        {
            // parse style sheets linked in document
            for(const auto& css : m_css)
            {
                media_query_list_list::ptr media;
                if(css.media != "")
                {
                    // Telegram (VENDOR.md edit 29.3): a <style media> the container refuses is not applied.
                    if(!container()->sanitize_media_query(normalize(css.media, f_componentize)))
                    {
                        parsed.end_sheet(); // Telegram (VENDOR.md edit 81)
                        continue;
                    }
                    auto mq_list = parse_media_query_list(css.media, shared_from_this());
                    media        = std::make_shared<media_query_list_list>();
                    media->add(mq_list);
                }
                parsed.parse_css_stylesheet(css.text, css.baseurl, shared_from_this(), media);
                parsed.end_sheet(); // Telegram (VENDOR.md edit 81)
            }
            // Sort css selectors using CSS rules.
            parsed.sort_selectors();
            if(styles_cacheable)
            {
                std::lock_guard<std::mutex> lock(styles_cache_mutex);
                if(styles_cache.size() > 128)
                    styles_cache.clear();
                styles_cache.emplace(std::move(styles_key), parsed);
            }
        }

        return parsed;
    }

    // Telegram (VENDOR.md edit 79): viewport units fold to pixels at parse (edit 11), so a text that may hold one is
    // cached per viewport.
    std::string document::environment_key(const std::string& text) const
    {
        if(!text_may_hold_viewport_unit(text))
            return std::string();
        media_features media;
        m_container->get_media_features(media);
        return "\x04" + std::to_string(media.width.value()) + "x" + std::to_string(media.height.value());
    }

    void document::finalize_from_external_root(const std::shared_ptr<element>& root, const std::string& base_master_styles,
                                               const std::string& user_styles)
    {
        invalidate_paint();
        // Finalization accumulates state that is never rolled back: the parsed stylesheets, the media
        // lists, the tabular elements, the render tree, and the used selectors of every element. A
        // second run would duplicate all of it, so a document is finalized once, exactly like a parsed
        // one. Build a new document to rebuild.
        if(m_finalized)
        {
            return;
        }
        m_finalized = true;

        m_root = root;

        const std::string master_styles =
            m_mode == quirks_mode && base_master_styles != ""
                ? base_master_styles + "\ntable { font-size: medium; font-weight: normal; font-style: normal; }\n"
                : base_master_styles;
        // Telegram (VENDOR.md edit 79): kept, to be parsed again under a new environment.
        m_master_styles = master_styles;
        if(master_styles != "")
            m_master_css = parsed_master(master_styles);
        if(user_styles != "")
        {
            container()->trusted_styles(true);
            m_user_css.parse_css_stylesheet(user_styles, "", shared_from_this());
            container()->trusted_styles(false);
            m_user_css.sort_selectors();
        }

        // Let's process created elements tree
        if(m_root)
        {
            container()->get_media_features(m_media);

            m_root->set_pseudo_class(_root_, true);
            count_master_content_owners(); // Telegram (VENDOR.md edit 84)

            // Telegram (VENDOR.md edit 18): the caller's matching gate, asked before the master sheet's pass with its rule
            // count and a bound of the elements it tests them against; declined, nothing is matched and the document is
            // left unfinished.
            if(m_matching_gate &&
               !m_matching_gate(m_master_css.selectors().size(), matching_bound(m_root, pseudo_element_creators({&m_master_css}))))
            {
                m_matching_gated = true;
                return;
            }

            // Telegram (VENDOR.md edit 84): the limits the style pass reaches, in order, and its content's transients.
            m_style_pass_limits.clear();
            m_recording_style_pass = true;
            m_generated_transient  = 0;
            m_walk_text_limited    = m_text_limited; // the text the walk made, cut or not, before any generated content
            // apply master CSS
            m_root->apply_stylesheet(m_master_css);

            // parse elements attributes
            m_root->parse_attributes();

            m_styles        = parsed_author(); // Telegram (VENDOR.md edit 79)
            m_sheets_parsed = true;            // Telegram (VENDOR.md edit 81)

            // Apply media features.
            update_media_lists(m_media);

            // Telegram (VENDOR.md edit 18): the gate again, before the author and host sheets' passes.
            if(m_matching_gate && !m_matching_gate(m_styles.selectors().size() + m_user_css.selectors().size(),
                                                   matching_bound(m_root, pseudo_element_creators())))
            {
                m_matching_gated = true;
                return;
            }

            m_style_rule_bytes = -1; // Telegram (VENDOR.md edit 21): the sheets are parsed
            // Apply parsed styles.
            m_root->apply_stylesheet(m_styles);

            // Apply user styles if any
            m_root->apply_stylesheet(m_user_css);
            m_recording_style_pass = false; // Telegram (VENDOR.md edit 84)

            // Initialize element::m_css
            m_root->compute_styles();

            // Create rendering tree
            m_table_cells = 0;
            m_observed.clear();   // Telegram (VENDOR.md edit 20): a build's render tree is reported by no one
            m_root_render = m_root->create_render_item(nullptr);
            m_render_tree_serial++;
            m_layout_marked.clear();
            m_relayout_roots.clear();
            m_layout_before_valid = false;

            // Now the m_tabular_elements is filled with tabular elements.
            // We have to check the tabular elements for missing table elements
            // and create the anonymous boxes in visual table layout
            fix_tables_layout();

            // Finally initialize elements
            // init() returns pointer to the render_init element because it can change its type
            if(m_root_render)
            {
                m_root_render = m_root_render->init();
            }
        }
    }

    // https://html.spec.whatwg.org/multipage/parsing.html#change-the-encoding
    encoding adjust_meta_encoding(encoding meta_encoding, encoding current_encoding)
    {
        // 1.
        if(current_encoding == encoding::utf_16le || current_encoding == encoding::utf_16be)
        {
            return current_encoding;
        }

        // 2.
        if(meta_encoding == encoding::utf_16le || meta_encoding == encoding::utf_16be)
        {
            return encoding::utf_8;
        }

        // 3.
        if(meta_encoding == encoding::x_user_defined)
        {
            return encoding::windows_1252;
        }

        // 4,5,6.
        return meta_encoding;
    }

    // https://html.spec.whatwg.org/multipage/parsing.html#parsing-main-inhead:change-the-encoding
    encoding get_meta_encoding(GumboNode* root)
    {
        // find <head>
        GumboNode* head = nullptr;
        for(size_t i = 0; i < root->v.element.children.length; i++)
        {
            auto* node = static_cast<GumboNode*>(root->v.element.children.data[i]);
            if(node->type == GUMBO_NODE_ELEMENT && node->v.element.tag == GUMBO_TAG_HEAD)
            {
                head = node;
                break;
            }
        }
        if(!head)
        {
            return encoding::null;
        }

        // go through <meta> tags in <head>
        for(size_t i = 0; i < head->v.element.children.length; i++)
        {
            auto* node = static_cast<GumboNode*>(head->v.element.children.data[i]);
            if(node->type != GUMBO_NODE_ELEMENT || node->v.element.tag != GUMBO_TAG_META)
            {
                continue;
            }

            auto* charset    = gumbo_get_attribute(&node->v.element.attributes, "charset");
            auto* http_equiv = gumbo_get_attribute(&node->v.element.attributes, "http-equiv");
            auto* content    = gumbo_get_attribute(&node->v.element.attributes, "content");
            // 1. If the element has a charset attribute...
            if(charset)
            {
                auto encoding = get_encoding(charset->value);
                if(encoding != encoding::null)
                {
                    return encoding;
                }
            }
            // 2. Otherwise, if the element has an http-equiv attribute...
            else if(http_equiv && t_strcasecmp(http_equiv->value, "content-type") == 0 && content)
            {
                auto encoding = extract_encoding_from_meta_element(content->value);
                if(encoding != encoding::null)
                {
                    return encoding;
                }
            }
        }

        return encoding::null;
    }

    // substitute for gumbo_parse that handles encodings
    GumboOutput* document::parse_html(estring str)
    {
        // https://html.spec.whatwg.org/multipage/parsing.html#the-input-byte-stream
        encoding_sniffing_algorithm(str);
        // cannot store output in local variable because gumbo keeps pointers into it,
        // which will be accessed later in gumbo_tag_from_original_text
        if(str.encoding == encoding::utf_8)
        {
            m_text = str;
        } else
        {
            decode(str, str.encoding, m_text);
        }

        // Gumbo does not support callbacks on node creation, so we cannot change encoding while parsing.
        // Instead, we parse entire file and then handle <meta> tags.

        // Using gumbo_parse_with_options to pass string length (m_text may contain NUL chars).
        GumboOutput* output = gumbo_parse_with_options(&kGumboDefaultOptions, m_text.data(), m_text.size());

        if(str.confidence == confidence::certain)
        {
            return output;
        }

        // Otherwise: confidence is tentative.

        // If valid HTML encoding is specified in <meta> tag...
        encoding meta_encoding = get_meta_encoding(output->root);
        if(meta_encoding != encoding::null)
        {
            // ...and it is different from currently used encoding...
            encoding new_encoding = adjust_meta_encoding(meta_encoding, str.encoding);
            if(new_encoding != str.encoding)
            {
                // ...reparse with the new encoding.
                gumbo_destroy_output(&kGumboDefaultOptions, output);
                m_text.clear();

                if(new_encoding == encoding::utf_8)
                {
                    m_text = str;
                } else
                {
                    decode(str, new_encoding, m_text);
                }
                output = gumbo_parse_with_options(&kGumboDefaultOptions, m_text.data(), m_text.size());
            }
        }

        return output;
    }

    void document::create_node(void* gnode, elements_list& elements, bool parseTextNode, bool process_root)
    {
        auto* node = static_cast<GumboNode*>(gnode);
        switch(node->type)
        {
        case GUMBO_NODE_ELEMENT:
            {
                if(process_root)
                {
                    string_map      attrs;
                    GumboAttribute* attr = nullptr;
                    for(unsigned int i = 0; i < node->v.element.attributes.length; i++)
                    {
                        attr              = static_cast<GumboAttribute*>(node->v.element.attributes.data[i]);
                        attrs[attr->name] = attr->value;
                    }

                    element::ptr ret;
                    const char*  tag = gumbo_normalized_tagname(node->v.element.tag);
                    std::string  name;
                    if(tag[0])
                    {
                        name = tag;
                    } else if(node->v.element.original_tag.data && node->v.element.original_tag.length)
                    {
                        gumbo_tag_from_original_text(&node->v.element.original_tag);
                        name.assign(node->v.element.original_tag.data, node->v.element.original_tag.length);
                    }
                    if(!name.empty())
                    {
                        if(m_build_depth >= max_build_depth || m_build_elements >= max_build_elements)
                        {
                            m_container->sanitizer_limit(m_build_depth >= max_build_depth ? "depth" : "elements");
                            m_build_limited = true;
                            break;
                        }
                        if(!m_container->sanitize_element(name, node->v.element.tag_namespace == GUMBO_NAMESPACE_SVG, attrs))
                        {
                            break;
                        }
                        ret = create_element(name.c_str(), attrs);
                        m_build_elements++;
                    }
                    if(!strcmp(tag, "script"))
                    {
                        parseTextNode = false;
                    }
                    if(ret)
                    {
                        if(m_track_nodes)
                            m_node_elements[node] = {ret};
                        elements_list child;
                        m_build_depth++;
                        for(unsigned int i = 0; i < node->v.element.children.length; i++)
                        {
                            child.clear();
                            create_node(static_cast<GumboNode*>(node->v.element.children.data[i]), child, parseTextNode,
                                        true);
                            std::for_each(child.begin(), child.end(),
                                          [&ret](element::ptr& el) { ret->appendChild(el); });
                        }
                        m_build_depth--;
                        elements.push_back(ret);
                    }
                } else
                {
                    for(unsigned int i = 0; i < node->v.element.children.length; i++)
                    {
                        create_node(static_cast<GumboNode*>(node->v.element.children.data[i]), elements, parseTextNode,
                                    true);
                    }
                }
            }
            break;
        case GUMBO_NODE_TEXT:
            {
                const size_t before = elements.size();
                if(!parseTextNode)
                {
                    // Telegram (VENDOR.md edit 10): no text piece past the cap.
                    if(!text_element_allowed())
                        m_build_limited = true;
                    else
                    elements.push_back(std::make_shared<el_text>(node->v.text.text, shared_from_this()));
                } else
                {
                    m_container->split_text(
                        node->v.text.text,
                        [this, &elements](const char* text) {
                            if(!text_element_allowed())
                                m_build_limited = true;
                            else
                            elements.push_back(std::make_shared<el_text>(text, shared_from_this()));
                        },
                        [this, &elements](const char* text) {
                            if(!text_element_allowed())
                                m_build_limited = true;
                            else
                            elements.push_back(std::make_shared<el_space>(text, shared_from_this()));
                        });
                }
                if(m_track_nodes)
                {
                    auto it = elements.begin();
                    std::advance(it, before);
                    m_node_elements[node] = std::vector<element::ptr>(it, elements.end());
                }
            }
            break;
        case GUMBO_NODE_CDATA:
            {
                element::ptr ret = std::make_shared<el_cdata>(shared_from_this());
                ret->set_data(node->v.text.text);
                elements.push_back(ret);
            }
            break;
        case GUMBO_NODE_COMMENT:
            {
                element::ptr ret = std::make_shared<el_comment>(shared_from_this());
                ret->set_data(node->v.text.text);
                elements.push_back(ret);
            }
            break;
        case GUMBO_NODE_WHITESPACE:
            {
                std::string str = node->v.text.text;
                for(size_t i = 0; i < str.length(); i++)
                {
                    // Telegram (VENDOR.md edit 10): no text piece past the cap.
                    if(!text_element_allowed())
                    {
                        m_build_limited = true;
                        break;
                    }
                    elements.push_back(std::make_shared<el_space>(str.substr(i, 1).c_str(), shared_from_this()));
                }
            }
            break;
        default:
            break;
        }
    }

    element::ptr document::create_element(const char* tag_name, const string_map& attributes)
    {
        element::ptr  newTag;
        document::ptr this_doc = shared_from_this();
        if(m_container)
        {
            newTag = m_container->create_element(tag_name, attributes, this_doc);
        }
        if(!newTag)
        {
            if(!strcmp(tag_name, "br"))
            {
                newTag = std::make_shared<el_break>(this_doc);
            } else if(!strcmp(tag_name, "p"))
            {
                newTag = std::make_shared<el_para>(this_doc);
            } else if(!strcmp(tag_name, "img") || !strcmp(tag_name, "canvas"))
            {
                newTag = std::make_shared<el_image>(this_doc);
            } else if(!strcmp(tag_name, "table"))
            {
                newTag = std::make_shared<el_table>(this_doc);
            } else if(!strcmp(tag_name, "td") || !strcmp(tag_name, "th"))
            {
                newTag = std::make_shared<el_td>(this_doc);
            } else if(!strcmp(tag_name, "link"))
            {
                newTag = std::make_shared<el_link>(this_doc);
            } else if(!strcmp(tag_name, "title"))
            {
                newTag = std::make_shared<el_title>(this_doc);
            } else if(!strcmp(tag_name, "a"))
            {
                newTag = std::make_shared<el_anchor>(this_doc);
            } else if(!strcmp(tag_name, "tr"))
            {
                newTag = std::make_shared<el_tr>(this_doc);
            } else if(!strcmp(tag_name, "style"))
            {
                newTag = std::make_shared<el_style>(this_doc);
            } else if(!strcmp(tag_name, "base"))
            {
                newTag = std::make_shared<el_base>(this_doc);
            } else if(!strcmp(tag_name, "body"))
            {
                newTag = std::make_shared<el_body>(this_doc);
            } else if(!strcmp(tag_name, "div"))
            {
                newTag = std::make_shared<el_div>(this_doc);
            } else if(!strcmp(tag_name, "script"))
            {
                newTag = std::make_shared<el_script>(this_doc);
            } else if(!strcmp(tag_name, "font"))
            {
                newTag = std::make_shared<el_font>(this_doc);
            } else if(!strcmp(tag_name, "svg"))
            {
                newTag = std::make_shared<el_svg>(this_doc);
            } else if(!strcmp(tag_name, "input"))
            {
                newTag = std::make_shared<el_input>(this_doc);
            } else if(!strcmp(tag_name, "select"))
            {
                newTag = std::make_shared<el_select>(this_doc);
            } else if(!strcmp(tag_name, "button"))
            {
                newTag = std::make_shared<el_button>(this_doc);
            } else if(!strcmp(tag_name, "textarea"))
            {
                newTag = std::make_shared<el_textarea>(this_doc);
            } else if(!strcmp(tag_name, "fieldset"))
            {
                newTag = std::make_shared<el_fieldset>(this_doc);
            } else if(!strcmp(tag_name, "progress") || !strcmp(tag_name, "meter"))
            {
                newTag = std::make_shared<el_meter>(this_doc);
            } else
            {
                newTag = std::make_shared<html_tag>(this_doc);
            }
        }

        if(newTag)
        {
            newTag->set_tagName(tag_name);
            for(const auto& attribute : attributes)
            {
                newTag->set_attr(attribute.first.c_str(), attribute.second.c_str());
            }
        }

        return newTag;
    }

    uint_ptr document::add_font(const font_description& descr, font_metrics* fm, const std::string& features)
    {
        uint_ptr ret = 0;

        std::string key = features.empty() ? descr.hash() : descr.hash() + ":ff=" + features;

        if(m_fonts.find(key) == m_fonts.end())
        {
            font_item fi = {0, {}};

            fi.font      = features.empty() ? m_container->create_font(descr, this, &fi.metrics)
                                        : m_container->create_font(descr, features, this, &fi.metrics);
            m_fonts[key] = fi;
            ret          = fi.font;
            if(fm)
            {
                *fm = fi.metrics;
            }
        }
        return ret;
    }

    uint_ptr document::get_font(const font_description& descr, font_metrics* fm, const std::string& features)
    {
        if(descr.size == 0_px)
        {
            // Telegram (VENDOR.md edit 18): no font has no metrics. Left as they were, an element restyled to font-size 0
            // (an attribute patch, edit 17) kept its old line height where a fresh one has none.
            if(fm)
                *fm = font_metrics();
            return 0;
        }

        auto key = features.empty() ? descr.hash() : descr.hash() + ":ff=" + features;

        auto el = m_fonts.find(key);

        if(el != m_fonts.end())
        {
            if(fm)
            {
                *fm = el->second.metrics;
            }
            return el->second.font;
        }
        return add_font(descr, fm, features);
    }

    std::atomic<uint64_t> document::s_layout_hits{0};
    std::atomic<uint64_t> document::s_layout_misses{0};
    std::atomic<uint64_t> document::s_layout_mismatches{0};

    bool document::layout_cache_enabled() const
    {
        static const bool disabled = getenv("LH_NO_LAYOUT_CACHE") != nullptr;
        return !disabled;
    }

    bool document::layout_cache_check() const
    {
        static const bool check = getenv("LH_LAYOUT_CACHE_CHECK") != nullptr;
        return check;
    }

    namespace
    {
        template <class F> void walk_boxes(render_item* item, float x, float y, bool unsafe, F&& visit)
        {
            const css_properties& css = item->src_el()->css();
            unsafe = unsafe || css.get_position() == element_position_absolute || css.get_position() == element_position_fixed ||
                     css.is_paint_group();
            const position& pos = item->pos_const();
            visit(item, x, y, unsafe);
            unsafe = unsafe || !css.get_transform().empty() || is_one_of(css.get_overflow(), overflow_scroll, overflow_auto);
            const float cx = x + pos.x.value();
            const float cy = y + pos.y.value();
            for(const auto& child : item->children())
                walk_boxes(child.get(), cx, cy, unsafe, visit);
        }

        bool paints_inside_box(const render_item* item)
        {
            const element::ptr&   el  = item->src_el();
            const css_properties& css = el->css();
            if(el->is_text())
            {
                const element::ptr parent = el->parent();
                return parent && parent->css().get_text_shadows().empty();
            }
            if(css.get_display() == display_list_item || !css.get_box_shadows().empty() || !css.get_text_shadows().empty() ||
               (css.get_outline().style != border_style_none && css.get_outline().style != border_style_hidden))
                return false;
            if(el->is_inline() && !el->is_inline_box())
            {
                const background* bg = el->get_background();
                if((bg && !bg->is_empty()) || css.get_borders().is_visible())
                    return false;
            }
            return !std::dynamic_pointer_cast<el_svg>(el);
        }
    }

    void document::snapshot_layout()
    {
        m_layout_before.clear();
        m_layout_before.reserve(m_layout_before.capacity());
        walk_boxes(m_root_render.get(), 0, 0, false, [this](const render_item* item, float x, float y, bool) {
            position box  = item->pos_const();
            box.x        += pixel_t(x);
            box.y        += pixel_t(y);
            m_layout_before.push_back({item, box});
        });
        m_layout_before_valid = true;
    }

    bool document::layout_dirty_regions(std::vector<position>& out)
    {
        out.clear();
        if(!m_layout_before_valid || !m_layout_after_render || !m_root_render)
            return false;
        if(!m_restyle_confined)
            return false;
        std::unordered_set<const element*> origins;
        std::vector<const element*>        sources(m_layout_origins.begin(), m_layout_origins.end());
        for(const auto& region : m_restyle_regions)
            sources.push_back(region.get());
        for(const element* el : sources)
        {
            const element* owner = el;
            while(owner && !const_cast<element*>(owner)->get_render_item())
            {
                const element::ptr parent = owner->parent();
                owner                     = parent.get();
            }
            if(!owner)
                return false;
            origins.insert(owner);
        }
        size_t index = 0;
        bool   ok    = true;
        double area  = 0;
        auto   add   = [&](const render_item* item, position box) {
            box += item->get_paddings_const();
            box += item->get_borders_const();
            pixel_t slack = 1_px;
            if(item->src_el()->is_text())
                slack = std::max(4_px, pixel_t(std::ceil(float(box.height) / 2) + 1)) + 1_px;
            box.x      -= slack;
            box.y      -= slack;
            box.width  += slack * 2;
            box.height += slack * 2;
            if(box.width <= 0_px || box.height <= 0_px)
                return;
            area += double(float(box.width)) * double(float(box.height));
            out.push_back(box);
        };
        walk_boxes(m_root_render.get(), 0, 0, false, [&](const render_item* item, float x, float y, bool unsafe) {
            if(!ok)
                return;
            if(index >= m_layout_before.size() || m_layout_before[index].item != item)
            {
                ok = false;
                return;
            }
            const position& before = m_layout_before[index++].box;
            position        box    = item->pos_const();
            box.x                 += pixel_t(x);
            box.y                 += pixel_t(y);
            if(before == box && !origins.count(item->src_el().get()))
                return;
            if(unsafe || !paints_inside_box(item))
            {
                ok = false;
                return;
            }
            add(item, before);
            add(item, box);
            if(out.size() > 128)
                ok = false;
        });
        if(!ok || index != m_layout_before.size())
            return false;
        const position& root = m_root_render->pos_const();
        if(!(area <= 0.5 * std::max(1.0, double(float(root.width)) * double(float(root.height)))))
            return false;
        return true;
    }

    pixel_t document::render(pixel_t max_width, render_type rt)
    {
        invalidate_paint();
        pixel_t ret = 0_px;
        m_layout_spent = 0;
        if(m_root && m_root_render && !m_layout_exhausted)
        {
            position viewport;
            m_container->get_viewport(viewport);
            if(!(viewport == m_layout_viewport))
            {
                m_layout_viewport = viewport;
                invalidate_layout_cache();
            }
            m_layout_marked.clear();
            containing_block_context cb_context;
            cb_context.width       = max_width;
            cb_context.width.type  = containing_block_context::cbc_value_type_absolute;
            cb_context.height      = viewport.height;
            cb_context.height.type = containing_block_context::cbc_value_type_absolute;

            if(rt == render_fixed_only)
            {
                m_fixed_boxes.clear();
                m_root_render->render_positioned(rt);
            } else
            {
                static const bool refetch = getenv("LH_NO_POSITIONED_REUSE") != nullptr;
                if(refetch || m_positioned_epoch != m_style_epoch || m_positioned_tree != m_render_tree_serial)
                {
                    m_has_positioned   = m_root_render->fetch_positioned();
                    m_positioned_epoch = m_style_epoch;
                    m_positioned_tree  = m_render_tree_serial;
                }
                ret = m_root_render->render(0_px, 0_px, cb_context, nullptr).natural_width;
                for(render_item* item : m_relayout_roots)
                    item->relayout();
                m_relayout_roots.clear();
                m_layout_after_render = true;
                if(m_has_positioned)
                {
                    m_fixed_boxes.clear();
                    m_root_render->render_positioned(rt);
                }
                m_size.width  = 0;
                m_size.height = 0;
                m_root_render->calc_document_size(m_size);
                m_replaced_hold.clear(); // Telegram (VENDOR.md edit 74): the layout let go of the replaced items
            }
        }
        // Telegram (VENDOR.md edit 20): the boxes this layout changed, each against what it was before.
        if(m_render_observer)
            report_observed_geometry();
        return ret;
    }

    // Telegram (VENDOR.md edit 20): the render observer.
    void document::set_render_observer(render_observer* observer)
    {
        m_render_observer = observer;
        m_observed.clear();
        m_patch_paint_order.clear();
        m_observe_epoch = m_observe_epoch + 1 == 0 ? 1 : m_observe_epoch + 1;
    }

    void document::report_observed_geometry()
    {
        std::vector<render_item*> items;
        items.swap(m_observed);
        // The next layout's first writes save again.
        m_observe_epoch = m_observe_epoch + 1 == 0 ? 1 : m_observe_epoch + 1;
        for(render_item* item : items)
        {
            const unsigned what = item->observed_change();
            if(what && m_render_observer)
                m_render_observer->item_geometry_changed(item, what);
        }
    }

    void document::draw(uint_ptr hdc, pixel_t x, pixel_t y, const position* clip, bool cull)
    {
        if(m_root && m_root_render)
        {
            const bool saved = m_cull_paint;
            m_cull_paint     = cull && clip && !g_paint_trace;
            if(!m_root->css().get_clip_path().empty())
            {
                m_root_render->draw_group(hdc, x, y, clip, m_root_render);
            } else
            {
#if 0
                m_root->draw(hdc, x, y, clip, m_root_render);
#else
                paint_or_visit(m_root_render, draw_root, hdc, x, y, clip);   // Telegram (VENDOR.md edit 20)
#endif
                m_root_render->draw_stacking_context(hdc, x, y, clip, true);
            }
            m_cull_paint = saved;
        }
    }

    pixel_t document::to_pixels(const css_length& val, const font_metrics& metrics, pixel_t size) const
    {
        if(val.is_predefined())
        {
            return 0_px;
        }
        // Telegram (VENDOR.md edit 48): a calc at `size`, its font-relative terms by `metrics`.
        if(val.is_calc())
        {
            return val.calc_value(size, calc_fonts_for(metrics));
        }
        // Telegram: viewport_changed() computes styles again only for a document that resolved a viewport unit.
        if(val.units() == css_units_vw || val.units() == css_units_vh || val.units() == css_units_vmin || val.units() == css_units_vmax)
            m_viewport_units = true;
        pixel_t ret;
        switch(val.units())
        {
        case css_units_percentage:
            ret = val.calc_percent(size);
            break;
        case css_units_em:
            ret = metrics.font_size * val.val();
            break;

        // https://drafts.csswg.org/css-values-4/#absolute-lengths
        case css_units_pt:
            ret = m_container->pt_to_px(val.val());
            break;
        case css_units_in:
            ret = m_container->pt_to_px(val.val() * 72); // 1in = 72pt
            break;
        case css_units_pc:
            ret = m_container->pt_to_px(val.val() * 12); // 1pc = (1/6)in = 12pt
            break;
        case css_units_cm:
            ret = m_container->pt_to_px(val.val() * 72 / 2.54f); // 1cm = (1/2.54)in = (72/2.54)pt
            break;
        case css_units_mm:
            ret = m_container->pt_to_px(val.val() * 72 / 2.54f / 10);
            break;

        case css_units_vw:
            ret = pixel_t(m_media.width.value() * val.val() / 100.0f);
            break;
        case css_units_vh:
            ret = pixel_t(m_media.height.value() * val.val() / 100.0f);
            break;
        case css_units_vmin:
            ret = pixel_t(std::min(m_media.height.value(), m_media.width.value()) * val.val() / 100.0f);
            break;
        case css_units_vmax:
            ret = pixel_t(std::max(m_media.height.value(), m_media.width.value()) * val.val() / 100.0f);
            break;
        case css_units_rem:
            ret = pixel_t(m_root->css().get_font_size().value() * val.val());
            break;
        case css_units_ex:
            ret = pixel_t(metrics.x_height.value() * val.val());
            break;
        case css_units_ch:
            ret = pixel_t(metrics.ch_width.value() * val.val());
            break;
        default:
            ret = pixel_t(val.val());
            break;
        }
        return ret;
    }

    // Telegram (VENDOR.md edit 48).
    calc_fonts document::calc_fonts_for(const font_metrics& metrics) const
    {
        calc_fonts fonts;
        fonts.em  = double(metrics.font_size);
        fonts.rem = m_root ? double(m_root->css().get_font_size().value()) : double(metrics.font_size);
        fonts.ex  = double(metrics.x_height.value());
        fonts.ch  = double(metrics.ch_width.value());
        return fonts;
    }

    void document::cvt_units(css_length& val, const font_metrics& metrics, pixel_t size) const
    {
        if(val.is_predefined())
        {
            return;
        }
        // Telegram (VENDOR.md edit 48): font-relative terms fold into px now; a calc left without a percentage is a
        // plain px length, and one with a percentage stays deferred to layout (calc_percent).
        if(val.is_calc())
        {
            const calc_fonts fonts  = calc_fonts_for(metrics);
            calc_ref         folded = calc_node::fold_fonts(val.calc(), fonts);
            if(!folded->has_percent())
                val.set_value(float(folded->eval(0, fonts)), css_units_px);
            else
                val.set_calc(std::move(folded));
            return;
        }
        if(val.units() != css_units_percentage)
        {
            val.set_value(static_cast<float>(to_pixels(val, metrics, size)), css_units_px);
        }
    }

    pixel_t document::width() const
    {
        return m_size.width;
    }

    pixel_t document::height() const
    {
        return m_size.height;
    }

    void document::add_stylesheet(const char* str, const char* baseurl, const char* media)
    {
        // Telegram (VENDOR.md edit 81): once finalize has parsed the sheets, apply_sheets reads them from the tree (a
        // <style> a patch inserts is parsed by its parse_attributes too).
        if(str && str[0] && !m_sheets_parsed)
        {
            m_style_rule_bytes = -1; // Telegram (VENDOR.md edit 21)
            m_css.emplace_back(str, baseurl, media);
        }
    }

    bool document::on_mouse_over(pixel_t x, pixel_t y, pixel_t client_x, pixel_t client_y,
                                 const std::function<void(const position&)>& redraw_box)
    {
        invalidate_paint();
        if(!m_root || !m_root_render)
        {
            return false;
        }

        element::ptr over_el = m_root_render->get_element_by_point(x, y, client_x, client_y, nullptr);

        bool state_was_changed = false;

        if(over_el != m_over_element)
        {
            if(m_over_element)
            {
                if(m_over_element->on_mouse_leave())
                {
                    m_container->on_mouse_event(m_over_element, mouse_event_leave);
                    state_was_changed = true;
                }
            }
            m_over_element = over_el;
        }

        std::string cursor;

        if(m_over_element)
        {
            if(m_over_element->on_mouse_over())
            {
                state_was_changed = true;
            }
            cursor = m_over_element->css().get_cursor();
        }

        m_container->set_cursor(cursor.c_str());

        if(state_was_changed)
        {
            m_container->on_mouse_event(m_over_element, mouse_event_enter);
            return m_root->find_styles_changes(redraw_box);
        }
        return false;
    }

    std::vector<scroll_values> document::on_scroll(pixel_t dx, pixel_t dy, pixel_t x, pixel_t y, pixel_t client_x,
                                                   pixel_t client_y) const
    {
        invalidate_paint();
        if(dy == 0_px && dx == 0_px)
        {
            return {};
        }

        element::ptr vscroll_el;
        element::ptr hscroll_el;

        if(dy != 0_px)
        {
            vscroll_el = m_root_render->get_element_by_point(
                x, y, client_x, client_y,
                [dy](const std::shared_ptr<render_item>& el) -> bool { return el->is_v_scrollable(dy); });
        }

        if(dx != 0_px)
        {
            hscroll_el = m_root_render->get_element_by_point(
                x, y, client_x, client_y,
                [dx](const std::shared_ptr<render_item>& el) -> bool { return el->is_h_scrollable(dx); });
        }

        if(!vscroll_el && !hscroll_el)
        {
            return {};
        }

        if(vscroll_el == hscroll_el)
        {
            scroll_values sv;
            sv.dx         = hscroll_el->h_scroll(dx);
            sv.dy         = vscroll_el->v_scroll(dy);
            sv.scroll_box = hscroll_el->get_placement();
            return {sv};
        }

        std::vector<scroll_values> ret;
        ret.reserve(2);

        if(vscroll_el)
        {
            scroll_values sv;
            sv.dy         = vscroll_el->v_scroll(dy);
            sv.scroll_box = vscroll_el->get_placement();
            ret.push_back(sv);
        }

        if(hscroll_el)
        {
            scroll_values sv;
            sv.dx         = hscroll_el->h_scroll(dx);
            sv.scroll_box = hscroll_el->get_placement();
            ret.push_back(sv);
        }

        return ret;
    }

    bool document::on_mouse_leave(const std::function<void(const position&)>& redraw_box)
    {
        invalidate_paint();
        if(!m_root || !m_root_render)
        {
            return false;
        }
        if(m_over_element)
        {
            auto el        = m_over_element;
            m_over_element = nullptr;
            if(el->on_mouse_leave())
            {
                m_container->on_mouse_event(el, mouse_event_leave);
                return m_root->find_styles_changes(redraw_box);
            }
        }
        return false;
    }

    bool document::on_lbutton_down(pixel_t x, pixel_t y, pixel_t client_x, pixel_t client_y,
                                   const std::function<void(const position&)>& redraw_box)
    {
        invalidate_paint();
        if(!m_root || !m_root_render)
        {
            return false;
        }

        element::ptr over_el = m_root_render->get_element_by_point(x, y, client_x, client_y, nullptr);
        m_active_element     = over_el;

        bool state_was_changed = false;

        if(over_el != m_over_element)
        {
            if(m_over_element)
            {
                if(m_over_element->on_mouse_leave())
                {
                    m_container->on_mouse_event(m_over_element, mouse_event_leave);
                    state_was_changed = true;
                }
            }
            m_over_element = over_el;
            if(m_over_element)
            {
                if(m_over_element->on_mouse_over())
                {
                    state_was_changed = true;
                }
            }
        }

        std::string cursor;

        if(m_over_element)
        {
            if(m_over_element->on_lbutton_down())
            {
                state_was_changed = true;
            }
            cursor = m_over_element->css().get_cursor();
        }

        m_container->set_cursor(cursor.c_str());

        if(state_was_changed)
        {
            m_container->on_mouse_event(m_over_element, mouse_event_enter);
            return m_root->find_styles_changes(redraw_box);
        }

        return false;
    }

    bool document::on_lbutton_up(pixel_t /*x*/, pixel_t /*y*/, pixel_t /*client_x*/, pixel_t /*client_y*/,
                                 const std::function<void(const position&)>& redraw_box)
    {
        invalidate_paint();
        if(!m_root || !m_root_render)
        {
            return false;
        }
        if(m_over_element)
        {
            if(m_over_element->on_lbutton_up(m_active_element == m_over_element))
            {
                return m_root->find_styles_changes(redraw_box);
            }
        }
        return false;
    }

    bool document::on_button_cancel(const std::function<void(const position&)>& redraw_box)
    {
        invalidate_paint();
        m_active_element = nullptr;
        return on_mouse_leave(redraw_box);
    }

    void document::get_fixed_boxes(position::vector& fixed_boxes)
    {
        fixed_boxes = m_fixed_boxes;
    }

    void document::add_fixed_box(const position& pos)
    {
        m_fixed_boxes.push_back(pos);
    }

    bool document::media_changed()
    {
        invalidate_paint();
        container()->get_media_features(m_media);
        m_rebuild_reason.clear(); // Telegram (VENDOR.md edit 73)
        if(update_media_lists(m_media))
        {
            m_rebuild_reason = "media";
#if 0
            m_root->refresh_styles();
#else
            refresh_all_styles(); // Telegram (VENDOR.md edit 21)
#endif
            m_root->compute_styles();
            // The set of rendered elements can change across a media breakpoint
            // (e.g. display:none <-> block on responsive nav/hero blocks). The render
            // tree is built once in createFromString() from the computed display values,
            // so rebuild it here to add/remove render items for elements whose display
            // just changed; otherwise a newly-shown element keeps no render item and
            // never lays out (it collapses to zero).
            replace_root_render(); // Telegram (VENDOR.md edit 74)
            report_replacements();
            return true;
        }
        return false;
    }

    // Telegram: the media features, and the viewport units computed from them, follow the container's
    // viewport. createFromString reads them once, at parse, when the container may not know its width yet
    // (LHDocument's is 0 until the first layout), and nothing reads them again, so every
    // @media width query and vw length was resolved against 0. The host calls this after it sets the viewport
    // a layout is measured against. When the viewport moved, the media lists are applied again, and styles
    // and the render tree are rebuilt as media_changed() does when a query flipped or the document resolved
    // a viewport unit (those turn into pixels while styles compute, and the style-sharing cache keeps
    // computed values, so it is emptied first).
    bool document::viewport_changed()
    {
        if(!m_root)
        {
            return false;
        }
        media_features media;
        container()->get_media_features(media);
        if(media.width.value() == m_media.width.value() && media.height.value() == m_media.height.value())
        {
            return false;
        }
        m_media = media;
        const bool flipped = update_media_lists(m_media);
        m_rebuild_reason.clear(); // Telegram (VENDOR.md edit 73)
        if(!flipped && !m_viewport_units)
        {
            return false;
        }
        m_rebuild_reason = flipped ? "media" : "viewport";
        invalidate_paint();
        m_share_cache.clear();
#if 0
        m_root->refresh_styles();
#else
        refresh_all_styles(); // Telegram (VENDOR.md edit 21)
#endif
        m_root->compute_styles();
        replace_root_render(); // Telegram (VENDOR.md edit 74)
        report_replacements();
        return true;
    }

    bool document::lang_changed()
    {
        invalidate_paint();
        if(!m_media_lists.empty())
        {
            std::string culture;
            container()->get_language(m_lang, culture);
            if(!culture.empty())
            {
                m_culture = m_lang + '-' + culture;
            } else
            {
                m_culture.clear();
            }
#if 0
            m_root->refresh_styles();
#else
            refresh_all_styles(); // Telegram (VENDOR.md edit 21)
#endif
            m_root->compute_styles();
            return true;
        }
        return false;
    }

    // Apply media features (determine which selectors are active).
    bool document::update_media_lists(const media_features& features)
    {
        invalidate_paint();
        bool update_styles = false;
        for(auto& media_list : m_media_lists)
        {
            if(media_list->apply_media_features(features))
            {
                update_styles = true;
            }
        }
        return update_styles;
    }

    void document::add_media_list(const media_query_list_list::ptr& list)
    {
        if(list && !contains(m_media_lists, list))
        {
            m_media_lists.push_back(list);
        }
    }

    void document::fix_tables_layout()
    {
        invalidate_paint();
        const std::size_t limit = m_tabular_elements.size() * 8 + 64;
        std::size_t       processed = 0;
        for(const auto& el_ptr : m_tabular_elements)
        {
            if(++processed > limit)
            {
                break;
            }
            switch(el_ptr->src_el()->css().get_display())
            {
            case display_inline_table:
            case display_table:
                fix_table_children(el_ptr, display_table_row_group, "table-row-group");
                break;
            case display_table_footer_group:
            case display_table_row_group:
            case display_table_header_group:
                {
                    auto parent = el_ptr->parent();
                    if(parent)
                    {
                        if(parent->src_el()->css().get_display() != display_inline_table)
                        {
                            fix_table_parent(el_ptr, display_table, "table");
                        }
                    }
                    fix_table_children(el_ptr, display_table_row, "table-row");
                }
                break;
            case display_table_row:
                fix_table_parent(el_ptr, display_table_row_group, "table-row-group");
                fix_table_children(el_ptr, display_table_cell, "table-cell");
                break;
            case display_table_cell:
                fix_table_parent(el_ptr, display_table_row, "table-row");
                break;
            // TODO: make table layout fix for table-caption, table-column etc. elements
            case display_table_caption:
            case display_table_column:
            case display_table_column_group:
            default:
                break;
            }
        }
        // Telegram (VENDOR.md edit 85): the list is this pass's alone. Kept, it held the items init() replaces after it,
        // and with them the elements under them: a replaced text's pieces stayed alive.
        m_tabular_elements.clear();
    }

    void document::fix_table_children(const std::shared_ptr<render_item>& el_ptr, style_display disp,
                                      const char* disp_str)
    {
        std::list<std::shared_ptr<render_item>> tmp;
        auto                                    first_iter = el_ptr->children().begin();
        auto                                    cur_iter   = el_ptr->children().begin();

        auto flush_elements = [&]() {
            element::ptr annon_tag = std::make_shared<html_tag>(el_ptr->src_el(), std::string("display:") + disp_str);
            std::shared_ptr<render_item> annon_ri;
            if(annon_tag->css().get_display() == display_table_cell)
            {
                annon_tag->set_tagName("table_cell");
                annon_ri = std::make_shared<render_item_block>(annon_tag);
            } else if(annon_tag->css().get_display() == display_table_row)
            {
                annon_ri = std::make_shared<render_item_table_row>(annon_tag);
            } else
            {
                annon_ri = std::make_shared<render_item_table_part>(annon_tag);
            }
            for(const auto& el : tmp)
            {
                annon_ri->add_child(el);
            }
            // add annon item as tabular for future processing
            add_tabular(annon_ri);
            annon_ri->parent(el_ptr);
            first_iter = el_ptr->children().insert(first_iter, annon_ri);
            cur_iter   = std::next(first_iter);
            while(cur_iter != el_ptr->children().end() && (*cur_iter)->parent() != el_ptr)
            {
                cur_iter = el_ptr->children().erase(cur_iter);
            }
            first_iter = cur_iter;
            tmp.clear();
        };

        while(cur_iter != el_ptr->children().end())
        {
            if((*cur_iter)->src_el()->css().get_display() != disp)
            {
                if(!(*cur_iter)->src_el()->is_table_skip() || ((*cur_iter)->src_el()->is_table_skip() && !tmp.empty()))
                {
                    if(disp != display_table_row_group ||
                       (*cur_iter)->src_el()->css().get_display() != display_table_caption)
                    {
                        if(tmp.empty())
                        {
                            first_iter = cur_iter;
                        }
                        tmp.push_back((*cur_iter));
                    }
                }
                cur_iter++;
            } else if(!tmp.empty())
            {
                flush_elements();
            } else
            {
                cur_iter++;
            }
        }
        if(!tmp.empty())
        {
            flush_elements();
        }
    }

    void document::fix_table_parent(const std::shared_ptr<render_item>& el_ptr, style_display disp,
                                    const char* disp_str)
    {
        auto parent = el_ptr->parent();

        if(parent->src_el()->css().get_display() != disp)
        {
            auto this_element = std::find_if(parent->children().begin(), parent->children().end(),
                                             [&](const std::shared_ptr<render_item>& el) { return el == el_ptr; });
            if(this_element != parent->children().end())
            {
                style_display el_disp = el_ptr->src_el()->css().get_display();
                auto          first   = this_element;
                auto          last    = this_element;
                auto          cur     = this_element;

                // find first element with same display
                while(true)
                {
                    if(cur == parent->children().begin())
                    {
                        break;
                    }
                    cur--;
                    if((*cur)->src_el()->is_table_skip() || (*cur)->src_el()->css().get_display() == el_disp)
                    {
                        first = cur;
                    } else
                    {
                        break;
                    }
                }

                // find last element with same display
                cur = this_element;
                while(true)
                {
                    cur++;
                    if(cur == parent->children().end())
                    {
                        break;
                    }

                    if((*cur)->src_el()->is_table_skip() || (*cur)->src_el()->css().get_display() == el_disp)
                    {
                        last = cur;
                    } else
                    {
                        break;
                    }
                }

                // extract elements with the same display and wrap them with anonymous object
                element::ptr annon_tag =
                    std::make_shared<html_tag>(parent->src_el(), std::string("display:") + disp_str);
                std::shared_ptr<render_item> annon_ri;
                if(annon_tag->css().get_display() == display_table ||
                   annon_tag->css().get_display() == display_inline_table)
                {
                    annon_ri = std::make_shared<render_item_table>(annon_tag);
                } else if(annon_tag->css().get_display() == display_table_row)
                {
                    annon_ri = std::make_shared<render_item_table_row>(annon_tag);
                } else
                {
                    annon_ri = std::make_shared<render_item_table_part>(annon_tag);
                }
                std::for_each(first, std::next(last, 1),
                              [&annon_ri](std::shared_ptr<render_item>& el) { annon_ri->add_child(el); });
                first = parent->children().erase(first, std::next(last));
                parent->children().insert(first, annon_ri);
                add_tabular(annon_ri);
                annon_ri->parent(parent);
            }
        }
    }

    void document::append_children_from_string(element& parent, const char* str, bool replace_existing)
    {
        invalidate_paint();
        // parent must belong to this document
        if(parent.get_document().get() != this)
        {
            return;
        }

        GumboOptions opts = kGumboDefaultOptions;
        // This is require to prevent creating html, head, body tags around the fragment
        // Although Gumbo always creates html tag anyway. We have to ignore it in create_node.
        opts.fragment_context = GUMBO_TAG_BODY;
        // parse document into GumboOutput
        GumboOutput* output = gumbo_parse_with_options(&opts, str, strlen(str));

        // Create litehtml::elements.
        elements_list child_elements;
        // Create elements excluding the root node
        create_node(output->root, child_elements, true, false);

        // Destroy GumboOutput
        gumbo_destroy_output(&kGumboDefaultOptions, output);

        auto parent_render = parent.get_render_item();

        if(replace_existing)
        {
            parent.clearRecursive();
            parent_render->children().clear();
        }

        // Let's process created elements tree
        for(const auto& child : child_elements)
        {
            // Add the child element to parent
            parent.appendChild(child);

            // apply master CSS
            child->apply_stylesheet(m_master_css);

            // parse elements attributes
            child->parse_attributes();

            // Apply parsed styles.
            child->apply_stylesheet(m_styles);

            // Apply user styles if any
            child->apply_stylesheet(m_user_css);

            // Initialize m_css
            child->compute_styles();

            // Finally initialize elements
            if(parent_render)
            {
                auto child_render = child->create_render_item(parent_render);
                if(child_render)
                {
                    child_render = child_render->init();
                    parent_render->add_child(child_render);
                }
            }
        }
        // Now the m_tabular_elements is filled with tabular elements.
        // We have to check the tabular elements for missing table elements
        // and create the anonymous boxes in visual table layout
        fix_tables_layout();
    }

    void document::dump(dumper& cout)
    {
        if(m_root_render)
        {
            m_root_render->dump(cout);
        }
    }

} // namespace litehtml
