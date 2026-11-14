#include "html.h"
#include "css_parser.h"
#include "el_before_after.h"
#include "el_text.h"
#include "el_space.h"
#include "el_image.h"
#include "utf8_strings.h"
#include "css_parser.h" // Telegram (VENDOR.md edit 19): max_css_list

namespace litehtml
{
    namespace
    {
        // Telegram (VENDOR.md edit 19): a content url() image, counted like its pseudo-element.
        class el_pseudo_image : public el_image
        {
          public:
            explicit el_pseudo_image(const document::ptr& doc) :
                el_image(doc)
            {
                if(doc)
                    doc->note_pseudo_element(1);
            }
            ~el_pseudo_image() override
            {
                if(const auto doc = get_document())
                    doc->note_pseudo_element(-1);
            }
        };
    } // namespace
} // namespace litehtml

litehtml::el_before_after_base::el_before_after_base(const std::shared_ptr<document>& doc, bool before) :
    html_tag(doc)
{
    m_tag = before ? __tag_before_ : __tag_after_;
    if(doc)
        doc->note_pseudo_element(1); // Telegram (VENDOR.md edit 19)
}

// Telegram (VENDOR.md edit 19).
litehtml::el_before_after_base::~el_before_after_base()
{
    if(const auto doc = get_document())
        doc->note_pseudo_element(-1);
}

void litehtml::el_before_after_base::add_style(const style& style)
{
    html_tag::add_style(style);

    auto children = m_children;
    m_children.clear();
    // Telegram (VENDOR.md edit 84): the content made before stays alive, and counted, while this one is made.
    if(const auto doc = get_document())
        doc->note_generated_transient(int(children.size()));

    const auto& content_property = style.get_property(_content_);
    if(content_property.is<std::string>() && !content_property.get<std::string>().empty())
    {
        const auto& str = content_property.get<std::string>();
        auto        idx = css_values(content_property_strings).value_index(str);
        if(idx == content_property_open_quote || idx == content_property_close_quote)
        {
            add_quote(idx == content_property_open_quote);
        } else if(!idx.has_value())
        {
            // The value is kept as written: read it with the tokenizer, as the sanitizer did, so that an
            // escaped quote stays inside its string and only a url token or url() names an image.
            // Telegram (VENDOR.md edit 19): at most max_css_list items (strings, images and functions) are kept.
            size_t items = 0;
            for(const css_token& tok : normalize(str, f_componentize | f_remove_whitespace))
            {
                if(items >= max_css_list)
                    break;
                std::string url;
                if(tok.type == STRING)
                {
                    items++;
                    add_text(tok.str(), false);
                } else if(tok.type == IDENT)
                {
                    std::string name = lowcase(tok.name());
                    if(name == "open-quote" || name == "close-quote")
                        add_quote(name == "open-quote");
                } else if(parse_url(tok, url))
                {
                    items++;
                    add_image(url);
                } else if(tok.type == CV_FUNCTION)
                {
                    items++;
                    add_function(lowcase(tok.name()), get_repr(tok.value));
                }
            }
        }
    }

    if(m_children.empty())
    {
        m_children = children;
    }
}

void litehtml::el_before_after_base::add_text(const std::string& txt, bool escapes)
{
    const document::generated_scope generated(get_document().get()); // Telegram (VENDOR.md edit 84)
    std::string word;
    std::string esc;
    // Telegram (VENDOR.md edit 10): content is made again on every restyle, for every element it matches, so it
    // stops at the document's text cap like parsed text does.
    const auto doc = get_document();

    for(auto chr : txt)
    {
        if(escapes && (chr == '\\' || (!esc.empty() && esc.length() < 5 &&
                                      ((chr >= '0' && chr <= '9') || (chr >= 'A' && chr <= 'Z') || (chr >= 'a' && chr <= 'z')))))
        {
            if(!esc.empty() && chr == '\\')
            {
                word += convert_escape(esc.c_str() + 1);
                esc.clear();
            }
            esc += chr;
        } else
        {
            if(!esc.empty())
            {
                word += convert_escape(esc.c_str() + 1);
                esc.clear();
            }
            if(isspace(static_cast<unsigned char>(chr)))
            {
                if(!word.empty())
                {
                    if(doc && !doc->text_element_allowed())
                        return;
                    element::ptr el = std::make_shared<el_text>(word.c_str(), get_document());
                    appendChild(el);
                    word.clear();
                }
                if(doc && !doc->text_element_allowed())
                    return;
                word            += chr;
                element::ptr el  = std::make_shared<el_space>(word.c_str(), get_document());
                appendChild(el);
                word.clear();
            } else
            {
                word += chr;
            }
        }
    }

    if(!esc.empty())
    {
        word += convert_escape(esc.c_str() + 1);
    }
    if(!word.empty())
    {
        if(doc && !doc->text_element_allowed())
            return;
        element::ptr el = std::make_shared<el_text>(word.c_str(), get_document());
        appendChild(el);
        word.clear();
    }
}

