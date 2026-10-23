//! Release notes as GPUI elements (the update's "What's new"): headings, paragraphs, emphasis, inline code,
//! links (opened in the browser), lists, quotes, code blocks, rules and tables. HTML and images are left out,
//! an image's alternative text kept.

use std::ops::Range;

use gpui::{
    AnyElement, ElementId, FontStyle, FontWeight, HighlightStyle, InteractiveText, SharedString,
    StrikethroughStyle, StyledText, UnderlineStyle, div, prelude::*, px, rgb, rgba,
};
use pulldown_cmark::{Event, HeadingLevel, Options, Parser, Tag, TagEnd};

const TEXT: u32 = 0xe6e8e7;
const MUTED: u32 = 0x9aa09e;
const LINK: u32 = 0x5fb7e8;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Span {
    Bold,
    Italic,
    Code,
    Strike,
    Link,
}

/// A block's text and the spans styled in it, with the links among them.
#[derive(Default, Debug)]
struct Inline {
    text: String,
    spans: Vec<(Range<usize>, Span)>,
    links: Vec<(Range<usize>, String)>,
}

#[derive(Debug)]
enum Block {
    Heading(u8, Inline),
    Paragraph(Inline),
    Item {
        depth: usize,
        marker: String,
        inline: Inline,
    },
    Quote(Inline),
    Code(String),
    Rule,
    Row {
        header: bool,
        cells: Vec<Inline>,
    },
}

