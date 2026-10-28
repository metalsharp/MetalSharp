//! Library geometry uses GPUI logical points, independent of a display's pixel density.

#[derive(Clone, Copy, Debug)]
pub(crate) struct LibraryLayout {
    pub scale: f32,
    pub compact: bool,
    pub page_padding: f32,
    pub content_width: f32,
    pub header_height: f32,
    pub footer_height: f32,
    pub control_height: f32,
    pub hero_height: f32,
    pub dock_card_width: f32,
    pub dock_card_height: f32,
    pub dock_gap: f32,
    // Total dock space above and below the cards, including their lift clearance.
    pub dock_vertical_padding: f32,
    pub collection_columns: usize,
    pub collection_card_width: f32,
    pub collection_gap: f32,
}

impl LibraryLayout {
    /**
     * Computes library dimensions while preserving readable controls on short windows.
     *
     * @param viewport_width - Viewport width in GPUI logical points.
     * @param viewport_height - Viewport height in GPUI logical points.
     * @returns Shared library metrics; short windows may need vertical scrolling.
     */
    pub(crate) fn for_viewport(viewport_width: f32, viewport_height: f32) -> Self {
        let width = viewport_width.max(0.0);
        let height = viewport_height.max(0.0);
        let scale = (width / 1360.0).sqrt().clamp(1.0, 1.6);
        let compact = width < 1000.0 || height < 720.0;
        let page_padding = if width < 1000.0 { 24.0 } else { 32.0 } * scale;
        let content_width = (width - page_padding * 2.0).clamp(0.0, 2800.0);
        let header_height = (64.0 * scale).min(96.0);
        let footer_height = (64.0 * scale).min(88.0);
        let control_height = (40.0 * scale).clamp(36.0, 52.0);

        // Five slots retain the existing arch; reserve arrows, gaps, padding, and tilted edges.
        let dock_card_width = ((width - 252.0 * scale) / 5.0).clamp(0.0, 176.0 * scale);
        let dock_card_height = dock_card_width * 1.35 + 52.0 * scale;
        let dock_gap = 16.0 * scale;
        let dock_vertical_padding = if compact { 88.0 } else { 128.0 } * scale;
        let body_height = (height - header_height - footer_height).max(0.0);
        let hero_fit = body_height - dock_card_height - dock_vertical_padding;
        let hero_minimum = if compact { 240.0 } else { 300.0 } * scale;
        let hero_maximum = (680.0 * scale).max(width * 0.4);
        let fill_target = if compact {
            0.0
        } else {
            body_height * 0.8 - dock_card_height - dock_vertical_padding
        };
        let hero_height = (height * 0.58)
            .max(fill_target)
            .clamp(hero_minimum, hero_maximum)
            .min(hero_fit.max(hero_minimum));

        let collection_gap = 18.0 * scale;
        let collection_columns = ((content_width + collection_gap)
            / (190.0 * scale + collection_gap))
            .floor()
            .max(1.0) as usize;
        let collection_card_width = ((content_width
            - (collection_columns - 1) as f32 * collection_gap)
            / collection_columns as f32)
            .min(220.0 * scale);

        Self {
            scale,
            compact,
            page_padding,
            content_width,
            header_height,
            footer_height,
            control_height,
            hero_height,
            dock_card_width,
            dock_card_height,
            dock_gap,
            dock_vertical_padding,
            collection_columns,
            collection_card_width,
            collection_gap,
        }
    }

    /**
     * Scales a library typography or spacing token within the viewport's bounded scale.
     *
     * @param base - Token size at the 1360-point baseline.
     * @returns Scaled size in GPUI logical points.
     */
    pub(crate) fn scaled(self, base: f32) -> f32 {
        base * self.scale
    }
}

#[cfg(test)]
mod tests {
    use super::LibraryLayout;