void litehtml::el_before_after_base::add_quote(bool open)
{
    std::vector<std::string> marks;
    for(element::ptr el = parent(); el && marks.empty(); el = el->parent())
    {
        auto tag = std::dynamic_pointer_cast<html_tag>(el);
        std::string value = tag ? tag->raw_declaration("quotes") : std::string();
        trim(value);
        if(value.empty() || value == "inherit" || value == "unset")
            continue;
        if(value == "none")
            return;
        for(size_t i = 0; i < value.size(); i++)
        {
            if(value[i] != '"' && value[i] != '\'')
                continue;
            size_t end = value.find(value[i], i + 1);
            if(end == std::string::npos)
                break;
            marks.push_back(value.substr(i + 1, end - i - 1));
            i = end;
        }
        break;
    }
    // auto, or nothing usable: English marks, as browsers use for lang=en.
    if(marks.size() < 2)
        marks = {"\xE2\x80\x9C", "\xE2\x80\x9D", "\xE2\x80\x98", "\xE2\x80\x99"};
    size_t depth = 0;
    if(auto q = parent())
        for(element::ptr el = q->parent(); el; el = el->parent())
            depth += el->tag() == _q_;
    size_t pair = std::min(depth, marks.size() / 2 - 1);
    add_text(marks[pair * 2 + (open ? 0 : 1)]);
}

void litehtml::el_before_after_base::add_function(const std::string& fnc, const std::string& params)
{
    constexpr auto content_function_strings = split_css_values<3>("attr;counter;counters");

    auto idx = css_values(content_function_strings).value_index(fnc);
    if(!idx.has_value())
    {
        return;
    }
    switch(idx.value())
    {
    // attr
    case 0:
        {
            std::string p_name = params;
            trim(p_name);
            lcase(p_name);
            element::ptr el_parent = parent();
            if(el_parent)
            {
                const char* attr_value = el_parent->get_attr(p_name.c_str());
                if(attr_value)
                {
                    add_text(attr_value, false);
                }
            }
        }
        break;
    // counter
    case 1:
        m_reads_counters = true; // Telegram (VENDOR.md edit 78)
        add_text(get_counter_value(params));
        break;
    // counters
    case 2:
        {
            m_reads_counters = true; // Telegram (VENDOR.md edit 78)
            string_vector tokens;
            split_string(params, tokens, ",");
            for(auto& str : tokens)
            {
                trim(str);
            }
            add_text(get_counters_value(tokens));
        }
        break;
    }
}

void litehtml::el_before_after_base::add_image(const std::string& url)
{
    if(url.empty())
    {
        return;
    }
    // Telegram (VENDOR.md edit 19): the element cap counts the image.
    const auto doc = get_document();
    if(doc && !doc->pseudo_element_allowed())
    {
        return;
    }
    element::ptr el = std::make_shared<el_pseudo_image>(get_document());
    el->set_attr("src", url.c_str());
    el->set_attr("style", "display:inline-block");
    el->set_tagName("img");
    appendChild(el);
    el->parse_attributes();
}

std::string litehtml::el_before_after_base::convert_escape(const char* txt)
{
    char*    str_end;
    char32_t u_str[2];
    u_str[0] = static_cast<char32_t>(strtol(txt, &str_end, 16));
    u_str[1] = 0;
    return litehtml_from_utf32(u_str).c_str();
}