/// The blocks of `source`, as pulldown-cmark reads GitHub's markdown.
fn parse(source: &str) -> Vec<Block> {
    let options =
        Options::ENABLE_TABLES | Options::ENABLE_STRIKETHROUGH | Options::ENABLE_TASKLISTS;
    let mut blocks = Vec::new();
    // the block being filled, and what it will be
    let mut current: Option<Block> = None;
    let mut lists: Vec<Option<u64>> = Vec::new();
    let mut quotes = 0usize;
    let mut open: Vec<(Span, usize, Option<String>)> = Vec::new();
    let mut code: Option<String> = None;
    let mut row: Option<(bool, Vec<Inline>)> = None;
    let mut cell: Option<Inline> = None;
    let mut in_head = false;

    fn inline_of(block: &mut Block) -> Option<&mut Inline> {
        match block {
            Block::Heading(_, inline) | Block::Paragraph(inline) | Block::Quote(inline) => {
                Some(inline)
            }
            Block::Item { inline, .. } => Some(inline),
            _ => None,
        }
    }
    fn flush(current: &mut Option<Block>, blocks: &mut Vec<Block>) {
        if let Some(mut block) = current.take() {
            let keep = inline_of(&mut block).is_none_or(|inline| !inline.text.trim().is_empty());
            if keep {
                blocks.push(block);
            }
        }
    }

    for event in Parser::new_ext(source, options) {
        // where inline text goes: a table cell, else the current block (a paragraph when there is none)
        macro_rules! target {
            () => {{
                if let Some(cell) = cell.as_mut() {
                    cell
                } else {
                    if current.as_mut().and_then(inline_of).is_none() {
                        flush(&mut current, &mut blocks);
                        current = Some(if quotes > 0 {
                            Block::Quote(Inline::default())
                        } else {
                            Block::Paragraph(Inline::default())
                        });
                    }
                    current.as_mut().and_then(inline_of).unwrap()
                }
            }};
        }
        match event {
            Event::Start(Tag::Heading { level, .. }) => {
                flush(&mut current, &mut blocks);
                let level = match level {
                    HeadingLevel::H1 => 1,
                    HeadingLevel::H2 => 2,
                    HeadingLevel::H3 => 3,
                    _ => 4,
                };
                current = Some(Block::Heading(level, Inline::default()));
            }
            Event::End(TagEnd::Heading(_)) => flush(&mut current, &mut blocks),
            // a list item's paragraphs are its own text; elsewhere a paragraph is a block
            Event::Start(Tag::Paragraph) => {
                let in_item = matches!(current, Some(Block::Item { .. }));
                if !in_item {
                    flush(&mut current, &mut blocks);
                    current = Some(if quotes > 0 {
                        Block::Quote(Inline::default())
                    } else {
                        Block::Paragraph(Inline::default())
                    });
                } else if let Some(inline) = current.as_mut().and_then(inline_of) {
                    if !inline.text.is_empty() {
                        inline.text.push('\n');
                    }
                }
            }
            Event::End(TagEnd::Paragraph) => {
                if !matches!(current, Some(Block::Item { .. })) {
                    flush(&mut current, &mut blocks);
                }
            }
            Event::Start(Tag::BlockQuote(_)) => {
                flush(&mut current, &mut blocks);
                quotes += 1;
            }
            Event::End(TagEnd::BlockQuote(_)) => {
                flush(&mut current, &mut blocks);
                quotes = quotes.saturating_sub(1);
            }
            Event::Start(Tag::List(start)) => {
                flush(&mut current, &mut blocks);
                lists.push(start);
            }
            Event::End(TagEnd::List(_)) => {
                flush(&mut current, &mut blocks);
                lists.pop();
            }
            Event::Start(Tag::Item) => {
                flush(&mut current, &mut blocks);
                let marker = match lists.last_mut() {
                    Some(Some(n)) => {
                        let marker = format!("{n}.");
                        *n += 1;
                        marker
                    }
                    _ => "•".to_owned(),
                };
                current = Some(Block::Item {
                    depth: lists.len().saturating_sub(1),
                    marker,
                    inline: Inline::default(),
                });
            }
            Event::End(TagEnd::Item) => flush(&mut current, &mut blocks),
            Event::Start(Tag::CodeBlock(_)) => {
                flush(&mut current, &mut blocks);
                code = Some(String::new());
            }
            Event::End(TagEnd::CodeBlock) => {
                if let Some(text) = code.take() {
                    blocks.push(Block::Code(text.trim_end_matches('\n').to_owned()));
                }
            }
            Event::Rule => {
                flush(&mut current, &mut blocks);
                blocks.push(Block::Rule);
            }
            Event::Start(Tag::Table(_)) => flush(&mut current, &mut blocks),
            Event::Start(Tag::TableHead) => {
                in_head = true;
                row = Some((true, Vec::new()));
            }
            Event::Start(Tag::TableRow) => row = Some((in_head, Vec::new())),
            Event::End(TagEnd::TableHead) | Event::End(TagEnd::TableRow) => {
                in_head = false;
                if let Some((header, cells)) = row.take() {
                    blocks.push(Block::Row { header, cells });
                }
            }
            Event::Start(Tag::TableCell) => cell = Some(Inline::default()),
            Event::End(TagEnd::TableCell) => {
                if let (Some(done), Some((_, cells))) = (cell.take(), row.as_mut()) {
                    cells.push(done);
                }
            }
            Event::Start(Tag::Emphasis) => open.push((Span::Italic, target!().text.len(), None)),
            Event::Start(Tag::Strong) => open.push((Span::Bold, target!().text.len(), None)),
            Event::Start(Tag::Strikethrough) => {
                open.push((Span::Strike, target!().text.len(), None))
            }
            Event::Start(Tag::Link { dest_url, .. })
            | Event::Start(Tag::Image { dest_url, .. }) => {
                open.push((Span::Link, target!().text.len(), Some(dest_url.to_string())))
            }
            Event::End(
                TagEnd::Emphasis
                | TagEnd::Strong
                | TagEnd::Strikethrough
                | TagEnd::Link
                | TagEnd::Image,
            ) => {
                if let Some((span, start, url)) = open.pop() {
                    let inline = target!();
                    let range = start..inline.text.len();
                    if !range.is_empty() {
                        if let Some(url) = url {
                            inline.links.push((range.clone(), url));
                        }
                        inline.spans.push((range, span));
                    }
                }
            }
            Event::Text(text) => {
                if let Some(code) = code.as_mut() {
                    code.push_str(&text);
                } else {
                    target!().text.push_str(&text);
                }
            }
            Event::Code(text) => {
                let inline = target!();
                let start = inline.text.len();
                inline.text.push_str(&text);
                let end = inline.text.len();
                inline.spans.push((start..end, Span::Code));
            }
            Event::SoftBreak => target!().text.push(' '),
            Event::HardBreak => target!().text.push('\n'),
            Event::TaskListMarker(done) => {
                target!().text.push_str(if done { "☑ " } else { "☐ " })
            }
            _ => {}
        }
    }
    flush(&mut current, &mut blocks);
    blocks
}