    #[test]
    fn large_viewports_grow_readable_elements_without_unbounded_chrome() {
        let screens = [
            (1360.0, 860.0, 1.0, 6),
            (1920.0, 1080.0, 1.188177, 7),
            (2560.0, 1440.0, 1.372, 8),
            (3840.0, 2160.0, 1.6, 8),
        ];
        let mut previous_card = 0.0;
        let mut previous_text = 0.0;
        for (width, height, expected_scale, columns) in screens {
            let layout = LibraryLayout::for_viewport(width, height);
            assert!((layout.scale - expected_scale).abs() < 0.001);
            assert_eq!(layout.collection_columns, columns);
            assert!(layout.collection_card_width > previous_card);
            assert!(layout.scaled(14.0) > previous_text);
            assert!((36.0..=52.0).contains(&layout.control_height));
            assert!(layout.header_height <= 96.0);
            assert!(layout.footer_height <= 88.0);
            previous_card = layout.collection_card_width;
            previous_text = layout.scaled(14.0);
        }
        let oversized = LibraryLayout::for_viewport(7680.0, 4320.0);
        assert_eq!(oversized.scale, 1.6);
        assert_eq!(oversized.content_width, 2800.0);
    }

    #[test]
    fn collection_rows_and_five_dock_slots_fit_across_supported_widths() {
        for width in (720..=3840).step_by(8) {
            let width = width as f32;
            let layout = LibraryLayout::for_viewport(width, 1080.0);
            let row_width = layout.collection_columns as f32 * layout.collection_card_width
                + (layout.collection_columns - 1) as f32 * layout.collection_gap;
            assert!(layout.content_width + layout.page_padding * 2.0 <= width + 0.001);
            assert!(row_width <= layout.content_width + 0.001);
            assert!(layout.collection_card_width >= 190.0 * layout.scale - 0.001);
            assert!(layout.collection_card_width <= 220.0 * layout.scale + 0.001);
            assert!(layout.dock_card_width * 5.0 + 252.0 * layout.scale <= width + 0.001);
            assert!(layout.dock_card_width <= 176.0 * layout.scale);
            assert!(layout.dock_card_height > layout.dock_card_width);
            assert!(layout.dock_gap >= 16.0);
        }
    }

    #[test]
    fn hero_carousel_and_rails_fill_and_fit_normal_screens() {
        for (width, height) in [
            (1360.0, 860.0),
            (1920.0, 1080.0),
            (2560.0, 1440.0),
            (3840.0, 2160.0),
        ] {
            let layout = LibraryLayout::for_viewport(width, height);
            let total_height = layout.header_height
                + layout.footer_height
                + layout.hero_height
                + layout.dock_card_height
                + layout.dock_vertical_padding;
            assert!(total_height <= height + 0.001, "{width} × {height}");
            let hero_minimum = if layout.compact { 240.0 } else { 300.0 } * layout.scale;
            assert!(layout.hero_height >= hero_minimum);
            assert!(layout.hero_height <= (680.0 * layout.scale).max(width * 0.4));
            if !layout.compact {
                let body_height = height - layout.header_height - layout.footer_height;
                let used_height = total_height - layout.header_height - layout.footer_height;
                assert!(used_height / body_height >= 0.8 - 0.001);
            }
        }
    }

    #[test]
    fn short_windows_scroll_instead_of_overlapping_the_primary_action() {
        for width in [720.0, 740.0, 1360.0] {
            let layout = LibraryLayout::for_viewport(width, 540.0);
            assert!(layout.compact);
            assert_eq!(layout.hero_height, 240.0 * layout.scale);
            let total_height = layout.header_height
                + layout.footer_height
                + layout.hero_height
                + layout.dock_card_height
                + layout.dock_vertical_padding;
            assert!(total_height > 540.0);
        }
        for (width, height) in [(1360.0, 720.0), (1920.0, 760.0), (3840.0, 1080.0)] {
            let layout = LibraryLayout::for_viewport(width, height);
            assert!(!layout.compact);
            assert_eq!(layout.hero_height, 300.0 * layout.scale);
            let total_height = layout.header_height
                + layout.footer_height
                + layout.hero_height
                + layout.dock_card_height
                + layout.dock_vertical_padding;
            assert!(total_height > height);
        }
        let layout = LibraryLayout::for_viewport(1360.0, 540.0);
        assert_eq!(
            layout.scale,
            LibraryLayout::for_viewport(1360.0, 2160.0).scale
        );
        assert!(LibraryLayout::for_viewport(999.0, 860.0).compact);
        assert!(LibraryLayout::for_viewport(1000.0, 719.0).compact);
        assert!(!LibraryLayout::for_viewport(1000.0, 720.0).compact);
    }
}
