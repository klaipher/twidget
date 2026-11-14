#include "html.h"
#include "html_microsyntaxes.h"
#include "el_image.h"
#include "render_image.h"
#include "document_container.h"

#include <algorithm>

litehtml::el_image::el_image(const document::ptr& doc) :
    html_tag(doc)
{
    m_css.set_display(display_inline_block);
}

void litehtml::el_image::get_content_size(size& sz, pixel_t /*max_width*/)
{
    if(tag() == _canvas_)
    {
        // A canvas takes its intrinsic size from its width and height attributes (default 300x150). The
        // Swift renderer keeps them. The core renderer strips them and gives a tw-canvas: source that
        // carries the drawn size, so a canvas with neither attribute but a source asks the container.
        const char* w = get_attr("width");
        const char* h = get_attr("height");
        if(!w && !h && !m_src.empty())
        {
            get_document()->container()->get_image_size(m_src.c_str(), nullptr, sz);
            return;
        }
        int width  = 300;
        int height = 150;
        if(w)
            html_parse_non_negative_integer(w, width);
        if(h)
            html_parse_non_negative_integer(h, height);
        sz.width  = pixel_t(width);
        sz.height = pixel_t(height);
        return;
    }
    get_document()->container()->get_image_size(m_src.c_str(), nullptr, sz);
}

bool litehtml::el_image::is_replaced() const
{
    return true;
}

void litehtml::el_image::update_src()
{
    m_src    = get_attr("src", "");
    m_loaded = false;
}

void litehtml::el_image::parse_attributes()
{
    m_src    = get_attr("src", "");
    m_loaded = false;

    // A canvas's width and height are its bitmap size, not presentation hints: they must not become a
    // fixed CSS size, or a responsive canvas (width:100%) would keep its bitmap height.
    if(tag() == _canvas_)
    {
        return;
    }

    // https://html.spec.whatwg.org/multipage/rendering.html#attributes-for-embedded-content-and-images:the-img-element-5
    const char* str = get_attr("width");
    if(str)
    {
        map_to_dimension_property(_width_, str);
    }

    str = get_attr("height");
    if(str)
    {
        map_to_dimension_property(_height_, str);
    }
}

// Telegram (VENDOR.md edit 37): the object rect (CSS Images 3 §5.5): object-fit sizes the natural size into the content
// box, object-position places it by percentages of the space left over. fill, or no natural size, is the content box.
static litehtml::position object_rect(const litehtml::position& box, const litehtml::size& natural,
                                      const litehtml::css_properties& css)
{
    using namespace litehtml;
    const object_fit fit = css.get_object_fit();
    if(fit == object_fit_fill || natural.width <= 0_px || natural.height <= 0_px)
        return box;
    float       w  = float(natural.width);
    float       h  = float(natural.height);
    const float sx = float(box.width) / w;
    const float sy = float(box.height) / h;
    float       s  = 1;
    switch(fit)
    {
    case object_fit_contain:
        s = std::min(sx, sy);
        break;
    case object_fit_cover:
        s = std::max(sx, sy);
        break;
    case object_fit_scale_down:
        s = std::min(1.0f, std::min(sx, sy));
        break;
    default:
        break;
    }
    w *= s;
    h *= s;
    position rect;
    rect.width  = pixel_t(w);
    rect.height = pixel_t(h);
    rect.x      = box.x + css.get_object_position_x().calc_percent(box.width - pixel_t(w));
    rect.y      = box.y + css.get_object_position_y().calc_percent(box.height - pixel_t(h));
    return rect;
}

void litehtml::el_image::draw(uint_ptr hdc, pixel_t x, pixel_t y, const position* clip,
                              const std::shared_ptr<render_item>& ri)
{
    html_tag::draw(hdc, x, y, clip, ri);
    position pos  = ri->pos();
    pos.x        += x;
    pos.y        += y;
    pos.round();

    // draw image as background
    if(pos.does_intersect(clip))
    {
        if(pos.width > 0_px && pos.height > 0_px)
        {
            background_layer layer;
            layer.clip_box    = pos;
            layer.origin_box  = fitted(pos);
            layer.border_box  = pos;
            layer.border_box += ri->get_paddings();
            layer.border_box += ri->get_borders();
            layer.repeat      = background_repeat_no_repeat;
            layer.border_radius =
                css().get_borders().radius.calc_percents(layer.border_box.width, layer.border_box.height);
            doc_ptr()->container()->draw_image(hdc, layer, m_src, {});
        }
    }
}

// Upstream's object-fit / object-position (VENDOR.md edit 37) place the image; a container that reports an
// SVG preserveAspectRatio for it (get_image_aspect) drives the fit and position when object-fit is the default fill.
litehtml::position litehtml::el_image::fitted(const position& box)
{
    size natural;
    get_document()->container()->get_image_size(m_src.c_str(), nullptr, natural);
    if(css().get_object_fit() != object_fit_fill)
        return object_rect(box, natural, css());
    const std::string aspect = get_document()->container()->get_image_aspect(m_src.c_str());
    if(aspect.empty() || natural.width <= 0_px || natural.height <= 0_px)
        return object_rect(box, natural, css());
    const bool  slice = aspect.find("slice") != std::string::npos;
    const float sx = (box.width / natural.width).value(), sy = (box.height / natural.height).value();
    const float scale = slice ? std::max(sx, sy) : std::min(sx, sy);
    const float w = natural.width.value() * scale, h = natural.height.value() * scale;
    const float room_x = box.width.value() - w, room_y = box.height.value() - h;
    const float dx = aspect.find("xmin") != std::string::npos ? 0.f : aspect.find("xmax") != std::string::npos ? room_x : room_x / 2;
    const float dy = aspect.find("ymin") != std::string::npos ? 0.f : aspect.find("ymax") != std::string::npos ? room_y : room_y / 2;
    return position(box.x + pixel_t(dx), box.y + pixel_t(dy), pixel_t(w), pixel_t(h));
}

void litehtml::el_image::compute_styles(bool recursive)
{
    html_tag::compute_styles(recursive);

    if(!m_src.empty() && !m_loaded)
    {
        m_loaded = true;
        if(!css().get_height().is_predefined() && !css().get_width().is_predefined())
        {
            get_document()->container()->load_image(m_src.c_str(), nullptr, true);
        } else
        {
            get_document()->container()->load_image(m_src.c_str(), nullptr, false);
        }
    }
}

std::string litehtml::el_image::dump_get_name()
{
    return "img src=\"" + m_src + "\"";
}

std::shared_ptr<litehtml::render_item> litehtml::el_image::create_render_item(
    const std::shared_ptr<render_item>& parent_ri)
{
    auto ret = std::make_shared<render_item_image>(shared_from_this());
    ret->parent(parent_ri);
    return ret;
}