fn style(span: Span) -> HighlightStyle {
    match span {
        Span::Bold => HighlightStyle {
            font_weight: Some(FontWeight::BOLD),
            ..Default::default()
        },
        Span::Italic => HighlightStyle {
            font_style: Some(FontStyle::Italic),
            ..Default::default()
        },
        Span::Code => HighlightStyle {
            background_color: Some(rgba(0xffffff1a).into()),
            color: Some(rgb(0xf0d9a8).into()),
            ..Default::default()
        },
        Span::Strike => HighlightStyle {
            strikethrough: Some(StrikethroughStyle {
                thickness: px(1.0),
                color: None,
            }),
            ..Default::default()
        },
        Span::Link => HighlightStyle {
            color: Some(rgb(LINK).into()),
            underline: Some(UnderlineStyle {
                thickness: px(1.0),
                color: Some(rgb(LINK).into()),
                wavy: false,
            }),
            ..Default::default()
        },
    }
}

/// The spans as GPUI wants them, in order and not overlapping: each piece between two span edges gets the
/// styles of every span over it.
fn highlights(inline: &Inline) -> Vec<(Range<usize>, HighlightStyle)> {
    let mut edges: Vec<usize> = inline
        .spans
        .iter()
        .flat_map(|(r, _)| [r.start, r.end])
        .collect();
    edges.sort_unstable();
    edges.dedup();
    let mut pieces = Vec::new();
    for pair in edges.windows(2) {
        let piece = pair[0]..pair[1];
        let mut merged: Option<HighlightStyle> = None;
        for (range, span) in &inline.spans {
            if range.start <= piece.start && piece.end <= range.end {
                let next = style(*span);
                merged = Some(match merged {
                    Some(style) => style.highlight(next),
                    None => next,
                });
            }
        }
        if let Some(style) = merged {
            pieces.push((piece, style));
        }
    }
    pieces
}

fn inline_element(id: ElementId, inline: Inline) -> AnyElement {
    let ranges = highlights(&inline);
    let text = StyledText::new(SharedString::from(inline.text)).with_highlights(ranges);
    if inline.links.is_empty() {
        return text.into_any_element();
    }
    let (ranges, urls): (Vec<_>, Vec<_>) = inline.links.into_iter().unzip();
    InteractiveText::new(id, text)
        .on_click(ranges, move |index, _, cx| {
            if let Some(url) = urls
                .get(index)
                .filter(|url| url.starts_with("https://") || url.starts_with("http://"))
            {
                cx.open_url(url);
            }
        })
        .into_any_element()
}

