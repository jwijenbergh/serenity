/*
 * Copyright (c) 2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <AK/IntegralMath.h>
#include <AK/Types.h>
#include <LibGfx/AntiAliasingPainter.h>
#include <LibGfx/EdgeFlagPathRasterizer.h>

#if defined(AK_COMPILER_GCC)
#    pragma GCC optimize("O3")
#endif

// This a pretty naive implementation of edge-flag scanline AA.
// The paper lists many possible optimizations, maybe implement one? (FIXME!)
// https://mlab.taik.fi/~kkallio/antialiasing/EdgeFlagAA.pdf
// This currently implements:
//      - The scanline buffer optimization (only allocate one scanline)
// Possible other optimizations according to the paper:
//      - Using fixed point numbers
//      - Edge tracking
//      - Mask tracking
//      - Loop unrolling (compilers might handle this better now, the paper is from 2007)
// Optimizations I think we could add:
//      - Using fast_u32_fills() for runs of solid colors
//      - Clipping the plotted edges earlier

namespace Gfx {

static Vector<Detail::Edge> prepare_edges(ReadonlySpan<Path::SplitLineSegment> lines, unsigned samples_per_pixel, FloatPoint origin)
{
    // FIXME: split_lines() gives similar information, but the form it's in is not that useful (and is const anyway).
    Vector<Detail::Edge> edges;
    edges.ensure_capacity(lines.size());

    for (auto& line : lines) {
        auto p0 = line.from - origin;
        auto p1 = line.to - origin;

        p0.scale_by(1, samples_per_pixel);
        p1.scale_by(1, samples_per_pixel);

        i8 winding = -1;
        if (p0.y() > p1.y()) {
            swap(p0, p1);
        } else {
            winding = 1;
        }

        if (p0.y() == p1.y())
            continue;

        auto dx = p1.x() - p0.x();
        auto dy = p1.y() - p0.y();
        float dxdy = float(dx) / dy;
        float x = p0.x();
        edges.unchecked_append(Detail::Edge {
            x,
            static_cast<int>(p0.y()),
            static_cast<int>(p1.y()),
            dxdy,
            winding,
            nullptr });
    }
    return edges;
}

template<unsigned SamplesPerPixel>
EdgeFlagPathRasterizer<SamplesPerPixel>::EdgeFlagPathRasterizer(IntSize size)
    : m_size(size.width() + 1, size.height() + 1)
{
    m_scanline.resize(m_size.width());
    m_edge_table.resize(m_size.height());
}

template<unsigned SamplesPerPixel>
void EdgeFlagPathRasterizer<SamplesPerPixel>::fill(Painter& painter, Path const& path, Color color, Painter::WindingRule winding_rule, FloatPoint offset)
{
    fill_internal(painter, path, color, winding_rule, offset);
}

template<unsigned SamplesPerPixel>
void EdgeFlagPathRasterizer<SamplesPerPixel>::fill(Painter& painter, Path const& path, PaintStyle const& style, Painter::WindingRule winding_rule, FloatPoint offset)
{
    style.paint(enclosing_int_rect(path.bounding_box()), [&](PaintStyle::SamplerFunction sampler) {
        fill_internal(painter, path, move(sampler), winding_rule, offset);
    });
}

template<unsigned SamplesPerPixel>
void EdgeFlagPathRasterizer<SamplesPerPixel>::fill_internal(Painter& painter, Path const& path, auto color_or_function, Painter::WindingRule winding_rule, FloatPoint offset)
{
    // FIXME: Figure out how painter scaling works here...
    VERIFY(painter.scale() == 1);

    auto bounding_box = enclosing_int_rect(path.bounding_box().translated(offset));
    auto dest_rect = bounding_box.translated(painter.translation());
    auto origin = bounding_box.top_left().to_type<float>() - offset;
    m_blit_origin = dest_rect.top_left();
    m_clip = dest_rect.intersected(painter.clip_rect());

    if (m_clip.is_empty())
        return;

    auto& lines = path.split_lines();
    if (lines.is_empty())
        return;

    auto edges = prepare_edges(lines, SamplesPerPixel, origin);

    int min_scanline = m_size.height();
    int max_scanline = 0;
    for (auto& edge : edges) {
        int start_scanline = edge.min_y / SamplesPerPixel;
        int end_scanline = edge.max_y / SamplesPerPixel;

        // Create a linked-list of edges starting on this scanline:
        edge.next_edge = m_edge_table[start_scanline];
        m_edge_table[start_scanline] = &edge;

        min_scanline = min(min_scanline, start_scanline);
        max_scanline = max(max_scanline, end_scanline);
    }

    Detail::Edge* active_edges = nullptr;

    // FIXME: We could probably clip some of the egde plotting if we know it won't be shown.
    // Though care would have to be taken to ensure the active edges are correct at the first drawn scaline.
    if (winding_rule == Painter::WindingRule::EvenOdd) {
        auto plot_edge = [&](Detail::Edge& edge, int start_subpixel_y, int end_subpixel_y) {
            for (int y = start_subpixel_y; y < end_subpixel_y; y++) {
                int xi = static_cast<int>(edge.x + SubpixelSample::nrooks_subpixel_offsets[y]);
                SampleType sample = 1 << y;
                m_scanline[xi] ^= sample;
                edge.x += edge.dxdy;
            }
        };
        for (int scanline = min_scanline; scanline <= max_scanline; scanline++) {
            active_edges = plot_edges_for_scanline(scanline, plot_edge, active_edges);
            accumulate_even_odd_scanline(painter, scanline, color_or_function);
        }
    } else {
        VERIFY(winding_rule == Painter::WindingRule::Nonzero);
        // Only allocate the winding buffer if needed.
        // NOTE: non-zero fills are a fair bit less efficient. So if you can do an even-odd fill do that :^)
        if (m_windings.is_empty())
            m_windings.resize(m_size.width());

        auto plot_edge = [&](Detail::Edge& edge, int start_subpixel_y, int end_subpixel_y) {
            for (int y = start_subpixel_y; y < end_subpixel_y; y++) {
                int xi = static_cast<int>(edge.x + SubpixelSample::nrooks_subpixel_offsets[y]);
                SampleType sample = 1 << y;
                m_scanline[xi] |= sample;
                m_windings[xi].counts[y] += edge.winding;
                edge.x += edge.dxdy;
            }
        };
        for (int scanline = min_scanline; scanline <= max_scanline; scanline++) {
            active_edges = plot_edges_for_scanline(scanline, plot_edge, active_edges);
            accumulate_non_zero_scanline(painter, scanline, color_or_function);
        }
    }
}

template<unsigned SamplesPerPixel>
Color EdgeFlagPathRasterizer<SamplesPerPixel>::scanline_color(int scanline, int offset, u8 alpha, auto& color_or_function)
{
    using ColorOrFunction = decltype(color_or_function);
    constexpr bool has_constant_color = IsSame<RemoveCVReference<ColorOrFunction>, Color>;
    auto color = [&] {
        if constexpr (has_constant_color) {
            return color_or_function;
        } else {
            return color_or_function({ offset, scanline });
        }
    }();
    return color.with_alpha(color.alpha() * alpha / 255);
}

template<unsigned SamplesPerPixel>
Detail::Edge* EdgeFlagPathRasterizer<SamplesPerPixel>::plot_edges_for_scanline(int scanline, auto plot_edge, Detail::Edge* active_edges)
{
    auto y_subpixel = [](int y) {
        return y & (SamplesPerPixel - 1);
    };

    auto* current_edge = active_edges;
    Detail::Edge* prev_edge = nullptr;

    // First iterate over the edge in the active edge table, these are edges added on earlier scanlines,
    // that have not yet reached their end scanline.
    while (current_edge) {
        int end_scanline = current_edge->max_y / SamplesPerPixel;
        if (scanline == end_scanline) {
            // This edge ends this scanline.
            plot_edge(*current_edge, 0, y_subpixel(current_edge->max_y));
            // Remove this edge from the AET
            current_edge = current_edge->next_edge;
            if (prev_edge)
                prev_edge->next_edge = current_edge;
            else
                active_edges = current_edge;
        } else {
            // This egde sticks around for a few more scanlines.
            plot_edge(*current_edge, 0, SamplesPerPixel);
            prev_edge = current_edge;
            current_edge = current_edge->next_edge;
        }
    }

    // Next, iterate over new edges for this line. If active_edges was null this also becomes the new
    // AET. Edges new will be appended here.
    current_edge = m_edge_table[scanline];
    while (current_edge) {
        int end_scanline = current_edge->max_y / SamplesPerPixel;
        if (scanline == end_scanline) {
            // This edge will end this scanlines (no need to add to AET).
            plot_edge(*current_edge, y_subpixel(current_edge->min_y), y_subpixel(current_edge->max_y));
        } else {
            // This edge will live on for a few more scanlines.
            plot_edge(*current_edge, y_subpixel(current_edge->min_y), SamplesPerPixel);
            // Add this edge to the AET
            if (prev_edge)
                prev_edge->next_edge = current_edge;
            else
                active_edges = current_edge;
            prev_edge = current_edge;
        }
        current_edge = current_edge->next_edge;
    }

    m_edge_table[scanline] = nullptr;
    return active_edges;
}

template<unsigned SamplesPerPixel>
void EdgeFlagPathRasterizer<SamplesPerPixel>::write_pixel(Painter& painter, int scanline, int offset, SampleType sample, auto& color_or_function)
{
    auto dest = IntPoint { offset, scanline } + m_blit_origin;
    if (!m_clip.contains_horizontally(dest.x()))
        return;
    // FIXME: We could detect runs of full coverage and use fast_u32_fills for those rather than many set_pixel() calls.
    auto coverage = SubpixelSample::compute_coverage(sample);
    if (coverage) {
        auto paint_color = scanline_color(scanline, offset, coverage_to_alpha(coverage), color_or_function);
        painter.set_physical_pixel(dest, paint_color, true);
    }
}

template<unsigned SamplesPerPixel>
void EdgeFlagPathRasterizer<SamplesPerPixel>::accumulate_even_odd_scanline(Painter& painter, int scanline, auto& color_or_function)
{
    auto dest_y = m_blit_origin.y() + scanline;
    if (!m_clip.contains_vertically(dest_y)) {
        // FIXME: This memset only really needs to be done on transition from clipped to not clipped,
        // or not at all if we properly clipped egde plotting.
        memset(m_scanline.data(), 0, sizeof(SampleType) * m_scanline.size());
        return;
    }

    SampleType sample = 0;
    for (int x = 0; x < m_size.width(); x += 1) {
        sample ^= m_scanline[x];
        write_pixel(painter, scanline, x, sample, color_or_function);
        m_scanline[x] = 0;
    }
}

template<unsigned SamplesPerPixel>
void EdgeFlagPathRasterizer<SamplesPerPixel>::accumulate_non_zero_scanline(Painter& painter, int scanline, auto& color_or_function)
{
    // NOTE: Same FIXMEs apply from accumulate_even_odd_scanline()
    auto dest_y = m_blit_origin.y() + scanline;
    if (!m_clip.contains_vertically(dest_y)) {
        memset(m_scanline.data(), 0, sizeof(SampleType) * m_scanline.size());
        memset(m_windings.data(), 0, sizeof(WindingCounts) * m_windings.size());
        return;
    }

    SampleType sample = 0;
    WindingCounts sum_winding = {};
    for (int x = 0; x < m_size.width(); x += 1) {
        if (auto edges = m_scanline[x]) {
            // We only need to process the windings when we hit some edges.
            for (auto y_sub = 0u; y_sub < SamplesPerPixel; y_sub++) {
                auto subpixel_bit = 1 << y_sub;
                if (edges & subpixel_bit) {
                    auto winding = m_windings[x].counts[y_sub];
                    auto previous_winding_count = sum_winding.counts[y_sub];
                    sum_winding.counts[y_sub] += winding;
                    // Toggle fill on change to/from zero
                    if ((previous_winding_count == 0 && sum_winding.counts[y_sub] != 0)
                        || (sum_winding.counts[y_sub] == 0 && previous_winding_count != 0)) {
                        sample ^= subpixel_bit;
                    }
                }
            }
        }
        write_pixel(painter, scanline, x, sample, color_or_function);
        m_scanline[x] = 0;
        m_windings[x] = {};
    }
}

static IntSize path_bounds(Gfx::Path const& path)
{
    return enclosing_int_rect(path.bounding_box()).size();
}

// Note: The AntiAliasingPainter and Painter now perform the same antialiasing,
// since it would be harder to turn it off for the standard painter.
// The samples are reduced to 8 for Gfx::Painter though as a "speedy" option.

void Painter::fill_path(Path const& path, Color color, WindingRule winding_rule)
{
    EdgeFlagPathRasterizer<8> rasterizer(path_bounds(path));
    rasterizer.fill(*this, path, color, winding_rule);
}

void Painter::fill_path(Path const& path, PaintStyle const& paint_style, Painter::WindingRule winding_rule)
{
    EdgeFlagPathRasterizer<8> rasterizer(path_bounds(path));
    rasterizer.fill(*this, path, paint_style, winding_rule);
}

void AntiAliasingPainter::fill_path(Path const& path, Color color, Painter::WindingRule winding_rule)
{
    EdgeFlagPathRasterizer<32> rasterizer(path_bounds(path));
    rasterizer.fill(m_underlying_painter, path, color, winding_rule, m_transform.translation());
}

void AntiAliasingPainter::fill_path(Path const& path, PaintStyle const& paint_style, Painter::WindingRule winding_rule)
{
    EdgeFlagPathRasterizer<32> rasterizer(path_bounds(path));
    rasterizer.fill(m_underlying_painter, path, paint_style, winding_rule, m_transform.translation());
}

template class EdgeFlagPathRasterizer<8>;
template class EdgeFlagPathRasterizer<16>;
template class EdgeFlagPathRasterizer<32>;

}
