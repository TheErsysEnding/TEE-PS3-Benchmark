// Draws the XMB pictures of TEE PS3 Benchmark: assets/ICON0.PNG (320x176, the app icon) and
// pkgfiles/PIC1.PNG (1920x1080, the background while the app is selected). On the Mac:
//   swiftc -O tools/make_xmb_images.swift -o /tmp/make_xmb_images && /tmp/make_xmb_images
//
// The look (since V0.5 BETA): near-black blue, the system font (SF Pro), and a gauge - a benchmark's needle near
// the top of its scale, the arc running from the app's orange to a hot red-orange for the stress test's
// heat. PIC1 puts the measured value in it (98 % of the Cell's peak, V8.4's 15-minute run) above a
// faint performance curve; the XMB's menu covers the left and the middle, so the motif sits right.
// No version number on either picture (V0.7: it would need updating with every release): the app shows its own.
// The PNGs keep only IHDR, IDAT and IEND: plain 8-bit RGB, nothing extra for the XMB.
import AppKit
import CoreGraphics
import Foundation
import ImageIO
import UniformTypeIdentifiers

let root = URL(fileURLWithPath: #filePath).deletingLastPathComponent().deletingLastPathComponent()
let space = CGColorSpace(name: CGColorSpace.sRGB)!

let orange: UInt32 = 0xFF9A3C       // C_ACCENT in source/main.c
let hot: UInt32 = 0xFF4A2B          // the arc's hot end
let ice: UInt32 = 0x9FB6E8          // "TEE" and secondary text
let track: UInt32 = 0x1F2735

func rgb(_ hex: UInt32, _ alpha: CGFloat = 1) -> CGColor {
    CGColor(srgbRed: CGFloat(hex >> 16 & 0xFF) / 255, green: CGFloat(hex >> 8 & 0xFF) / 255,
            blue: CGFloat(hex & 0xFF) / 255, alpha: alpha)
}

func mix(_ a: UInt32, _ b: UInt32, _ t: CGFloat) -> CGColor {
    func ch(_ v: UInt32, _ s: UInt32) -> CGFloat { CGFloat(v >> s & 0xFF) / 255 }
    return CGColor(srgbRed: ch(a, 16) + (ch(b, 16) - ch(a, 16)) * t, green: ch(a, 8) + (ch(b, 8) - ch(a, 8)) * t,
                   blue: ch(a, 0) + (ch(b, 0) - ch(a, 0)) * t, alpha: 1)
}

/// Drops every chunk but IHDR, IDAT and IEND (ImageIO adds sRGB and eXIf); returns what is left.
func plainPNG(_ url: URL) -> String {
    let d = [UInt8](try! Data(contentsOf: url))
    var out = Array(d[0..<8]), i = 8, kept: [String] = []
    while i + 12 <= d.count {
        let n = Int(d[i]) << 24 | Int(d[i + 1]) << 16 | Int(d[i + 2]) << 8 | Int(d[i + 3])
        let type = String(decoding: d[(i + 4)..<(i + 8)], as: UTF8.self)
        if type == "IHDR" || type == "IDAT" || type == "IEND" {
            out += d[i..<(i + 12 + n)]
            if kept.last != type { kept.append(type) }
        }
        i += 12 + n
    }
    try! Data(out).write(to: url)
    return kept.joined(separator: " ")
}

/// A context with y going down (like the screen), drawn into by body, written as an RGB PNG.
func render(_ width: Int, _ height: Int, to path: String, _ body: (CGContext) -> Void) {
    let ctx = CGContext(data: nil, width: width, height: height, bitsPerComponent: 8, bytesPerRow: 0,
                        space: space, bitmapInfo: CGImageAlphaInfo.noneSkipLast.rawValue)!
    ctx.translateBy(x: 0, y: CGFloat(height))
    ctx.scaleBy(x: 1, y: -1)
    NSGraphicsContext.saveGraphicsState()
    NSGraphicsContext.current = NSGraphicsContext(cgContext: ctx, flipped: true)
    body(ctx)
    NSGraphicsContext.restoreGraphicsState()
    let url = root.appendingPathComponent(path)
    try? FileManager.default.createDirectory(at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
    let dest = CGImageDestinationCreateWithURL(url as CFURL, UTType.png.identifier as CFString, 1, nil)!
    CGImageDestinationAddImage(dest, ctx.makeImage()!, nil)
    CGImageDestinationFinalize(dest)
    let chunks = plainPNG(url)
    let size = ((try? FileManager.default.attributesOfItem(atPath: url.path))?[.size] as? NSNumber)?.intValue ?? 0
    print("\(path): \(width)x\(height), \(size) Bytes, \(chunks)")
}

func linear(_ ctx: CGContext, _ colours: [CGColor], from a: CGPoint, to b: CGPoint) {
    let g = CGGradient(colorsSpace: space, colors: colours as CFArray, locations: nil)!
    ctx.drawLinearGradient(g, start: a, end: b, options: [.drawsBeforeStartLocation, .drawsAfterEndLocation])
}

func glow(_ ctx: CGContext, _ colour: UInt32, at c: CGPoint, radius: CGFloat, alpha: CGFloat) {
    let g = CGGradient(colorsSpace: space, colors: [rgb(colour, alpha), rgb(colour, 0)] as CFArray, locations: nil)!
    ctx.drawRadialGradient(g, startCenter: c, startRadius: 0, endCenter: c, endRadius: radius, options: [])
}

/// the system font, SF Pro
func font(_ size: CGFloat, _ weight: NSFont.Weight) -> NSFont {
    NSFont.systemFont(ofSize: size, weight: weight)
}

/// text with its top-left corner at p; returns its size
@discardableResult
func text(_ s: String, at p: CGPoint, size: CGFloat, weight: NSFont.Weight, colour: CGColor, kern: CGFloat = 0) -> CGSize {
    let str = NSAttributedString(string: s, attributes: [.font: font(size, weight),
                                                         .foregroundColor: NSColor(cgColor: colour)!, .kern: kern])
    str.draw(at: NSPoint(x: p.x, y: p.y))
    return str.size()
}

func textSize(_ s: String, size: CGFloat, weight: NSFont.Weight, kern: CGFloat = 0) -> CGSize {
    NSAttributedString(string: s, attributes: [.font: font(size, weight), .kern: kern]).size()
}

/// centred on x, top at y (the kern after the last letter left out)
func centred(_ s: String, x: CGFloat, y: CGFloat, size: CGFloat, weight: NSFont.Weight, colour: CGColor,
             kern: CGFloat = 0) {
    let w = textSize(s, size: size, weight: weight, kern: kern).width - kern
    text(s, at: CGPoint(x: x - w / 2, y: y), size: size, weight: weight, colour: colour, kern: kern)
}

/// a rounded label: fill, then text centred in it
func pill(_ ctx: CGContext, _ label: String, at p: CGPoint, size: CGFloat, fill: CGColor, ink: CGColor,
          kern: CGFloat = 0) {
    let ts = textSize(label, size: size, weight: .bold, kern: kern)
    let r = CGRect(x: p.x, y: p.y, width: ts.width - kern + size * 1.2, height: ts.height + size * 0.2)
    ctx.setFillColor(fill)
    ctx.addPath(CGPath(roundedRect: r, cornerWidth: r.height / 2, cornerHeight: r.height / 2, transform: nil))
    ctx.fillPath()
    text(label, at: CGPoint(x: r.minX + size * 0.6, y: r.minY + size * 0.1), size: size, weight: .bold, colour: ink,
         kern: kern)
}

// ---- the gauge -------------------------------------------------------------------------------------
// 270 degrees from bottom left over the top to bottom right (y goes down, so angles turn clockwise);
// value 0...1 is how far the coloured arc and the needle go.

func angle(_ t: CGFloat) -> CGFloat {
    (135 + 270 * t) * .pi / 180
}

func point(_ c: CGPoint, _ r: CGFloat, _ t: CGFloat) -> CGPoint {
    CGPoint(x: c.x + r * cos(angle(t)), y: c.y + r * sin(angle(t)))
}

func gauge(_ ctx: CGContext, centre c: CGPoint, radius r: CGFloat, width w: CGFloat, value: CGFloat, ticks: Bool) {
    ctx.setLineCap(.round)
    // the track
    ctx.setStrokeColor(rgb(track))
    ctx.setLineWidth(w)
    ctx.addArc(center: c, radius: r, startAngle: angle(0), endAngle: angle(1), clockwise: false)
    ctx.strokePath()
    // a soft glow under the coloured arc
    ctx.saveGState()
    ctx.setShadow(offset: .zero, blur: w * 1.4, color: rgb(hot, 0.55))
    let steps = 180
    ctx.setLineCap(.butt)
    for i in 0..<steps {
        let a = value * CGFloat(i) / CGFloat(steps), b = value * CGFloat(i + 1) / CGFloat(steps)
        ctx.setStrokeColor(mix(orange, hot, a / value))
        ctx.setLineWidth(w)
        ctx.addArc(center: c, radius: r, startAngle: angle(a), endAngle: angle(b) + 0.004, clockwise: false)
        ctx.strokePath()
    }
    ctx.restoreGState()
    // round ends on the coloured arc
    for (t, col) in [(CGFloat(0), rgb(orange)), (value, rgb(hot))] {
        let p = point(c, r, t)
        ctx.setFillColor(col)
        ctx.fillEllipse(in: CGRect(x: p.x - w / 2, y: p.y - w / 2, width: w, height: w))
    }
    // ticks inside the arc
    if ticks {
        for i in 0...20 {
            let t = CGFloat(i) / 20, major = i % 5 == 0
            let a = point(c, r - w * 1.05, t), b = point(c, r - w * (major ? 1.75 : 1.4), t)
            ctx.setStrokeColor(rgb(t <= value ? 0xFFD9B8 : 0x5A6577, major ? 0.95 : 0.6))
            ctx.setLineWidth(max(1, w * (major ? 0.16 : 0.09)))
            ctx.setLineCap(.round)
            ctx.move(to: a)
            ctx.addLine(to: b)
            ctx.strokePath()
        }
    }
    // the needle: a slim tapered blade from the hub to just inside the arc
    let tip = point(c, r - w * (ticks ? 1.9 : 1.2), value), dir = angle(value) + .pi / 2
    let base = w * 0.42, back = point(c, w * 0.9, value + 0.5)
    ctx.saveGState()
    ctx.setShadow(offset: .zero, blur: w * 0.8, color: rgb(0xFFFFFF, 0.35))
    ctx.setFillColor(rgb(0xFFFFFF))
    ctx.move(to: tip)
    ctx.addLine(to: CGPoint(x: c.x + base * cos(dir), y: c.y + base * sin(dir)))
    ctx.addLine(to: back)
    ctx.addLine(to: CGPoint(x: c.x - base * cos(dir), y: c.y - base * sin(dir)))
    ctx.closePath()
    ctx.fillPath()
    ctx.restoreGState()
    // the hub
    ctx.setFillColor(rgb(0x0E131C))
    ctx.fillEllipse(in: CGRect(x: c.x - w * 0.75, y: c.y - w * 0.75, width: w * 1.5, height: w * 1.5))
    ctx.setStrokeColor(rgb(orange))
    ctx.setLineWidth(max(1.5, w * 0.22))
    ctx.strokeEllipse(in: CGRect(x: c.x - w * 0.62, y: c.y - w * 0.62, width: w * 1.24, height: w * 1.24))
}

/// a measured-looking performance curve with a glow, left to right through rect r
func curve(_ ctx: CGContext, in r: CGRect, colour: UInt32, width: CGFloat) {
    let path = CGMutablePath()
    let n = 160
    for i in 0...n {
        let t = CGFloat(i) / CGFloat(n)
        // a ramp up to a high plateau with small wiggles, like the stress test's history
        let ramp = 1 - exp(-t * 9)
        let wig = 0.035 * sin(t * 47) + 0.02 * sin(t * 113 + 1.3)
        let v = 0.15 + 0.7 * ramp + wig * ramp
        let p = CGPoint(x: r.minX + t * r.width, y: r.maxY - v * r.height)
        i == 0 ? path.move(to: p) : path.addLine(to: p)
    }
    ctx.saveGState()
    ctx.setShadow(offset: .zero, blur: width * 6, color: rgb(colour, 0.7))
    ctx.setStrokeColor(rgb(colour, 0.85))
    ctx.setLineWidth(width)
    ctx.setLineJoin(.round)
    ctx.addPath(path)
    ctx.strokePath()
    ctx.restoreGState()
}

// ---- ICON0.PNG: 320x176 ---------------------------------------------------------------------------
render(320, 176, to: "assets/ICON0.PNG") { ctx in
    linear(ctx, [rgb(0x0A0E16), rgb(0x141B29)], from: CGPoint(x: 0, y: 0), to: CGPoint(x: 320, y: 176))
    glow(ctx, hot, at: CGPoint(x: 84, y: 92), radius: 120, alpha: 0.22)
    glow(ctx, 0x3E6BFF, at: CGPoint(x: 300, y: 10), radius: 120, alpha: 0.16)
    // a fine frame, like a card
    ctx.setStrokeColor(rgb(0xFFFFFF, 0.08))
    ctx.setLineWidth(1)
    ctx.addPath(CGPath(roundedRect: CGRect(x: 6.5, y: 6.5, width: 307, height: 163), cornerWidth: 16, cornerHeight: 16,
                       transform: nil))
    ctx.strokePath()

    gauge(ctx, centre: CGPoint(x: 84, y: 94), radius: 54, width: 12, value: 0.86, ticks: false)

    let x: CGFloat = 160
    text("TEE", at: CGPoint(x: x + 1, y: 30), size: 19, weight: .semibold, colour: rgb(ice), kern: 6)
    text("PS3", at: CGPoint(x: x - 2, y: 46), size: 60, weight: .heavy, colour: rgb(0xFFFFFF), kern: -1)
    text("BENCHMARK", at: CGPoint(x: x + 1, y: 117), size: 18, weight: .bold, colour: rgb(orange), kern: 2.6)
}

// ---- PIC1.PNG: 1920x1080 --------------------------------------------------------------------------
render(1920, 1080, to: "pkgfiles/PIC1.PNG") { ctx in
    linear(ctx, [rgb(0x070A10), rgb(0x0E1420), rgb(0x161226)], from: CGPoint(x: 0, y: 0), to: CGPoint(x: 1920, y: 1080))
    glow(ctx, hot, at: CGPoint(x: 1400, y: 430), radius: 700, alpha: 0.16)
    glow(ctx, 0x3E6BFF, at: CGPoint(x: 1800, y: 60), radius: 700, alpha: 0.14)
    glow(ctx, orange, at: CGPoint(x: 260, y: 1060), radius: 560, alpha: 0.08)

    // a faint measuring grid
    ctx.setStrokeColor(rgb(0xFFFFFF, 0.035))
    ctx.setLineWidth(1)
    var g: CGFloat = 0
    while g <= 1920 {
        ctx.move(to: CGPoint(x: g, y: 0))
        ctx.addLine(to: CGPoint(x: g, y: 1080))
        g += 80
    }
    g = 0
    while g <= 1080 {
        ctx.move(to: CGPoint(x: 0, y: g))
        ctx.addLine(to: CGPoint(x: 1920, y: g))
        g += 80
    }
    ctx.strokePath()

    curve(ctx, in: CGRect(x: 0, y: 700, width: 1920, height: 300), colour: orange, width: 4)

    let c = CGPoint(x: 1400, y: 420)
    gauge(ctx, centre: c, radius: 280, width: 42, value: 0.98, ticks: true)
    // the value in the upper half: the needle points down to the right and never gets there
    centred("98 %", x: c.x, y: c.y - 190, size: 96, weight: .heavy, colour: rgb(0xFFFFFF))
    centred("OF THE CELL'S PEAK", x: c.x, y: c.y - 78, size: 24, weight: .semibold, colour: rgb(ice), kern: 4)

    let title = "TEE PS3 BENCHMARK", ts = textSize(title, size: 64, weight: .heavy, kern: 2)
    text(title, at: CGPoint(x: c.x - (ts.width - 2) / 2, y: 790), size: 64, weight: .heavy, colour: rgb(0xFFFFFF), kern: 2)
    centred("CELL  ·  RSX  ·  RAM     BENCHMARK AND STRESS TEST", x: c.x, y: 880, size: 26, weight: .semibold,
            colour: rgb(ice, 0.9), kern: 3)
}