/// `source` as elements, their ids made from `id`.
pub fn render(id: &str, source: &str) -> Vec<AnyElement> {
    parse(source)
        .into_iter()
        .enumerate()
        .map(|(index, block)| {
            let id = |part: usize| ElementId::Name(format!("{id}-{index}-{part}").into());
            match block {
                Block::Heading(level, inline) => div()
                    .pt(px(if index == 0 { 0.0 } else { 6.0 }))
                    .text_size(px(match level {
                        1 => 18.0,
                        2 => 16.0,
                        3 => 14.5,
                        _ => 13.5,
                    }))
                    .font_weight(FontWeight::BOLD)
                    .text_color(rgb(TEXT))
                    .child(inline_element(id(0), inline))
                    .into_any_element(),
                Block::Paragraph(inline) => div()
                    .text_size(px(13.0))
                    .line_height(px(19.0))
                    .text_color(rgb(TEXT))
                    .child(inline_element(id(0), inline))
                    .into_any_element(),
                Block::Item {
                    depth,
                    marker,
                    inline,
                } => div()
                    .flex()
                    .gap(px(7.0))
                    .pl(px(depth as f32 * 16.0))
                    .text_size(px(13.0))
                    .line_height(px(19.0))
                    .text_color(rgb(TEXT))
                    .child(
                        div()
                            .flex_none()
                            .min_w(px(12.0))
                            .text_color(rgb(MUTED))
                            .child(marker),
                    )
                    .child(
                        div()
                            .flex_1()
                            .min_w_0()
                            .child(inline_element(id(0), inline)),
                    )
                    .into_any_element(),
                Block::Quote(inline) => div()
                    .pl(px(10.0))
                    .border_l_2()
                    .border_color(rgba(0xffffff2e))
                    .text_size(px(13.0))
                    .line_height(px(19.0))
                    .text_color(rgb(MUTED))
                    .child(inline_element(id(0), inline))
                    .into_any_element(),
                Block::Code(text) => div()
                    .p(px(10.0))
                    .rounded(px(6.0))
                    .bg(rgba(0xffffff0d))
                    .font_family("Menlo")
                    .text_size(px(12.0))
                    .line_height(px(17.0))
                    .text_color(rgb(TEXT))
                    .child(text)
                    .into_any_element(),
                Block::Rule => div()
                    .h(px(1.0))
                    .my(px(4.0))
                    .bg(rgba(0xffffff1f))
                    .into_any_element(),
                Block::Row { header, cells } => div()
                    .flex()
                    .gap(px(12.0))
                    .py(px(4.0))
                    .border_b_1()
                    .border_color(rgba(0xffffff14))
                    .text_size(px(12.5))
                    .text_color(rgb(TEXT))
                    .when(header, |row| row.font_weight(FontWeight::SEMIBOLD))
                    .children(cells.into_iter().enumerate().map(|(column, cell)| {
                        div()
                            .flex_1()
                            .min_w_0()
                            .child(inline_element(id(column), cell))
                    }))
                    .into_any_element(),
            }
        })
        .collect()
}

#[cfg(test)]
mod tests {
    use super::*;

    fn texts(source: &str) -> Vec<String> {
        parse(source)
            .into_iter()
            .map(|block| match block {
                Block::Heading(level, inline) => format!("h{level}:{}", inline.text),
                Block::Paragraph(inline) => format!("p:{}", inline.text),
                Block::Item {
                    depth,
                    marker,
                    inline,
                } => format!("li{depth}{marker}{}", inline.text),
                Block::Quote(inline) => format!("q:{}", inline.text),
                Block::Code(text) => format!("code:{text}"),
                Block::Rule => "hr".into(),
                Block::Row { header, cells } => format!(
                    "{}:{}",
                    if header { "th" } else { "td" },
                    cells
                        .iter()
                        .map(|c| c.text.as_str())
                        .collect::<Vec<_>>()
                        .join("|")
                ),
            })
            .collect()
    }

    #[test]
    fn blocks() {
        let notes = "# MetalSharp 1.0.0\n\nThe **Arm64** build, with *FEX*.\n\n## Changes\n\n- one\n- two\n  - nested\n\n1. first\n2. second\n\n> note\n\n```\ncode\n```\n\n---\n\n| a | b |\n|---|---|\n| 1 | 2 |\n";
        assert_eq!(
            texts(notes),
            [
                "h1:MetalSharp 1.0.0",
                "p:The Arm64 build, with FEX.",
                "h2:Changes",
                "li0•one",
                "li0•two",
                "li1•nested",
                "li01.first",
                "li02.second",
                "q:note",
                "code:code",
                "hr",
                "th:a|b",
                "td:1|2",
            ]
        );
    }

    #[test]
    fn spans_and_links() {
        let blocks = parse("A **bold [link](https://example.com)** and `code`.");
        let Block::Paragraph(inline) = &blocks[0] else {
            panic!("{blocks:?}")
        };
        assert_eq!(inline.text, "A bold link and code.");
        assert_eq!(inline.links, [(7..11, "https://example.com".to_owned())]);
        // the link inside the bold text: bold, then bold and link, without overlapping ranges
        let pieces = highlights(inline);
        let ranges: Vec<_> = pieces.iter().map(|(range, _)| range.clone()).collect();
        assert_eq!(ranges, [2..7, 7..11, 16..20]);
        assert_eq!(pieces[1].1.font_weight, Some(FontWeight::BOLD));
        assert!(pieces[1].1.underline.is_some());
    }

    #[test]
    fn loose_items_keep_their_paragraphs() {
        assert_eq!(
            texts("- one\n\n  more\n\n- two\n"),
            ["li0•one\nmore", "li0•two"]
        );
    }
}
