import CoreGraphics
import Foundation

public struct TreemapItem {
    public let node: Node
    public let rect: CGRect
}

/// Squarified treemap: rectangles with an area proportional to size, as close to squares as possible.
public enum Treemap {
    public static func layout(_ nodes: [Node], in rect: CGRect) -> [TreemapItem] {
        let sized = nodes.filter { !$0.removed }.map { ($0, $0.liveSize) }.filter { $0.1 > 0 }.sorted { $0.1 > $1.1 }  // sizes read once (a live scan keeps adding)
        let items = sized.map { $0.0 }
        let total = sized.reduce(0.0) { $0 + Double($1.1) }
        if items.isEmpty || total <= 0 || rect.width < 1 || rect.height < 1 { return [] }
        let scale = Double(rect.width * rect.height) / total
        let areas = sized.map { Double($0.1) * scale }
        var result: [TreemapItem] = []
        var r = rect
        var i = 0

        func worst(_ from: Int, _ to: Int, _ sum: Double, _ side: Double) -> Double {
            let big = areas[from], small = areas[to]  // sorted, so first is the biggest and last the smallest
            return max(side * side * big / (sum * sum), (sum * sum) / (side * side * small))
        }

        while i < items.count {
            let side = Double(min(r.width, r.height))
            if side < 1 { break }
            var rowEnd = i
            var rowArea = 0.0
            var best = Double.infinity
            while rowEnd < items.count {
                let sum = rowArea + areas[rowEnd]
                let w = worst(i, rowEnd, sum, side)
                if rowEnd > i && w > best { break }
                best = w
                rowArea = sum
                rowEnd += 1
            }
            let thickness = rowArea / side
            if r.width >= r.height {  // a column on the left, items stacked in it
                var y = Double(r.minY)
                for k in i..<rowEnd {
                    let h = areas[k] / thickness
                    result.append(TreemapItem(node: items[k], rect: CGRect(x: r.minX, y: CGFloat(y), width: CGFloat(thickness), height: CGFloat(h))))
                    y += h
                }
                r = CGRect(x: r.minX + CGFloat(thickness), y: r.minY, width: r.width - CGFloat(thickness), height: r.height)
            } else {  // a row on top, items side by side
                var x = Double(r.minX)
                for k in i..<rowEnd {
                    let w = areas[k] / thickness
                    result.append(TreemapItem(node: items[k], rect: CGRect(x: CGFloat(x), y: r.minY, width: CGFloat(w), height: CGFloat(thickness))))
                    x += w
                }
                r = CGRect(x: r.minX, y: r.minY + CGFloat(thickness), width: r.width, height: r.height - CGFloat(thickness))
            }
            i = rowEnd
        }
        return result
    }
}
