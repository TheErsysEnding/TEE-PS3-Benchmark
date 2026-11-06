// Draws the text atlas of TEE PS3 Benchmark's interface (V0.6): every printable ASCII character of
// Roboto (fonts/, Apache License 2.0) in the sizes the screens use, plus three shapes - a white block
// (plain colour), a circle (round corners, dots, the ring) and a soft glow. On the Mac:
//   swiftc -O tools/make_font_atlas.swift -o /tmp/make_font_atlas && /tmp/make_font_atlas
// Writes data/ui_atlas.bin (one byte of coverage a pixel, the top row first; the app spreads it into
// the alpha of a white A8R8G8B8 texture) and source/ui_atlas.c / .h (where each glyph sits, its offset
// from the pen and its advance, in pixels of a 1920x1080 screen).
import CoreGraphics
import CoreText
import Foundation

let root = URL(fileURLWithPath: #filePath).deletingLastPathComponent().deletingLastPathComponent()

struct Style {
    let name: String, file: String, size: CGFloat, chars: String
}

let ascii = String((32...126).map { Character(UnicodeScalar($0)!) }) + "\u{00B0}"   // '°' stands at 127
let digits = " 0123456789:.%/-+"
let styles = [
    Style(name: "SMALL", file: "Roboto-Regular", size: 22, chars: ascii),
    Style(name: "BODY", file: "Roboto-Regular", size: 28, chars: ascii),
    Style(name: "LABEL", file: "Roboto-Medium", size: 28, chars: ascii),
    Style(name: "SUB", file: "Roboto-Medium", size: 38, chars: ascii),
    Style(name: "NUM", file: "Roboto-Bold", size: 48, chars: ascii),
    Style(name: "TITLE", file: "Roboto-Bold", size: 60, chars: ascii),
    Style(name: "HERO", file: "Roboto-Black", size: 88, chars: ascii),
    Style(name: "BIG", file: "Roboto-Black", size: 120, chars: digits),
]
let pad = 2                 // empty pixels around each bitmap: bilinear filtering never reads a neighbour
let atlasW = 2048

func loadFont(_ file: String, _ size: CGFloat) -> CTFont {
    let url = root.appendingPathComponent("fonts/\(file).ttf")
    let descs = CTFontManagerCreateFontDescriptorsFromURL(url as CFURL) as! [CTFontDescriptor]
    return CTFontCreateWithFontDescriptor(descs[0], size, nil)
}

struct Item {
    let style: Int, code: Int, w: Int, h: Int, ox: Int, oy: Int, adv: CGFloat
    let glyph: CGGlyph?, font: CTFont?, shape: String?
    var x = 0, y = 0
}

var items: [Item] = []
var metrics: [(ascent: CGFloat, descent: CGFloat, line: CGFloat, cap: CGFloat)] = []
for (si, st) in styles.enumerated() {
    let f = loadFont(st.file, st.size)
    metrics.append((CTFontGetAscent(f), CTFontGetDescent(f), CTFontGetAscent(f) + CTFontGetDescent(f) + CTFontGetLeading(f),
                    CTFontGetCapHeight(f)))
    for ch in st.chars {
        let code = ch == "\u{00B0}" ? 127 : Int(ch.asciiValue!)
        var uni = Array(String(ch).utf16), g = [CGGlyph](repeating: 0, count: uni.count)
        CTFontGetGlyphsForCharacters(f, &uni, &g, uni.count)
        var adv = CGSize.zero
        CTFontGetAdvancesForGlyphs(f, .horizontal, g, &adv, 1)
        var b = CTFontGetBoundingRectsForGlyphs(f, .horizontal, g, nil, 1)
        if ch == " " || b.isEmpty {
            b = .zero
        }
        let minX = Int(floor(b.minX)), maxX = Int(ceil(b.maxX)), minY = Int(floor(b.minY)), maxY = Int(ceil(b.maxY))
        let w = ch == " " ? 0 : maxX - minX + 2 * pad, h = ch == " " ? 0 : maxY - minY + 2 * pad
        items.append(Item(style: si, code: code, w: w, h: h, ox: minX - pad, oy: maxY + pad, adv: adv.width,
                          glyph: g[0], font: f, shape: nil))
    }
}
// the shapes: style -1
items.append(Item(style: -1, code: 0, w: 8, h: 8, ox: 0, oy: 0, adv: 0, glyph: nil, font: nil, shape: "WHITE"))
items.append(Item(style: -1, code: 1, w: 100, h: 100, ox: 0, oy: 0, adv: 0, glyph: nil, font: nil, shape: "CIRCLE"))
items.append(Item(style: -1, code: 2, w: 128, h: 128, ox: 0, oy: 0, adv: 0, glyph: nil, font: nil, shape: "GLOW"))
// the icons, white line drawings 72x72 (64 and a margin of 4): the pad's buttons and the menu's pictures
let icons = ["CROSS", "CIRCLE_BTN", "TRIANGLE", "SQUARE", "WAVE", "HASH", "CHIP", "LIST", "BARS", "REPEAT", "FLAME",
             "TABLE", "CHECK", "GPU"]
for (k, n) in icons.enumerated() {
    items.append(Item(style: -1, code: 10 + k, w: 72, h: 72, ox: 0, oy: 0, adv: 0, glyph: nil, font: nil, shape: "ICON_" + n))
}

/// one icon into the 64x64 box r (y up), drawn with round white strokes
func icon(_ ctx: CGContext, _ name: String, _ r: CGRect) {
    let lw: CGFloat = 5.5
    func P(_ x: CGFloat, _ y: CGFloat) -> CGPoint { CGPoint(x: r.minX + x, y: r.maxY - y) }   // y down inside the box
    ctx.saveGState()
    ctx.setStrokeColor(gray: 1, alpha: 1)
    ctx.setFillColor(gray: 1, alpha: 1)
    ctx.setLineWidth(lw)
    ctx.setLineCap(.round)
    ctx.setLineJoin(.round)
    switch name {
    case "CROSS":
        ctx.move(to: P(14, 14)); ctx.addLine(to: P(50, 50)); ctx.move(to: P(50, 14)); ctx.addLine(to: P(14, 50))
        ctx.strokePath()
    case "CIRCLE_BTN":
        ctx.strokeEllipse(in: CGRect(x: r.minX + 12, y: r.minY + 12, width: 40, height: 40))
    case "TRIANGLE":
        ctx.move(to: P(32, 11)); ctx.addLine(to: P(54, 49)); ctx.addLine(to: P(10, 49)); ctx.closePath(); ctx.strokePath()
    case "SQUARE":
        ctx.stroke(CGRect(x: r.minX + 13, y: r.minY + 13, width: 38, height: 38))
    case "WAVE":
        ctx.move(to: P(6, 32))
        for i in 1...52 {
            let t = CGFloat(i) / 52
            ctx.addLine(to: P(6 + 52 * t, 32 - 18 * sin(t * 2 * .pi * 1.5)))
        }
        ctx.strokePath()
    case "HASH":
        ctx.move(to: P(24, 8)); ctx.addLine(to: P(18, 56)); ctx.move(to: P(44, 8)); ctx.addLine(to: P(38, 56))
        ctx.move(to: P(10, 24)); ctx.addLine(to: P(56, 24)); ctx.move(to: P(8, 42)); ctx.addLine(to: P(54, 42))
        ctx.strokePath()
    case "CHIP":
        ctx.addPath(CGPath(roundedRect: CGRect(x: r.minX + 16, y: r.minY + 16, width: 32, height: 32), cornerWidth: 5,
                           cornerHeight: 5, transform: nil))
        ctx.strokePath()
        for k in 0..<3 {
            let o = CGFloat(22 + k * 10)
            ctx.move(to: P(o, 5)); ctx.addLine(to: P(o, 13)); ctx.move(to: P(o, 51)); ctx.addLine(to: P(o, 59))
            ctx.move(to: P(5, o)); ctx.addLine(to: P(13, o)); ctx.move(to: P(51, o)); ctx.addLine(to: P(59, o))
        }
        ctx.strokePath()
    case "LIST":   // the lines first: fillEllipse throws away the path collected so far
        for (k, len) in [44.0, 34.0, 40.0].enumerated() {
            let y = CGFloat(16 + k * 16)
            ctx.move(to: P(22, y)); ctx.addLine(to: P(22 + CGFloat(len), y))
        }
        ctx.strokePath()
        for k in 0..<3 {
            let y = CGFloat(16 + k * 16)
            ctx.fillEllipse(in: CGRect(x: r.minX + 6, y: r.maxY - y - 4, width: 8, height: 8))
        }
    case "BARS":
        for (k, h) in [16.0, 26.0, 38.0, 50.0].enumerated() {
            let x = CGFloat(10 + k * 14)
            ctx.move(to: P(x, 56)); ctx.addLine(to: P(x, 56 - CGFloat(h)))
        }
        ctx.strokePath()
    case "REPEAT":
        ctx.addArc(center: P(32, 32), radius: 22, startAngle: .pi * 0.15, endAngle: .pi * 1.75, clockwise: false)
        ctx.strokePath()
        let tip = P(32 + 22 * cos(.pi * 0.15), 32 - 22 * sin(.pi * 0.15))
        ctx.move(to: CGPoint(x: tip.x - 11, y: tip.y + 3)); ctx.addLine(to: tip); ctx.addLine(to: CGPoint(x: tip.x + 2, y: tip.y + 12))
        ctx.strokePath()
    case "FLAME":
        let f = CGMutablePath()
        f.move(to: P(32, 58))
        f.addCurve(to: P(14, 38), control1: P(20, 58), control2: P(13, 49))
        f.addCurve(to: P(28, 6), control1: P(15, 24), control2: P(26, 18))
        f.addCurve(to: P(40, 24), control1: P(32, 14), control2: P(37, 20))
        f.addCurve(to: P(46, 18), control1: P(43, 22), control2: P(45, 20))
        f.addCurve(to: P(50, 38), control1: P(50, 26), control2: P(52, 32))
        f.addCurve(to: P(32, 58), control1: P(50, 50), control2: P(43, 58))
        ctx.addPath(f)
        ctx.strokePath()
    case "TABLE":
        ctx.addPath(CGPath(roundedRect: CGRect(x: r.minX + 8, y: r.minY + 10, width: 48, height: 44), cornerWidth: 5,
                           cornerHeight: 5, transform: nil))
        ctx.move(to: P(8, 24)); ctx.addLine(to: P(56, 24)); ctx.move(to: P(8, 39)); ctx.addLine(to: P(56, 39))
        ctx.move(to: P(26, 10)); ctx.addLine(to: P(26, 54))
        ctx.strokePath()
    case "CHECK":
        ctx.move(to: P(12, 34)); ctx.addLine(to: P(26, 48)); ctx.addLine(to: P(52, 18)); ctx.strokePath()
    default:   // GPU: a card with a fan
        ctx.addPath(CGPath(roundedRect: CGRect(x: r.minX + 6, y: r.minY + 16, width: 52, height: 32), cornerWidth: 5,
                           cornerHeight: 5, transform: nil))
        ctx.strokePath()
        ctx.strokeEllipse(in: CGRect(x: r.minX + 30, y: r.minY + 22, width: 20, height: 20))
        ctx.move(to: P(14, 48)); ctx.addLine(to: P(14, 56)); ctx.move(to: P(24, 48)); ctx.addLine(to: P(24, 56))
        ctx.strokePath()
    }
    ctx.restoreGState()
}

// shelf packing, the tallest first
var order = items.indices.filter { items[$0].w > 0 }.sorted { items[$0].h > items[$1].h }
var x = 0, y = 0, shelf = 0
for i in order {
    if x + items[i].w > atlasW {
        x = 0
        y += shelf
        shelf = 0
    }
    items[i].x = x
    items[i].y = y
    x += items[i].w
    shelf = max(shelf, items[i].h)
}
let atlasH = (y + shelf + 7) / 8 * 8

// draw: an 8-bit grey context, white on black; CoreGraphics counts y upwards from the bottom row
let ctx = CGContext(data: nil, width: atlasW, height: atlasH, bitsPerComponent: 8, bytesPerRow: atlasW,
                    space: CGColorSpaceCreateDeviceGray(), bitmapInfo: CGImageAlphaInfo.none.rawValue)!
ctx.setFillColor(gray: 0, alpha: 1)
ctx.fill(CGRect(x: 0, y: 0, width: atlasW, height: atlasH))
ctx.setFillColor(gray: 1, alpha: 1)
ctx.setShouldAntialias(true)
ctx.setShouldSmoothFonts(false)
for it in items where it.w > 0 {
    let top = CGFloat(atlasH - it.y)   // the bitmap's top edge, y up
    if let s = it.shape {
        let r = CGRect(x: CGFloat(it.x), y: top - CGFloat(it.h), width: CGFloat(it.w), height: CGFloat(it.h))
        if s.hasPrefix("ICON_") {
            icon(ctx, String(s.dropFirst(5)), r.insetBy(dx: 4, dy: 4))
            continue
        }
        switch s {
        case "WHITE":
            ctx.fill(r)
        case "CIRCLE":
            ctx.fillEllipse(in: r.insetBy(dx: 2, dy: 2))
        default:
            let c = CGPoint(x: r.midX, y: r.midY)
            let grad = CGGradient(colorsSpace: CGColorSpaceCreateDeviceGray(),
                                  colors: [CGColor(gray: 1, alpha: 1), CGColor(gray: 1, alpha: 0)] as CFArray,
                                  locations: [0, 1])!
            ctx.saveGState()
            ctx.clip(to: r)
            ctx.drawRadialGradient(grad, startCenter: c, startRadius: 0, endCenter: c, endRadius: r.width / 2 - 1,
                                   options: [])
            ctx.restoreGState()
        }
        continue
    }
    var g = it.glyph!
    var pos = CGPoint(x: CGFloat(it.x - it.ox), y: top - CGFloat(it.oy))
    CTFontDrawGlyphs(it.font!, &g, &pos, 1, ctx)
}

// coverage, the top row first
let data = ctx.data!.assumingMemoryBound(to: UInt8.self)
let bytes = Data(bytes: data, count: atlasW * atlasH)
try! bytes.write(to: root.appendingPathComponent("data/ui_atlas.bin"))

// the tables
var h = """
/* made by tools/make_font_atlas.swift - do not edit. Roboto (Apache License 2.0) in the interface's
   sizes, in pixels of a 1920x1080 screen; the atlas is data/ui_atlas.bin. */
#ifndef UI_ATLAS_INCLUDED
#define UI_ATLAS_INCLUDED

#define UI_ATLAS_W  \(atlasW)
#define UI_ATLAS_H  \(atlasH)

enum { \(styles.map { "UI_\($0.name)" }.joined(separator: ", ")), UI_STYLES };

typedef struct {
\tunsigned short x, y, w, h;   /* in the atlas; w 0: nothing to draw */
\tshort ox, oy;                /* the bitmap's left edge from the pen, its top above the baseline */
\tfloat adv;                   /* how far the pen moves on */
} ui_glyph;

typedef struct {
\tfloat size, ascent, descent, line, cap;
\tconst ui_glyph *glyph;      /* 96 entries: ' ' (32) to '~' (126) and the degree sign at 127 */
} ui_style;

extern const ui_style UI_STYLE[UI_STYLES];
/* the shapes: x, y, w, h in the atlas */
extern const unsigned short UI_WHITE[4], UI_CIRCLE[4], UI_GLOW[4];
/* the icons, white line drawings in 72x72 (a 64x64 picture and a margin of 4) */
enum { \(icons.map { "UI_ICON_\($0)" }.joined(separator: ", ")), UI_ICONS };
extern const unsigned short UI_ICON[UI_ICONS][4];

#endif

"""
try! h.write(to: root.appendingPathComponent("source/ui_atlas.h"), atomically: true, encoding: .utf8)

var c = "/* made by tools/make_font_atlas.swift - do not edit */\n#include \"ui_atlas.h\"\n\n"
for (si, st) in styles.enumerated() {
    c += "static const ui_glyph G_\(st.name)[96] = {\n"
    for code in 32...127 {
        if let it = items.first(where: { $0.style == si && $0.code == code }) {
            c += "\t{ \(it.x), \(it.y), \(it.w), \(it.h), \(it.ox), \(it.oy), \(String(format: "%.3f", it.adv))f },\n"
        } else {
            c += "\t{ 0, 0, 0, 0, 0, 0, 0.0f },\n"
        }
    }
    c += "};\n\n"
}
c += "const ui_style UI_STYLE[UI_STYLES] = {\n"
for (si, st) in styles.enumerated() {
    let m = metrics[si]
    c += "\t{ \(st.size)f, \(String(format: "%.3f", m.ascent))f, \(String(format: "%.3f", m.descent))f, "
    c += "\(String(format: "%.3f", m.line))f, \(String(format: "%.3f", m.cap))f, G_\(st.name) },\n"
}
c += "};\n\n"
for s in ["WHITE", "CIRCLE", "GLOW"] {
    let it = items.first { $0.shape == s }!
    c += "const unsigned short UI_\(s)[4] = { \(it.x), \(it.y), \(it.w), \(it.h) };\n"
}
c += "const unsigned short UI_ICON[UI_ICONS][4] = {\n"
for n in icons {
    let it = items.first { $0.shape == "ICON_" + n }!
    c += "\t{ \(it.x), \(it.y), \(it.w), \(it.h) },   /* \(n) */\n"
}
c += "};\n"
try! c.write(to: root.appendingPathComponent("source/ui_atlas.c"), atomically: true, encoding: .utf8)
print("atlas \(atlasW)x\(atlasH), \(items.count) items, \(bytes.count) bytes")
